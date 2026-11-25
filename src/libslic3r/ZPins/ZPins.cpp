#include "ZPins.hpp"

#include "../ClipperUtils.hpp"
#include "../ExPolygon.hpp"
#include "../ExtrusionEntity.hpp"
#include "../ExtrusionEntityCollection.hpp"
#include "../Flow.hpp"
#include "../Layer.hpp"
#include "../Print.hpp"
#include "../Surface.hpp"

#include <boost/log/trivial.hpp>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>

namespace Slic3r {

ZPinPlan plan_z_pin_columns(const std::vector<std::vector<char>> &eligible, const std::vector<int> &phase, const ZPinColumnParams &params)
{
    ZPinPlan plan;
    const int n_layers = int(eligible.size());
    plan.open.assign(n_layers, {});
    plan.fills.assign(n_layers, {});
    if (n_layers == 0)
        return plan;
    const size_t n_pins     = eligible.front().size();
    const int    interval   = std::max(1, params.interval);
    const int    min_layers = std::max(1, params.min_layers);
    const int    up         = std::max(0, params.top_window);
    const int    down       = std::max(0, params.bottom_window);

    std::vector<int> prefix(n_layers + 1);
    std::vector<char> open(n_layers);
    for (size_t pin = 0; pin < n_pins; ++ pin) {
        // prefix[i]: number of eligible layers below layer i.
        prefix[0] = 0;
        for (int l = 0; l < n_layers; ++ l)
            prefix[l + 1] = prefix[l] + (eligible[l][pin] ? 1 : 0);
        // Open on l if every layer of [l - down, l + up] is eligible (and inside the object).
        for (int l = 0; l < n_layers; ++ l) {
            const int lo = l - down, hi = l + up;
            open[l] = lo >= 0 && hi < n_layers && prefix[hi + 1] - prefix[lo] == hi - lo + 1;
        }
        const int ph = phase.empty() ? 0 : phase[pin] % interval;
        for (int a = 0; a < n_layers;) {
            if (! open[a]) {
                ++ a;
                continue;
            }
            int b = a;
            while (b + 1 < n_layers && open[b + 1])
                ++ b;
            if (b - a + 1 >= min_layers) {
                // Fill after layer l when (l - a + 1 + ph) is a multiple of the interval, and at the top.
                // The pins with a phase get a shorter first segment, so their seams sit between the
                // seams of their neighbours.
                std::vector<int> ends;
                for (int l = a; l <= b; ++ l)
                    if ((l - a + 1 + ph) % interval == 0)
                        ends.emplace_back(l);
                if (ends.empty() || ends.back() != b)
                    ends.emplace_back(b);
                // No injection may fill fewer than min_layers: drop the earlier of two close ends
                // (the later one then fills both parts), or the first end if it is too shallow.
                std::vector<int> kept;
                int start = a;
                for (size_t i = 0; i < ends.size(); ++ i) {
                    const bool last = i + 1 == ends.size();
                    if (! last && (ends[i] - start + 1 < min_layers || ends[i + 1] - ends[i] < min_layers))
                        continue;
                    kept.emplace_back(ends[i]);
                    start = ends[i] + 1;
                }
                start = a;
                for (int e : kept) {
                    plan.fills[e].push_back({ uint32_t(pin), start });
                    start = e + 1;
                }
                for (int l = a; l <= b; ++ l)
                    plan.open[l].emplace_back(uint32_t(pin));
            }
            a = b + 1;
        }
    }
    return plan;
}

Points z_pin_grid(const BoundingBox &bbox, coord_t spacing, bool staggered, std::vector<int> *phase_set)
{
    Points out;
    if (phase_set)
        phase_set->clear();
    if (! bbox.defined || spacing <= 0)
        return out;
    const Point   c     = bbox.center();
    const coord_t dy    = staggered ? coord_t(std::round(double(spacing) * std::sqrt(3.) / 2.)) : spacing;
    const int     ny    = int((bbox.max.y() - c.y()) / dy) + 1;
    const int     nx    = int((bbox.max.x() - c.x()) / spacing) + 1;
    for (int j = -ny; j <= ny; ++ j) {
        const coord_t y     = c.y() + j * dy;
        const coord_t shift = (staggered && (j & 1)) ? spacing / 2 : 0;
        if (y < bbox.min.y() || y > bbox.max.y())
            continue;
        for (int i = -nx - 1; i <= nx; ++ i) {
            const coord_t x = c.x() + i * spacing + shift;
            if (x < bbox.min.x() || x > bbox.max.x())
                continue;
            out.emplace_back(x, y);
            if (phase_set)
                // Checkerboard on a square grid; alternate rows on a hexagonal one (every pin then has
                // two neighbours of its own set and four of the other).
                phase_set->emplace_back(staggered ? (j & 1) : ((i + j) & 1));
        }
    }
    return out;
}

Polygon z_pin_circle(const Point &center, coord_t radius, double max_chord_error)
{
    const double r     = unscale<double>(radius);
    const double err   = std::clamp(max_chord_error, 1e-4, std::max(1e-4, 0.5 * r));
    int          n     = int(std::ceil(M_PI / std::acos(std::max(-1., 1. - err / std::max(r, 1e-6)))));
    n                  = std::clamp(n, 8, 64);
    Polygon      out;
    out.points.reserve(n);
    for (int i = 0; i < n; ++ i) {
        const double a = 2. * M_PI * double(i) / double(n);
        out.points.emplace_back(center.x() + coord_t(std::round(double(radius) * std::cos(a))),
                                center.y() + coord_t(std::round(double(radius) * std::sin(a))));
    }
    return out;
}

static bool is_clippable_role(ExtrusionRole role)
{
    // Inner walls and the gap fill between them. Outer and overhang walls stay whole.
    return role == erPerimeter || role == erGapFill;
}

static bool is_clippable(const ExtrusionPath &path)
{
    // Wave-overhang traces are left alone: pins never reach overhangs, but a trace may wander.
    return is_clippable_role(path.role()) && ! path.wave_overhang && ! path.wave_overhang_perimeter;
}

// Clip a chain of paths. Returns false (and leaves `out` empty) when no clippable path runs through `cut`.
static bool clip_paths(const ExtrusionPaths &paths, const ExPolygons &cut, const BoundingBox &cut_bbox, coord_t min_len,
                       ExtrusionEntityCollection &out, bool &touched)
{
    touched = false;
    for (const ExtrusionPath &path : paths) {
        if (! is_clippable(path))
            continue;
        BoundingBox bb;
        for (const Point3 &p : path.polyline.points)
            bb.merge(Point(p.x(), p.y()));
        if (bb.overlap(cut_bbox) && ! intersection_pl(Polylines{ path.polyline.to_polyline() }, cut).empty()) {
            touched = true;
            break;
        }
    }
    if (! touched)
        return false;
    for (const ExtrusionPath &path : paths) {
        if (! is_clippable(path)) {
            out.entities.emplace_back(new ExtrusionPath(path));
            continue;
        }
        Polylines pieces = diff_pl(Polylines{ path.polyline.to_polyline() }, cut);
        for (Polyline &pl : pieces)
            if (pl.length() >= double(min_len))
                out.entities.emplace_back(new ExtrusionPath(Polyline3(pl), path));
    }
    return true;
}

bool z_pin_clip_inner_extrusions(ExtrusionEntityCollection &collection, const ExPolygons &cut, coord_t min_piece_length)
{
    if (cut.empty())
        return false;
    const BoundingBox cut_bbox = get_extents(cut);
    bool              any      = false;
    ExtrusionEntitiesPtr result;
    result.reserve(collection.entities.size());
    for (ExtrusionEntity *ee : collection.entities) {
        if (auto *sub = dynamic_cast<ExtrusionEntityCollection*>(ee)) {
            any |= z_pin_clip_inner_extrusions(*sub, cut, min_piece_length);
            result.emplace_back(ee);
            continue;
        }
        if (! is_clippable_role(ee->role()) && ee->role() != erMixed) {
            result.emplace_back(ee);
            continue;
        }
        const ExtrusionPaths *paths = nullptr;
        ExtrusionPaths        single;
        if (auto *loop = dynamic_cast<ExtrusionLoop*>(ee))
            paths = &loop->paths;
        else if (auto *multi = dynamic_cast<ExtrusionMultiPath*>(ee))
            paths = &multi->paths;
        else if (auto *path = dynamic_cast<ExtrusionPath*>(ee)) {
            single.emplace_back(*path);
            paths = &single;
        }
        auto pieces  = std::make_unique<ExtrusionEntityCollection>();
        bool touched = false;
        if (paths == nullptr || ! clip_paths(*paths, cut, cut_bbox, min_piece_length, *pieces, touched)) {
            result.emplace_back(ee);
            continue;
        }
        any = true;
        // The pieces take the wall's place, in order and flat: G-code expects the extrusions of an
        // island's walls to be loops and paths, not nested collections.
        for (ExtrusionEntity *piece : pieces->entities) {
            piece->inset_idx = ee->inset_idx;
            result.emplace_back(piece);
        }
        pieces->entities.clear();
        delete ee;
    }
    collection.entities = std::move(result);
    return any;
}

// Shell layers of a region as Orca counts them: layer count, or thickness when that is more.
static int shell_layers(int layers, double thickness, double layer_height)
{
    int n = std::max(0, layers);
    if (thickness > 0. && layer_height > 0.)
        n = std::max(n, int(std::ceil(thickness / layer_height - EPSILON)));
    return n;
}

void PrintObject::generate_z_pins()
{
    for (Layer *layer : m_layers) {
        layer->z_pin_holes.clear();
        layer->z_pin_injections.clear();
    }
    const PrintObjectConfig &cfg = m_config;
    if (! cfg.z_pins.value || m_layers.size() < 3 || this->num_printing_regions() == 0)
        return;
    const bool in_infill = cfg.z_pin_in_sparse_infill.value;
    const bool in_walls  = cfg.z_pin_in_inner_walls.value;
    const bool in_solid  = cfg.z_pin_in_solid_infill.value;
    if (! in_infill && ! in_walls)
        return;
    if (m_print->config().spiral_mode.value)
        return;

    const coord_t radius  = scaled<coord_t>(0.5 * cfg.z_pin_diameter.value);
    const coord_t spacing = scaled<coord_t>(cfg.z_pin_spacing.value);
    if (radius <= 0 || spacing <= 2 * radius)
        return;

    BoundingBox bbox;
    for (const Layer *layer : m_layers)
        for (const BoundingBox &bb : layer->lslices_bboxes)
            bbox.merge(bb);
    std::vector<int> pin_set;
    const Points pins = z_pin_grid(bbox, spacing, cfg.z_pin_pattern.value == ZPinPattern::zppStaggered, &pin_set);
    if (pins.empty())
        return;

    // Wall line used for the channel wall and the offsets: the widest inner wall of the regions.
    auto layer_widths = [](const Layer &layer, coord_t &wall, coord_t &ext) {
        wall = ext = 0;
        for (const LayerRegion *layerm : layer.regions()) {
            wall = std::max(wall, layerm->flow(frPerimeter).scaled_width());
            ext  = std::max(ext, layerm->flow(frExternalPerimeter).scaled_width());
        }
    };

    // 1) Which pins fit on which layer.
    std::vector<std::vector<char>> eligible(m_layers.size(), std::vector<char>(pins.size(), 0));
    tbb::parallel_for(tbb::blocked_range<size_t>(0, m_layers.size()), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t idx = range.begin(); idx < range.end(); ++ idx) {
            m_print->throw_if_canceled();
            const Layer &layer = *m_layers[idx];
            if (layer.lslices.empty())
                continue;
            coord_t wall, ext;
            layer_widths(layer, wall, ext);
            ExPolygons fill;
            for (const LayerRegion *layerm : layer.regions())
                append(fill, layerm->fill_expolygons);
            fill = union_ex(fill);
            // The channel and its wall inside the infill area.
            const ExPolygons infill_area = offset_ex(fill, -float(radius + wall));
            ExPolygons area;
            if (in_walls) {
                // Anywhere inside the outer wall, keeping half a line between the channel wall and it.
                area = offset_ex(layer.lslices, -float(ext + wall / 2 + radius + wall));
                if (! in_infill)
                    area = diff_ex(area, infill_area);
            } else
                area = infill_area;
            if (area.empty())
                continue;
            const BoundingBox abb = get_extents(area);
            std::vector<BoundingBox> bbs;
            bbs.reserve(area.size());
            for (const ExPolygon &ex : area)
                bbs.emplace_back(get_extents(ex.contour));
            for (size_t pin = 0; pin < pins.size(); ++ pin) {
                const Point &p = pins[pin];
                if (! abb.contains(p))
                    continue;
                for (size_t i = 0; i < area.size(); ++ i)
                    if (bbs[i].contains(p) && area[i].contains(p)) {
                        eligible[idx][pin] = 1;
                        break;
                    }
            }
        }
    });
    m_print->throw_if_canceled();

    // 2) Columns: keep the channels out of the top and bottom shells unless solid areas are allowed.
    int n_top = 0, n_bottom = 0;
    for (size_t i = 0; i < this->num_printing_regions(); ++ i) {
        const PrintRegionConfig &rc = this->printing_region(i).config();
        n_top    = std::max(n_top, shell_layers(rc.top_shell_layers.value, rc.top_shell_thickness.value, cfg.layer_height.value));
        n_bottom = std::max(n_bottom, shell_layers(rc.bottom_shell_layers.value, rc.bottom_shell_thickness.value, cfg.layer_height.value));
    }
    ZPinColumnParams params;
    params.interval      = std::max(1, cfg.z_pin_interval.value);
    params.top_window    = std::max(1, cfg.z_pin_top_margin.value + (in_solid ? 0 : n_top));
    params.bottom_window = std::max(1, cfg.z_pin_bottom_margin.value + (in_solid ? 0 : n_bottom));
    params.min_layers    = 2;
    std::vector<int> phase(pins.size(), 0);
    if (cfg.z_pin_alternate.value)
        for (size_t i = 0; i < pins.size(); ++ i)
            phase[i] = pin_set[i] ? params.interval / 2 : 0;
    const ZPinPlan plan = plan_z_pin_columns(eligible, phase, params);

    // 3) Geometry: cut the channels, clip the walls they cross, add a wall around each one.
    const double overfill = std::max(0., cfg.z_pin_overfill.value) / 100.;
    const double area_mm2 = M_PI * sqr(0.5 * cfg.z_pin_diameter.value);
    size_t       n_open = 0, n_fills = 0;
    double       volume = 0.;
    for (size_t idx = 0; idx < m_layers.size(); ++ idx) {
        n_open += plan.open[idx].size();
        Layer &layer = *m_layers[idx];
        for (const ZPinEvent &ev : plan.fills[idx]) {
            const Layer &first = *m_layers[ev.first_layer];
            ZPinInjection inj;
            inj.pos    = pins[ev.pin];
            inj.depth  = layer.print_z - (first.print_z - first.height);
            inj.volume = area_mm2 * inj.depth * overfill;
            volume += inj.volume;
            layer.z_pin_injections.emplace_back(inj);
            ++ n_fills;
        }
    }
    tbb::parallel_for(tbb::blocked_range<size_t>(0, m_layers.size()), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t idx = range.begin(); idx < range.end(); ++ idx) {
            m_print->throw_if_canceled();
            Layer &layer = *m_layers[idx];
            if (plan.open[idx].empty())
                continue;
            coord_t wall, ext;
            layer_widths(layer, wall, ext);
            Polygons cut_fill, cut_no_overlap, cut_walls;
            for (uint32_t pin : plan.open[idx]) {
                const Point &p = pins[pin];
                layer.z_pin_holes.emplace_back(p);
                // Infill stops where it meets the channel wall (with the usual small overlap).
                cut_fill.emplace_back(z_pin_circle(p, radius + coord_t(0.85 * wall)));
                cut_no_overlap.emplace_back(z_pin_circle(p, radius + wall));
                // Walls stop at the centre line of the channel wall, so they join it.
                cut_walls.emplace_back(z_pin_circle(p, radius + wall / 2));
            }
            const ExPolygons cut_walls_ex = union_ex(cut_walls);
            for (LayerRegion *layerm : layer.regions()) {
                if (! layerm->fill_expolygons.empty())
                    layerm->fill_expolygons = diff_ex(layerm->fill_expolygons, cut_fill);
                if (! layerm->fill_no_overlap_expolygons.empty())
                    layerm->fill_no_overlap_expolygons = diff_ex(layerm->fill_no_overlap_expolygons, cut_no_overlap);
                Surfaces surfaces;
                for (const Surface &s : layerm->fill_surfaces.surfaces)
                    for (ExPolygon &ex : diff_ex(s.expolygon, cut_fill))
                        surfaces.emplace_back(s, std::move(ex));
                layerm->fill_surfaces.surfaces = std::move(surfaces);
                if (in_walls) {
                    z_pin_clip_inner_extrusions(layerm->perimeters, cut_walls_ex, wall);
                    z_pin_clip_inner_extrusions(layerm->thin_fills, cut_walls_ex, wall);
                }
            }
            // The channel wall: one inner-wall loop, owned by the region the pin sits in.
            for (uint32_t pin : plan.open[idx]) {
                const Point &p      = pins[pin];
                LayerRegion *owner  = nullptr;
                for (LayerRegion *layerm : layer.regions())
                    if (std::any_of(layerm->slices.surfaces.begin(), layerm->slices.surfaces.end(),
                                    [&p](const Surface &s) { return s.expolygon.contains(p); })) {
                        owner = layerm;
                        break;
                    }
                if (owner == nullptr)
                    owner = layer.regions().front();
                const Flow flow = owner->flow(frPerimeter);
                Polygon    ring = z_pin_circle(p, radius + flow.scaled_width() / 2);
                ring.make_clockwise();  // a hole
                ExtrusionPath path(erPerimeter, flow.mm3_per_mm(), flow.width(), flow.height());
                path.polyline = Polyline3(ring.split_at_first_point());
                auto loop = new ExtrusionLoop(std::move(path), elrHole);
                loop->inset_idx = 1;
                auto coll = new ExtrusionEntityCollection();
                coll->entities.emplace_back(loop);
                owner->perimeters.entities.emplace_back(coll);
            }
        }
    });
    m_print->throw_if_canceled();
    BOOST_LOG_TRIVIAL(info) << "Z-pins: " << pins.size() << " grid points, " << n_open << " channel layers, " << n_fills
                            << " injections, " << volume << " mm3";
}

} // namespace Slic3r
