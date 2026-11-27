#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <regex>
#include <string>
#include <vector>

#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/libslic3r.h"

#include "test_helpers.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

// One extrusion move of the G-code, with the layer and feature it belongs to.
struct Move
{
    std::string type;      // ;TYPE: value
    double      layer_z;   // nominal Z of the layer (;Z: tag)
    double      z;         // actual nozzle Z
    double      e_per_mm;  // filament per mm of XY travel
    double      length;
    size_t      index;     // order in the file
};

std::vector<Move> moves(const std::string &gcode)
{
    std::vector<Move> out;
    GCodeReader       reader;
    std::string       type;
    double            layer_z = 0.;
    size_t            index   = 0;
    reader.parse_buffer(gcode, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        const std::string &raw = line.raw();
        if (raw.rfind(";TYPE:", 0) == 0)
            type = raw.substr(6);
        else if (raw.rfind(";Z:", 0) == 0)
            layer_z = std::stod(raw.substr(3));
        else if (line.extruding(self) && line.dist_XY(self) > 0.5)
            out.push_back({type, layer_z, line.new_Z(self), line.dist_E(self) / line.dist_XY(self), line.dist_XY(self), index});
        ++index;
    });
    return out;
}

std::string slice_cube(bool brick, std::initializer_list<ConfigBase::SetDeserializeItem> extra = {})
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "layer_height", "0.2" },
        { "initial_layer_print_height", "0.2" },
        { "wall_loops", "3" },
        { "sparse_infill_density", "20%" },
        { "top_shell_layers", "3" },
        { "bottom_shell_layers", "3" },
        { "only_one_wall_top", "0" },
        { "enable_arc_fitting", "0" },
        { "seam_slope_type", "none" },
        { "z_hop", "0" },
        { "use_relative_e_distances", "1" },
        { "brick_layers", brick ? "1" : "0" },
    });
    config.set_deserialize_strict(extra);
    // 20 mm cube at 0.2 mm: 100 layers.
    return slice({ cube(20.) }, config);
}

bool raised(const Move &m) { return m.z > m.layer_z + 0.05; }

} // namespace

TEST_CASE("Brick layers raise alternate inner walls by half a layer", "[BrickLayers]")
{
    const std::vector<Move> all = moves(slice_cube(true));
    REQUIRE(!all.empty());

    std::map<double, std::vector<const Move *>> by_layer;
    for (const Move &m : all)
        by_layer[m.layer_z].push_back(&m);
    REQUIRE(by_layer.size() == 100);

    size_t raised_layers = 0;
    for (const auto &[layer_z, ms] : by_layer) {
        CAPTURE(layer_z);
        for (const Move *m : ms) {
            // Only inner walls ever leave the layer plane, and only by half a layer.
            if (raised(*m)) {
                CHECK(m->type == "Inner wall");
                CHECK_THAT(m->z, WithinAbs(layer_z + 0.1, 1e-3));
            } else
                CHECK_THAT(m->z, WithinAbs(layer_z, 1e-3));
        }
        const bool has_raised = std::any_of(ms.begin(), ms.end(), [](const Move *m) { return raised(*m); });
        raised_layers += has_raised;
        // The first and the top layer are never raised.
        if (layer_z < 0.3 || layer_z > 19.9)
            CHECK_FALSE(has_raised);
        if (has_raised) {
            // Raised walls are printed after every infill line of the layer.
            size_t last_infill = 0, first_raised = SIZE_MAX;
            for (const Move *m : ms) {
                if (m->type.find("infill") != std::string::npos)
                    last_infill = std::max(last_infill, m->index);
                if (raised(*m))
                    first_raised = std::min(first_raised, m->index);
            }
            CHECK(first_raised > last_infill);
        }
    }
    // Layers 2..99 of 100.
    CHECK(raised_layers == 98);
}

TEST_CASE("Brick layers stretch the first and squash the last raised wall", "[BrickLayers]")
{
    const std::vector<Move> all = moves(slice_cube(true));

    // Reference flow: the unraised inner wall (wall 3) of a middle layer.
    auto inner_flows = [&](double layer_z, bool want_raised) {
        std::vector<double> out;
        for (const Move &m : all)
            if (std::abs(m.layer_z - layer_z) < 1e-3 && m.type == "Inner wall" && raised(m) == want_raised)
                out.push_back(m.e_per_mm);
        return out;
    };
    auto median = [](std::vector<double> v) { REQUIRE(!v.empty()); std::sort(v.begin(), v.end()); return v[v.size() / 2]; };

    const double normal = median(inner_flows(10.0, false));
    // A full brick in the middle has the flow of an ordinary wall.
    CHECK_THAT(median(inner_flows(10.0, true)), WithinRel(normal, 0.03));
    // Layer 2 (Z 0.4): the first brick reaches down to layer 1, 1.5 layers tall.
    CHECK_THAT(median(inner_flows(0.4, true)), WithinRel(1.5 * normal, 0.03));
    // Layer 100 (Z 20): the last brick is not raised and only half a layer tall.
    const std::vector<double> top = inner_flows(20.0, false);
    CHECK_THAT(*std::min_element(top.begin(), top.end()), WithinRel(0.5 * normal, 0.03));
}

TEST_CASE("Brick layers off leaves every move in the layer plane", "[BrickLayers]")
{
    for (const Move &m : moves(slice_cube(false)))
        CHECK_THAT(m.z, WithinAbs(m.layer_z, 1e-3));
}

TEST_CASE("Brick layers keep wave overhangs and leave their layers unraised", "[BrickLayers][WaveOverhangs]")
{
    auto slice_overhang = [](bool brick) {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            { "layer_height", "0.2" },
            { "initial_layer_print_height", "0.2" },
            { "wall_loops", "3" },
            { "enable_support", "0" },
            { "wave_overhangs", "1" },
            { "wave_overhang_debug_gcode", "1" },
            // Plain waves: the staircase (on by default) has its own test below.
            { "wave_overhang_staircase", "0" },
            { "brick_layers", brick ? "1" : "0" },
        });
        return slice({ TestMesh::overhang }, config);
    };
    const std::string with_brick = slice_overhang(true);
    const std::string without    = slice_overhang(false);

    auto count = [](const std::string &s, const std::string &token) {
        size_t n = 0;
        for (size_t pos = s.find(token); pos != std::string::npos; pos = s.find(token, pos + 1))
            ++n;
        return n;
    };
    // The waves are generated exactly as without brick layers.
    const size_t waves = count(without, "; WAVE_OVERHANG_START");
    REQUIRE(waves > 0);
    CHECK(count(with_brick, "; WAVE_OVERHANG_START") == waves);

    // No raised wall on a layer that prints wave traces, and brick layers do happen elsewhere.
    std::map<double, bool> wave_layer;
    {
        GCodeReader reader;
        double      layer_z = 0.;
        reader.parse_buffer(with_brick, [&](GCodeReader &, const GCodeReader::GCodeLine &line) {
            if (line.raw().rfind(";Z:", 0) == 0)
                layer_z = std::stod(line.raw().substr(3));
            else if (line.raw().rfind("; WAVE_OVERHANG_START", 0) == 0)
                wave_layer[layer_z] = true;
        });
    }
    REQUIRE(!wave_layer.empty());
    size_t raised_moves = 0;
    for (const Move &m : moves(with_brick))
        if (raised(m)) {
            ++raised_moves;
            CHECK_FALSE(wave_layer.count(m.layer_z));
        }
    CHECK(raised_moves > 0);
}

TEST_CASE("Brick layers leave Z-pin channel walls at the layer height", "[BrickLayers][ZPins]")
{
    // The pin fill circles inside the channel with the nozzle just above the layer, so a raised channel
    // wall would be in its way. Everything else keeps its bricks.
    const std::string gcode = slice_cube(true, {
        { "z_pins", "1" },
        { "z_pin_diameter", "1.5" },
        { "z_pin_spacing", "6" },
        { "z_pin_interval", "8" },
    });
    std::vector<Vec2d> pins;
    {
        const std::regex re(R"(; Z_PIN_INJECT X([-\d.]+) Y([-\d.]+))");
        std::smatch      m;
        std::string      rest = gcode;
        while (std::regex_search(rest, m, re)) {
            const Vec2d p(std::stod(m[1]), std::stod(m[2]));
            if (std::none_of(pins.begin(), pins.end(), [&p](const Vec2d &q) { return (p - q).norm() < 0.01; }))
                pins.push_back(p);
            rest = m.suffix();
        }
    }
    REQUIRE(!pins.empty());

    // Channel wall centre line: pin radius + half a line; anything raised must stay a line width beyond it.
    const double keep_out = 0.75 + 0.45 + 0.2;
    size_t       raised_moves = 0, near_pin_raised = 0;
    GCodeReader  reader;
    double       layer_z = 0.;
    std::string  type;
    reader.parse_buffer(gcode, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        const std::string &raw = line.raw();
        if (raw.rfind(";Z:", 0) == 0)
            layer_z = std::stod(raw.substr(3));
        else if (raw.rfind(";TYPE:", 0) == 0)
            type = raw.substr(6);
        // Walls only: the pin fill itself circles inside the channel just above the layer (sparse infill).
        else if (type.find("wall") != std::string::npos && line.extruding(self) && line.dist_XY(self) > 0. &&
                 line.new_Z(self) > layer_z + 0.05) {
            ++raised_moves;
            const Vec2d a(self.x(), self.y()), b(line.new_X(self), line.new_Y(self));
            for (const Vec2d &p : pins)
                if ((a - p).norm() < keep_out || (b - p).norm() < keep_out)
                    ++near_pin_raised;
        }
    });
    CHECK(raised_moves > 0);
    CHECK(near_pin_raised == 0);
}

TEST_CASE("Brick layers leave the layers of a staircase wave band unraised", "[BrickLayers][WaveOverhangs][Staircase]")
{
    // A 10 x 20 x 8 mm tower with a 30 mm shelf (1.2 mm thick) sticking out of one side at z 4.
    TriangleMesh tower = make_cube(10., 20., 8.);
    TriangleMesh shelf = make_cube(30., 20., 1.2);
    shelf.translate(10., 0., 4.);
    tower.merge(shelf);

    // The band (4 layers) reaches higher than the wave floor layers (1): brick layers skip the band's layers too.
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "layer_height", "0.2" },
        { "initial_layer_print_height", "0.2" },
        { "wall_loops", "3" },
        { "enable_support", "0" },
        { "wave_overhangs", "1" },
        { "wave_overhangs_instead_of_bridges", "1" },
        { "wave_overhang_floor_layers", "1" },
        { "wave_overhang_debug_gcode", "1" },
        { "wave_overhang_staircase", "1" },
        { "wave_overhang_staircase_layers", "4" },
        { "wave_overhang_staircase_max_height", "0.6" },
        { "brick_layers", "1" },
    });
    const std::string gcode = slice({ tower }, config);

    std::vector<double> wave_z;
    {
        GCodeReader reader;
        double      layer_z = 0.;
        reader.parse_buffer(gcode, [&](GCodeReader &, const GCodeReader::GCodeLine &line) {
            if (line.raw().rfind(";Z:", 0) == 0)
                layer_z = std::stod(line.raw().substr(3));
            else if (line.raw().rfind("; WAVE_OVERHANG_START", 0) == 0 &&
                     std::find(wave_z.begin(), wave_z.end(), layer_z) == wave_z.end())
                wave_z.push_back(layer_z);
        });
    }
    REQUIRE(!wave_z.empty());
    size_t raised_moves = 0;
    for (const Move &m : moves(gcode))
        if (raised(m) && m.type == "Inner wall") {
            ++raised_moves;
            CAPTURE(m.layer_z);
            for (double z : wave_z)
                CHECK_FALSE((m.layer_z > z - 1e-3 && m.layer_z < z + 3 * 0.2 + 1e-3));
        }
    CHECK(raised_moves > 0);
}
