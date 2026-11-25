#include <catch2/catch_all.hpp>

#include <algorithm>

#include "libslic3r/libslic3r.h"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/WaveOverhangs/WaveOverhangs.hpp"
#include "libslic3r/WaveOverhangs/WaveStaircase.hpp"

using namespace Slic3r;
namespace WO = Slic3r::WaveOverhangs;
namespace WS = Slic3r::WaveOverhangs::Staircase;

namespace {

Polygon rect_mm(double x0, double y0, double x1, double y1)
{
    Polygon poly;
    poly.points = { Point::new_scale(x0, y0), Point::new_scale(x1, y0), Point::new_scale(x1, y1), Point::new_scale(x0, y1) };
    return poly;
}

// A shelf: island 36 x 20 mm, the first 6 mm (x) stand on the layer below, the rest is a
// 30 mm overhang. Layers are 0.2 mm; the steps' fill is the island inside two 0.45 mm walls.
WS::BandInput shelf(int layers, double stagger)
{
    const Polygon island = rect_mm(0., 0., 36., 20.);
    const Polygon lower  = rect_mm(0., 0., 6., 20.);

    WO::CommonParams wp;
    wp.perimeter_count = 1;
    wp.line_spacing    = 0.35;
    wp.line_width      = 0.4;
    wp.overhang_flow   = Flow(0.42f, 0.2f, 0.4f);
    wp.pattern         = WaveOverhangPattern::Smart;
    WO::GenerateResult waves = WO::generate(ExPolygons{ ExPolygon(island) }, Polygons{ lower }, wp);

    WS::BandInput in;
    for (const ExtrusionPaths &region : waves.paths)
        append(in.waves, region);
    in.footprint = diff(Polygons{ island }, Polygons{ lower });
    in.footprint = union_(in.footprint, intersection(waves.residual, Polygons{ island }));
    in.island    = { island };
    in.anchor    = { lower };
    for (int k = 1; k < layers; ++k) {
        WS::StepLayer s;
        s.dz     = 0.2 * k;
        s.fill   = shrink(Polygons{ island }, float(scale_(0.9)));
        s.slices = { island };
        s.flow   = Flow(0.42f, 0.2f, 0.4f);
        in.steps.push_back(s);
    }
    (void) stagger;
    return in;
}

WS::Params params(int layers, double stagger, WS::StepPattern pattern = WS::StepPattern::Waves)
{
    WS::Params p;
    p.layers         = layers;
    p.stagger        = stagger;
    p.rings_per_step = 3;
    p.ring_spacing   = 0.35;
    p.pattern        = pattern;
    return p;
}

double z_of(const ExtrusionPath &p) { return p.z_contoured ? unscale<double>(p.polyline.points.front().z()) : 0.; }

} // namespace

TEST_CASE("staircase steps nest and keep clear of later, lower material", "[WaveOverhangs][Staircase]")
{
    const double stagger = 1.6;
    WS::BandInput in = shelf(3, stagger);
    REQUIRE_FALSE(in.waves.empty());
    const std::vector<Polygons> regions = WS::step_regions(in, params(3, stagger));
    REQUIRE(regions.size() == 2);
    for (const Polygons &r : regions)
        REQUIRE(area(r) > scale_(1.) * scale_(1.) * 50.);
    // Each step stands on the one below.
    CHECK(area(diff(regions[1], regions[0])) < scale_(0.01) * scale_(0.01));
    // Step 2 keeps the stagger (plus line widths) away from what layer L+1 prints later.
    const Polygons later = diff(in.steps[0].slices, regions[0]);
    const Polygons too_close = intersection(regions[1], expand(later, float(scale_(stagger + 0.42 - 0.01))));
    CHECK(area(too_close) < scale_(0.05) * scale_(0.05));
    // The steps reach back over the supported side, so the cantilever root is full height.
    CHECK(!intersection(regions[1], in.anchor).empty());
}

TEST_CASE("staircase band interleaves steps and never leaves the nozzle under a higher step", "[WaveOverhangs][Staircase]")
{
    const double stagger = 1.6;
    for (WS::StepPattern pattern : { WS::StepPattern::Waves, WS::StepPattern::Rectilinear, WS::StepPattern::Hilbert, WS::StepPattern::Concentric }) {
        CAPTURE(int(pattern));
        WS::BandInput  in   = shelf(3, stagger);
        WS::BandResult band = WS::build_band(in, params(3, stagger, pattern));
        REQUIRE_FALSE(band.paths.empty());
        CHECK(band.rounds > 5);

        // Every step is present at its own height.
        size_t count[3] = { 0, 0, 0 };
        size_t first_step2 = band.paths.size(), last_wave = 0;
        for (size_t i = 0; i < band.paths.size(); ++i) {
            const ExtrusionPath &p = band.paths[i];
            REQUIRE(p.staircase_band);
            REQUIRE(p.staircase_step >= 0);
            REQUIRE(p.staircase_step <= 2);
            ++count[p.staircase_step];
            CHECK(z_of(p) == Catch::Approx(0.2 * p.staircase_step).margin(1e-6));
            if (p.staircase_step == 0) {
                CHECK(p.wave_overhang);
                last_wave = i;
            }
            if (p.staircase_step == 2)
                first_step2 = std::min(first_step2, i);
        }
        CHECK(count[0] > 0);
        CHECK(count[1] > 0);
        CHECK(count[2] > 0);
        // A staircase, not three layers one after the other: the top step starts long before
        // the wave front is done.
        CHECK(first_step2 < last_wave);

        // Nothing is ever printed next to (within the stagger of) material that is higher.
        const WS::ClearanceReport rep = WS::check_sequence(band.paths, stagger);
        CHECK(rep.violations == 0);
        CHECK(rep.min_clearance >= stagger - 0.1);
    }
}

TEST_CASE("staircase band keeps every wave path", "[WaveOverhangs][Staircase]")
{
    WS::BandInput  in   = shelf(3, 1.6);
    WS::BandResult band = WS::build_band(in, params(3, 1.6));
    double wave_len = 0., band_wave_len = 0.;
    for (const ExtrusionPath &p : in.waves)
        wave_len += p.length();
    for (const ExtrusionPath &p : band.paths)
        if (p.staircase_step == 0)
            band_wave_len += p.length();
    CHECK(band_wave_len == Catch::Approx(wave_len).epsilon(0.01));
    // Wave fronts stay whole instead of being scattered over rounds (each piece costs a travel).
    size_t band_waves = 0;
    for (const ExtrusionPath &p : band.paths)
        band_waves += p.staircase_step == 0;
    CHECK(band_waves <= in.waves.size() + in.waves.size() / 10 + 2);
}

TEST_CASE("staircase is a no-op without upper layers", "[WaveOverhangs][Staircase]")
{
    WS::BandInput in = shelf(3, 1.6);
    in.steps.clear();
    CHECK(WS::build_band(in, params(3, 1.6)).paths.empty());
    WS::BandInput in2 = shelf(3, 1.6);
    CHECK(WS::build_band(in2, params(1, 1.6)).paths.empty());
}

TEST_CASE("staircase clearance check flags a lower line next to a higher one", "[WaveOverhangs][Staircase]")
{
    ExtrusionPath high(erSolidInfill, 0.08, 0.42f, 0.2f);
    high.polyline    = Polyline3(Polyline{ Point::new_scale(0., 0.), Point::new_scale(10., 0.) }, coord_t(scale_(0.4)));
    high.z_contoured = true;
    ExtrusionPath low(erOverhangPerimeter, 0.08, 0.4f, 0.2f);
    low.polyline = Polyline3(Polyline{ Point::new_scale(0., 1.0), Point::new_scale(10., 1.0) });
    ExtrusionPath far_low(erOverhangPerimeter, 0.08, 0.4f, 0.2f);
    far_low.polyline = Polyline3(Polyline{ Point::new_scale(0., 3.0), Point::new_scale(10., 3.0) });

    CHECK(WS::check_sequence({ low, high }, 1.6).violations == 0);  // lower first: fine
    CHECK(WS::check_sequence({ high, low }, 1.6).violations > 0);   // tip 1 mm from a higher line
    const WS::ClearanceReport ok = WS::check_sequence({ high, far_low }, 1.6);
    CHECK(ok.violations == 0);
    CHECK(ok.min_clearance == Catch::Approx(3.0 - 0.21).margin(0.02));
}
