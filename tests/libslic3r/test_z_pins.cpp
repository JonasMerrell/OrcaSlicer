#include <catch2/catch_all.hpp>

#include <algorithm>
#include <set>

#include "libslic3r/libslic3r.h"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/ZPins/ZPins.hpp"

using namespace Slic3r;

namespace {

std::vector<std::vector<char>> all_eligible(int layers, size_t pins)
{
    return std::vector<std::vector<char>>(layers, std::vector<char>(pins, 1));
}

// Layers each pin is filled over, in order: every open layer must be filled exactly once.
std::vector<int> filled_layers(const ZPinPlan &plan, uint32_t pin)
{
    std::vector<int> out;
    for (int l = 0; l < int(plan.fills.size()); ++ l)
        for (const ZPinEvent &ev : plan.fills[l])
            if (ev.pin == pin)
                for (int k = ev.first_layer; k <= l; ++ k)
                    out.emplace_back(k);
    return out;
}

std::vector<int> open_layers(const ZPinPlan &plan, uint32_t pin)
{
    std::vector<int> out;
    for (int l = 0; l < int(plan.open.size()); ++ l)
        if (std::find(plan.open[l].begin(), plan.open[l].end(), pin) != plan.open[l].end())
            out.emplace_back(l);
    return out;
}

} // namespace

TEST_CASE("Z-pin columns keep the margins and fill every open layer once", "[ZPins]")
{
    ZPinColumnParams params;
    params.interval      = 5;
    params.top_window    = 3;
    params.bottom_window = 2;
    const ZPinPlan plan  = plan_z_pin_columns(all_eligible(30, 1), {}, params);

    const std::vector<int> open = open_layers(plan, 0);
    REQUIRE(! open.empty());
    // Layers 0-1 below and 27-29 above stay closed.
    CHECK(open.front() == 2);
    CHECK(open.back() == 26);
    CHECK(int(open.size()) == 25);
    // Filled exactly over the open layers, each layer once, in 5-layer steps.
    CHECK(filled_layers(plan, 0) == open);
    for (int l = 0; l < 30; ++ l)
        for (const ZPinEvent &ev : plan.fills[l])
            CHECK(l - ev.first_layer + 1 == 5);
}

TEST_CASE("Z-pin columns stop at holes and drop short pieces", "[ZPins]")
{
    ZPinColumnParams params;
    params.interval      = 4;
    params.top_window    = 1;
    params.bottom_window = 1;
    params.min_layers    = 2;
    auto eligible = all_eligible(20, 1);
    // Cavities on layers 10 and 16 split the column into three pieces.
    eligible[10][0] = 0;
    eligible[16][0] = 0;
    const ZPinPlan plan = plan_z_pin_columns(eligible, {}, params);
    const std::vector<int> open = open_layers(plan, 0);
    // Layers next to a cavity or the object's ends stay closed: 1..8 and 12..14 open; 18 alone is too short.
    CHECK(open == std::vector<int>{ 1, 2, 3, 4, 5, 6, 7, 8, 12, 13, 14 });
    CHECK(filled_layers(plan, 0) == open);
    // No injection fills fewer than two layers.
    for (int l = 0; l < 20; ++ l)
        for (const ZPinEvent &ev : plan.fills[l])
            CHECK(l - ev.first_layer + 1 >= 2);
}

TEST_CASE("Alternating Z-pin sets are filled half an interval apart", "[ZPins]")
{
    ZPinColumnParams params;
    params.interval      = 8;
    params.top_window    = 1;
    params.bottom_window = 1;
    const ZPinPlan plan  = plan_z_pin_columns(all_eligible(40, 2), { 0, 4 }, params);
    std::set<int> ends_a, ends_b;
    for (int l = 0; l < 40; ++ l)
        for (const ZPinEvent &ev : plan.fills[l])
            (ev.pin == 0 ? ends_a : ends_b).insert(l);
    // Joints of the two sets never share a layer, except the common top.
    std::vector<int> common;
    std::set_intersection(ends_a.begin(), ends_a.end(), ends_b.begin(), ends_b.end(), std::back_inserter(common));
    CHECK(common == std::vector<int>{ 38 });
    CHECK(filled_layers(plan, 0) == open_layers(plan, 0));
    CHECK(filled_layers(plan, 1) == open_layers(plan, 1));
    // The B set's first fill is half as deep.
    CHECK(*ends_a.begin() == 8);
    CHECK(*ends_b.begin() == 4);
}

TEST_CASE("Z-pin grid is centred and stays inside the box", "[ZPins]")
{
    const BoundingBox bbox(Point::new_scale(-10, -5), Point::new_scale(10, 5));
    std::vector<int>  set;
    const Points      grid = z_pin_grid(bbox, scaled<coord_t>(5.), false, &set);
    // x: -10..10 step 5 = 5 columns, y: -5..5 = 3 rows.
    CHECK(grid.size() == 15);
    CHECK(set.size() == grid.size());
    CHECK(std::find(grid.begin(), grid.end(), Point(0, 0)) != grid.end());
    for (const Point &p : grid)
        CHECK(bbox.contains(p));
    // Checkerboard: right-hand neighbour is in the other set.
    for (size_t i = 0; i < grid.size(); ++ i)
        for (size_t j = 0; j < grid.size(); ++ j)
            if (grid[j] == grid[i] + Point(scaled<coord_t>(5.), 0))
                CHECK(set[i] != set[j]);

    const Points hex = z_pin_grid(bbox, scaled<coord_t>(5.), true, &set);
    REQUIRE(! hex.empty());
    for (const Point &p : hex)
        CHECK(bbox.contains(p));
    // Neighbouring rows are offset by half the spacing.
    std::set<coord_t> xs_row0, xs_row1;
    const coord_t dy = coord_t(std::round(scaled<double>(5.) * std::sqrt(3.) / 2.));
    for (const Point &p : hex) {
        if (p.y() == 0) xs_row0.insert(p.x());
        if (p.y() == dy) xs_row1.insert(p.x());
    }
    REQUIRE(! xs_row0.empty());
    REQUIRE(! xs_row1.empty());
    CHECK(xs_row0.count(0) == 1);
    CHECK(xs_row1.count(scaled<coord_t>(2.5)) == 1);
}

TEST_CASE("Z-pin channels clip inner walls but never the outer wall", "[ZPins]")
{
    auto square_loop = [](double half, ExtrusionRole role) {
        Polygon sq;
        sq.points = { Point::new_scale(-half, -half), Point::new_scale(half, -half), Point::new_scale(half, half), Point::new_scale(-half, half) };
        ExtrusionPath path(role, 0.05, 0.45f, 0.2f);
        path.polyline = Polyline3(sq.split_at_first_point());
        return new ExtrusionLoop(std::move(path));
    };
    ExtrusionEntityCollection walls;
    walls.entities.emplace_back(square_loop(10., erExternalPerimeter));
    walls.entities.emplace_back(square_loop(9.5, erPerimeter));
    walls.entities.back()->inset_idx = 1;
    // A channel on the inner wall's right side, and one far away.
    ExPolygons cut = { ExPolygon(z_pin_circle(Point::new_scale(9.5, 0.), scaled<coord_t>(1.))),
                       ExPolygon(z_pin_circle(Point::new_scale(0., 0.), scaled<coord_t>(1.))) };
    const double inner_len = walls.entities[1]->length();
    REQUIRE(z_pin_clip_inner_extrusions(walls, cut, scaled<coord_t>(0.4)));
    // The outer loop is untouched and the inner loop became flat pieces about 2 mm shorter, still
    // inner walls (G-code wants loops and paths in a wall collection, never nested collections).
    REQUIRE(walls.entities.size() >= 2);
    CHECK(walls.entities[0]->is_loop());
    CHECK(walls.entities[0]->role() == erExternalPerimeter);
    double len = 0.;
    for (size_t k = 1; k < walls.entities.size(); ++ k) {
        const ExtrusionEntity *e = walls.entities[k];
        CHECK_FALSE(e->is_collection());
        CHECK(e->role() == erPerimeter);
        CHECK(e->inset_idx == 1);
        len += e->length();
    }
    CHECK(len == Catch::Approx(inner_len - scaled<double>(2.)).margin(scaled<double>(0.05)));

    // A cut that touches nothing clips nothing.
    ExtrusionEntityCollection again;
    again.entities.emplace_back(square_loop(9.5, erPerimeter));
    CHECK_FALSE(z_pin_clip_inner_extrusions(again, { ExPolygon(z_pin_circle(Point(0, 0), scaled<coord_t>(1.))) }, 0));
    CHECK(again.entities.front()->is_loop());
}
