#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>
#include <fstream>
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

// Smallest horizontal distance from any nozzle position (every move, travels included, sampled
// every 0.2 mm) to an earlier extrusion laid higher than the nozzle is at that point.
double min_distance_to_higher_material(const std::string &gcode, int *worst_line = nullptr)
{
    struct P { double x, y, z; };
    std::vector<P>     high;   // samples of extrusions laid above their layer
    std::istringstream in(gcode);
    std::string        line;
    double x = 0., y = 0., z = 0., layer_z = 0., best = 1e9;
    int    line_no = 0;
    auto   value = [](const std::string &l, char axis, double &out) {
        const std::string body = l.substr(0, l.find(';'));
        for (size_t i = 0; i < body.size(); ++i)
            if (body[i] == axis && (i == 0 || body[i - 1] == ' ')) {
                out = std::atof(body.c_str() + i + 1);
                return true;
            }
        return false;
    };
    while (std::getline(in, line)) {
        ++line_no;
        if (line.rfind(";Z:", 0) == 0) {
            layer_z = std::atof(line.c_str() + 3);
            continue;
        }
        if (line.rfind("G1 ", 0) != 0 && line.rfind("G0 ", 0) != 0)
            continue;
        double nx = x, ny = y, nz = z, e = 0.;
        value(line, 'X', nx); value(line, 'Y', ny); value(line, 'Z', nz);
        const bool extrudes = value(line, 'E', e) && e > 0.;
        const double len = std::hypot(nx - x, ny - y) + std::abs(nz - z);
        const int    n   = std::max(1, int(len / 0.2));
        std::vector<P> pts;
        for (int i = 0; i <= n; ++i) {
            const double t = double(i) / n;
            pts.push_back({ x + (nx - x) * t, y + (ny - y) * t, z + (nz - z) * t });
        }
        for (const P &p : pts)
            for (const P &q : high)
                if (q.z > p.z + 0.01 && std::abs(q.x - p.x) < 3. && std::abs(q.y - p.y) < 3.) {
                    const double d = std::hypot(q.x - p.x, q.y - p.y);
                    if (d < best) {
                        best = d;
                        if (worst_line)
                            *worst_line = line_no;
                    }
                }
        if (extrudes && nz > layer_z + 0.01)
            high.insert(high.end(), pts.begin(), pts.end());
        x = nx; y = ny; z = nz;
    }
    return best;
}

std::string slice_shelf(bool staircase, int copies = 1)
{
    if (copies == 2)
        return slice({ shelf_mesh(), shelf_mesh() }, {
            { "layer_height",                     0.2 },
            { "initial_layer_print_height",       0.2 },
            { "wall_loops",                       2 },
            { "enable_support",                   false },
            { "gcode_comments",                   true },
            { "wave_overhangs",                   true },
            { "wave_overhangs_instead_of_bridges", true },
            { "wave_overhang_staircase",          staircase },
            { "wave_overhang_staircase_stagger",  1.6 },
        });
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

// Two shelves on one plate: a layer change travels from one object to the other and rises from
// the old layer's height on the way, over the other shelf's top step.
TEST_CASE("staircase waves never leave the nozzle beside higher material, travels included", "[WaveOverhangs][Staircase]")
{
    const std::string gcode = slice_shelf(true, 2);
    REQUIRE(gcode.find("wave staircase: clear the higher steps") != std::string::npos);
    // Nozzle centre to the centre line of higher material: the stagger less a line's half width.
    int          worst = 0;
    const double d     = min_distance_to_higher_material(gcode, &worst);
    INFO("closest at G-code line " << worst);
    if (d <= 1.6 - 0.3) {
        std::ofstream("staircase_two_shelves.gcode") << gcode;
        UNSCOPED_INFO("G-code written to staircase_two_shelves.gcode");
    }
    CHECK(d > 1.6 - 0.3);
}

// Jonas's presets avoid crossing walls: a rerouted travel between two edge wall pieces of a step
// must not sink to the layer height into the step it just printed.
TEST_CASE("staircase travels rerouted around walls stay on the step", "[WaveOverhangs][Staircase]")
{
    const std::string gcode = slice({ shelf_mesh() }, {
        { "layer_height",                      0.2 },
        { "initial_layer_print_height",        0.2 },
        { "wall_loops",                        2 },
        { "enable_support",                    false },
        { "gcode_comments",                    true },
        { "reduce_crossing_wall",              true },
        { "wave_overhangs",                    true },
        { "wave_overhangs_instead_of_bridges", true },
        { "wave_overhang_staircase",           true },
        { "wave_overhang_staircase_walls",     true },
        { "wave_overhang_staircase_stagger",   1.6 },
    });
    int          worst = 0;
    const double d     = min_distance_to_higher_material(gcode, &worst);
    INFO("closest at G-code line " << worst);
    if (d <= 1.6 - 0.3)
        std::ofstream("staircase_rerouted.gcode") << gcode;
    CHECK(d > 1.6 - 0.3);
}

namespace {

struct Move { double x0, y0, x1, y1, z, layer_z, f; bool extrudes; };

std::vector<Move> moves_of(const std::string &gcode)
{
    std::vector<Move>  out;
    std::istringstream in(gcode);
    std::string        line;
    double x = 0., y = 0., z = 0., f = 0., layer_z = 0.;
    auto value = [](const std::string &l, char axis, double &v) {
        const std::string body = l.substr(0, l.find(';'));
        for (size_t i = 0; i < body.size(); ++i)
            if (body[i] == axis && (i == 0 || body[i - 1] == ' ')) {
                v = std::atof(body.c_str() + i + 1);
                return true;
            }
        return false;
    };
    while (std::getline(in, line)) {
        if (line.rfind(";Z:", 0) == 0) {
            layer_z = std::atof(line.c_str() + 3);
            continue;
        }
        if (line.rfind("G1 ", 0) != 0 && line.rfind("G0 ", 0) != 0)
            continue;
        double nx = x, ny = y, e = 0.;
        const bool xy = value(line, 'X', nx) | value(line, 'Y', ny);
        value(line, 'Z', z);
        value(line, 'F', f);
        if (xy)
            out.push_back({ x, y, nx, ny, z, layer_z, f, value(line, 'E', e) && e > 0. });
        x = nx; y = ny;
    }
    return out;
}

std::string slice_floor_speed(bool staircase)
{
    return slice({ shelf_mesh() }, {
        { "layer_height",                      0.2 },
        { "initial_layer_print_height",        0.2 },
        { "wall_loops",                        2 },
        { "enable_support",                    false },
        { "gcode_comments",                    true },
        { "wave_overhangs",                    true },
        { "wave_overhangs_instead_of_bridges", true },
        { "wave_overhang_floor_layers",        2 },
        { "wave_overhang_floor_print_speed",   7. },
        { "slow_down_for_layer_cooling",       false },
        { "wave_overhang_staircase",           staircase },
        { "wave_overhang_staircase_layers",    4 },
        { "wave_overhang_staircase_stagger",   1.6 },
    });
}

} // namespace

// wave_overhang_floor_print_speed with the ordinary (non-Hilbert) floor: only the infill directly
// above the wave strip slows down, the rest of the layer keeps its speed. With the staircase, the
// steps that stand in for the floor layers (+1, +2) slow down too, the step above them does not.
TEST_CASE("wave floor speed applies only directly above the wave", "[WaveOverhangs][Staircase]")
{
    for (bool staircase : { false, true }) {
        CAPTURE(staircase);
        const std::vector<Move> mv = moves_of(slice_floor_speed(staircase));
        double min_x = 1e9;
        for (const Move &m : mv)
            if (m.extrudes && m.layer_z > 0.5)
                min_x = std::min({ min_x, m.x0, m.x1 });
        REQUIRE(min_x < 1e8);
        // The tower is 10 mm wide; the shelf sticks out past x = min_x + 10.
        const double edge = min_x + 10.;
        int slow = 0, slow_on_tower = 0, slow_steps[4] = { 0, 0, 0, 0 }, fast_steps[4] = { 0, 0, 0, 0 };
        for (const Move &m : mv) {
            if (!m.extrudes)
                continue;
            const bool is_slow = std::abs(m.f - 7. * 60.) < 1.;
            const int  step    = int(std::lround((m.z - m.layer_z) / 0.2));
            if (step >= 1 && step <= 3)
                (is_slow ? slow_steps : fast_steps)[step] += 1;
            if (!is_slow)
                continue;
            ++slow;
            // Staircase steps (z above the layer) are slowed whole, root extension over the tower
            // included; only the ordinary layer's moves must stay over the wave. An infill line
            // may cross the strip's anchor overlap by a line width or so.
            if (step == 0 && std::min(m.x0, m.x1) < edge - 1.5)
                ++slow_on_tower;
        }
        CHECK(slow > 10);
        CHECK(slow_on_tower == 0);
        if (staircase) {
            CHECK(slow_steps[1] > 10);
            CHECK(slow_steps[2] > 10);
            CHECK(fast_steps[1] == 0);
            CHECK(fast_steps[2] == 0);
            CHECK(slow_steps[3] == 0);
            CHECK(fast_steps[3] > 5);
        }
    }
}
