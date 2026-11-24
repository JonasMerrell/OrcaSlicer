#include "NonplanarInfill.hpp"

#include "ClipperUtils.hpp"
#include "ExtrusionEntity.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "Geometry.hpp"
#include "PrintConfig.hpp"

#include <algorithm>
#include <cmath>

namespace Slic3r {

double NonplanarInfillWave::unit(double x, double y, size_t layer_id) const
{
    const double k     = 2. * M_PI / std::max(wavelength, EPSILON);
    const double phase = phase_per_layer * double(layer_id);
    const double c = std::cos(angle), s = std::sin(angle);
    const double u = x * c + y * s;
    if (! egg_crate)
        return std::sin(k * u + phase);
    const double v = -x * s + y * c;
    return 0.5 * (std::sin(k * u + phase) + std::sin(k * v + phase));
}

double nonplanar_infill_amplitude(double amplitude_abs, double layer_height, double max_dz)
{
    return std::max(0., std::min({ amplitude_abs, layer_height, max_dz }));
}

NonplanarInfillWave NonplanarInfillWave::from_config(const PrintRegionConfig &config, double layer_height)
{
    NonplanarInfillWave wave;
    wave.amplitude       = nonplanar_infill_amplitude(config.nonplanar_infill_amplitude.get_abs_value(layer_height), layer_height,
                                                      config.nonplanar_infill_max_dz.value);
    wave.wavelength      = std::max(1., config.nonplanar_infill_wavelength.value);
    wave.phase_per_layer = Geometry::deg2rad(config.nonplanar_infill_phase_shift.value);
    wave.angle           = Geometry::deg2rad(config.nonplanar_infill_angle.value);
    wave.egg_crate       = config.nonplanar_infill_pattern.value == NonplanarInfillPattern::EggCrate;
    return wave;
}

double nonplanar_infill_extrusion_ratio(double height, double dz, double dz_below)
{
    if (height <= EPSILON)
        return 1.;
    return std::clamp((height + dz - dz_below) / height, 0.25, 2.);
}

double nonplanar_infill_segment_length(double wavelength)
{
    // 16 points per wave keep the chord within 2% of the amplitude, and no point is closer than 0.25 mm so the
    // G-code stays reasonably small.
    return std::clamp(wavelength / 16., 0.25, 1.);
}

NonplanarInfillField::NonplanarInfillField(const NonplanarInfillWave &wave, size_t layer_id,
                                           const std::vector<const ExPolygons *> &stack, int blend_layers, double edge_blend)
    : m_wave(wave), m_layer_id(layer_id), m_edge_blend(std::max(0., edge_blend))
{
    blend_layers = std::max(1, blend_layers);
    const int mid = int(stack.size()) / 2;
    if (m_wave.amplitude <= 0. || stack.empty() || stack[mid] == nullptr || stack[mid]->empty())
        return;
    // Level k: sparse infill on every layer within k of this one. The levels nest, each inside the one before.
    ExPolygons area = *stack[mid];
    for (int k = 1; k <= blend_layers; ++k) {
        for (int j : { mid - k, mid + k }) {
            if (j < 0 || j >= int(stack.size()) || stack[j] == nullptr || stack[j]->empty()) {
                area.clear();
                break;
            }
            area = intersection_ex(area, *stack[j]);
            if (area.empty())
                break;
        }
        if (area.empty())
            break;
        Level level;
        level.area      = area;
        level.bbox      = get_extents(area);
        level.distancer = AABBTreeLines::LinesDistancer<Line>(to_lines(area));
        level.weight    = double(k) / double(blend_layers);
        m_levels.emplace_back(std::move(level));
    }
}

double NonplanarInfillField::taper(const Point &p) const
{
    double t = 0.;
    // The levels nest, and a deeper one weighs more: take the best one the point is inside of, tapered by its distance
    // from that level's edge.
    for (auto it = m_levels.rbegin(); it != m_levels.rend(); ++it) {
        const Level &level = *it;
        if (level.weight <= t)
            break;
        if (! level.bbox.contains(p))
            continue;
        double lateral;
        if (m_edge_blend <= EPSILON) {
            lateral = level.distancer.outside(p) < 0 ? 1. : 0.;
        } else {
            // Negative inside.
            const double d = -level.distancer.distance_from_lines<true>(p);
            lateral        = std::clamp(unscale<double>(d) / m_edge_blend, 0., 1.);
        }
        t = std::max(t, level.weight * lateral);
    }
    // Smoothstep, so the wave starts and ends without a kink.
    return t * t * (3. - 2. * t);
}

double NonplanarInfillField::dz(const Point &p) const
{
    if (this->empty())
        return 0.;
    const double t = this->taper(p);
    if (t <= 0.)
        return 0.;
    return m_wave.amplitude * t * m_wave.unit(unscale<double>(p.x()), unscale<double>(p.y()), m_layer_id);
}

bool nonplanar_infill_contour_path(ExtrusionPath &path, const NonplanarInfillField &field, double segment_length)
{
    const Points3 &points = path.polyline.points;
    if (points.size() < 2 || field.empty())
        return false;
    segment_length = std::max(segment_length, 0.05);
    Points3 out;
    out.reserve(points.size() * 4);
    bool   any = false;
    auto   add = [&](const Vec2d &p) {
        const Point  pt(scale_(p.x()), scale_(p.y()));
        const double dz = field.dz(pt);
        if (std::abs(dz) > EPSILON)
            any = true;
        out.emplace_back(pt.x(), pt.y(), coord_t(scale_(dz)));
    };
    add(unscale(points.front().to_point()));
    for (size_t i = 1; i < points.size(); ++i) {
        const Vec2d a = unscale(points[i - 1].to_point());
        const Vec2d b = unscale(points[i].to_point());
        const double len = (b - a).norm();
        const int    n   = std::max(1, int(std::ceil(len / segment_length)));
        for (int s = 1; s <= n; ++s)
            add(a + (b - a) * (double(s) / double(n)));
    }
    if (! any)
        return false;
    path.polyline.points = std::move(out);
    path.polyline.fitting_result.clear();
    path.z_contoured = true;
    return true;
}

static size_t contour_entity(ExtrusionEntity *entity, const NonplanarInfillField &field, double segment_length)
{
    if (auto *path = dynamic_cast<ExtrusionPath *>(entity)) {
        // A sloped path carries its own Z.
        if (dynamic_cast<ExtrusionPathSloped *>(path) != nullptr || path->role() != erInternalInfill || path->z_contoured)
            return 0;
        return nonplanar_infill_contour_path(*path, field, segment_length) ? 1 : 0;
    }
    size_t count = 0;
    if (auto *multipath = dynamic_cast<ExtrusionMultiPath *>(entity)) {
        for (ExtrusionPath &path : multipath->paths)
            count += contour_entity(&path, field, segment_length);
    } else if (auto *loop = dynamic_cast<ExtrusionLoop *>(entity)) {
        if (dynamic_cast<ExtrusionLoopSloped *>(loop) == nullptr)
            for (ExtrusionPath &path : loop->paths)
                count += contour_entity(&path, field, segment_length);
    } else if (auto *collection = dynamic_cast<ExtrusionEntityCollection *>(entity)) {
        count += nonplanar_infill_contour_collection(*collection, field, segment_length);
    }
    return count;
}

size_t nonplanar_infill_contour_collection(ExtrusionEntityCollection &collection, const NonplanarInfillField &field,
                                           double segment_length)
{
    size_t count = 0;
    for (ExtrusionEntity *entity : collection.entities)
        count += contour_entity(entity, field, segment_length);
    return count;
}

} // namespace Slic3r
