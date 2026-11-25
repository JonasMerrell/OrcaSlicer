#include <catch2/catch_all.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <vector>

#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/NonplanarInfill.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/libslic3r.h"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

ExPolygons square(double x0, double y0, double x1, double y1)
{
    return { ExPolygon(Points{ Point::new_scale(x0, y0), Point::new_scale(x1, y0), Point::new_scale(x1, y1), Point::new_scale(x0, y1) }) };
}

NonplanarInfillWave sine_wave(double amplitude = 0.1, double wavelength = 10.)
{
    NonplanarInfillWave wave;
    wave.amplitude  = amplitude;
    wave.wavelength = wavelength;
    return wave;
}

// A stack of 2 * blend + 1 layers, each with the given sparse area, nullptr where a layer has none.
std::vector<const ExPolygons *> stack_of(const std::vector<const ExPolygons *> &layers) { return layers; }

} // namespace

TEST_CASE("Non-planar infill wave", "[NonplanarInfill]")
{
    NonplanarInfillWave wave = sine_wave();
    SECTION("a sine along X repeats every wavelength and does not change along Y") {
        CHECK_THAT(wave.unit(0., 0., 0), WithinAbs(0., 1e-12));
        CHECK_THAT(wave.unit(2.5, 0., 0), WithinAbs(1., 1e-12));
        CHECK_THAT(wave.unit(7.5, 3., 0), WithinAbs(-1., 1e-12));
        CHECK_THAT(wave.unit(12.5, 17., 5), WithinAbs(1., 1e-12));
    }
    SECTION("the phase moves by the shift on every layer") {
        wave.phase_per_layer = M_PI;
        CHECK_THAT(wave.unit(2.5, 0., 1), WithinAbs(-1., 1e-12));
        CHECK_THAT(wave.unit(2.5, 0., 2), WithinAbs(1., 1e-12));
    }
    SECTION("the angle turns the wave") {
        wave.angle = M_PI / 2.;
        CHECK_THAT(wave.unit(2.5, 0., 0), WithinAbs(0., 1e-12));
        CHECK_THAT(wave.unit(0., 2.5, 0), WithinAbs(1., 1e-12));
    }
    SECTION("the egg crate waves along both axes and stays within [-1, 1]") {
        wave.egg_crate = true;
        CHECK_THAT(wave.unit(2.5, 2.5, 0), WithinAbs(1., 1e-12));
        CHECK_THAT(wave.unit(2.5, 0., 0), WithinAbs(0.5, 1e-12));
        CHECK_THAT(wave.unit(0., 2.5, 0), WithinAbs(0.5, 1e-12));
        for (double x = 0.; x < 20.; x += 0.37)
            for (double y = 0.; y < 20.; y += 0.41)
                CHECK(std::abs(wave.unit(x, y, 3)) <= 1. + 1e-12);
    }
}

TEST_CASE("Non-planar infill amplitude never exceeds one layer or the maximum Z offset", "[NonplanarInfill]")
{
    CHECK_THAT(nonplanar_infill_amplitude(0.1, 0.2, 0.3), WithinAbs(0.1, 1e-12));
    CHECK_THAT(nonplanar_infill_amplitude(0.5, 0.2, 0.3), WithinAbs(0.2, 1e-12));
    CHECK_THAT(nonplanar_infill_amplitude(0.5, 0.4, 0.3), WithinAbs(0.3, 1e-12));
    CHECK_THAT(nonplanar_infill_amplitude(-1., 0.2, 0.3), WithinAbs(0., 1e-12));

    DynamicPrintConfig dyn = DynamicPrintConfig::full_print_config();
    PrintRegionConfig  config;
    config.apply(dyn, true);
    // The default amplitude is half the layer height.
    CHECK_THAT(NonplanarInfillWave::from_config(config, 0.2).amplitude, WithinAbs(0.1, 1e-9));
    // and the default maximum Z offset caps thick layers
    CHECK_THAT(NonplanarInfillWave::from_config(config, 0.8).amplitude, WithinAbs(0.3, 1e-9));
    CHECK_FALSE(config.nonplanar_infill.value);
}

TEST_CASE("Non-planar infill flow follows the gap to the layer below", "[NonplanarInfill]")
{
    // Same wave below: the layer keeps its thickness.
    CHECK_THAT(nonplanar_infill_extrusion_ratio(0.2, 0.08, 0.08), WithinAbs(1., 1e-12));
    // Up from a flat layer: thicker.
    CHECK_THAT(nonplanar_infill_extrusion_ratio(0.2, 0.1, 0.), WithinAbs(1.5, 1e-12));
    // Opposite phase below: twice the offset.
    CHECK_THAT(nonplanar_infill_extrusion_ratio(0.2, 0.05, -0.05), WithinAbs(1.5, 1e-12));
    CHECK_THAT(nonplanar_infill_extrusion_ratio(0.2, -0.05, 0.05), WithinAbs(0.5, 1e-12));
    // Limited.
    CHECK_THAT(nonplanar_infill_extrusion_ratio(0.2, -0.2, 0.2), WithinAbs(0.25, 1e-12));
    CHECK_THAT(nonplanar_infill_extrusion_ratio(0.2, 0.4, 0.), WithinAbs(2., 1e-12));
}

TEST_CASE("Non-planar infill field tapers next to anything that is not sparse infill", "[NonplanarInfill]")
{
    const ExPolygons sq = square(0., 0., 40., 40.);
    const NonplanarInfillWave wave = sine_wave(0.1, 10.);
    // x = 12.5 is a crest of the wave.
    const Point centre = Point::new_scale(12.5, 20.);

    SECTION("deep inside a tall block of sparse infill the wave is full") {
        NonplanarInfillField field(wave, 4, stack_of({ &sq, &sq, &sq, &sq, &sq }), 2, 2.);
        REQUIRE_FALSE(field.empty());
        CHECK_THAT(field.taper(centre), WithinAbs(1., 1e-9));
        CHECK_THAT(field.dz(centre), WithinAbs(0.1, 1e-9));
        // It fades to flat at the edge of the area, over the edge blend distance.
        CHECK_THAT(field.taper(Point::new_scale(0.0, 20.)), WithinAbs(0., 1e-9));
        CHECK_THAT(field.taper(Point::new_scale(-1., 20.)), WithinAbs(0., 1e-9));
        CHECK_THAT(field.taper(Point::new_scale(1.0, 20.)), WithinAbs(0.5, 1e-6));
        CHECK_THAT(field.taper(Point::new_scale(2.0, 20.)), WithinAbs(1., 1e-6));
        // And it never leaves the amplitude.
        for (double x = -2.; x < 42.; x += 0.3)
            CHECK(std::abs(field.dz(Point::new_scale(x, 20.))) <= 0.1 + 1e-9);
    }
    SECTION("one layer away from a solid layer the wave is partial") {
        ExPolygons none;
        NonplanarInfillField field(wave, 4, stack_of({ &sq, &sq, &sq, &sq, &none }), 2, 2.);
        CHECK_THAT(field.taper(centre), WithinAbs(0.5, 1e-9));
    }
    SECTION("right next to a solid layer, above or below, the infill is flat") {
        ExPolygons none;
        NonplanarInfillField above(wave, 4, stack_of({ &sq, &sq, &sq, &none, &sq }), 2, 2.);
        NonplanarInfillField below(wave, 4, stack_of({ nullptr, nullptr, &sq, &sq, &sq }), 2, 2.);
        CHECK(above.empty());
        CHECK(below.empty());
        CHECK_THAT(above.dz(centre), WithinAbs(0., 1e-12));
        CHECK_THAT(below.dz(centre), WithinAbs(0., 1e-12));
    }
    SECTION("where a solid area starts on the next layer the wave fades out laterally") {
        // The layer above is sparse only on the left half.
        const ExPolygons left = square(0., 0., 20., 40.);
        NonplanarInfillField field(wave, 4, stack_of({ &sq, &sq, &sq, &left, &sq }), 2, 2.);
        CHECK_THAT(field.taper(Point::new_scale(10., 20.)), WithinAbs(1., 1e-9));
        CHECK_THAT(field.taper(Point::new_scale(20., 20.)), WithinAbs(0., 1e-9));
        CHECK_THAT(field.taper(Point::new_scale(30., 20.)), WithinAbs(0., 1e-9));
        // Continuous: no jump between 19 and 21 mm.
        double last = field.dz(Point::new_scale(17., 20.));
        for (double x = 17.; x < 23.; x += 0.05) {
            const double dz = field.dz(Point::new_scale(x, 20.));
            CHECK(std::abs(dz - last) < 0.01);
            last = dz;
        }
    }
}

TEST_CASE("Non-planar infill bends sparse infill paths only", "[NonplanarInfill]")
{
    const ExPolygons sq = square(0., 0., 40., 40.);
    NonplanarInfillField field(sine_wave(0.1, 10.), 4, stack_of({ &sq, &sq, &sq, &sq, &sq }), 2, 2.);

    auto line = [](ExtrusionRole role) {
        ExtrusionPath path(role, 0.05, 0.45, 0.2);
        path.polyline = Polyline3(Polyline(Points{ Point::new_scale(5., 20.), Point::new_scale(35., 20.) }));
        return path;
    };
    ExtrusionPath sparse = line(erInternalInfill);
    const double  seg    = nonplanar_infill_segment_length(10.);
    CHECK_THAT(seg, WithinAbs(0.625, 1e-12));
    REQUIRE(nonplanar_infill_contour_path(sparse, field, seg));
    CHECK(sparse.z_contoured);
    CHECK(sparse.polyline.points.size() == 49);
    CHECK(sparse.polyline.points.front().to_point() == Point::new_scale(5., 20.));
    CHECK(sparse.polyline.points.back().to_point() == Point::new_scale(35., 20.));
    double zmin = 1e9, zmax = -1e9;
    for (const Point3 &p : sparse.polyline.points) {
        zmin = std::min(zmin, unscale<double>(p.z()));
        zmax = std::max(zmax, unscale<double>(p.z()));
        CHECK_THAT(unscale<double>(p.z()), WithinAbs(field.dz(p.to_point()), 1e-5));
    }
    CHECK_THAT(zmax, WithinAbs(0.1, 1e-3));
    CHECK_THAT(zmin, WithinAbs(-0.1, 1e-3));

    ExtrusionEntityCollection fills;
    fills.append(line(erSolidInfill));
    fills.append(line(erTopSolidInfill));
    fills.append(line(erInternalInfill));
    CHECK(nonplanar_infill_contour_collection(fills, field, seg) == 1);
    CHECK_FALSE(static_cast<const ExtrusionPath *>(fills.entities[0])->z_contoured);
    CHECK_FALSE(static_cast<const ExtrusionPath *>(fills.entities[1])->z_contoured);
    CHECK(static_cast<const ExtrusionPath *>(fills.entities[2])->z_contoured);

    // A path outside the field stays as it is.
    ExtrusionPath outside(erInternalInfill, 0.05, 0.45, 0.2);
    outside.polyline = Polyline3(Polyline(Points{ Point::new_scale(50., 20.), Point::new_scale(60., 20.) }));
    CHECK_FALSE(nonplanar_infill_contour_path(outside, field, seg));
    CHECK(outside.polyline.points.size() == 2);
}
