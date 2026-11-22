#include "GCodeHtmlExport.hpp"

#include "I18N.hpp"

#include "libvgcode/include/PathVertex.hpp"
#include "libvgcode/include/Types.hpp"
#include "libvgcode/include/Viewer.hpp"

#include <boost/beast/core/detail/base64.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>
#include <miniz.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <sstream>

namespace Slic3r {
namespace GUI {

namespace {

const char* const DATA_PLACEHOLDER = "\"__ORCA_GCODE_DATA__\"";

// Column-oriented packing: each column is one typed array in the browser. Positions and ids are
// delta coded, which leaves long runs of small numbers that deflate compresses well.
class Columns
{
public:
    template<class T> void add(const std::string& name, const std::vector<T>& values, const char* type)
    {
        // Typed arrays need their offset aligned to the element size.
        while (m_bytes.size() % 4 != 0)
            m_bytes.push_back(0);
        const size_t offset = m_bytes.size();
        const size_t size   = values.size() * sizeof(T);
        m_bytes.resize(offset + size);
        if (size > 0)
            std::memcpy(m_bytes.data() + offset, values.data(), size);
        m_table.push_back({{"name", name}, {"type", type}, {"offset", offset}, {"count", values.size()}});
    }

    const std::vector<unsigned char>& bytes() const { return m_bytes; }
    const nlohmann::json&             table() const { return m_table; }

private:
    std::vector<unsigned char> m_bytes;
    nlohmann::json             m_table = nlohmann::json::array();
};

template<class T> T clamp_round(double v, double lo, double hi)
{
    if (!std::isfinite(v))
        return T(0);
    return static_cast<T>(std::lround(std::clamp(v, lo, hi)));
}

std::string hex(uint8_t r, uint8_t g, uint8_t b)
{
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", r, g, b);
    return buf;
}

std::string view_type_name(libvgcode::EViewType type)
{
    using V = libvgcode::EViewType;
    switch (type) {
    case V::ColorPrint: return "colorprint";
    case V::Speed:
    case V::ActualSpeed: return "speed";
    case V::Height: return "height";
    case V::Width: return "width";
    case V::VolumetricFlowRate:
    case V::ActualVolumetricFlowRate: return "flow";
    case V::LayerTimeLinear:
    case V::LayerTimeLogarithmic: return "layertime";
    case V::FanSpeed: return "fan";
    case V::Temperature: return "temperature";
    case V::PressureAdvance: return "pa";
    case V::Acceleration: return "acceleration";
    case V::Tool: return "tool";
    default: return "feature";
    }
}

std::vector<std::string> role_names()
{
    // Same order as libvgcode::EGCodeExtrusionRole.
    return {_u8L("Undefined"),         _u8L("Inner wall"),         _u8L("Outer wall"),      _u8L("Overhang wall"),
            _u8L("Sparse infill"),  _u8L("Internal solid infill"), _u8L("Top surface"),  _u8L("Ironing"),
            _u8L("Bridge"),         _u8L("Gap infill"),         _u8L("Skirt"),           _u8L("Support"),
            _u8L("Support interface"), _u8L("Prime tower"),     _u8L("Custom"),          _u8L("Bottom surface"),
            _u8L("Internal Bridge"), _u8L("Brim"),              _u8L("Support transition"), _u8L("Multiple")};
}

} // namespace

bool export_gcode_html(const libvgcode::Viewer& viewer,
                       const GCodeHtmlInfo&     info,
                       const std::string&       template_path,
                       const std::string&       filename,
                       std::string&             error)
{
    std::string html;
    {
        boost::nowide::ifstream in(template_path, std::ios::binary);
        if (!in) {
            error = "Missing viewer template: " + template_path;
            return false;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        html = ss.str();
    }
    const size_t placeholder = html.find(DATA_PLACEHOLDER);
    if (placeholder == std::string::npos) {
        error = "The viewer template has no data placeholder: " + template_path;
        return false;
    }

    const size_t n = viewer.get_vertices_count();
    if (n == 0) {
        error = "There are no toolpaths to export.";
        return false;
    }

    std::vector<int32_t>  px(n), py(n), pz(n), layer(n), line(n);
    std::vector<uint16_t> width(n), height(n), feedrate(n), temperature(n);
    std::vector<uint8_t>  fan(n), role(n), type(n), tool(n), color(n);
    std::vector<float>    mm3_per_mm(n), time(n), acceleration(n), pa(n);

    int32_t last_x = 0, last_y = 0, last_z = 0, last_layer = 0, last_line = 0;
    for (size_t i = 0; i < n; ++i) {
        const libvgcode::PathVertex& v = viewer.get_vertex_at(i);
        const bool   valid = v.position[0] != FLT_MAX;
        const double x     = valid ? (double(v.position[0]) - info.origin_x) * 1000.0 : last_x;
        const double y     = valid ? (double(v.position[1]) - info.origin_y) * 1000.0 : last_y;
        const double z     = valid ? double(v.position[2]) * 1000.0 : last_z;
        const int32_t ix   = clamp_round<int32_t>(x, -2.0e9, 2.0e9);
        const int32_t iy   = clamp_round<int32_t>(y, -2.0e9, 2.0e9);
        const int32_t iz   = clamp_round<int32_t>(z, -2.0e9, 2.0e9);
        px[i]    = ix - last_x;
        py[i]    = iy - last_y;
        pz[i]    = iz - last_z;
        last_x   = ix;
        last_y   = iy;
        last_z   = iz;
        layer[i] = int32_t(v.layer_id) - last_layer;
        line[i]  = int32_t(v.gcode_id) - last_line;
        last_layer = int32_t(v.layer_id);
        last_line  = int32_t(v.gcode_id);

        width[i]       = clamp_round<uint16_t>(v.width * 1000.0, 0.0, 65535.0);
        height[i]      = clamp_round<uint16_t>(v.height * 1000.0, 0.0, 65535.0);
        feedrate[i]    = clamp_round<uint16_t>(v.feedrate * 10.0, 0.0, 65535.0);
        temperature[i] = clamp_round<uint16_t>(v.temperature, 0.0, 65535.0);
        fan[i]         = clamp_round<uint8_t>(v.fan_speed, 0.0, 255.0);
        role[i]        = uint8_t(v.role);
        type[i]        = uint8_t(v.type);
        tool[i]        = v.extruder_id;
        color[i]       = v.color_id;
        mm3_per_mm[i]  = v.mm3_per_mm;
        time[i]        = v.times[0];
        acceleration[i] = v.acceleration;
        pa[i]          = v.pressure_advance;
    }

    Columns columns;
    columns.add("x", px, "i32d");
    columns.add("y", py, "i32d");
    columns.add("z", pz, "i32d");
    columns.add("layer", layer, "i32d");
    columns.add("line", line, "i32d");
    columns.add("width", width, "u16");
    columns.add("height", height, "u16");
    columns.add("feedrate", feedrate, "u16");
    columns.add("temperature", temperature, "u16");
    columns.add("fan", fan, "u8");
    columns.add("role", role, "u8");
    columns.add("type", type, "u8");
    columns.add("tool", tool, "u8");
    columns.add("color", color, "u8");
    columns.add("mm3_per_mm", mm3_per_mm, "f32");
    columns.add("time", time, "f32");
    columns.add("acceleration", acceleration, "f32");
    columns.add("pa", pa, "f32");

    const std::vector<unsigned char>& raw = columns.bytes();
    mz_ulong                          compressed_size = mz_compressBound(mz_ulong(raw.size()));
    std::vector<unsigned char>        compressed(compressed_size);
    if (mz_compress2(compressed.data(), &compressed_size, raw.data(), mz_ulong(raw.size()), MZ_BEST_COMPRESSION) != MZ_OK) {
        error = "Could not compress the toolpaths.";
        return false;
    }
    compressed.resize(compressed_size);

    std::string blob;
    blob.resize(boost::beast::detail::base64::encoded_size(compressed.size()));
    blob.resize(boost::beast::detail::base64::encode(blob.data(), compressed.data(), compressed.size()));

    nlohmann::json meta;
    meta["format"]    = 1;
    meta["title"]     = info.title;
    meta["printer"]   = info.printer;
    meta["generator"] = info.generator;
    meta["vertices"]  = n;
    meta["columns"]   = columns.table();
    meta["raw_size"]  = raw.size();
    meta["view"]      = view_type_name(viewer.get_view_type());
    meta["estimated_time"] = viewer.get_estimated_time();
    meta["layers_z"]  = viewer.get_layers_zs();

    nlohmann::json roles = nlohmann::json::array();
    const std::vector<std::string> names = role_names();
    for (size_t i = 0; i < libvgcode::GCODE_EXTRUSION_ROLES_COUNT; ++i) {
        const libvgcode::Color& c = viewer.get_extrusion_role_color(libvgcode::EGCodeExtrusionRole(i));
        roles.push_back({{"name", i < names.size() ? names[i] : std::to_string(i)}, {"color", hex(c[0], c[1], c[2])}});
    }
    meta["roles"] = roles;

    auto palette = [](const libvgcode::PaletteWithAlpha& colors) {
        nlohmann::json out = nlohmann::json::array();
        for (const auto& c : colors)
            out.push_back(hex(c[0], c[1], c[2]));
        return out;
    };
    meta["tools"]        = palette(viewer.get_tool_colors());
    meta["color_prints"] = palette(viewer.get_color_print_colors());
    meta["filaments"]    = info.filaments;

    const libvgcode::Color& travel = viewer.get_option_color(libvgcode::EOptionType::Travels);
    const libvgcode::Color& wipe   = viewer.get_option_color(libvgcode::EOptionType::Wipes);
    meta["travel_color"] = hex(travel[0], travel[1], travel[2]);
    meta["wipe_color"]   = hex(wipe[0], wipe[1], wipe[2]);
    meta["move_types"]   = {{"extrude", int(libvgcode::EMoveType::Extrude)},
                            {"travel", int(libvgcode::EMoveType::Travel)},
                            {"wipe", int(libvgcode::EMoveType::Wipe)}};

    nlohmann::json bed = nlohmann::json::array();
    for (const auto& p : info.bed)
        bed.push_back({p[0], p[1]});
    meta["bed"]        = bed;
    meta["bed_height"] = info.bed_height;
    meta["data"]       = std::move(blob);

    std::string json = meta.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    // The JSON sits inside a <script> element: keep "</script>" in a name from closing it.
    std::string safe;
    safe.reserve(json.size() + 16);
    for (char ch : json) {
        if (ch == '<')
            safe += "\\u003c";
        else
            safe += ch;
    }

    html.replace(placeholder, std::strlen(DATA_PLACEHOLDER), safe);

    const std::string tmp = filename + ".tmp";
    {
        boost::nowide::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "Could not open " + filename + " for writing.";
            return false;
        }
        out.write(html.data(), std::streamsize(html.size()));
        out.close();
        if (!out) {
            error = "Could not write " + filename + ".";
            return false;
        }
    }
    boost::system::error_code ec;
    boost::filesystem::rename(tmp, filename, ec);
    if (ec) {
        // rename does not replace an existing file on every platform
        boost::filesystem::remove(filename, ec);
        boost::filesystem::rename(tmp, filename, ec);
        if (ec) {
            error = "Could not write " + filename + ": " + ec.message();
            return false;
        }
    }
    return true;
}

} // namespace GUI
} // namespace Slic3r
