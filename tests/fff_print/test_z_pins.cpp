#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>
#include <map>
#include <regex>
#include <sstream>
#include <string>

#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/libslic3r.h"

#include "test_helpers.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

struct Injection
{
    double x, y, z, depth, volume, e;
};

// The Z_PIN_INJECT blocks of the G-code: position, the layer Z, the planned depth and volume, and the
// filament length actually extruded in the block (relative E).
std::vector<Injection> injections(const std::string &gcode)
{
    std::vector<Injection> out;
    std::istringstream     in(gcode);
    std::string            line;
    const std::regex       re(R"(; Z_PIN_INJECT X([-\d.]+) Y([-\d.]+) depth ([\d.]+) volume ([\d.]+) mm3)");
    const std::regex       e_re(R"(\bE([-\d.]+))");
    const std::regex       z_re(R"(^G1 Z([\d.]+))");
    double                 z     = 0.;
    Injection             *cur   = nullptr;
    std::smatch            m;
    while (std::getline(in, line)) {
        if (std::regex_search(line, m, z_re))
            z = std::stod(m[1]);
        if (std::regex_search(line, m, re)) {
            out.push_back({ std::stod(m[1]), std::stod(m[2]), z, std::stod(m[3]), std::stod(m[4]), 0. });
            cur = &out.back();
            continue;
        }
        if (cur == nullptr)
            continue;
        if (line.rfind("G1 X", 0) == 0 && std::regex_search(line, m, e_re) && std::stod(m[1]) > 0)
            cur->e += std::stod(m[1]);
        else if (line.rfind("G1 F", 0) == 0 || line.empty() || line[0] == ';' || line.rfind("G4", 0) == 0)
            ;
        else
            cur = nullptr;
    }
    return out;
}

DynamicPrintConfig pin_config(bool on)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "z_pins", on ? "1" : "0" },
        { "z_pin_diameter", "1.5" },
        { "z_pin_spacing", "6" },
        { "z_pin_interval", "8" },
        { "z_pin_overfill", "110%" },
        { "layer_height", "0.2" },
        { "initial_layer_print_height", "0.2" },
        { "wall_loops", "2" },
        { "top_shell_layers", "3" },
        { "bottom_shell_layers", "3" },
        { "sparse_infill_density", "20%" },
        { "use_relative_e_distances", "1" },
        { "filament_diameter", "1.75" },
        { "filament_max_volumetric_speed", "10" },
        { "nozzle_diameter", "0.4" },
        { "brim_type", "no_brim" },
        { "skirt_loops", "0" },
    });
    return config;
}

} // namespace

TEST_CASE("Z-pins inject aligned channels with the planned volume", "[ZPins]")
{
    const DynamicPrintConfig config = pin_config(true);
    Print                    print;
    Model                    model;
    init_print({ TriangleMesh(its_make_cube(20, 20, 10)) }, print, model, config);
    GCodeProcessorResult result;
    const std::string    gc = Slic3r::Test::gcode(print, &result);
    const std::vector<Injection> inj = injections(gc);
    REQUIRE(! inj.empty());

    const double area     = M_PI * 0.75 * 0.75;
    const double cross    = M_PI * 0.875 * 0.875;
    const double h        = 0.2;
    // Every injection extrudes its planned volume, and the volume is the channel volume plus 10%.
    std::map<std::pair<long, long>, std::vector<const Injection*>> by_xy;
    for (const Injection &i : inj) {
        CHECK(i.volume == Catch::Approx(area * i.depth * 1.1).epsilon(0.01));
        CHECK(i.e * cross == Catch::Approx(i.volume).epsilon(0.02));
        // At most interval + 1 layers deep, never fewer than 2.
        CHECK(i.depth <= 9 * h + 1e-6);
        CHECK(i.depth >= 2 * h - 1e-6);
        by_xy[{ std::lround(i.x * 100), std::lround(i.y * 100) }].push_back(&i);
    }
    // Channels are aligned: few distinct XY positions, each filled several times up the part, and each
    // fill starts where the one below ended.
    CHECK(by_xy.size() < inj.size());
    for (auto &[xy, list] : by_xy) {
        REQUIRE(list.size() >= 2);
        std::sort(list.begin(), list.end(), [](auto *a, auto *b) { return a->z < b->z; });
        for (size_t k = 1; k < list.size(); ++ k)
            CHECK(list[k]->z - list[k]->depth == Catch::Approx(list[k - 1]->z).margin(1e-3));
        // Pins stay out of the 3 bottom + 1 margin layers and the 3 top + 1 margin layers.
        CHECK(list.front()->z - list.front()->depth >= 4 * h - 1e-3);
        CHECK(list.back()->z <= 10. - 4 * h + 1e-3);
    }

    // The filament estimate includes the pins.
    Print   print_off;
    Model   model_off;
    init_print({ TriangleMesh(its_make_cube(20, 20, 10)) }, print_off, model_off, pin_config(false));
    GCodeProcessorResult result_off;
    const std::string    gc_off = Slic3r::Test::gcode(print_off, &result_off);
    CHECK(injections(gc_off).empty());
    double injected = 0.;
    for (const Injection &i : inj)
        injected += i.volume;
    const double vol_on  = result.print_statistics.total_volumes_per_extruder.empty() ? 0. :
                                                                                         result.print_statistics.total_volumes_per_extruder.begin()->second;
    const double vol_off = result_off.print_statistics.total_volumes_per_extruder.empty() ? 0. :
                                                                                             result_off.print_statistics.total_volumes_per_extruder.begin()->second;
    INFO("on " << vol_on << " off " << vol_off << " injected " << injected);
    CHECK(vol_on > vol_off + 0.5 * injected);
}
