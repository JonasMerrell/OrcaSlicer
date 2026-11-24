///|/ Toolhead clearance for non-planar printing. See ToolheadClearance.hpp.
#include "ToolheadClearance.hpp"

#include "../PrintConfig.hpp"
#include "../Print.hpp"
#include "../Layer.hpp"
#include "../TriangleMesh.hpp"
#include "../Model.hpp"
#include "../Format/STL.hpp"
#include "../Format/OBJ.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace Slic3r {
namespace Clearance {

// Profiles and meshes are rasterised out to this radius; anything further out counts
// toward far_height (a plate-wide plane, e.g. the X beam).
static constexpr double MAX_EXTENT = 100.;

// ---- ToolheadShape -----------------------------------------------------------------

ToolheadShape ToolheadShape::from_profile(std::vector<ProfileStep> steps, double cell)
{
    ToolheadShape s;
    s.m_cell = std::max(cell, 0.05);
    std::sort(steps.begin(), steps.end(), [](const ProfileStep &a, const ProfileStep &b) { return a.radius < b.radius; });
    steps.erase(std::remove_if(steps.begin(), steps.end(), [](const ProfileStep &p) { return !(p.radius > 0.) || std::isnan(p.height); }), steps.end());
    if (steps.empty())
        return s;
    // Steps reaching past MAX_EXTENT become the far plane; the grid covers the rest.
    double grid_r = 0.;
    for (const ProfileStep &p : steps) {
        if (p.radius <= MAX_EXTENT)
            grid_r = p.radius;
        else
            s.m_far = std::min(s.m_far, std::max(0., p.height));
    }
    if (grid_r <= 0.)
        grid_r = std::min(steps.front().radius, MAX_EXTENT);
    s.m_half    = int(std::ceil(grid_r / s.m_cell)) + 1;
    const int n = 2 * s.m_half + 1;
    s.m_cells.assign(size_t(n) * n, INF);
    for (int iy = -s.m_half; iy <= s.m_half; ++iy)
        for (int ix = -s.m_half; ix <= s.m_half; ++ix) {
            const double r = std::hypot(ix * s.m_cell, iy * s.m_cell);
            double       h = INF;
            for (const ProfileStep &p : steps)
                if (r < p.radius) {
                    h = std::max(0., p.height);
                    break;
                }
            s.m_cells[size_t(iy + s.m_half) * n + (ix + s.m_half)] = h;
        }
    s.finish();
    return s;
}

ToolheadShape ToolheadShape::from_mesh(const indexed_triangle_set &its, double cell)
{
    ToolheadShape s;
    s.m_cell = std::max(cell, 0.05);
    if (its.indices.empty())
        return s;
    double ext = 0.;
    for (const stl_vertex &v : its.vertices)
        if (std::abs(v.x()) <= MAX_EXTENT && std::abs(v.y()) <= MAX_EXTENT)
            ext = std::max({ext, double(std::abs(v.x())), double(std::abs(v.y()))});
    s.m_half    = int(std::ceil(ext / s.m_cell)) + 1;
    const int n = 2 * s.m_half + 1;
    s.m_cells.assign(size_t(n) * n, INF);
    auto put = [&](int ix, int iy, double z) {
        if (std::abs(ix) > s.m_half || std::abs(iy) > s.m_half)
            return;
        double &c = s.m_cells[size_t(iy + s.m_half) * n + (ix + s.m_half)];
        c         = std::min(c, std::max(0., z));
    };
    for (const stl_triangle_vertex_indices &t : its.indices) {
        const Vec3d a = its.vertices[t[0]].cast<double>(), b = its.vertices[t[1]].cast<double>(), c = its.vertices[t[2]].cast<double>();
        const bool far_a = std::abs(a.x()) > MAX_EXTENT || std::abs(a.y()) > MAX_EXTENT;
        const bool far_b = std::abs(b.x()) > MAX_EXTENT || std::abs(b.y()) > MAX_EXTENT;
        const bool far_c = std::abs(c.x()) > MAX_EXTENT || std::abs(c.y()) > MAX_EXTENT;
        if (far_a || far_b || far_c)
            s.m_far = std::min(s.m_far, std::max(0., std::min({a.z(), b.z(), c.z()})));
        // Vertices count for their own cells (catches thin and vertical faces).
        for (const Vec3d *v : {&a, &b, &c})
            put(int(std::lround(v->x() / s.m_cell)), int(std::lround(v->y() / s.m_cell)), v->z());
        // Cell centres inside the triangle's XY projection get the interpolated Z.
        const double d = (b.x() - a.x()) * (c.y() - a.y()) - (c.x() - a.x()) * (b.y() - a.y());
        const int x0 = int(std::floor(std::min({a.x(), b.x(), c.x()}) / s.m_cell)), x1 = int(std::ceil(std::max({a.x(), b.x(), c.x()}) / s.m_cell));
        const int y0 = int(std::floor(std::min({a.y(), b.y(), c.y()}) / s.m_cell)), y1 = int(std::ceil(std::max({a.y(), b.y(), c.y()}) / s.m_cell));
        if (std::abs(d) < 1e-12) {
            // Vertical face: its lowest edge points along the projected line.
            const int    steps = std::max(1, int(std::ceil(std::max(x1 - x0, y1 - y0))));
            for (const auto &e : {std::make_pair(a, b), std::make_pair(b, c), std::make_pair(c, a)})
                for (int k = 0; k <= steps; ++k) {
                    const Vec3d p = e.first + (e.second - e.first) * (double(k) / steps);
                    put(int(std::lround(p.x() / s.m_cell)), int(std::lround(p.y() / s.m_cell)), p.z());
                }
            continue;
        }
        for (int iy = std::max(y0, -s.m_half); iy <= std::min(y1, s.m_half); ++iy)
            for (int ix = std::max(x0, -s.m_half); ix <= std::min(x1, s.m_half); ++ix) {
                const double px = ix * s.m_cell, py = iy * s.m_cell;
                const double w1 = ((b.x() - px) * (c.y() - py) - (c.x() - px) * (b.y() - py)) / d;
                const double w2 = ((c.x() - px) * (a.y() - py) - (a.x() - px) * (c.y() - py)) / d;
                const double w3 = 1. - w1 - w2;
                if (w1 < -1e-9 || w2 < -1e-9 || w3 < -1e-9)
                    continue;
                put(ix, iy, w1 * a.z() + w2 * b.z() + w3 * c.z());
            }
    }
    s.finish();
    return s;
}

ToolheadShape ToolheadShape::from_mesh_file(const std::string &path, double cell, std::string *err)
{
    TriangleMesh mesh;
    bool         ok = false;
    std::string  msg;
    try {
        if (boost::iends_with(path, ".stl")) {
            ok = mesh.ReadSTLFile(path.c_str(), false);
        } else if (boost::iends_with(path, ".obj")) {
            ObjInfo info;
            ok = load_obj(path.c_str(), &mesh, info, msg);
        } else {
            msg = "toolhead clearance mesh must be .stl or .obj";
        }
    } catch (const std::exception &e) {
        msg = e.what();
        ok  = false;
    }
    if (!ok || mesh.empty()) {
        if (err)
            *err = path + ": " + (msg.empty() ? std::string("could not read the mesh") : msg);
        return {};
    }
    return from_mesh(mesh.its, cell);
}

ToolheadShape ToolheadShape::from_config(const PrintConfig &cfg, std::string *err)
{
    if (!cfg.toolhead_clearance_enable.value)
        return {};
    ToolheadShape            prof, mesh;
    std::vector<ProfileStep> steps;
    for (const Vec2d &p : cfg.toolhead_clearance_profile.values)
        if (p.x() > 0.)
            steps.push_back({p.x(), p.y()});
    if (!steps.empty())
        prof = from_profile(steps);
    if (!cfg.toolhead_clearance_mesh.value.empty())
        mesh = from_mesh_file(cfg.toolhead_clearance_mesh.value, 0.25, err);
    if (prof.empty())
        return mesh;
    if (mesh.empty())
        return prof;
    // Lower of the two at every point, on the larger grid.
    ToolheadShape &big = prof.m_half >= mesh.m_half ? prof : mesh;
    ToolheadShape &sml = prof.m_half >= mesh.m_half ? mesh : prof;
    const int      n   = 2 * big.m_half + 1;
    for (int iy = -big.m_half; iy <= big.m_half; ++iy)
        for (int ix = -big.m_half; ix <= big.m_half; ++ix) {
            double &c = big.m_cells[size_t(iy + big.m_half) * n + (ix + big.m_half)];
            c         = std::min(c, sml.height_at(ix * big.m_cell, iy * big.m_cell));
        }
    big.m_far = std::min(big.m_far, sml.m_far);
    big.finish();
    return std::move(big);
}

void ToolheadShape::finish()
{
    const int n = 2 * m_half + 1;
    m_sorted.clear();
    for (int iy = -m_half; iy <= m_half; ++iy)
        for (int ix = -m_half; ix <= m_half; ++ix) {
            const double h = m_cells[size_t(iy + m_half) * n + (ix + m_half)];
            if (h < INF)
                m_sorted.push_back({ix, iy, h});
        }
    std::stable_sort(m_sorted.begin(), m_sorted.end(), [](const Cell &a, const Cell &b) { return a.h < b.h; });
    // Min filter over a printed-volume tile (plus a cell of rounding), padded so that a
    // tile partly overlapping the grid still sees the cells it overlaps.
    m_box_r     = PrintedVolume::TILE / 2 + 1;
    m_box_half  = m_half + m_box_r;
    const int nb = 2 * m_box_half + 1;
    std::vector<double> rows(size_t(nb) * nb, INF);
    // pass 1: along x, padded output
    for (int iy = -m_box_half; iy <= m_box_half; ++iy)
        for (int ix = -m_box_half; ix <= m_box_half; ++ix) {
            if (std::abs(iy) > m_half)
                continue;
            double v = INF;
            for (int k = std::max(-m_half, ix - m_box_r); k <= std::min(m_half, ix + m_box_r); ++k)
                v = std::min(v, m_cells[size_t(iy + m_half) * n + (k + m_half)]);
            rows[size_t(iy + m_box_half) * nb + (ix + m_box_half)] = v;
        }
    m_box.assign(size_t(nb) * nb, INF);
    for (int iy = -m_box_half; iy <= m_box_half; ++iy)
        for (int ix = -m_box_half; ix <= m_box_half; ++ix) {
            double v = INF;
            for (int k = std::max(-m_box_half, iy - m_box_r); k <= std::min(m_box_half, iy + m_box_r); ++k)
                v = std::min(v, rows[size_t(k + m_box_half) * nb + (ix + m_box_half)]);
            m_box[size_t(iy + m_box_half) * nb + (ix + m_box_half)] = v;
        }
}

double ToolheadShape::height_at(double dx, double dy) const
{
    if (m_cells.empty())
        return m_far;
    const long ix = std::lround(dx / m_cell), iy = std::lround(dy / m_cell);
    if (std::abs(ix) > m_half || std::abs(iy) > m_half)
        return m_far;
    return std::min(m_far, m_cells[size_t(iy + m_half) * (2 * m_half + 1) + (ix + m_half)]);
}

double ToolheadShape::box_min(double dx, double dy) const
{
    if (m_box.empty())
        return m_far;
    const long ix = std::lround(dx / m_cell), iy = std::lround(dy / m_cell);
    if (std::abs(ix) > m_box_half || std::abs(iy) > m_box_half)
        return m_far;
    return std::min(m_far, m_box[size_t(iy + m_box_half) * (2 * m_box_half + 1) + (ix + m_box_half)]);
}

double ToolheadShape::min_height_within(double radius) const
{
    double h = m_far;
    for (const Cell &c : m_sorted) {
        if (c.h >= h)
            break;
        if (std::hypot(c.ix * m_cell, c.iy * m_cell) <= radius)
            return std::min(h, c.h);
    }
    return h;
}

std::vector<ProfileStep> ToolheadShape::radial_profile(double band) const
{
    band = std::max(band, m_cell);
    std::vector<ProfileStep> out;
    const int nb = int(std::ceil(extent() / band)) + 1;
    std::vector<double> low(nb, INF);
    for (const Cell &c : m_sorted) {
        const int b = std::min(nb - 1, int(std::hypot(c.ix * m_cell, c.iy * m_cell) / band));
        low[b]      = std::min(low[b], c.h);
    }
    for (int b = 0; b < nb; ++b) {
        const double h = std::min(low[b], m_far);
        if (h >= INF)
            continue;
        if (!out.empty() && std::abs(out.back().height - h) < 1e-6)
            out.back().radius = (b + 1) * band;
        else
            out.push_back({(b + 1) * band, h});
    }
    if (m_far < INF)
        out.push_back({1000., m_far});
    return out;
}

// ---- PrintedVolume -----------------------------------------------------------------

PrintedVolume::PrintedVolume(const BoundingBoxf &area, double cell) : m_area(area), m_cell(std::max(cell, 0.05))
{
    if (!area.defined)
        return;
    const Vec2d sz = area.size();
    m_nx           = std::max(1, int(std::ceil(sz.x() / m_cell)));
    m_ny           = std::max(1, int(std::ceil(sz.y() / m_cell)));
    m_top.assign(size_t(m_nx) * m_ny, -std::numeric_limits<float>::infinity());
    m_tx = (m_nx + TILE - 1) / TILE;
    m_ty = (m_ny + TILE - 1) / TILE;
    m_tile.assign(size_t(m_tx) * m_ty, -std::numeric_limits<float>::infinity());
}

int PrintedVolume::ix_of(double x) const { return int(std::floor((x - m_area.min.x()) / m_cell)); }
int PrintedVolume::iy_of(double y) const { return int(std::floor((y - m_area.min.y()) / m_cell)); }

void PrintedVolume::raise(int ix, int iy, double z)
{
    if (ix < 0 || iy < 0 || ix >= m_nx || iy >= m_ny)
        return;
    float &c = m_top[size_t(iy) * m_nx + ix];
    if (float(z) > c) {
        c        = float(z);
        float &t = m_tile[size_t(iy / TILE) * m_tx + ix / TILE];
        t        = std::max(t, c);
        m_max    = std::max(m_max, double(c));
    }
}

void PrintedVolume::add_layer(const ExPolygons &slices, double top_z)
{
    if (!valid())
        return;
    // Even-odd scanline fill at cell-centre rows.
    std::vector<Line> edges;
    for (const ExPolygon &ex : slices) {
        Lines l = ex.lines();
        edges.insert(edges.end(), l.begin(), l.end());
    }
    if (edges.empty())
        return;
    BoundingBox bb = get_extents(slices);
    const int   y0 = std::max(0, iy_of(unscale<double>(bb.min.y())) - 1), y1 = std::min(m_ny - 1, iy_of(unscale<double>(bb.max.y())) + 1);
    std::vector<double> xs;
    for (int iy = y0; iy <= y1; ++iy) {
        const double y = m_area.min.y() + (iy + 0.5) * m_cell;
        xs.clear();
        for (const Line &e : edges) {
            const double ay = unscale<double>(e.a.y()), by = unscale<double>(e.b.y());
            if ((ay <= y) == (by <= y))
                continue;
            const double ax = unscale<double>(e.a.x()), bx = unscale<double>(e.b.x());
            xs.push_back(ax + (y - ay) * (bx - ax) / (by - ay));
        }
        std::sort(xs.begin(), xs.end());
        for (size_t k = 0; k + 1 < xs.size(); k += 2) {
            const int x0 = std::max(0, int(std::ceil((xs[k] - m_area.min.x()) / m_cell - 0.5)));
            const int x1 = std::min(m_nx - 1, int(std::floor((xs[k + 1] - m_area.min.x()) / m_cell - 0.5)));
            for (int ix = x0; ix <= x1; ++ix)
                raise(ix, iy, top_z);
        }
    }
}

void PrintedVolume::add_layers_below(const PrintObject &object, double max_print_z, const Point &shift)
{
    add_layers_between(object, -INF, max_print_z, shift);
}

void PrintedVolume::add_layers_between(const PrintObject &object, double min_print_z, double max_print_z, const Point &shift)
{
    for (const Layer *layer : object.layers()) {
        if (layer->print_z <= min_print_z + EPSILON)
            continue;
        if (layer->print_z > max_print_z + EPSILON)
            break;
        ExPolygons s = layer->lslices;
        for (ExPolygon &e : s)
            e.translate(shift);
        add_layer(s, layer->print_z);
    }
    for (const SupportLayer *layer : object.support_layers()) {
        if (layer->print_z <= min_print_z + EPSILON || layer->print_z > max_print_z + EPSILON)
            continue;
        ExPolygons s = layer->support_islands;
        for (ExPolygon &e : s)
            e.translate(shift);
        add_layer(s, layer->print_z);
    }
}

void PrintedVolume::add_segment(const Vec3d &a, const Vec3d &b, double width)
{
    if (!valid())
        return;
    const double r  = std::max(width, m_cell) * 0.5;
    const int    x0 = std::max(0, ix_of(std::min(a.x(), b.x()) - r)), x1 = std::min(m_nx - 1, ix_of(std::max(a.x(), b.x()) + r));
    const int    y0 = std::max(0, iy_of(std::min(a.y(), b.y()) - r)), y1 = std::min(m_ny - 1, iy_of(std::max(a.y(), b.y()) + r));
    const Vec2d  ab = (b - a).head<2>();
    const double l2 = ab.squaredNorm();
    for (int iy = y0; iy <= y1; ++iy)
        for (int ix = x0; ix <= x1; ++ix) {
            const Vec2d  p(m_area.min.x() + (ix + 0.5) * m_cell, m_area.min.y() + (iy + 0.5) * m_cell);
            const double t = l2 > 0. ? std::clamp((p - a.head<2>()).dot(ab) / l2, 0., 1.) : 0.;
            if ((a.head<2>() + ab * t - p).squaredNorm() <= r * r)
                raise(ix, iy, a.z() + (b.z() - a.z()) * t);
        }
}

void PrintedVolume::add_path(const std::vector<Vec3d> &path, double width)
{
    if (path.size() == 1)
        add_segment(path[0], path[0], width);
    for (size_t i = 1; i < path.size(); ++i)
        add_segment(path[i - 1], path[i], width);
}

double PrintedVolume::top_at(double x, double y) const { return top_cell(ix_of(x), iy_of(y)); }

double PrintedVolume::max_top_in(const BoundingBoxf &box) const
{
    if (!valid())
        return -INF;
    if (box.min.x() <= m_area.min.x() && box.min.y() <= m_area.min.y() && box.max.x() >= m_area.max.x() && box.max.y() >= m_area.max.y())
        return m_max;
    const int tx0 = std::max(0, ix_of(box.min.x()) / TILE), tx1 = std::min(m_tx - 1, std::max(-1, ix_of(box.max.x())) / TILE);
    const int ty0 = std::max(0, iy_of(box.min.y()) / TILE), ty1 = std::min(m_ty - 1, std::max(-1, iy_of(box.max.y())) / TILE);
    if (ix_of(box.max.x()) < 0 || iy_of(box.max.y()) < 0)
        return -INF;
    double m = -INF;
    for (int ty = ty0; ty <= ty1; ++ty)
        for (int tx = tx0; tx <= tx1; ++tx)
            m = std::max(m, double(m_tile[size_t(ty) * m_tx + tx]));
    return m;
}

// ---- ClearanceChecker --------------------------------------------------------------

ClearanceChecker::ClearanceChecker(ToolheadShape shape, const BoundingBoxf &plate, Options opt, double cell)
    : m_shape(std::move(shape)), m_printed(plate, cell), m_opt(opt)
{}

double ClearanceChecker::required_z(double x, double y, double floor) const
{
    const double a    = m_opt.allowance;
    const auto   heff = [this](double h) { return std::max(0., h - m_opt.margin); };
    double       best = floor;
    if (!m_printed.valid())
        return best;
    // The far plane (gantry) spans the whole plate.
    if (m_shape.far_height() < INF)
        best = std::max(best, m_printed.max_top() - a - heff(m_shape.far_height()));
    if (m_shape.cells_by_height().empty())
        return best;
    const double ext = m_shape.extent();
    // Quick out: nothing printed in the footprint stands above the floor.
    if (m_printed.max_top_in(BoundingBoxf(Vec2d(x - ext, y - ext), Vec2d(x + ext, y + ext))) - a <= best)
        return best;
    const PrintedVolume &pv   = m_printed;
    const int            T    = PrintedVolume::TILE;
    const double         c    = pv.cell();
    const double         ox   = pv.area().min.x(), oy = pv.area().min.y();
    const int            ix0  = std::max(0, pv.ix_of(x - ext)), ix1 = std::min(pv.nx() - 1, pv.ix_of(x + ext));
    const int            iy0  = std::max(0, pv.iy_of(y - ext)), iy1 = std::min(pv.ny() - 1, pv.iy_of(y + ext));
    if (ix0 > ix1 || iy0 > iy1)
        return best;
    for (int ty = iy0 / T; ty <= iy1 / T; ++ty)
        for (int tx = ix0 / T; tx <= ix1 / T; ++tx) {
            const double tmax = pv.tile_max(tx, ty);
            if (tmax - a <= best)
                continue;
            // Lowest toolhead height over this tile bounds what the tile can require.
            const double tcx = ox + (tx * T + T * 0.5) * c, tcy = oy + (ty * T + T * 0.5) * c;
            const double hmin = m_shape.box_min(tcx - x, tcy - y);
            if (hmin >= INF || tmax - a - heff(hmin) <= best)
                continue;
            for (int iy = std::max(iy0, ty * T); iy <= std::min(iy1, ty * T + T - 1); ++iy)
                for (int ix = std::max(ix0, tx * T); ix <= std::min(ix1, tx * T + T - 1); ++ix) {
                    const double top = pv.top_cell(ix, iy);
                    if (top - a <= best)
                        continue;
                    const double h = m_shape.height_at(ox + (ix + 0.5) * c - x, oy + (iy + 0.5) * c - y);
                    if (h >= INF)
                        continue;
                    best = std::max(best, top - a - heff(h));
                }
        }
    return best;
}

double ClearanceChecker::min_nozzle_z(double x, double y) const { return required_z(x, y, -INF); }

PathResult ClearanceChecker::check_path(const std::vector<Vec3d> &path, double reference_z) const
{
    PathResult r;
    r.min_z.resize(path.size(), -INF);
    r.max_dip.resize(path.size(), INF);
    const double step = m_opt.step > 0. ? m_opt.step : m_printed.cell();
    auto sample = [&](const Vec3d &p, size_t idx) {
        const double req = required_z(p.x(), p.y(), std::min(p.z(), reference_z) - m_opt.depth);
        const double pen = req - p.z();
        if (pen > 1e-6) {
            if (!r.collides)
                r.first_collision = idx;
            r.collides          = true;
            r.worst_penetration = std::max(r.worst_penetration, pen);
        }
        return req;
    };
    for (size_t i = 0; i < path.size(); ++i) {
        const double req = sample(path[i], i);
        r.min_z[i]       = req;
        r.max_dip[i]     = reference_z - req;
        r.max_dip_path   = std::min(r.max_dip_path, r.max_dip[i]);
        if (i + 1 < path.size()) {
            const Vec3d  d = path[i + 1] - path[i];
            const int    n = int(std::floor(d.head<2>().norm() / step));
            for (int k = 1; k < n; ++k) {
                const double q = sample(path[i] + d * (double(k) / n), i);
                r.max_dip_path = std::min(r.max_dip_path, reference_z - q);
            }
        }
    }
    return r;
}

// ---- post-slice G-code check -------------------------------------------------------

std::string GCodeCheckReport::to_json() const
{
    std::ostringstream o;
    o.precision(4);
    o << std::fixed;
    o << "{\"ran\": " << (ran ? "true" : "false");
    if (!error.empty()) {
        std::string e;
        for (char ch : error)
            e += (ch == '"' || ch == '\\') ? std::string("\\") + ch : (ch == '\n' ? std::string(" ") : std::string(1, ch));
        o << ", \"error\": \"" << e << "\"";
    }
    o << ", \"moves\": " << moves_total << ", \"moves_checked\": " << moves_checked << ", \"collisions\": " << collisions;
    if (min_gap < INF)
        o << ", \"min_gap_mm\": " << min_gap << ", \"min_gap_line\": " << min_gap_line;
    else
        o << ", \"min_gap_mm\": null";
    o << ", \"first\": [";
    for (size_t i = 0; i < first.size(); ++i) {
        const GCodeCollision &c = first[i];
        o << (i ? ", " : "") << "{\"line\": " << c.line << ", \"x\": " << c.nozzle.x() << ", \"y\": " << c.nozzle.y() << ", \"z\": " << c.nozzle.z()
          << ", \"penetration_mm\": " << c.penetration << ", \"travel\": " << (c.travel ? "true" : "false") << "}";
    }
    o << "]}";
    return o.str();
}

namespace {
// Value of parameter letter `L` in a G-code line (comment already stripped).
bool param(const char *s, char L, double &out)
{
    for (const char *p = s; *p; ++p) {
        if ((*p == L || *p == L + 32) && (p == s || p[-1] == ' ' || p[-1] == '\t')) {
            char *end = nullptr;
            out       = std::strtod(p + 1, &end);
            if (end != p + 1)
                return true;
        }
    }
    return false;
}
} // namespace

GCodeCheckReport check_gcode(const std::string &gcode_path, const ToolheadShape &shape, const BoundingBoxf &plate, const Options &opt,
                             double default_width, size_t max_report, double cell)
{
    GCodeCheckReport rep;
    if (shape.empty()) {
        rep.error = "no toolhead clearance shape";
        return rep;
    }
    boost::nowide::ifstream in(gcode_path);
    if (!in) {
        rep.error = "cannot open " + gcode_path;
        return rep;
    }
    rep.ran = true;
    ClearanceChecker chk(shape, plate, opt, cell);
    const double     ext       = shape.extent();
    const double     step      = opt.step > 0. ? opt.step : cell;
    const double     gap_floor = 2.0; // gaps above this aren't measured exactly
    Vec3d            pos(0, 0, 0);
    double           e = 0.;
    bool             abs_xyz = true, abs_e = true;
    double           width = default_width;
    std::string      line;
    size_t           ln = 0;

    auto check_move = [&](const Vec3d &a, const Vec3d &b, bool travel) {
        const PrintedVolume &pv = chk.printed();
        if (!pv.valid() || pv.max_top() == -INF)
            return;
        ++rep.moves_total;
        const double zmin = std::min(a.z(), b.z());
        double       far  = -INF;
        if (shape.far_height() < INF)
            far = pv.max_top() - opt.allowance - std::max(0., shape.far_height() - opt.margin);
        BoundingBoxf box(Vec2d(std::min(a.x(), b.x()) - ext, std::min(a.y(), b.y()) - ext), Vec2d(std::max(a.x(), b.x()) + ext, std::max(a.y(), b.y()) + ext));
        if (pv.max_top_in(box) - opt.allowance <= zmin && far <= zmin)
            return; // planar fast path: nothing printed nearby stands above the nozzle tip
        ++rep.moves_checked;
        const int n     = std::max(1, int(std::ceil((b - a).head<2>().norm() / step)));
        double    worst = INF;
        Vec3d     wp    = a;
        for (int k = 0; k <= n; ++k) {
            const Vec3d  p   = a + (b - a) * (double(k) / n);
            const double req = chk.required_z(p.x(), p.y(), p.z() - gap_floor);
            const double gap = p.z() - req;
            if (gap < worst) {
                worst = gap;
                wp    = p;
            }
        }
        if (worst < rep.min_gap) {
            rep.min_gap      = worst;
            rep.min_gap_line = ln;
        }
        if (worst < -1e-6) {
            ++rep.collisions;
            if (rep.first.size() < max_report)
                rep.first.push_back({ln, wp, -worst, travel});
        }
    };

    auto move_to = [&](const Vec3d &to, double new_e) {
        const bool extrude = new_e > e + 1e-9 && (to - pos).head<2>().squaredNorm() > 1e-12;
        if ((to - pos).squaredNorm() > 1e-12)
            check_move(pos, to, !extrude);
        if (extrude)
            chk.printed().add_segment(pos, to, width);
        pos = to;
        e   = new_e;
    };

    while (std::getline(in, line)) {
        ++ln;
        const size_t sc = line.find(';');
        if (sc != std::string::npos) {
            const char *cm = line.c_str() + sc + 1;
            if (std::strncmp(cm, "WIDTH:", 6) == 0)
                width = std::max(0.05, std::atof(cm + 6));
            line.resize(sc);
        }
        size_t i = line.find_first_not_of(" \t");
        if (i == std::string::npos)
            continue;
        const char *s = line.c_str() + i;
        const char  c = char(std::toupper(s[0]));
        if (c != 'G' && c != 'M')
            continue;
        char      *end = nullptr;
        const long num = std::strtol(s + 1, &end, 10);
        if (end == s + 1 || (*end != ' ' && *end != '\t' && *end != 0))
            continue;
        const char *args = end;
        if (c == 'M') {
            if (num == 82) abs_e = true;
            else if (num == 83) abs_e = false;
            continue;
        }
        double v;
        if (num == 90) { abs_xyz = abs_e = true; continue; }
        if (num == 91) { abs_xyz = abs_e = false; continue; }
        if (num == 92) {
            if (param(args, 'X', v)) pos.x() = v;
            if (param(args, 'Y', v)) pos.y() = v;
            if (param(args, 'Z', v)) pos.z() = v;
            if (param(args, 'E', v)) e = v;
            continue;
        }
        if (num == 28) { // homing: position unknown until the next absolute move; nothing to check
            continue;
        }
        if (num < 0 || num > 3)
            continue;
        Vec3d  to = pos;
        double ne = e;
        if (param(args, 'X', v)) to.x() = abs_xyz ? v : pos.x() + v;
        if (param(args, 'Y', v)) to.y() = abs_xyz ? v : pos.y() + v;
        if (param(args, 'Z', v)) to.z() = abs_xyz ? v : pos.z() + v;
        if (param(args, 'E', v)) ne = abs_e ? v : e + v;
        if (num <= 1) {
            move_to(to, ne);
            continue;
        }
        // Arcs: linearise about the centre (I, J), ~0.5 mm chords.
        double I = 0., J = 0.;
        param(args, 'I', I);
        param(args, 'J', J);
        const Vec2d  ctr  = pos.head<2>() + Vec2d(I, J);
        const double rad  = Vec2d(I, J).norm();
        double       a0   = std::atan2(pos.y() - ctr.y(), pos.x() - ctr.x());
        double       a1   = std::atan2(to.y() - ctr.y(), to.x() - ctr.x());
        const bool   cw   = num == 2;
        double       span = cw ? a0 - a1 : a1 - a0;
        while (span <= 1e-9) span += 2. * M_PI;
        const int    n  = std::max(1, int(std::ceil(span * rad / 0.5)));
        const Vec3d  p0 = pos;
        const double e0 = e;
        for (int k = 1; k <= n; ++k) {
            const double t  = double(k) / n;
            const double an = a0 + (cw ? -span : span) * t;
            Vec3d        p(ctr.x() + rad * std::cos(an), ctr.y() + rad * std::sin(an), p0.z() + (to.z() - p0.z()) * t);
            if (k == n)
                p = to;
            move_to(p, e0 + (ne - e0) * t);
        }
    }
    return rep;
}

} // namespace Clearance
} // namespace Slic3r
