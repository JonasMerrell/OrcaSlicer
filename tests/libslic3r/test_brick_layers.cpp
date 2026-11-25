#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <memory>
#include <vector>

#include "libslic3r/libslic3r.h"
#include "libslic3r/BrickLayers.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Polyline.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
namespace BL = Slic3r::BrickLayers;

namespace {

constexpr double width = 0.45;
constexpr double mm3   = 0.08;

Polygon square_mm(double half)
{
    Polygon poly;
    poly.points = { Point::new_scale(-half, -half), Point::new_scale(half, -half), Point::new_scale(half, half), Point::new_scale(-half, half) };
    return poly;
}

// A closed wall loop around a square of the given half size, as the perimeter generator emits it.
ExtrusionLoop wall(double half, int inset_idx, float height)
{
    ExtrusionPath path(inset_idx == 0 ? erExternalPerimeter : erPerimeter, mm3, float(width), height);
    Polyline      pl = square_mm(half).split_at_first_point();
    path.polyline    = Polyline3(pl);
    ExtrusionLoop loop(std::move(path));
    loop.inset_idx = inset_idx;
    return loop;
}

// A stack of layers of square walls: wall_count concentric loops 0.45 mm apart.
struct Stack
{
    std::vector<ExtrusionEntityCollection> walls;
    std::vector<BL::LayerInput>            inputs;

    Stack(const std::vector<double> &heights, int wall_count, BL::Params params = {})
    {
        walls.resize(heights.size());
        inputs.resize(heights.size());
        for (size_t l = 0; l < heights.size(); ++l) {
            for (int i = 0; i < wall_count; ++i)
                walls[l].append(wall(10. - width * i, i, float(heights[l])));
            inputs[l].height  = heights[l];
            inputs[l].enabled = l > 0;
            inputs[l].perimeters.emplace_back(&walls[l], params);
        }
    }
    const ExtrusionLoop &loop(size_t layer, size_t inset) const { return *static_cast<const ExtrusionLoop *>(walls[layer].entities[inset]); }
};

double z_offset_mm(const ExtrusionPath &path) { return unscale<double>(path.polyline.points.front().z()); }

} // namespace

TEST_CASE("Every second inner wall is raised, starting at the configured wall", "[BrickLayers]")
{
    CHECK_FALSE(BL::is_raised_wall(0, 2));
    CHECK(BL::is_raised_wall(1, 2));
    CHECK_FALSE(BL::is_raised_wall(2, 2));
    CHECK(BL::is_raised_wall(3, 2));
    CHECK_FALSE(BL::is_raised_wall(1, 3));
    CHECK(BL::is_raised_wall(2, 3));
    CHECK(BL::is_raised_wall(4, 3));
    // The outer wall is never raised, even if asked to start there.
    CHECK_FALSE(BL::is_raised_wall(0, 1));
    CHECK_FALSE(BL::is_raised_wall(-1, 2));
}

TEST_CASE("inside_intervals cuts a polyline at the region boundary in path order", "[BrickLayers]")
{
    // A 20 mm line along X through a region covering x in [5, 12].
    const Polyline line{ Point::new_scale(0, 0), Point::new_scale(20, 0) };
    Polygon        region;
    region.points = { Point::new_scale(5, -1), Point::new_scale(12, -1), Point::new_scale(12, 1), Point::new_scale(5, 1) };

    // Axis-aligned segments are what walls of boxy parts are made of.
    const BL::Intervals in = BL::inside_intervals(line, { region }, scale_(1.));
    REQUIRE(in.size() == 1);
    CHECK_THAT(unscale<double>(in.front().first), WithinAbs(5., 1e-3));
    CHECK_THAT(unscale<double>(in.front().second), WithinAbs(12., 1e-3));

    // A diagonal through the same region enters at x = 5 and leaves at x = 12 (y stays within +-1).
    const Polyline diagonal{ Point::new_scale(0, -0.5), Point::new_scale(20, 0.5) };
    const BL::Intervals din = BL::inside_intervals(diagonal, { region }, scale_(1.));
    REQUIRE(din.size() == 1);
    const double dlen = unscale<double>(diagonal.length());
    CHECK_THAT(unscale<double>(din.front().first), WithinAbs(5. / 20. * dlen, 1e-3));
    CHECK_THAT(unscale<double>(din.front().second), WithinAbs(12. / 20. * dlen, 1e-3));

    // Runs shorter than min_run are absorbed: a 0.5 mm wide region does not cut the line.
    Polygon thin;
    thin.points = { Point::new_scale(5, -1), Point::new_scale(5.5, -1), Point::new_scale(5.5, 1), Point::new_scale(5, 1) };
    CHECK(BL::inside_intervals(line, { thin }, scale_(1.)).empty());

    // Fully inside and empty region.
    CHECK(BL::inside_intervals(line, { square_mm(30.) }, scale_(1.)).size() == 1);
    CHECK(BL::inside_intervals(line, {}, scale_(1.)).empty());
}

TEST_CASE("sub_polyline extracts the stretch between two arc lengths", "[BrickLayers]")
{
    const Polyline pl{ Point::new_scale(0, 0), Point::new_scale(10, 0), Point::new_scale(10, 10) };
    const Polyline part = BL::sub_polyline(pl, scale_(5.), scale_(15.));
    REQUIRE(part.points.size() == 3);
    CHECK(part.points.front() == Point::new_scale(5, 0));
    CHECK(part.points[1] == Point::new_scale(10, 0));
    CHECK(part.points.back() == Point::new_scale(10, 5));
    CHECK_THAT(unscale<double>(part.length()), WithinAbs(10., 1e-3));
}

TEST_CASE("A wall stack interlocks with stretched first and squashed last bricks", "[BrickLayers]")
{
    // 6 layers of 0.2 mm, 3 walls. Wall 2 (inset 1) is raised.
    const double h = 0.2;
    Stack        stack(std::vector<double>(6, h), 3);
    const size_t raised = BL::apply(stack.inputs);
    CHECK(raised == 4); // layers 2..5 (1-based); layer 1 sits on the bed, layer 6 is the top

    // The outer wall and wall 3 never change.
    for (size_t l = 0; l < 6; ++l)
        for (size_t inset : { size_t(0), size_t(2) }) {
            const ExtrusionLoop &loop = stack.loop(l, inset);
            REQUIRE(loop.paths.size() == 1);
            CHECK_FALSE(loop.paths.front().z_contoured);
            CHECK_THAT(loop.paths.front().mm3_per_mm, WithinRel(mm3, 1e-9));
            CHECK_FALSE(loop.print_after_infill);
        }

    // First layer: untouched.
    CHECK_FALSE(stack.loop(0, 1).paths.front().z_contoured);
    CHECK_THAT(stack.loop(0, 1).paths.front().mm3_per_mm, WithinRel(mm3, 1e-9));

    for (size_t l = 1; l <= 4; ++l) {
        CAPTURE(l);
        const ExtrusionLoop &loop = stack.loop(l, 1);
        REQUIRE(loop.paths.size() == 1);
        const ExtrusionPath &p = loop.paths.front();
        CHECK(p.z_contoured);
        CHECK(loop.print_after_infill);
        for (const Point3 &pt : p.polyline.points)
            CHECK_THAT(unscale<double>(pt.z()), WithinAbs(h / 2., 1e-6));
        // The first brick reaches down to the top of layer 1: 1.5 layers tall.
        const double expected = l == 1 ? 1.5 : 1.0;
        CHECK_THAT(BL::emitted_mm3_per_mm(p), WithinRel(mm3 * expected, 1e-6));
        CHECK_THAT(double(p.height), WithinRel(h * expected, 1e-6));
    }

    // Top layer: nominal Z, half a layer tall on the raised brick below.
    const ExtrusionPath &top = stack.loop(5, 1).paths.front();
    CHECK_FALSE(top.z_contoured);
    CHECK_THAT(z_offset_mm(top), WithinAbs(0., 1e-9));
    CHECK_THAT(top.mm3_per_mm, WithinRel(mm3 * 0.5, 1e-9));
    CHECK_THAT(double(top.height), WithinRel(h * 0.5, 1e-6));
}

TEST_CASE("Brick heights follow variable layer heights and the flow ratio", "[BrickLayers]")
{
    // First layer 0.3, then 0.2, 0.2, 0.1, 0.1.
    BL::Params params;
    params.flow_ratio = 1.05;
    Stack stack({ 0.3, 0.2, 0.2, 0.1, 0.1 }, 2, params);
    BL::apply(stack.inputs);

    // Layer 2: top at z + 0.1, bottom at the top of layer 1 (z - 0.2): 0.3 tall.
    CHECK_THAT(BL::emitted_mm3_per_mm(stack.loop(1, 1).paths.front()), WithinRel(mm3 * 0.3 / 0.2 * 1.05, 1e-6));
    // Layer 3: a full brick.
    CHECK_THAT(BL::emitted_mm3_per_mm(stack.loop(2, 1).paths.front()), WithinRel(mm3 * 1.05, 1e-6));
    // Layer 4 (0.1): top at z + 0.05, bottom at the raised top of layer 3 (z - 0.1 + 0.1): 0.05 tall.
    const ExtrusionPath &l4 = stack.loop(3, 1).paths.front();
    CHECK(l4.z_contoured);
    CHECK_THAT(unscale<double>(l4.polyline.points.front().z()), WithinAbs(0.05, 1e-6));
    CHECK_THAT(double(l4.height), WithinRel(0.05, 1e-5));
    // Top layer: nominal, bottom at z - 0.1 + 0.05: 0.05 tall.
    const ExtrusionPath &l5 = stack.loop(4, 1).paths.front();
    CHECK_FALSE(l5.z_contoured);
    CHECK_THAT(double(l5.height), WithinRel(0.05, 1e-5));
}

TEST_CASE("first_wall 3 raises the third wall and leaves the second alone", "[BrickLayers]")
{
    BL::Params params;
    params.first_wall = 3;
    Stack stack(std::vector<double>(4, 0.2), 4, params);
    BL::apply(stack.inputs);
    CHECK_FALSE(stack.loop(2, 1).paths.front().z_contoured);
    CHECK(stack.loop(2, 2).paths.front().z_contoured);
    CHECK_FALSE(stack.loop(2, 3).paths.front().z_contoured);
}

TEST_CASE("A wall is raised only where the layer above continues it", "[BrickLayers]")
{
    // Layers 1-3 have walls around a 20 mm square; layer 4 only has the left half of wall 2
    // (as an open path), so layer 3 is raised on the left and squashed on the right.
    const double h = 0.2;
    Stack        stack(std::vector<double>(3, h), 2);
    stack.walls.emplace_back();
    {
        ExtrusionPath path(erPerimeter, mm3, float(width), float(h));
        const double  half = 10. - width;
        path.polyline       = Polyline3(Polyline{ Point::new_scale(0, -half), Point::new_scale(-half, -half), Point::new_scale(-half, half), Point::new_scale(0, half) });
        ExtrusionMultiPath mp(path);
        mp.inset_idx = 1;
        stack.walls.back().append(wall(10., 0, float(h)));
        stack.walls.back().append(mp);
    }
    stack.inputs.emplace_back();
    stack.inputs.back().height  = h;
    stack.inputs.back().enabled = true;
    for (size_t l = 0; l < stack.walls.size(); ++l)
        stack.inputs[l].perimeters = { { &stack.walls[l], BL::Params{} } };

    BL::apply(stack.inputs);

    const ExtrusionLoop &loop = stack.loop(2, 1);
    REQUIRE(loop.paths.size() >= 2);
    CHECK(loop.print_after_infill);
    size_t raised = 0, squashed = 0;
    for (size_t i = 0; i < loop.paths.size(); ++i) {
        const ExtrusionPath &p = loop.paths[i];
        // The pieces still form one closed loop.
        CHECK(p.polyline.points.back().to_point() == loop.paths[(i + 1) % loop.paths.size()].polyline.points.front().to_point());
        const Point mid = p.polyline.to_polyline().points[p.polyline.points.size() / 2];
        if (p.z_contoured) {
            ++raised;
            CHECK(mid.x() <= SCALED_EPSILON);
        } else {
            ++squashed;
            CHECK_THAT(p.mm3_per_mm, WithinRel(mm3 * 0.5, 1e-9));
            CHECK(mid.x() >= -SCALED_EPSILON);
        }
    }
    CHECK(raised >= 1);
    CHECK(squashed >= 1);

    // The open wall on the top layer is half a layer tall where it sits on the raised left side.
    const auto &top = *static_cast<const ExtrusionMultiPath *>(stack.walls[3].entities[1]);
    for (const ExtrusionPath &p : top.paths) {
        CHECK_FALSE(p.z_contoured);
        CHECK_THAT(p.mm3_per_mm, WithinRel(mm3 * 0.5, 1e-9));
    }
}

TEST_CASE("Wave-overhang walls, overhang paths and disabled layers are not raised", "[BrickLayers][WaveOverhangs]")
{
    const double h = 0.2;
    Stack        stack(std::vector<double>(5, h), 2);
    // Layer 3 is a wave layer: no candidates, so layer 2 ends and layer 4 starts a brick run.
    stack.inputs[2].enabled = false;
    // On layer 4 the raised wall is tagged as a wave-layer wall.
    for (ExtrusionPath &p : static_cast<ExtrusionLoop *>(stack.walls[3].entities[1])->paths)
        p.wave_overhang_perimeter = true;

    BL::apply(stack.inputs);
    CHECK(stack.loop(1, 1).paths.front().z_contoured == false); // nothing above it to continue
    CHECK_THAT(stack.loop(1, 1).paths.front().mm3_per_mm, WithinRel(mm3, 1e-9));
    CHECK_FALSE(stack.loop(2, 1).paths.front().z_contoured);
    CHECK_FALSE(stack.loop(3, 1).paths.front().z_contoured);
    CHECK_THAT(stack.loop(3, 1).paths.front().mm3_per_mm, WithinRel(mm3, 1e-9));
    CHECK_FALSE(stack.loop(4, 1).paths.front().z_contoured);
}
