#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>
#include <map>
#include <sstream>
#include <string>

#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_helpers.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

// A 10 x 20 x 8 mm tower with a 30 mm shelf (1.2 mm thick) sticking out of one side at z 4.
TriangleMesh shelf_mesh()
{
    TriangleMesh tower = make_cube(10., 20., 8.);
    TriangleMesh shelf = make_cube(30., 20., 1.2);
    shelf.translate(10., 0., 4.);
    tower.merge(shelf);
    return tower;
}

struct LayerZ
{
    double              layer_z = 0.;
    std::map<int, int>  extrusions_by_dz; // dz in 0.01 mm -> moves
};

// Extrusion moves of every layer, by their height above the layer's own Z.
std::vector<LayerZ> extrusion_heights(const std::string &gcode)
{
    std::vector<LayerZ> layers;
    std::istringstream  in(gcode);
    std::string         line;
    double z = 0., e = 0.;
    bool   relative = false;
    auto   value = [](const std::string &l, char axis, double &out) {
        const size_t c = l.find(';');
        const std::string body = l.substr(0, c);
        for (size_t i = 0; i < body.size(); ++i)
            if (body[i] == axis && (i == 0 || body[i - 1] == ' ')) {
                out = std::atof(body.c_str() + i + 1);
                return true;
            }
        return false;
    };
    while (std::getline(in, line)) {
        if (line.rfind(";Z:", 0) == 0 || line.rfind("; Z_HEIGHT:", 0) == 0) {
            layers.emplace_back();
            layers.back().layer_z = std::atof(line.c_str() + line.find(':') + 1);
            continue;
        }
        if (line.rfind("M83", 0) == 0) relative = true;
        if (line.rfind("M82", 0) == 0) relative = false;
        if (line.rfind("G1 ", 0) != 0 && line.rfind("G0 ", 0) != 0)
            continue;
        double v;
        if (value(line, 'Z', v)) z = v;
        double x, y, ne;
        const bool xy = value(line, 'X', x) || value(line, 'Y', y);
        if (value(line, 'E', ne)) {
            const bool extrudes = relative ? ne > 0. : ne > e;
            if (!relative) e = ne;
            if (extrudes && xy && !layers.empty())
                ++layers.back().extrusions_by_dz[int(std::lround((z - layers.back().layer_z) * 100.))];
        }
    }
    return layers;
}

std::string slice_shelf(bool staircase)
{
    return slice({ shelf_mesh() }, {
        { "layer_height",                     0.2 },
        { "initial_layer_print_height",       0.2 },
        { "wall_loops",                       2 },
        { "enable_support",                   false },
        { "gcode_comments",                   true },
        { "wave_overhangs",                   true },
        { "wave_overhangs_instead_of_bridges", true },
        { "wave_overhang_staircase",          staircase },
        { "wave_overhang_staircase_layers",   3 },
        { "wave_overhang_staircase_stagger",  1.6 },
    });
}

} // namespace

TEST_CASE("staircase waves print the shelf's next layers inside the wave layer", "[WaveOverhangs][Staircase]")
{
    const std::string plain = slice_shelf(false);
    const std::string stair = slice_shelf(true);

    // Off: everything extrudes at its layer's height.
    int plain_above = 0;
    for (const LayerZ &l : extrusion_heights(plain))
        for (auto [dz, n] : l.extrusions_by_dz)
            if (dz > 1)
                plain_above += n;
    CHECK(plain_above == 0);
    CHECK(plain.find("wave staircase") == std::string::npos);

    // On: some layer extrudes at +0.2 and +0.4 as well, never higher than the cap.
    int at_h = 0, at_2h = 0, too_high = 0;
    for (const LayerZ &l : extrusion_heights(stair))
        for (auto [dz, n] : l.extrusions_by_dz) {
            if (dz == 20) at_h += n;
            if (dz == 40) at_2h += n;
            if (dz > 61) too_high += n;
        }
    CHECK(at_h > 20);
    CHECK(at_2h > 20);
    CHECK(too_high == 0);
    // Travels over the higher steps are lifted.
    CHECK(stair.find("wave staircase: clear the higher steps") != std::string::npos);
}
