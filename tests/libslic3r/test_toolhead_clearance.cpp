#include <catch2/catch_all.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Clearance/ToolheadClearance.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <chrono>
#include <cstdlib>
#include <iostream>

using namespace Slic3r;
using Catch::Approx;
namespace C = Slic3r::Clearance;

namespace {
ExPolygon square_mm(double x0, double y0, double x1, double y1)
{
    Polygon p;
    p.points = {Point::new_scale(x0, y0), Point::new_scale(x1, y0), Point::new_scale(x1, y1), Point::new_scale(x0, y1)};
    return ExPolygon(p);
}
// Nozzle cone to r 0.5 at 0, block at 3 mm out to r 10, probe 1 mm up out to r 30, gantry at 30.
C::ToolheadShape test_shape() { return C::ToolheadShape::from_profile({{0.5, 0.}, {10., 3.}, {30., 1.}, {1000., 30.}}); }
BoundingBoxf plate() { return BoundingBoxf(Vec2d(0, 0), Vec2d(200, 200)); }
} // namespace

TEST_CASE("Toolhead shape from a stepped profile", "[ToolheadClearance]")
{
    C::ToolheadShape s = test_shape();
    REQUIRE_FALSE(s.empty());
    CHECK(s.height_at(0, 0) == Approx(0.));
    CHECK(s.height_at(5, 0) == Approx(3.));
    CHECK(s.height_at(0, -20) == Approx(1.));
    CHECK(s.height_at(25, 0) == Approx(1.));
    CHECK(s.height_at(50, 0) == Approx(30.)); // far plane
    CHECK(s.far_height() == Approx(30.));
    CHECK(s.min_height_within(12.) == Approx(0.));
    auto prof = s.radial_profile(0.5);
    REQUIRE(prof.size() >= 3);
    CHECK(prof.front().height == Approx(0.));
    CHECK(prof.back().height == Approx(30.));
}

TEST_CASE("Toolhead shape from a mesh", "[ToolheadClearance]")
{
    // A 20x20 box from z 2 to 10 centred on the nozzle axis, with the nozzle below it.
    TriangleMesh box(its_make_cube(20, 20, 8));
    box.translate(-10, -10, 2);
    C::ToolheadShape s = C::ToolheadShape::from_mesh(box.its, 0.5);
    CHECK(s.height_at(0, 0) == Approx(2.));
    CHECK(s.height_at(9, -9) == Approx(2.));
    CHECK(s.height_at(15, 0) == C::INF);
}

TEST_CASE("Planar printing never collides", "[ToolheadClearance]")
{
    C::ClearanceChecker chk(test_shape(), plate());
    for (int l = 1; l <= 20; ++l)
        chk.printed().add_layer({square_mm(50, 50, 100, 100)}, 0.2 * l);
    // Next layer at 4.2 over the part, and along the edge.
    for (double x : {40., 50., 75., 100., 120.}) {
        CHECK(chk.gap(Vec3d(x, 75, 4.2)) > 0.);
        CHECK(chk.min_nozzle_z(x, 75) <= 4.2);
    }
    auto r = chk.check_path({Vec3d(30, 75, 4.2), Vec3d(130, 75, 4.2)}, 4.2);
    CHECK_FALSE(r.collides);
}

TEST_CASE("Dipping next to a taller part is limited by the toolhead", "[ToolheadClearance]")
{
    C::ClearanceChecker chk(test_shape(), plate());
    // A 10 mm tall tower at x 100..110.
    chk.printed().add_layer({square_mm(100, 100, 110, 110)}, 10.);
    // 20 mm from the tower only the probe step (1 mm, margin 0.2 -> 0.8) is over it:
    // min Z = 10 - 0.1 - 0.8 = 9.1
    CHECK(chk.min_nozzle_z(80, 105) == Approx(9.1).margin(0.05));
    // 5 mm away the block (3 mm) covers the near half, but the far half is under the
    // 1 mm probe step again: still 9.1
    CHECK(chk.min_nozzle_z(95, 105) == Approx(9.1).margin(0.05));
    // Right over the tower the nozzle tip itself rules: 10 - 0.1
    CHECK(chk.min_nozzle_z(105, 105) == Approx(9.9).margin(0.05));
    // 40 mm away only the gantry far plane: 10 - 0.1 - 29.8 < 0
    CHECK(chk.min_nozzle_z(40, 105) < 0.);
    CHECK(chk.max_dip(80, 105, 10.) == Approx(0.9).margin(0.05));

    // A path at z 5 sweeping toward the tower collides once within 30 mm.
    auto r = chk.check_path({Vec3d(40, 105, 5.), Vec3d(90, 105, 5.)}, 5.);
    CHECK(r.collides);
    CHECK(r.worst_penetration > 2.);
    // The same path at z 11 is clear.
    CHECK_FALSE(chk.check_path({Vec3d(40, 105, 11.), Vec3d(90, 105, 11.)}, 11.).collides);
}

TEST_CASE("Same-layer paths count", "[ToolheadClearance]")
{
    C::ClearanceChecker chk(test_shape(), plate());
    chk.printed().add_path({Vec3d(50, 50, 2.0), Vec3d(60, 50, 2.0)}, 0.45);
    CHECK(chk.printed().top_at(55, 50) == Approx(2.0));
    // A dipped path 0.45 mm beside it at 1.5 is under the nozzle tip region: collision
    CHECK(chk.gap(Vec3d(55, 50.3, 1.5)) < 0.);
    // and at 2.0 (same Z) it is fine
    CHECK(chk.gap(Vec3d(55, 50.3, 2.0)) >= 0.);
}

TEST_CASE("Post-slice G-code check", "[ToolheadClearance]")
{
    auto path = (boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("clr-%%%%%%.gcode")).string();
    {
        boost::nowide::ofstream f(path);
        f << "G90\nM83\n;WIDTH:0.45\n";
        // A 10 mm tall wall, planar
        for (int l = 1; l <= 50; ++l)
            f << "G1 X100 Y100 Z" << 0.2 * l << " F6000\nG1 X120 Y100 E1\n";
        // Planar travel above it: fine
        f << "G1 Z10.4\nG1 X60 Y100\n";
        // Non-planar dip next to it: the probe passes over the wall at Z 5
        f << "G1 X90 Y100 Z5 E0.5\n";
        f << "G1 Z20\n";
    }
    C::GCodeCheckReport rep = C::check_gcode(path, test_shape(), plate());
    boost::filesystem::remove(path);
    CHECK(rep.ran);
    // the dip (line 106) and the lift that starts from it (line 107)
    CHECK(rep.collisions == 2);
    REQUIRE(rep.first.size() == 2);
    CHECK(rep.first[0].line == 106);
    CHECK_FALSE(rep.first[0].travel);
    CHECK(rep.first[0].penetration > 2.);
    CHECK(rep.to_json().find("\"collisions\": 2") != std::string::npos);
}

TEST_CASE("Post-slice check of a planar file is clean", "[ToolheadClearance]")
{
    auto path = (boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("clr-%%%%%%.gcode")).string();
    {
        boost::nowide::ofstream f(path);
        f << "G90\nM82\nG92 E0\n";
        double e = 0;
        for (int l = 1; l <= 30; ++l) {
            f << "G1 Z" << 0.2 * l << "\n";
            for (int k = 0; k < 10; ++k)
                f << "G1 X" << 50 + k * 0.45 << " Y50\nG1 Y80 E" << (e += 1) << "\n";
            f << "G2 X64.05 Y80 I5 J0 E" << (e += 0.5) << "\n";
        }
    }
    C::GCodeCheckReport rep = C::check_gcode(path, test_shape(), plate());
    boost::filesystem::remove(path);
    CHECK(rep.ran);
    CHECK(rep.collisions == 0);
}

// Manual: check a G-code file against a profile, e.g.
//   CLEARANCE_GCODE=part.gcode CLEARANCE_PROFILE="1.5x0, 12x1, 47x1.13, 1000x29.2" \
//   libslic3r_tests "[ToolheadClearanceManual]"
// Optional CLEARANCE_MESH (STL/OBJ in the nozzle frame) and CLEARANCE_MARGIN.
TEST_CASE("Manual G-code clearance check", "[.][ToolheadClearanceManual]")
{
    const char *g = std::getenv("CLEARANCE_GCODE");
    const char *p = std::getenv("CLEARANCE_PROFILE");
    const char *m = std::getenv("CLEARANCE_MESH");
    REQUIRE(g != nullptr);
    std::vector<C::ProfileStep> steps;
    if (p) {
        std::string s(p);
        size_t      at = 0;
        while (at < s.size()) {
            size_t comma = s.find(',', at);
            std::string tok = s.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
            size_t x = tok.find('x');
            if (x != std::string::npos)
                steps.push_back({std::atof(tok.substr(0, x).c_str()), std::atof(tok.substr(x + 1).c_str())});
            if (comma == std::string::npos)
                break;
            at = comma + 1;
        }
    }
    C::ToolheadShape shape = C::ToolheadShape::from_profile(steps);
    if (m) {
        std::string err;
        C::ToolheadShape mesh = C::ToolheadShape::from_mesh_file(m, 0.25, &err);
        INFO(err);
        REQUIRE_FALSE(mesh.empty());
        shape = steps.empty() ? mesh : shape;
        if (!steps.empty()) {
            // lower of both: check both and report the worse
            C::Options o;
            if (const char *mg = std::getenv("CLEARANCE_MARGIN")) o.margin = std::atof(mg);
            auto rm = C::check_gcode(g, mesh, BoundingBoxf(Vec2d(-50, -50), Vec2d(400, 400)), o);
            std::cout << "mesh:    " << rm.to_json() << std::endl;
        }
    }
    C::Options o;
    if (const char *mg = std::getenv("CLEARANCE_MARGIN")) o.margin = std::atof(mg);
    auto t0  = std::chrono::steady_clock::now();
    auto rep = C::check_gcode(g, shape, BoundingBoxf(Vec2d(-50, -50), Vec2d(400, 400)), o);
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::cout << (steps.empty() ? "mesh:    " : "profile: ") << rep.to_json() << "  (" << ms << " ms)" << std::endl;
    CHECK(rep.ran);
}
