#include "BrickLayers.hpp"

#include "AABBTreeLines.hpp"
#include "BoundingBox.hpp"
#include "ClipperUtils.hpp"
#include "ExtrusionEntity.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "Layer.hpp"
#include "Print.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include <boost/log/trivial.hpp>
#include <tbb/parallel_for.h>

namespace Slic3r {
namespace BrickLayers {

bool is_raised_wall(int inset_idx, int first_wall)
{
    // first_wall is 1-based and never the outer wall.
    const int first = std::max(1, first_wall - 1);
    return inset_idx >= first && (inset_idx - first) % 2 == 0;
}

Intervals inside_intervals(const Polyline &polyline, const Polygons &region, double min_run)
{
    Intervals out;
    if (polyline.points.size() < 2 || region.empty())
        return out;

    const AABBTreeLines::LinesDistancer<Line> grid(to_lines(region));
    const BoundingBox                         region_bbox = get_extents(region);

    struct Run { double from, to; bool inside; };
    std::vector<Run> runs;
    auto push = [&runs](double from, double to, bool inside) {
        if (to <= from)
            return;
        if (!runs.empty() && runs.back().inside == inside)
            runs.back().to = to;
        else
            runs.push_back({from, to, inside});
    };

    double              start = 0.;
    std::vector<double> ts;
    for (size_t i = 1; i < polyline.points.size(); ++i) {
        const Point &a   = polyline.points[i - 1];
        const Point &b   = polyline.points[i];
        const Vec2d  ab  = (b - a).cast<double>();
        const double len = ab.norm();
        if (len <= 0.)
            continue;
        ts.assign({0., 1.});
        // No bounding-box prefilter: the AABB tree prunes, and a BoundingBox of an axis-aligned
        // segment is degenerate (undefined) and would skip the query.
        for (const auto &hit : grid.intersections_with_line<false>(Line(a, b)))
                ts.push_back(std::clamp((hit.first.cast<double>() - a.cast<double>()).dot(ab) / (len * len), 0., 1.));
        std::sort(ts.begin(), ts.end());
        for (size_t k = 1; k < ts.size(); ++k) {
            if (ts[k] - ts[k - 1] <= 0.)
                continue;
            const Vec2d mid    = a.cast<double>() + ab * (0.5 * (ts[k - 1] + ts[k]));
            const bool  inside = region_bbox.contains(mid.cast<coord_t>()) && grid.outside(mid.cast<coord_t>()) < 0;
            push(start + ts[k - 1] * len, start + ts[k] * len, inside);
        }
        start += len;
    }

    // Absorb short runs into their neighbours, shortest first. A run's neighbours always have the
    // other state, so flipping it merges the three into one.
    while (runs.size() > 1) {
        size_t shortest = runs.size();
        double length   = min_run;
        for (size_t i = 0; i < runs.size(); ++i)
            if (runs[i].to - runs[i].from < length) {
                length   = runs[i].to - runs[i].from;
                shortest = i;
            }
        if (shortest == runs.size())
            break;
        runs[shortest].inside = !runs[shortest].inside;
        std::vector<Run> merged;
        merged.reserve(runs.size());
        for (const Run &run : runs)
            if (!merged.empty() && merged.back().inside == run.inside)
                merged.back().to = run.to;
            else
                merged.push_back(run);
        runs = std::move(merged);
    }

    for (const Run &run : runs)
        if (run.inside)
            out.emplace_back(run.from, run.to);
    return out;
}

Polyline sub_polyline(const Polyline &polyline, double from, double to)
{
    Polyline out;
    if (polyline.points.empty())
        return out;
    double start = 0.;
    for (size_t i = 1; i < polyline.points.size(); ++i) {
        const Point &a   = polyline.points[i - 1];
        const Point &b   = polyline.points[i];
        const double len = (b - a).cast<double>().norm();
        const double end = start + len;
        auto at = [&](double s) -> Point {
            return len <= 0. ? a : (a.cast<double>() + (b - a).cast<double>() * ((s - start) / len)).cast<coord_t>();
        };
        if (out.points.empty() && from <= end)
            out.points.push_back(from <= start ? a : at(from));
        if (!out.points.empty()) {
            if (to <= end) {
                const Point last = to >= end ? b : at(to);
                if (last != out.points.back() || out.points.size() == 1)
                    out.points.push_back(last);
                return out;
            }
            if (b != out.points.back())
                out.points.push_back(b);
        }
        start = end;
    }
    if (out.points.size() == 1)
        out.points.push_back(polyline.points.back());
    return out;
}

double emitted_mm3_per_mm(const ExtrusionPath &path)
{
    if (!path.z_contoured || path.polyline.points.empty() || path.height <= 0.f)
        return path.mm3_per_mm;
    const double z = unscale<double>(path.polyline.points.front().z());
    return path.mm3_per_mm * (path.height + z) / path.height;
}

namespace {

// A wall path that may be raised.
struct Candidate
{
    ExtrusionEntity *owner;    // the loop or multipath holding the path
    ExtrusionPaths  *paths;
    size_t           idx;
    Params           params;
    Polyline         polyline;
    double           length;
    double           half_width;
    Intervals        up;       // stretches with a candidate wall above: raised
    Intervals        below;    // stretches on a raised piece of the layer below
};

bool eligible(const ExtrusionPath &path)
{
    return path.role() == erPerimeter && !path.z_contoured && path.mm3_per_mm > 0. && path.polyline.points.size() >= 2 &&
           !path.wave_overhang && !path.wave_overhang_floor && !path.wave_overhang_perimeter && !path.wave_overhang_floor_perimeter;
}

void collect(ExtrusionEntityCollection &collection, const Params &params, std::vector<Candidate> &out)
{
    for (ExtrusionEntity *entity : collection.entities) {
        ExtrusionPaths *paths = nullptr;
        if (auto *child = dynamic_cast<ExtrusionEntityCollection *>(entity)) {
            collect(*child, params, out);
            continue;
        } else if (auto *loop = dynamic_cast<ExtrusionLoop *>(entity)) {
            // Orca: Z-pin channel walls stay flat, the pin fill runs the nozzle over them.
            if (loop->z_pin_channel)
                continue;
            paths = &loop->paths;
        }
        else if (auto *multipath = dynamic_cast<ExtrusionMultiPath *>(entity))
            paths = &multipath->paths;
        if (paths == nullptr || !is_raised_wall(entity->inset_idx, params.first_wall))
            continue;
        for (size_t i = 0; i < paths->size(); ++i) {
            const ExtrusionPath &path = (*paths)[i];
            if (!eligible(path))
                continue;
            Candidate c{entity, paths, i, params, path.polyline.to_polyline(), 0., scale_(0.5 * path.width), {}, {}};
            c.length = c.polyline.length();
            if (c.length > 0.)
                out.push_back(std::move(c));
        }
    }
}

bool contains(const Intervals &intervals, double s)
{
    return std::any_of(intervals.begin(), intervals.end(), [s](const Interval &i) { return i.first <= s && s <= i.second; });
}

Polygons footprint(const std::vector<Candidate> &candidates, const Intervals Candidate::*part)
{
    Polygons out;
    for (const Candidate &c : candidates) {
        if (part == nullptr)
            append(out, offset(c.polyline, float(c.half_width)));
        else
            for (const Interval &i : c.*part)
                append(out, offset(sub_polyline(c.polyline, i.first, i.second), float(c.half_width)));
    }
    return union_(out);
}

struct Piece { double from, to; bool up, below; };

} // namespace

size_t apply(std::vector<LayerInput> &layers)
{
    const size_t n = layers.size();
    std::vector<std::vector<Candidate>> candidates(n);
    std::vector<Polygons>               walls(n);   // footprint of all candidate walls
    std::vector<Polygons>               raised(n);  // footprint of the raised pieces

    tbb::parallel_for(tbb::blocked_range<size_t>(0, n), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t l = range.begin(); l < range.end(); ++l) {
            if (l == 0 || !layers[l].enabled)
                continue;
            for (auto &[collection, params] : layers[l].perimeters)
                collect(*collection, params, candidates[l]);
            walls[l] = footprint(candidates[l], nullptr);
        }
    });

    // A run must be at least this long to change height, so walls don't step for a few microns.
    auto min_run = [](const Candidate &c) { return std::max(scale_(1.), 4. * c.half_width); };

    tbb::parallel_for(tbb::blocked_range<size_t>(0, n), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t l = range.begin(); l < range.end(); ++l) {
            if (l + 1 >= n || candidates[l].empty() || walls[l + 1].empty())
                continue;
            for (Candidate &c : candidates[l])
                c.up = inside_intervals(c.polyline, walls[l + 1], min_run(c));
            raised[l] = footprint(candidates[l], &Candidate::up);
        }
    });

    tbb::parallel_for(tbb::blocked_range<size_t>(0, n), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t l = range.begin(); l < range.end(); ++l) {
            if (l == 0 || candidates[l].empty() || raised[l - 1].empty())
                continue;
            for (Candidate &c : candidates[l])
                c.below = inside_intervals(c.polyline, raised[l - 1], min_run(c));
        }
    });

    std::vector<size_t> raised_count(n, 0);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, n), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t l = range.begin(); l < range.end(); ++l) {
            std::vector<Candidate> &cands = candidates[l];
            if (cands.empty())
                continue;
            const double h       = layers[l].height;
            const double h_below = layers[l - 1].height;

            // Candidates of one owner are contiguous and in path order; rebuild each owner's paths once.
            for (size_t begin = 0; begin < cands.size();) {
                size_t end = begin + 1;
                while (end < cands.size() && cands[end].owner == cands[begin].owner)
                    ++end;
                ExtrusionPaths &old_paths = *cands[begin].paths;
                ExtrusionPaths  new_paths;
                bool            any_raised = false;
                size_t          next       = begin;
                for (size_t i = 0; i < old_paths.size(); ++i) {
                    if (next == end || cands[next].idx != i) {
                        new_paths.emplace_back(std::move(old_paths[i]));
                        continue;
                    }
                    const Candidate &c = cands[next++];

                    std::vector<double> cuts{0., c.length};
                    for (const Interval &iv : c.up)    { cuts.push_back(iv.first); cuts.push_back(iv.second); }
                    for (const Interval &iv : c.below) { cuts.push_back(iv.first); cuts.push_back(iv.second); }
                    std::sort(cuts.begin(), cuts.end());
                    std::vector<Piece> pieces;
                    for (size_t k = 1; k < cuts.size(); ++k) {
                        const double from = std::max(0., cuts[k - 1]), to = std::min(c.length, cuts[k]);
                        if (to - from <= SCALED_EPSILON)
                            continue;
                        const double mid   = 0.5 * (from + to);
                        const bool   up    = contains(c.up, mid);
                        const bool   below = contains(c.below, mid);
                        if (!pieces.empty() && pieces.back().up == up && pieces.back().below == below)
                            pieces.back().to = to;
                        else
                            pieces.push_back({from, to, up, below});
                    }
                    if (pieces.empty()) {
                        new_paths.emplace_back(std::move(old_paths[i]));
                        continue;
                    }
                    if (!pieces.empty()) {
                        pieces.front().from = 0.;
                        pieces.back().to    = c.length;
                    }

                    auto adjust = [&](ExtrusionPath &path, const Piece &piece) {
                        if (!piece.up && !piece.below)
                            return;
                        const double z_offset = piece.up ? 0.5 * h : 0.;
                        // Clamp so a run of much thinner layers never asks for a bead of no height.
                        const double height   = std::max(0.1 * h, h + z_offset - (piece.below ? 0.5 * h_below : 0.));
                        path.mm3_per_mm *= height / h * c.params.flow_ratio;
                        path.height = float(height);
                        if (piece.up) {
                            // The G-code writer scales the flow of Z-contoured points by (height + z) / height
                            // (Z anti-aliasing extends a lowered bead). Undo that here so the bead gets exactly
                            // the flow of its real height. See emitted_mm3_per_mm().
                            path.mm3_per_mm *= height / (height + z_offset);
                            const coord_t z = coord_t(scale_(z_offset));
                            for (Point3 &p : path.polyline.points)
                                p.z() = z;
                            path.z_contoured = true;
                            any_raised       = true;
                            ++raised_count[l];
                        }
                    };

                    if (pieces.size() == 1) {
                        // Keep the path (and any arc fitting) as it is, only its height changes.
                        ExtrusionPath path = std::move(old_paths[i]);
                        adjust(path, pieces.front());
                        new_paths.emplace_back(std::move(path));
                        continue;
                    }
                    for (const Piece &piece : pieces) {
                        Polyline part = sub_polyline(c.polyline, piece.from, piece.to);
                        if (part.points.size() < 2)
                            continue;
                        ExtrusionPath path(Polyline3(part), old_paths[i]);
                        adjust(path, piece);
                        new_paths.emplace_back(std::move(path));
                    }
                }
                old_paths = std::move(new_paths);
                if (any_raised) {
                    // A raised wall stands half a layer proud of the infill next to it; print it once
                    // that infill is down so the nozzle never drags through it.
                    if (auto *loop = dynamic_cast<ExtrusionLoop *>(cands[begin].owner))
                        loop->print_after_infill = true;
                    else if (auto *multipath = dynamic_cast<ExtrusionMultiPath *>(cands[begin].owner))
                        multipath->print_after_infill = true;
                }
                begin = end;
            }
        }
    });

    size_t total = 0;
    for (size_t c : raised_count)
        total += c;
    return total;
}

void apply(PrintObject &object)
{
    // Spiral vase has a single wall; belt printers tilt Z, so a Z offset is not "up" there.
    if (object.print()->config().spiral_mode.value || object.print()->config().belt_printer.value)
        return;
    const LayerPtrs &layers = object.layers();
    if (layers.size() < 3)
        return;

    bool any         = false;
    int  wave_floors = 0;
    for (size_t r = 0; r < object.num_printing_regions(); ++r) {
        const PrintRegionConfig &config = object.printing_region(r).config();
        any |= config.brick_layers.value;
        if (config.wave_overhangs.value) {
            wave_floors = std::max(wave_floors, std::max(1, config.wave_overhang_floor_layers.value));
            // Orca: a staircase band prints the overhang of the next layers-1 layers along with the wave layer.
            if (config.wave_overhang_staircase.value)
                wave_floors = std::max(wave_floors, config.wave_overhang_staircase_layers.value - 1);
        }
    }
    if (!any)
        return;

    std::vector<LayerInput> inputs(layers.size());
    int                     since_wave = std::numeric_limits<int>::max() / 2;
    for (size_t l = 0; l < layers.size(); ++l) {
        const Layer *layer = layers[l];
        // Walls on wave-overhang layers and on the floor layers above them rest on the wave traces;
        // leave them where the wave code put them.
        const bool wave = !layer->wave_overhang_floor_polygons.empty() || !layer->wave_overhang_covered_polygons.empty();
        since_wave      = wave ? 0 : since_wave + 1;
        LayerInput &in  = inputs[l];
        in.height       = layer->height;
        in.enabled      = l > 0 && since_wave > wave_floors;
        for (LayerRegion *layerm : layer->regions()) {
            const PrintRegionConfig &config = layerm->region().config();
            if (!config.brick_layers.value || int(l) + 1 < config.brick_layers_start_layer.value)
                continue;
            in.perimeters.emplace_back(&layerm->perimeters,
                                       Params{config.brick_layers_first_wall.value, config.brick_layers_flow_ratio.value});
        }
    }

    const size_t raised = apply(inputs);
    BOOST_LOG_TRIVIAL(info) << "Brick layers: raised " << raised << " wall pieces in " << layers.size() << " layers";
}

} // namespace BrickLayers
} // namespace Slic3r
