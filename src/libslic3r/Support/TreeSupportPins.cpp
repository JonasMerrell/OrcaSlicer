///|/ Resin-style pin supports under the free edge of wave overhangs.
///|/
///|/ Released under the terms of the AGPLv3 or higher.
///|/
#include "TreeSupportPins.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include <boost/log/trivial.hpp>
#include <tbb/parallel_for.h>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/WaveOverhangs/WaveOverhangs.hpp"

namespace Slic3r::PinSupports {

Params Params::from_config(const PrintObjectConfig &config)
{
    Params p;
    p.tip_diameter  = config.wave_overhang_pin_tip_diameter.value;
    p.base_diameter = std::max(config.wave_overhang_pin_base_diameter.value, p.tip_diameter);
    p.taper_angle   = config.wave_overhang_pin_taper_angle.value;
    p.spacing       = config.wave_overhang_pin_spacing.value;
    p.edge_inset    = config.wave_overhang_pin_edge_inset.value;
    p.rows          = std::max(1, config.wave_overhang_pin_rows.value);
    p.row_spacing   = config.wave_overhang_pin_row_spacing.value;
    p.min_overhang  = config.wave_overhang_pin_min_overhang.value;
    p.gap           = config.wave_overhang_pin_gap.value;
    return p;
}

namespace {

// A bend sharper than this between two edges of a row makes a corner, which always gets a pin.
constexpr double CORNER_TURN = 35. * M_PI / 180.;

double turn_angle(const Point &a, const Point &b, const Point &c)
{
    const Vec2d u = (b - a).cast<double>();
    const Vec2d v = (c - b).cast<double>();
    const double nu = u.norm(), nv = v.norm();
    if (nu < EPSILON || nv < EPSILON)
        return 0.;
    return std::acos(std::clamp(u.dot(v) / (nu * nv), -1., 1.));
}

// A closed row as an open polyline starting and ending at a corner if it has one, so the cut
// of the loop lands where a pin goes anyway.
Polyline open_loop(const Polygon &poly)
{
    const Points &pts = poly.points;
    const size_t  n   = pts.size();
    size_t        start = 0;
    for (size_t i = 0; i < n; ++i)
        if (turn_angle(pts[(i + n - 1) % n], pts[i], pts[(i + 1) % n]) > CORNER_TURN) {
            start = i;
            break;
        }
    Polyline pl;
    pl.points.reserve(n + 1);
    for (size_t i = 0; i <= n; ++i)
        pl.points.emplace_back(pts[(start + i) % n]);
    return pl;
}

// Split a polyline at its sharp corners.
Polylines split_at_corners(const Polyline &pl)
{
    Polylines out;
    Polyline  cur;
    for (size_t i = 0; i < pl.points.size(); ++i) {
        cur.points.emplace_back(pl.points[i]);
        if (i > 0 && i + 1 < pl.points.size() && turn_angle(pl.points[i - 1], pl.points[i], pl.points[i + 1]) > CORNER_TURN) {
            out.emplace_back(std::move(cur));
            cur = Polyline();
            cur.points.emplace_back(pl.points[i]);
        }
    }
    if (cur.points.size() > 1)
        out.emplace_back(std::move(cur));
    return out;
}

Point point_at(const Polyline &pl, double dist)
{
    for (size_t i = 1; i < pl.points.size(); ++i) {
        const Vec2d  a   = pl.points[i - 1].cast<double>();
        const Vec2d  b   = pl.points[i].cast<double>();
        const double len = (b - a).norm();
        if (dist <= len || i + 1 == pl.points.size()) {
            const double t = len > EPSILON ? std::clamp(dist / len, 0., 1.) : 0.;
            return (a + (b - a) * t).cast<coord_t>();
        }
        dist -= len;
    }
    return pl.points.back();
}

// Pins at both ends of a row piece and at most `spacing` apart in between. A piece shorter than
// half a spacing gets a single pin in its middle.
void sample(const Polyline &pl, double spacing, Points &out)
{
    const double len = pl.length();
    if (len < 0.5 * spacing) {
        out.emplace_back(point_at(pl, 0.5 * len));
        return;
    }
    const int n = std::max(1, int(std::ceil(len / spacing - 1e-6)));
    for (int i = 0; i <= n; ++i)
        out.emplace_back(point_at(pl, len * double(i) / double(n)));
}

bool inside(const ExPolygons &polys, const BoundingBox &bbox, const Point &pt)
{
    if (!bbox.contains(pt))
        return false;
    for (const ExPolygon &ep : polys)
        if (ep.contains(pt))
            return true;
    return false;
}

Polygon circle(const Point &c, double r)
{
    // Segments no longer than ~0.25 mm, at least 12.
    const int n = std::clamp(int(std::ceil(2. * M_PI * unscaled(r) / 0.25)), 12, 96);
    Polygon   poly;
    poly.points.reserve(n);
    for (int i = 0; i < n; ++i) {
        const double a = 2. * M_PI * double(i) / double(n);
        poly.points.emplace_back(c.x() + coord_t(std::lround(r * std::cos(a))), c.y() + coord_t(std::lround(r * std::sin(a))));
    }
    return poly;
}

} // namespace

Points place_pins(const ExPolygons &overhang, const ExPolygons &below, const Params &params)
{
    Points pins;
    if (overhang.empty() || params.spacing <= 0.)
        return pins;
    const ExPolygons far     = params.min_overhang > 0. && !below.empty() ?
                                   diff_ex(overhang, offset_ex(below, float(scaled(params.min_overhang)))) :
                                   overhang;
    if (far.empty())
        return pins;
    const double spacing     = scaled(params.spacing);
    const double row_spacing = scaled(params.row_spacing > 0. ? params.row_spacing : params.spacing);
    // Slightly grown, so a row that runs right along the boundary of the far zone stays in.
    const ExPolygons far_clip = offset_ex(far, float(scaled(0.01)));

    for (int row = 0; row < params.rows; ++row) {
        const double     inset = scaled(params.edge_inset) + row * row_spacing;
        const ExPolygons ring  = inset > 0. ? offset_ex(overhang, -float(inset)) : overhang;
        if (ring.empty())
            break;
        Polylines loops;
        for (const ExPolygon &ep : ring) {
            loops.emplace_back(open_loop(ep.contour));
            for (const Polygon &hole : ep.holes)
                loops.emplace_back(open_loop(hole));
        }
        Points row_pins;
        for (const Polyline &piece : intersection_pl(loops, far_clip))
            for (const Polyline &edge : split_at_corners(piece))
                sample(edge, spacing, row_pins);
        // Drop pins that crowd an earlier one (shared corners, the cut of a loop, the row inside).
        const double min_dist2 = sqr(0.5 * spacing);
        for (const Point &p : row_pins)
            if (std::none_of(pins.begin(), pins.end(), [&](const Point &q) { return (p - q).cast<double>().squaredNorm() < min_dist2; }))
                pins.emplace_back(p);
    }
    return pins;
}

std::vector<coord_t> base_radius_caps(const Points &pins, coord_t base_radius, coord_t tip_radius, coord_t gap)
{
    std::vector<coord_t> caps(pins.size(), base_radius);
    for (size_t i = 0; i < pins.size(); ++i) {
        double nearest = std::numeric_limits<double>::max();
        for (size_t j = 0; j < pins.size(); ++j)
            if (i != j)
                nearest = std::min(nearest, (pins[i] - pins[j]).cast<double>().norm());
        if (nearest < std::numeric_limits<double>::max())
            caps[i] = std::min<coord_t>(caps[i], coord_t(0.5 * (nearest - double(gap))));
        caps[i] = std::max(caps[i], tip_radius);
    }
    return caps;
}

double pin_radius(const Params &params, double depth, double base_radius_cap)
{
    const double r = 0.5 * params.tip_diameter + std::max(0., depth) * std::tan(params.taper_angle * M_PI / 180.);
    return std::min(r, std::max(base_radius_cap, 0.5 * params.tip_diameter));
}

std::vector<ExPolygons> plan_object(const PrintObject &object, std::vector<Pin> *pins_out)
{
    const size_t            n_layers = object.layer_count();
    std::vector<ExPolygons> areas(n_layers);
    if (!object.config().wave_overhang_pin_supports.value || n_layers < 2)
        return areas;
    const Params p = Params::from_config(object.config());

    // 1) Pin centres under every wave overhang: the tip is on the layer under the wave layer.
    struct Planned { Point pos; size_t top; };
    std::vector<Planned> planned;
    for (size_t l = 1; l < n_layers; ++l) {
        const Layer *layer = object.get_layer(int(l));
        if (layer->wave_overhang_covered_polygons.empty())
            continue;
        const Layer   *lower = object.get_layer(int(l - 1));
        const Polygons lower_polys = to_polygons(lower->lslices);
        // The strip as it hangs over air, edge to edge (the wave itself stops inside the outer walls).
        const ExPolygons overhang = union_ex(WaveOverhangs::detail::painted_overhang_zone(
            to_polygons(layer->lslices), lower_polys, layer->wave_overhang_covered_polygons, scaled<coord_t>(0.5)));
        for (const Point &pt : place_pins(overhang, lower->lslices, p))
            planned.push_back({ pt, l - 1 });
    }
    if (planned.empty())
        return areas;

    Points centres;
    centres.reserve(planned.size());
    for (const Planned &pl : planned)
        centres.emplace_back(pl.pos);
    const std::vector<coord_t> caps = base_radius_caps(centres, scaled<coord_t>(0.5 * p.base_diameter),
                                                       scaled<coord_t>(0.5 * p.tip_diameter), scaled<coord_t>(p.gap));

    std::vector<BoundingBox> bboxes(n_layers);
    std::vector<ExPolygons>  keep_out(n_layers); // the model grown by the pin gap
    tbb::parallel_for(size_t(0), n_layers, [&](size_t k) {
        const Layer *layer = object.get_layer(int(k));
        bboxes[k]   = get_extents(layer->lslices);
        keep_out[k] = offset_ex(layer->lslices, float(scaled(p.gap)));
    });

    // 2) Drop each pin straight down until it reaches the bed or the model. Pins never move
    //    sideways and never merge: that is what makes them pins rather than branches.
    std::vector<std::vector<ExPolygon>> per_layer(n_layers);
    std::vector<Pin>                    pins;
    for (size_t i = 0; i < planned.size(); ++i) {
        const Planned &pl    = planned[i];
        const double   top_z = object.get_layer(int(pl.top))->print_z;
        size_t         bottom = pl.top + 1;
        std::vector<std::pair<size_t, ExPolygon>> column;
        for (int k = int(pl.top); k >= 0; --k) {
            const Layer *layer = object.get_layer(k);
            // Model right under the pin: it stands on it, fused, like on the bed.
            if (inside(layer->lslices, bboxes[k], pl.pos))
                break;
            const double r = scaled(pin_radius(p, top_z - layer->print_z, unscaled(caps[i])));
            ExPolygons   pieces = diff_ex(ExPolygons{ ExPolygon(circle(pl.pos, r)) }, keep_out[k]);
            const ExPolygon *mine = nullptr;
            for (const ExPolygon &ep : pieces)
                if (ep.contains(pl.pos))
                    mine = &ep;
            ExPolygons squeezed;
            if (mine == nullptr) {
                // Closer to the model than the gap: keep going as a tip-thin pin that only keeps clear
                // of the model itself, so the pin still reaches the bed.
                squeezed = diff_ex(ExPolygons{ ExPolygon(circle(pl.pos, scaled(0.5 * p.tip_diameter))) }, layer->lslices);
                for (const ExPolygon &ep : squeezed)
                    if (mine == nullptr || ep.area() > mine->area())
                        mine = &ep;
            }
            if (mine == nullptr)
                break;
            column.emplace_back(size_t(k), *mine);
            bottom = size_t(k);
        }
        if (column.empty())
            continue;
        for (auto &[k, ep] : column)
            per_layer[k].emplace_back(std::move(ep));
        pins.push_back({ pl.pos, pl.top, bottom, caps[i] });
    }
    for (size_t k = 0; k < n_layers; ++k)
        if (!per_layer[k].empty())
            areas[k] = union_ex(per_layer[k]);
    BOOST_LOG_TRIVIAL(info) << "Wave overhang pin supports: " << pins.size() << " pins for object " << object.model_object()->name;
    if (pins_out)
        *pins_out = std::move(pins);
    return areas;
}

namespace {

// Concentric loops filling a pin's cross-section solid, outside in. A tip thinner than one line
// still gets one small loop.
void pin_loops(ExtrusionEntitiesPtr &dst, const ExPolygon &area, const Flow &flow)
{
    const float spacing = float(flow.scaled_spacing());
    Polygons    loops;
    ExPolygons  ring = offset_ex(ExPolygons{ area }, -0.5f * spacing);
    if (ring.empty()) {
        const double r = std::sqrt(std::max(0., area.area()) / M_PI);
        ring           = offset_ex(ExPolygons{ area }, -float(0.5 * r));
    }
    while (!ring.empty()) {
        polygons_append(loops, to_polygons(ring));
        ring = offset_ex(ring, -spacing);
    }
    if (loops.empty())
        return;
    auto *coll    = new ExtrusionEntityCollection();
    coll->no_sort = true;
    extrusion_entities_append_loops(coll->entities, std::move(loops), erSupportMaterial, float(flow.mm3_per_mm()), float(flow.width()),
                                    float(flow.height()));
    for (ExtrusionEntity *ee : coll->entities)
        if (auto *loop = dynamic_cast<ExtrusionLoop *>(ee))
            for (ExtrusionPath &path : loop->paths)
                path.pin_support = true;
    dst.emplace_back(coll);
}

} // namespace

void generate(PrintObject &object)
{
    if (!object.config().wave_overhang_pin_supports.value)
        return;
    const std::vector<ExPolygons> areas = plan_object(object);

    SupportLayerPtrs &sls = object.support_layers();
    bool              added = false;
    for (size_t k = 0; k < areas.size(); ++k) {
        if (areas[k].empty())
            continue;
        const Layer  *layer = object.get_layer(int(k));
        SupportLayer *sl    = object.get_support_layer_at_printz(layer->print_z, EPSILON);
        if (sl == nullptr) {
            auto it = std::find_if(sls.begin(), sls.end(), [z = layer->print_z](const SupportLayer *s) { return s->print_z > z; });
            it      = object.insert_support_layer(it, 0, 0, layer->height, layer->print_z, layer->slice_z);
            sl      = *it;
            added   = true;
        }
        const Flow flow = k == 0 && object.slicing_parameters().raft_layers() == 0 ?
                              support_material_1st_layer_flow(&object, float(layer->height)) :
                              support_material_flow(&object, float(layer->height));
        for (const ExPolygon &ep : areas[k])
            pin_loops(sl->support_fills.entities, ep, flow);
        append(sl->support_islands, areas[k]);
    }
    if (added) {
        for (size_t i = 0; i < sls.size(); ++i) {
            sls[i]->set_id(i);
            sls[i]->lower_layer = i > 0 ? sls[i - 1] : nullptr;
            sls[i]->upper_layer = i + 1 < sls.size() ? sls[i + 1] : nullptr;
        }
    }
}

} // namespace Slic3r::PinSupports
