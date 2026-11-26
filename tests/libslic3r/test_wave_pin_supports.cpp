#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cmath>

#include "libslic3r/libslic3r.h"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Support/TreeSupportPins.hpp"

using namespace Slic3r;
namespace PS = Slic3r::PinSupports;

namespace {

ExPolygon rect_mm(double x0, double y0, double x1, double y1)
{
    Polygon poly;
    poly.points = { Point::new_scale(x0, y0), Point::new_scale(x1, y0), Point::new_scale(x1, y1), Point::new_scale(x0, y1) };
    return ExPolygon(poly);
}

// A 36 x 30 mm cantilevered shelf strip hanging off a wall that ends at x = 0, like shelf36.
const ExPolygons overhang{ rect_mm(0., 0., 36., 30.) };
const ExPolygons below{ rect_mm(-12., 0., 0., 30.) };

double x_mm(const Point &p) { return unscaled(p.x()); }
double y_mm(const Point &p) { return unscaled(p.y()); }

} // namespace

TEST_CASE("pins line the free edge and the far corners of a wave strip", "[WaveOverhangs][PinSupports]")
{
    PS::Params p;
    p.spacing      = 4.;
    p.edge_inset   = 2.5;
    p.min_overhang = 25.;
    const Points pins = PS::place_pins(overhang, below, p);

    // Every pin is 2.5 mm in from the strip's edge and at least 25 mm out from the wall.
    REQUIRE(!pins.empty());
    for (const Point &pt : pins) {
        const double d_edge = std::min({ 36. - x_mm(pt), y_mm(pt), 30. - y_mm(pt) });
        CHECK(d_edge == Catch::Approx(2.5).margin(0.05));
        CHECK(x_mm(pt) >= 25. - 0.05);
    }
    // Both corners of the free edge get a pin.
    auto has = [&](double x, double y) {
        return std::any_of(pins.begin(), pins.end(), [&](const Point &q) { return std::abs(x_mm(q) - x) < 0.05 && std::abs(y_mm(q) - y) < 0.05; });
    };
    CHECK(has(33.5, 2.5));
    CHECK(has(33.5, 27.5));
    // The free edge (y 2.5 .. 27.5 at x 33.5, 25 mm long) gets pins at most 4 mm apart: 8 of them.
    std::vector<double> edge_y;
    for (const Point &pt : pins)
        if (std::abs(x_mm(pt) - 33.5) < 0.05)
            edge_y.push_back(y_mm(pt));
    std::sort(edge_y.begin(), edge_y.end());
    CHECK(edge_y.size() == 8);
    for (size_t i = 1; i < edge_y.size(); ++i)
        CHECK(edge_y[i] - edge_y[i - 1] <= 4. + 1e-3);
}

TEST_CASE("pins: a denser spacing gives more pins, and no two pins crowd each other", "[WaveOverhangs][PinSupports]")
{
    PS::Params p;
    p.min_overhang = 20.;
    p.spacing      = 6.25;
    const size_t sparse = PS::place_pins(overhang, below, p).size();
    p.spacing           = 3.;
    const Points dense  = PS::place_pins(overhang, below, p);
    CHECK(dense.size() > sparse);
    for (size_t i = 0; i < dense.size(); ++i)
        for (size_t j = i + 1; j < dense.size(); ++j)
            CHECK((dense[i] - dense[j]).cast<double>().norm() >= scaled(0.5 * p.spacing) - 1.);
}

TEST_CASE("pins: more rows stand further in, and short overhangs get none", "[WaveOverhangs][PinSupports]")
{
    PS::Params p;
    p.min_overhang = 20.;
    p.rows         = 2;
    p.row_spacing  = 3.;
    const Points pins = PS::place_pins(overhang, below, p);
    const bool inner_row = std::any_of(pins.begin(), pins.end(), [](const Point &q) { return std::abs(x_mm(q) - 30.5) < 0.05; });
    CHECK(inner_row);
    p.rows = 1;
    CHECK(PS::place_pins(overhang, below, p).size() < pins.size());

    // A 4 mm overhang never reaches 5 mm out from the wall.
    p.min_overhang = 5.;
    CHECK(PS::place_pins(ExPolygons{ rect_mm(0., 0., 4., 30.) }, below, p).empty());
}

TEST_CASE("pins taper from the tip and stay apart", "[WaveOverhangs][PinSupports]")
{
    PS::Params p;
    p.tip_diameter  = 0.8;
    p.base_diameter = 5.;
    p.taper_angle   = 16.;
    CHECK(PS::pin_radius(p, 0., 2.5) == Catch::Approx(0.4));
    CHECK(PS::pin_radius(p, 2., 2.5) == Catch::Approx(0.4 + 2. * std::tan(16. * M_PI / 180.)));
    CHECK(PS::pin_radius(p, 50., 2.5) == Catch::Approx(2.5));
    p.taper_angle = 0.;
    CHECK(PS::pin_radius(p, 50., 2.5) == Catch::Approx(0.4));

    // Pins 4 mm apart with a 1 mm gap: base radius 1.5 mm, not the 2.5 mm asked for.
    const Points pins{ Point::new_scale(0., 0.), Point::new_scale(4., 0.), Point::new_scale(20., 0.) };
    const auto caps = PS::base_radius_caps(pins, scaled<coord_t>(2.5), scaled<coord_t>(0.4), scaled<coord_t>(1.));
    CHECK(unscaled(caps[0]) == Catch::Approx(1.5).margin(1e-4));
    CHECK(unscaled(caps[1]) == Catch::Approx(1.5).margin(1e-4));
    CHECK(unscaled(caps[2]) == Catch::Approx(2.5).margin(1e-4));
    // Never thinner than the tip.
    const auto tight = PS::base_radius_caps({ Point(0, 0), Point::new_scale(0.5, 0.) }, scaled<coord_t>(2.5), scaled<coord_t>(0.4), scaled<coord_t>(1.));
    CHECK(unscaled(tight[0]) == Catch::Approx(0.4).margin(1e-4));
}

TEST_CASE("pin settings default to the 2026-11-26 test print, denser", "[WaveOverhangs][PinSupports]")
{
    const PrintObjectConfig cfg;
    CHECK_FALSE(cfg.wave_overhang_pin_supports.value);
    const PS::Params p = PS::Params::from_config(cfg);
    CHECK(p.tip_diameter == Catch::Approx(0.8));
    CHECK(p.edge_inset == Catch::Approx(2.5));
    CHECK(p.spacing < 6.25);
    CHECK(p.rows == 1);
}
