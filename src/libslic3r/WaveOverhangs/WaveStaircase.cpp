///|/ Staircase wave overhangs (multi-layer wave bands). See WaveStaircase.hpp.
///|/
///|/ Released under the terms of the AGPLv3 or higher.
///|/
#include "WaveStaircase.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <unordered_map>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Fill/FillBase.hpp"
#include "libslic3r/Surface.hpp"

namespace Slic3r::WaveOverhangs::Staircase {

namespace {

int floor_div(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

// Opening that drops slivers narrower than 2 * r.
Polygons drop_slivers(const Polygons &p, coord_t r)
{
    if (p.empty() || r <= 0)
        return p;
    return offset(offset(p, float(-r), jtRound, 0.), float(r), jtRound, 0.);
}

// Nested distance field over the band's domain. R(j) is everything within j ring spacings of
// the supported area, measured along the domain (geodesic, like the wave fronts); for j < 0 it is
// the supported area shrunk by |j| spacings. R(j) grows with j.
class Field
{
public:
    Field(const Polygons &anchor, const Polygons &domain, coord_t spacing)
    {
        if (domain.empty() || spacing <= 0)
            return;
        Polygons r0 = intersection(anchor, domain);
        if (r0.empty())
            r0 = intersection(expand(anchor, float(spacing)), domain);
        if (r0.empty())
            return;
        // Inside the supported area: shrink.
        std::vector<Polygons> neg;
        for (int j = 1; j < 2000; ++j) {
            Polygons r = intersection(shrink(anchor, float(j) * float(spacing)), domain);
            if (r.empty())
                break;
            neg.emplace_back(std::move(r));
        }
        m_j_min = -int(neg.size());
        for (auto it = neg.rbegin(); it != neg.rend(); ++it)
            push(std::move(*it));
        double prev_area = area(r0);
        push(std::move(r0));
        // Outside: grow along the domain.
        const double min_growth = 0.01 * double(spacing) * double(spacing);
        for (int j = 1; j < 8000; ++j) {
            Polygons next = intersection(offset(m_polys.back(), float(spacing), jtRound, 0.), domain);
            const double a = area(next);
            if (next.empty() || a <= prev_area + min_growth)
                break;
            prev_area = a;
            push(std::move(next));
        }
    }

    bool empty() const { return m_polys.empty(); }
    int  j_min() const { return m_j_min; }
    int  j_max() const { return m_j_min + int(m_polys.size()) - 1; }
    const Polygons &ring_area(int j) const { return m_polys[size_t(j - m_j_min)]; }

    // Smallest j with p inside R(j); j_max() + 1 when p is outside all of them.
    int ring_of(const Point &p) const
    {
        if (m_polys.empty() || !contains(j_max(), p))
            return j_max() + 1;
        int lo = j_min(), hi = j_max();
        while (lo < hi) {
            const int mid = floor_div(lo + hi, 2);
            if (contains(mid, p))
                hi = mid;
            else
                lo = mid + 1;
        }
        return lo;
    }

private:
    void push(Polygons &&p)
    {
        m_bboxes.emplace_back(get_extents(p));
        m_ex.emplace_back(union_ex(p));
        m_polys.emplace_back(std::move(p));
    }
    bool contains(int j, const Point &p) const
    {
        const size_t i = size_t(j - m_j_min);
        if (!m_bboxes[i].contains(p))
            return false;
        for (const ExPolygon &e : m_ex[i])
            if (e.contains(p))
                return true;
        return false;
    }

    int                     m_j_min = 0;
    std::vector<Polygons>   m_polys;
    std::vector<ExPolygons> m_ex;
    std::vector<BoundingBox> m_bboxes;
};

struct Piece
{
    Polyline      pl;
    int           step  = 0;
    int           band  = 0;
    ExtrusionPath proto;
};

// Cut a polyline where it crosses from one band into the next, keeping its order and direction.
// Pieces shorter than min_len join the piece before them.
std::vector<std::pair<Polyline, int>> cut_into_bands(const Polyline &pl, const Field &field, int rings_per_step, coord_t step_len, coord_t min_len)
{
    std::vector<std::pair<Polyline, int>> out;
    if (pl.points.size() < 2)
        return out;
    auto band_of = [&](const Point &p) { return floor_div(field.ring_of(p), rings_per_step); };
    Points pts;
    pts.reserve(pl.points.size() * 2);
    pts.push_back(pl.points.front());
    for (size_t i = 1; i < pl.points.size(); ++i) {
        const Point a = pl.points[i - 1], b = pl.points[i];
        const double len = (b - a).cast<double>().norm();
        const int n = std::max(1, int(std::ceil(len / double(step_len))));
        for (int s = 1; s <= n; ++s)
            pts.push_back((a.cast<double>() + (b - a).cast<double>() * (double(s) / n)).cast<coord_t>());
    }
    int      cur = band_of(pts.front());
    Polyline piece;
    piece.points.push_back(pts.front());
    for (size_t i = 1; i < pts.size(); ++i) {
        const int b = band_of(pts[i]);
        if (b != cur) {
            const Point mid = ((pts[i - 1].cast<double>() + pts[i].cast<double>()) * 0.5).cast<coord_t>();
            piece.points.push_back(mid);
            out.emplace_back(std::move(piece), cur);
            piece = Polyline();
            piece.points.push_back(mid);
            cur = b;
        }
        piece.points.push_back(pts[i]);
    }
    out.emplace_back(std::move(piece), cur);
    // Merge pieces too short to be worth a travel into their neighbour.
    std::vector<std::pair<Polyline, int>> merged;
    for (auto &pb : out) {
        if (!merged.empty() && pb.first.length() < double(min_len)) {
            Polyline &prev = merged.back().first;
            prev.points.insert(prev.points.end(), pb.first.points.begin() + 1, pb.first.points.end());
        } else
            merged.emplace_back(std::move(pb));
    }
    if (merged.size() > 1 && merged.front().first.length() < double(min_len)) {
        // A short leading piece joins the one after it.
        Polyline &first = merged.front().first, &second = merged[1].first;
        first.points.insert(first.points.end(), second.points.begin() + 1, second.points.end());
        merged[1].first = std::move(first);
        merged.erase(merged.begin());
    }
    for (auto &pb : merged)
        pb.first.remove_duplicate_points();
    merged.erase(std::remove_if(merged.begin(), merged.end(), [](const auto &pb) { return pb.first.points.size() < 2; }), merged.end());
    return merged;
}

Polylines fill_pattern(const Polygons &region, StepPattern pattern, const Flow &flow, float angle, size_t layer_id, double z)
{
    InfillPattern ip = ipRectilinear;
    switch (pattern) {
    case StepPattern::Hilbert:    ip = ipHilbertCurve; break;
    case StepPattern::Concentric: ip = ipConcentric; break;
    default:                      ip = ipRectilinear; break;
    }
    Polylines out;
    const ExPolygons ex = union_ex(region);
    if (ex.empty())
        return out;
    std::unique_ptr<Fill> f(Fill::new_from_type(ip));
    f->set_bounding_box(get_extents(ex));
    f->layer_id        = layer_id;
    f->z               = z;
    f->angle           = angle;
    f->spacing         = flow.spacing();
    f->link_max_length = coord_t(scale_(3. * flow.spacing()));
    f->loop_clipping   = 0;
    f->overlap         = 0.;
    FillParams fp;
    fp.density           = 1.f;
    fp.dont_adjust       = false;
    fp.flow              = flow;
    fp.extrusion_role    = erSolidInfill;
    fp.pattern           = ip;
    fp.layer_height      = flow.height();
    fp.anchor_length     = 1000.f;
    fp.anchor_length_max = 1000.f;
    for (const ExPolygon &e : ex) {
        Surface surf(stInternalSolid, e);
        try {
            append(out, f->fill_surface(&surf, fp));
        } catch (const std::exception &) {
            // A failed fill leaves this piece of the step to the normal infill: the caller
            // only removes what the band prints.
        }
    }
    return out;
}

// Sample grid for "which pieces come within r of this one".
struct Samples
{
    struct S { Point p; int piece; };
    coord_t                                    cell = 1;
    std::unordered_map<int64_t, std::vector<S>> grid;
    static int64_t key(int64_t ix, int64_t iy) { return (ix << 32) ^ (iy & 0xffffffff); }
    void add(const Points &pts, int piece)
    {
        for (const Point &p : pts)
            grid[key(p.x() / cell - (p.x() < 0), p.y() / cell - (p.y() < 0))].push_back({ p, piece });
    }
    template<class Fn> void near(const Point &p, Fn &&fn) const
    {
        const int64_t ix = p.x() / cell - (p.x() < 0), iy = p.y() / cell - (p.y() < 0);
        for (int64_t dx = -1; dx <= 1; ++dx)
            for (int64_t dy = -1; dy <= 1; ++dy) {
                auto it = grid.find(key(ix + dx, iy + dy));
                if (it != grid.end())
                    for (const S &s : it->second)
                        fn(s);
            }
    }
};

Points sample(const Polyline &pl, coord_t step)
{
    Points out;
    if (pl.points.empty())
        return out;
    out.push_back(pl.points.front());
    for (size_t i = 1; i < pl.points.size(); ++i) {
        const Point a = pl.points[i - 1], b = pl.points[i];
        const double len = (b - a).cast<double>().norm();
        const int n = std::max(1, int(std::ceil(len / double(step))));
        for (int s = 1; s <= n; ++s)
            out.push_back((a.cast<double>() + (b - a).cast<double>() * (double(s) / n)).cast<coord_t>());
    }
    return out;
}

} // namespace

std::vector<Polygons> step_regions(const BandInput &in, const Params &params)
{
    std::vector<Polygons> out;
    if (in.steps.empty() || in.footprint.empty() || params.layers < 2)
        return out;
    double w = 0.;
    for (const StepLayer &s : in.steps)
        w = std::max(w, double(s.flow.width()));
    for (const ExtrusionPath &p : in.waves)
        w = std::max(w, double(p.width));
    // Keep-out between a step and anything printed later at a lower height, centreline to
    // region edge: the stagger plus both lines' half widths.
    const coord_t d     = coord_t(scale_(params.stagger + w));
    const coord_t sliver = coord_t(scale_(0.5 * w));
    // Let the steps reach back over the supported side, so every step is full height right
    // at the overhang edge despite the taper between steps.
    const coord_t root  = coord_t(scale_((params.stagger + w) * double(in.steps.size())));

    Polygons base = union_(in.footprint, intersection(expand(in.footprint, float(root)), in.island));
    Polygons prev;
    for (size_t k = 0; k < in.steps.size(); ++k) {
        Polygons s;
        if (k == 0) {
            s = intersection(base, in.steps[0].fill);
            if (!in.rest0.empty())
                s = diff(s, expand(in.rest0, float(d)));
        } else {
            s = intersection(prev, in.steps[k].fill);
            const Polygons later_lower = diff(in.steps[k - 1].slices, prev);
            if (!later_lower.empty())
                s = diff(s, expand(later_lower, float(d)));
        }
        s = drop_slivers(s, sliver);
        if (s.empty())
            break;
        out.push_back(s);
        prev = std::move(s);
    }
    return out;
}

BandResult build_band(const BandInput &in, const Params &params)
{
    BandResult res;
    res.step_regions = step_regions(in, params);
    if (res.step_regions.empty() || in.waves.empty())
        return BandResult{};

    const int     rings      = std::max(1, params.rings_per_step);
    const coord_t spacing    = std::max<coord_t>(coord_t(scale_(0.05)), coord_t(scale_(params.ring_spacing)));
    Polygons      domain     = union_(in.footprint, res.step_regions.front());
    const Field   field(in.anchor, domain, spacing);
    if (field.empty())
        return BandResult{};

    const coord_t cut_step = std::max<coord_t>(spacing / 2, 1);
    const coord_t min_len  = coord_t(scale_(0.6));

    std::vector<Piece> pieces;
    // Step 0: the wave paths, cut into bands, in their print order.
    // A wave front stays whole: it takes the latest band it reaches, which only ever delays it
    // (the steps wait for the waves, never the other way round). Cutting a front where it brushes
    // a band edge would scatter it over rounds and send the nozzle back and forth along it. Only
    // paths that really cross several bands (zig-zag chains of fronts) are cut.
    for (const ExtrusionPath &wave : in.waves) {
        std::vector<std::pair<Polyline, int>> cut = cut_into_bands(wave.polyline.to_polyline(), field, rings, cut_step, min_len);
        if (cut.size() > 1) {
            int lo = std::numeric_limits<int>::max(), hi = std::numeric_limits<int>::min();
            for (const auto &pb : cut) {
                lo = std::min(lo, pb.second);
                hi = std::max(hi, pb.second);
            }
            if (hi - lo <= 1) {
                cut.clear();
                cut.emplace_back(wave.polyline.to_polyline(), hi);
            } else
                cut = cut_into_bands(wave.polyline.to_polyline(), field, rings, cut_step, coord_t(scale_(5.)));
        }
        for (auto &pb : cut) {
            Piece p;
            p.pl    = std::move(pb.first);
            p.step  = 0;
            p.band  = pb.second;
            p.proto = wave;
            pieces.emplace_back(std::move(p));
        }
    }
    // Upper steps.
    for (size_t k = 0; k < res.step_regions.size(); ++k) {
        const StepLayer &sl   = in.steps[k];
        const Polygons  &reg  = res.step_regions[k];
        Flow             flow = sl.flow;
        if (params.pattern == StepPattern::Waves)
            flow = flow.with_spacing(float(unscale<double>(spacing)));
        ExtrusionPath proto(erSolidInfill, flow.mm3_per_mm(), flow.width(), flow.height());
        proto.z_contoured   = true;
        proto.staircase_step = int8_t(k + 1);
        auto add = [&](Polyline &&pl, int band) {
            if (pl.points.size() < 2 || pl.length() < double(scale_(0.2)))
                return;
            Piece p;
            p.pl    = std::move(pl);
            p.step  = int(k + 1);
            p.band  = band;
            p.proto = proto;
            pieces.emplace_back(std::move(p));
        };
        if (params.pattern == StepPattern::Waves) {
            const Polygons trim = shrink(reg, float(flow.scaled_width() / 2), jtRound, 0.);
            if (trim.empty())
                continue;
            const BoundingBox trim_bb = get_extents(trim);
            for (int j = field.j_min(); j <= field.j_max(); ++j) {
                const Polygons &ra = field.ring_area(j);
                if (!get_extents(ra).overlap(trim_bb))
                    continue;
                Polylines fronts = intersection_pl(to_polylines(ra), trim);
                for (Polyline &f : fronts)
                    add(std::move(f), floor_div(j, rings));
            }
        } else {
            Polylines lines = fill_pattern(reg, params.pattern, flow, sl.angle + float(k) * float(M_PI / 2.), k + 1, sl.dz);
            for (const Polyline &l : lines)
                for (auto &pb : cut_into_bands(l, field, rings, cut_step, min_len))
                    add(std::move(pb.first), pb.second);
        }
    }

    // Dependencies: a step-k piece waits for every lower piece within reach of the nozzle
    // tip, so nothing lower is ever printed next to it afterwards.
    const size_t n = pieces.size();
    double max_w = 0.;
    for (const Piece &p : pieces)
        max_w = std::max(max_w, double(p.proto.width));
    const coord_t sample_step = coord_t(scale_(0.25));
    Samples grid;
    grid.cell = std::max<coord_t>(1, coord_t(scale_(params.stagger + max_w)) + sample_step);
    std::vector<Points> samples(n);
    for (size_t i = 0; i < n; ++i) {
        samples[i] = sample(pieces[i].pl, sample_step);
        grid.add(samples[i], int(i));
    }
    std::vector<std::vector<int>> deps(n);
    std::vector<int>              seen(n, -1);
    for (size_t i = 0; i < n; ++i) {
        if (pieces[i].step == 0)
            continue;
        const double r  = scale_(params.stagger + 0.5 * double(pieces[i].proto.width)) + 0.5 * double(sample_step);
        const double r2 = r * r;
        const int    k  = pieces[i].step;
        for (const Point &p : samples[i])
            grid.near(p, [&](const Samples::S &s) {
                if (seen[s.piece] == int(i) || pieces[s.piece].step >= k)
                    return;
                if ((s.p - p).cast<double>().squaredNorm() < r2) {
                    seen[s.piece] = int(i);
                    deps[i].push_back(s.piece);
                }
            });
    }

    // Schedule in rounds: the wave front advances one band, then each step above prints what
    // its dependencies allow, nearest first.
    const int nsteps = int(res.step_regions.size()) + 1;
    std::vector<std::vector<int>> by_step(nsteps);
    int band_min = std::numeric_limits<int>::max(), band_max = std::numeric_limits<int>::min();
    for (size_t i = 0; i < n; ++i) {
        by_step[pieces[i].step].push_back(int(i));
        band_min = std::min(band_min, pieces[i].band);
        band_max = std::max(band_max, pieces[i].band);
    }
    for (int k = 1; k < nsteps; ++k)
        std::stable_sort(by_step[k].begin(), by_step[k].end(), [&](int a, int b) { return pieces[a].band < pieces[b].band; });

    std::vector<char> done(n, 0);
    std::vector<std::pair<int, bool>> order; // piece, reversed
    order.reserve(n);
    Point pos = pieces.empty() ? Point(0, 0) : pieces.front().pl.first_point();
    auto ready = [&](int i) {
        for (int d : deps[i])
            if (!done[d])
                return false;
        return true;
    };
    auto emit_batch = [&](std::vector<int> batch, bool reversible) {
        while (!batch.empty()) {
            size_t best = 0;
            bool   best_rev = false;
            double best_d = std::numeric_limits<double>::max();
            for (size_t b = 0; b < batch.size(); ++b) {
                const Polyline &pl = pieces[batch[b]].pl;
                const double d0 = (pl.first_point() - pos).cast<double>().squaredNorm();
                if (d0 < best_d) { best_d = d0; best = b; best_rev = false; }
                if (reversible) {
                    const double d1 = (pl.last_point() - pos).cast<double>().squaredNorm();
                    if (d1 < best_d) { best_d = d1; best = b; best_rev = true; }
                }
            }
            const int i = batch[best];
            done[i] = 1;
            order.emplace_back(i, best_rev);
            pos = best_rev ? pieces[i].pl.first_point() : pieces[i].pl.last_point();
            batch.erase(batch.begin() + best);
        }
    };
    std::vector<size_t> round_end; // index into `order` of the last piece of each round
    auto run_round = [&](int band_limit, bool flush) {
        const size_t before = order.size();
        // Step 0 keeps the wave generator's order and direction.
        for (int i : by_step[0])
            if (!done[i] && (flush || pieces[i].band <= band_limit)) {
                done[i] = 1;
                order.emplace_back(i, false);
                pos = pieces[i].pl.last_point();
            }
        for (int k = 1; k < nsteps; ++k) {
            std::vector<int> batch;
            for (int i : by_step[k])
                if (!done[i] && ready(i)) {
                    // ready() looks at lower steps only, so the batch can be marked at once.
                    batch.push_back(i);
                }
            emit_batch(std::move(batch), true);
        }
        if (order.size() > before)
            round_end.push_back(order.size() - 1);
    };
    if (n > 0) {
        for (int m = band_min; m <= band_max; ++m)
            run_round(m, false);
        // Everything is ready by now (lower steps are all done), but loop to be safe.
        for (int guard = 0; guard < nsteps + 1 && order.size() < n; ++guard)
            run_round(band_max, true);
    }

    res.rounds = int(round_end.size());
    size_t next_end = 0;
    for (size_t o = 0; o < order.size(); ++o) {
        const Piece &p = pieces[order[o].first];
        Polyline pl = p.pl;
        if (order[o].second)
            pl.reverse();
        const coord_t z = p.step == 0 ? 0 : coord_t(scale_(in.steps[size_t(p.step - 1)].dz));
        ExtrusionPath path(Polyline3(pl, z), p.proto);
        path.staircase_step      = int8_t(p.step);
        path.staircase_band      = true;
        path.staircase_round_end = next_end < round_end.size() && round_end[next_end] == o;
        if (path.staircase_round_end)
            ++next_end;
        res.paths.emplace_back(std::move(path));
    }
    return res;
}

ClearanceReport check_sequence(const ExtrusionPaths &sequence, double stagger, double tolerance)
{
    ClearanceReport rep;
    struct S { Point p; double z; double half_w; };
    const coord_t cell = coord_t(scale_(std::max(0.5, stagger + 1.)));
    std::unordered_map<int64_t, std::vector<S>> grid;
    auto key = [&](const Point &p) {
        const int64_t ix = p.x() / cell - (p.x() < 0), iy = p.y() / cell - (p.y() < 0);
        return std::make_pair(ix, iy);
    };
    auto k64 = [](int64_t ix, int64_t iy) { return (ix << 32) ^ (iy & 0xffffffff); };
    const coord_t step = coord_t(scale_(0.1));
    for (const ExtrusionPath &path : sequence) {
        if (path.polyline.points.size() < 2)
            continue;
        const double z = path.z_contoured ? unscale<double>(path.polyline.points.front().z()) : 0.;
        const Points pts = sample(path.polyline.to_polyline(), step);
        for (const Point &p : pts) {
            const auto [ix, iy] = key(p);
            for (int64_t dx = -1; dx <= 1; ++dx)
                for (int64_t dy = -1; dy <= 1; ++dy) {
                    auto it = grid.find(k64(ix + dx, iy + dy));
                    if (it == grid.end())
                        continue;
                    for (const S &s : it->second) {
                        if (s.z <= z + 1e-6)
                            continue;
                        const double c = unscale<double>((s.p - p).cast<double>().norm()) - s.half_w;
                        rep.min_clearance = std::min(rep.min_clearance, c);
                        if (c < stagger - tolerance)
                            ++rep.violations;
                    }
                }
        }
        for (const Point &p : pts) {
            const auto [ix, iy] = key(p);
            grid[k64(ix, iy)].push_back({ p, z, 0.5 * double(path.width) });
        }
    }
    return rep;
}

} // namespace Slic3r::WaveOverhangs::Staircase
