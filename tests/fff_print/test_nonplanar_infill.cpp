#include <catch2/catch_all.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/NonplanarInfill.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/libslic3r.h"

#include "test_helpers.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

// One extrusion move of the G-code, with the layer's nominal Z and the role it was printed as.
struct Move
{
    std::string role;
    double      layer_z;
    double      z;
    double      x, y;
    double      length; // XY
    double      e;
};

std::vector<Move> extrusion_moves(const std::string &gcode)
{
    std::vector<Move> moves;
    std::string       role;
    double            layer_z = 0.;
    GCodeReader       reader;
    reader.parse_buffer(gcode, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        const std::string &raw = line.raw();
        if (raw.rfind(";TYPE:", 0) == 0) {
            role = raw.substr(6);
            return;
        }
        if (raw.rfind(";Z:", 0) == 0) {
            layer_z = std::stod(raw.substr(3));
            return;
        }
        if (! line.extruding(self) || line.dist_XY(self) <= EPSILON)
            return;
        Move m;
        m.role    = role;
        m.layer_z = layer_z;
        m.z       = line.has(Z) ? line.z() : self.z();
        m.x       = line.has(X) ? line.x() : self.x();
        m.y       = line.has(Y) ? line.y() : self.y();
        m.length  = line.dist_XY(self);
        m.e       = line.dist_E(self);
        moves.push_back(m);
    });
    return moves;
}

DynamicPrintConfig cube_config(std::initializer_list<ConfigBase::SetDeserializeItem> extra)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "layer_height", "0.2" },
        { "initial_layer_print_height", "0.2" },
        { "sparse_infill_density", "20%" },
        { "sparse_infill_pattern", "rectilinear" },
        { "top_shell_layers", "3" },
        { "bottom_shell_layers", "3" },
        { "wall_loops", "2" },
        { "enable_arc_fitting", "0" },
        { "small_area_infill_flow_compensation", "0" },
        { "gcode_comments", "1" },
    });
    config.set_deserialize_strict(extra);
    return config;
}

bool is_sparse(const Move &m) { return m.role == "Sparse infill"; }

} // namespace

TEST_CASE("Non-planar infill is off by default", "[NonplanarInfill]")
{
    const std::string gcode = slice({ cube(20) }, cube_config({}));
    for (const Move &m : extrusion_moves(gcode))
        CHECK_THAT(m.z, Catch::Matchers::WithinAbs(m.layer_z, 1e-4));
}

TEST_CASE("Non-planar infill waves the sparse infill of a cube and leaves the rest flat", "[NonplanarInfill]")
{
    const std::string       gcode = slice({ cube(20) }, cube_config({ { "nonplanar_infill", "1" },
                                                                      { "nonplanar_infill_amplitude", "0.1" },
                                                                      { "nonplanar_infill_wavelength", "8" } }));
    const std::vector<Move> moves = extrusion_moves(gcode);

    size_t                   waved = 0;
    double                   max_dz = 0.;
    std::map<double, double> layer_max_dz; // by layer Z, sparse infill only
    for (const Move &m : moves) {
        const double dz = m.z - m.layer_z;
        if (is_sparse(m)) {
            max_dz = std::max(max_dz, std::abs(dz));
            layer_max_dz[m.layer_z] = std::max(layer_max_dz[m.layer_z], std::abs(dz));
            if (std::abs(dz) > 0.02)
                ++waved;
        } else {
            // Walls, solid, top and bottom infill stay on the layer's plane.
            INFO("role " << m.role << " at layer " << m.layer_z);
            CHECK_THAT(dz, Catch::Matchers::WithinAbs(0., 1e-4));
        }
    }
    CHECK(waved > 100);
    CHECK(max_dz <= 0.1 + 1e-3);
    CHECK(max_dz >= 0.09);

    // The sparse infill layers right on the bottom shell and right under the top shell are flat; the ones between
    // reach the full wave.
    REQUIRE(layer_max_dz.size() >= 8);
    CHECK(layer_max_dz.begin()->second <= 1e-4);
    CHECK(layer_max_dz.rbegin()->second <= 1e-4);
    auto mid = layer_max_dz.begin();
    std::advance(mid, layer_max_dz.size() / 2);
    CHECK(mid->second >= 0.09);

    // Flow: with the same wave on every layer each layer keeps its thickness, so in the middle of the stack the
    // extrusion per mm matches the flat print.
    const std::vector<Move> flat = extrusion_moves(slice({ cube(20) }, cube_config({})));
    double flat_e = 0., flat_len = 0.;
    for (const Move &m : flat)
        if (is_sparse(m) && std::abs(m.layer_z - mid->first) < 1e-4) {
            flat_e += m.e;
            flat_len += m.length;
        }
    REQUIRE(flat_len > 0.);
    const double flat_e_per_mm = flat_e / flat_len;
    for (const Move &m : moves)
        if (is_sparse(m) && std::abs(m.layer_z - mid->first) < 1e-4 && m.length > 0.3) {
            // A 3D move's extrusion is spread over its XY length here; the Z part is tiny.
            CHECK(m.e / m.length / flat_e_per_mm == Catch::Approx(1.).epsilon(0.03));
        }
}

TEST_CASE("Non-planar infill with a phase shift gives more flow where the gap below opens", "[NonplanarInfill]")
{
    const std::vector<Move> moves = extrusion_moves(slice({ cube(20) }, cube_config({ { "nonplanar_infill", "1" },
                                                                                      { "nonplanar_infill_amplitude", "0.05" },
                                                                                      { "nonplanar_infill_wavelength", "8" },
                                                                                      { "nonplanar_infill_phase_shift", "180" } })));
    // Middle sparse layer.
    std::vector<double> layers;
    for (const Move &m : moves)
        if (is_sparse(m) && (layers.empty() || layers.back() != m.layer_z))
            layers.push_back(m.layer_z);
    REQUIRE(layers.size() >= 8);
    const double mid = layers[layers.size() / 2];
    // Flat-print extrusion per mm of the mid layer, taken from moves that sit near the zero crossings of the wave.
    std::vector<double> up, down, level;
    for (const Move &m : moves) {
        if (! is_sparse(m) || std::abs(m.layer_z - mid) > 1e-4 || m.length < 0.3)
            continue;
        const double dz = m.z - m.layer_z;
        const double e_per_mm = m.e / m.length;
        if (dz > 0.04)
            up.push_back(e_per_mm);
        else if (dz < -0.04)
            down.push_back(e_per_mm);
        else if (std::abs(dz) < 0.005)
            level.push_back(e_per_mm);
    }
    REQUIRE(! up.empty());
    REQUIRE(! down.empty());
    REQUIRE(! level.empty());
    auto mean = [](const std::vector<double> &v) { double s = 0.; for (double x : v) s += x; return s / double(v.size()); };
    // The layer below has the opposite wave, so up by dz opens the gap by 2 dz: about 1 + 2 * 0.045 / 0.2 = 1.45.
    CHECK(mean(up) / mean(level) > 1.3);
    CHECK(mean(down) / mean(level) < 0.7);
}

TEST_CASE("Non-planar infill marks sparse infill paths as Z contoured on the layers", "[NonplanarInfill]")
{
    Print print;
    init_and_process_print({ cube(20) }, print, cube_config({ { "nonplanar_infill", "1" } }));
    const PrintObject &object = *print.objects().front();
    size_t with_field = 0;
    for (const Layer *layer : object.layers())
        for (const LayerRegion *layerm : layer->regions())
            if (layerm->nonplanar_infill)
                ++with_field;
    CHECK(with_field > 0);
    // Turning it off again makes the paths flat.
    DynamicPrintConfig off = cube_config({ { "nonplanar_infill", "0" } });
    print.apply(print.model(), off);
    print.process();
    for (const Layer *layer : object.layers())
        for (const LayerRegion *layerm : layer->regions())
            CHECK_FALSE(layerm->nonplanar_infill);
}

TEST_CASE("Non-planar combined infill takes its flow from the layer under the line's bottom", "[NonplanarInfill]")
{
    // Infill combined over two layers with the wave turned 180 degrees per layer: each combined line lies on the line
    // two layers down, which has the same wave, so the gap stays the layer height everywhere. The skipped layer in
    // between (half a turn off) must not count.
    const std::vector<Move> moves = extrusion_moves(slice({ cube(20) }, cube_config({ { "nonplanar_infill", "1" },
                                                                                      { "nonplanar_infill_amplitude", "0.05" },
                                                                                      { "nonplanar_infill_wavelength", "8" },
                                                                                      { "nonplanar_infill_phase_shift", "180" },
                                                                                      { "infill_combination", "1" } })));
    std::vector<double> layers;
    for (const Move &m : moves)
        if (is_sparse(m) && (layers.empty() || layers.back() != m.layer_z))
            layers.push_back(m.layer_z);
    REQUIRE(layers.size() >= 6);
    // Combined: sparse infill on every other layer only.
    CHECK_THAT(layers[layers.size() / 2 + 1] - layers[layers.size() / 2], Catch::Matchers::WithinAbs(0.4, 1e-4));
    const double mid = layers[layers.size() / 2];
    std::vector<double> waved, level;
    for (const Move &m : moves) {
        if (! is_sparse(m) || std::abs(m.layer_z - mid) > 1e-4 || m.length < 0.3)
            continue;
        const double dz = m.z - m.layer_z;
        (std::abs(dz) > 0.04 ? waved : level).push_back(m.e / m.length);
    }
    REQUIRE(! waved.empty());
    REQUIRE(! level.empty());
    for (const double e : waved)
        CHECK(e / level.front() == Catch::Approx(1.).epsilon(0.03));
}

TEST_CASE("Non-planar infill can be turned on for one object only", "[NonplanarInfill]")
{
    // Per-object settings (the GUI's object list, a 3MF's object config, the CLI's assemble list print_params) reach
    // the region config of that object only.
    Print print;
    Model model;
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides = { { { "nonplanar_infill", "1" } }, {} };
    init_print(std::vector<TriangleMesh>{ cube(20), cube(20) }, print, model, cube_config({}), &overrides);
    print.process();
    REQUIRE(print.objects().size() == 2);
    auto waved_layers = [](const PrintObject &object) {
        size_t n = 0;
        for (const Layer *layer : object.layers())
            for (const LayerRegion *layerm : layer->regions())
                if (layerm->nonplanar_infill)
                    ++n;
        return n;
    };
    // Print holds its own copy of the model: tell the objects apart by their config.
    size_t on = 0, off = 0;
    for (const PrintObject *object : print.objects()) {
        REQUIRE(object->num_printing_regions() == 1);
        if (object->printing_region(0).config().nonplanar_infill.value) {
            ++on;
            CHECK(waved_layers(*object) > 0);
        } else {
            ++off;
            CHECK(waved_layers(*object) == 0);
        }
    }
    CHECK(on == 1);
    CHECK(off == 1);
}
