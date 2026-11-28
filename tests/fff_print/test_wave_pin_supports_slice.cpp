#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>
#include <functional>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_helpers.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

// A 10 x 20 x 8 mm tower with a 30 mm shelf (1.2 mm thick) sticking out of one side at z 4.
// With `floor`, a 2 mm plate under the whole shelf, so the pins stand on the model.
TriangleMesh shelf_mesh(bool floor = false)
{
    TriangleMesh tower = make_cube(10., 20., 8.);
    TriangleMesh shelf = make_cube(30., 20., 1.2);
    shelf.translate(10., 0., 4.);
    tower.merge(shelf);
    if (floor) {
        TriangleMesh plate = make_cube(40., 20., 2.);
        tower.merge(plate);
    }
    return tower;
}

void slice_shelf(Print &print, bool pins, bool floor = false, double spacing = 4.)
{
    init_and_process_print({ shelf_mesh(floor) }, print, {
        { "layer_height",                      0.2 },
        { "initial_layer_print_height",        0.2 },
        { "wall_loops",                        2 },
        { "enable_support",                    false },
        { "wave_overhangs",                    true },
        { "wave_overhangs_instead_of_bridges", true },
        { "wave_overhang_pin_supports",        pins },
        { "wave_overhang_pin_spacing",         spacing },
    });
}

double wave_length(const ExtrusionEntity &ee)
{
    if (auto *path = dynamic_cast<const ExtrusionPath *>(&ee))
        return path->wave_overhang ? unscaled(path->length()) : 0.;
    double len = 0.;
    if (auto *mp = dynamic_cast<const ExtrusionMultiPath *>(&ee))
        for (const ExtrusionPath &p : mp->paths) len += wave_length(p);
    else if (auto *loop = dynamic_cast<const ExtrusionLoop *>(&ee))
        for (const ExtrusionPath &p : loop->paths) len += wave_length(p);
    else if (auto *coll = dynamic_cast<const ExtrusionEntityCollection *>(&ee))
        for (const ExtrusionEntity *e : coll->entities) len += wave_length(*e);
    return len;
}

double wave_length(const PrintObject &object)
{
    double len = 0.;
    for (const Layer *layer : object.layers())
        for (const LayerRegion *region : layer->regions())
            len += wave_length(region->perimeters);
    return len;
}

// Support layers that print something, by print_z.
std::vector<const SupportLayer *> printing_support_layers(const PrintObject &object)
{
    std::vector<const SupportLayer *> out;
    for (const SupportLayer *sl : object.support_layers())
        if (!sl->support_fills.entities.empty())
            out.push_back(sl);
    return out;
}

} // namespace

TEST_CASE("pin supports hold the shelf's free edge from the bed, fused, separate, under unchanged waves", "[WaveOverhangs][PinSupports]")
{
    Print plain_print;
    slice_shelf(plain_print, false);
    const PrintObject &plain = *plain_print.objects().front();
    CHECK(printing_support_layers(plain).empty());

    Print pin_print;
    slice_shelf(pin_print, true);
    const PrintObject &pinned = *pin_print.objects().front();
    const auto         sls    = printing_support_layers(pinned);
    REQUIRE(!sls.empty());

    // From the bed up to the layer right under the shelf (z 4.0): no gap under the wave layer.
    CHECK(sls.front()->print_z == Catch::Approx(0.2).margin(1e-6));
    CHECK(sls.back()->print_z == Catch::Approx(4.0).margin(1e-6));
    CHECK(sls.size() == 20);

    // Separate pins: as many islands on the bed as under the shelf, none merged.
    const size_t tips = sls.back()->support_islands.size();
    CHECK(tips >= 6);
    CHECK(union_ex(sls.front()->support_islands).size() == tips);
    // Every tip is thin: at most the 0.8 mm tip diameter plus one layer of taper.
    for (const ExPolygon &tip : sls.back()->support_islands) {
        const BoundingBox bb = get_extents(tip);
        CHECK(unscaled(bb.size().x()) < 0.8 + 0.1);
    }
    // ... and the base is wider than the tip.
    CHECK(area(sls.front()->support_islands) > 4. * area(sls.back()->support_islands));
    // The pins stand under the far part of the shelf only (5 mm and more out from the tower).
    const double tower_x0 = unscaled(get_extents(pinned.get_layer(0)->lslices).min.x());
    for (const ExPolygon &ep : sls.back()->support_islands)
        CHECK(unscaled(get_extents(ep).min.x()) > tower_x0 + 10. + 5. - 0.5);

    // The shelf above the pins still prints as waves, exactly as without pins.
    const double plain_waves = wave_length(plain);
    REQUIRE(plain_waves > 100.);
    CHECK(wave_length(pinned) == Catch::Approx(plain_waves).epsilon(0.001));

    // A denser spacing gives more pins, still separate and the pin gap (1 mm) apart all the way
    // down: at 2.5 mm spacing the taper alone (0.4 + 4 mm * tan 16 = 1.55 mm) would merge them.
    Print dense_print;
    slice_shelf(dense_print, true, false, 2.5);
    const auto dense = printing_support_layers(*dense_print.objects().front());
    const size_t dense_tips = dense.back()->support_islands.size();
    CHECK(dense_tips > tips);
    const ExPolygons base = union_ex(dense.front()->support_islands);
    CHECK(base.size() == dense_tips);
    double closest = 1e9;
    for (size_t i = 0; i < base.size(); ++i)
        for (size_t j = i + 1; j < base.size(); ++j)
            if (intersection_ex(offset_ex(base[i], scaled<float>(0.45)), offset_ex(base[j], scaled<float>(0.45))).empty() == false)
                closest = 0.;
    CHECK(closest > 0.);
}

TEST_CASE("pin supports stand on the model below the overhang", "[WaveOverhangs][PinSupports]")
{
    Print print;
    slice_shelf(print, true, true);
    const auto sls = printing_support_layers(*print.objects().front());
    REQUIRE(!sls.empty());
    // The plate is 2 mm thick: the pins start on its top (first pin layer at z 2.2) and still reach z 4.0.
    CHECK(sls.front()->print_z == Catch::Approx(2.2).margin(1e-6));
    CHECK(sls.back()->print_z == Catch::Approx(4.0).margin(1e-6));
}

TEST_CASE("pin supports go into tree support layers when tree supports are on", "[WaveOverhangs][PinSupports]")
{
    Print print;
    init_and_process_print({ shelf_mesh() }, print, {
        { "layer_height",                      0.2 },
        { "initial_layer_print_height",        0.2 },
        { "wall_loops",                        2 },
        { "enable_support",                    true },
        { "support_type",                      "tree(auto)" },
        { "support_style",                     "tree_slim" },
        { "wave_overhangs",                    true },
        { "wave_overhangs_instead_of_bridges", true },
        { "wave_overhang_pin_supports",        true },
    });
    const PrintObject &object = *print.objects().front();
    // Support layers stay sorted by height, with no two at the same height.
    const auto sls = object.support_layers();
    for (size_t i = 1; i < sls.size(); ++i)
        CHECK(sls[i]->print_z > sls[i - 1]->print_z + EPSILON);
    // The pin tips are there, right under the shelf.
    const SupportLayer *under = object.get_support_layer_at_printz(4.0, EPSILON);
    REQUIRE(under != nullptr);
    CHECK(!under->support_fills.entities.empty());
}
