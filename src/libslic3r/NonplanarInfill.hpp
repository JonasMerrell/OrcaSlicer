#ifndef slic3r_NonplanarInfill_hpp_
#define slic3r_NonplanarInfill_hpp_

// Orca: non-planar (wavy in Z) sparse infill.
//
// Sparse infill lines are bent up and down along a smooth wave field z = A * t(x, y) * w(x, y, layer), defined over the
// PrintObject's XY frame, so all instances and all paths of a layer see the same wave. The idea follows Roman Tenger's
// NonPlanarInfill post-processing script (GPL-3.0, https://github.com/TengerTechnologies/NonPlanarInfill); this is an
// independent implementation inside the slicer that also tapers the wave locally next to anything that is not sparse
// infill and compensates the flow for the local gap to the layer below.
//
// t(x, y) is the taper. It is 0 where the point is not sparse infill on the layer directly above or below (so the
// solid, top and bottom layers stay flat and are printed on a flat surface) and grows to 1 over `blend_layers` layers
// vertically and `edge_blend` mm laterally from the edge of the sparse infill area. The offsets are carried by the
// existing Z contouring support of ExtrusionPath (Point3::z() is the offset from the layer's print_z, and
// path.z_contoured is set).

#include "ExPolygon.hpp"
#include "Point.hpp"
#include "AABBTreeLines.hpp"
#include "BoundingBox.hpp"

#include <memory>
#include <vector>

namespace Slic3r {

class ExtrusionPath;
class ExtrusionEntityCollection;
class PrintRegionConfig;

// The wave itself, without the taper.
struct NonplanarInfillWave
{
    // Crest height in mm, already limited to the layer height and the maximum Z offset.
    double amplitude       = 0.;
    double wavelength      = 10.;
    // Radians the wave moves on each layer.
    double phase_per_layer = 0.;
    // Direction of the wave in radians from the X axis.
    double angle           = 0.;
    bool   egg_crate       = false;

    // The wave at (x, y) in mm on layer `layer_id`, in [-1, 1].
    double unit(double x, double y, size_t layer_id) const;
    // Built from a region's config for a layer of the given height.
    static NonplanarInfillWave from_config(const PrintRegionConfig &config, double layer_height);
};

// The amplitude that the config asks for at the given layer height, after the limits: never more than one layer height
// or the maximum Z offset.
double nonplanar_infill_amplitude(double amplitude_abs, double layer_height, double max_dz);

// The flow factor of a point of an infill line printed with the nozzle `dz` above the layer's plane onto material whose
// top is `dz_below` above the plane of the layer below: the local gap over the nominal layer height, limited to
// [0.25, 2] so a bad offset never starves or floods the line.
double nonplanar_infill_extrusion_ratio(double height, double dz, double dz_below);

// Per layer and region: where the wave may go, and how far it is tapered.
class NonplanarInfillField
{
public:
    // `stack` holds the sparse infill area of the layers layer_id - blend_layers ... layer_id + blend_layers, the layer
    // itself in the middle; a missing layer (below the first or above the last) is nullptr or empty.
    NonplanarInfillField(const NonplanarInfillWave &wave, size_t layer_id, const std::vector<const ExPolygons *> &stack,
                         int blend_layers, double edge_blend);

    // 0 (flat) .. 1 (full wave) at a point of the PrintObject's frame.
    double taper(const Point &p) const;
    // The Z offset in mm at a point.
    double dz(const Point &p) const;
    bool   empty() const { return m_levels.empty() || m_wave.amplitude <= 0.; }

    const NonplanarInfillWave &wave() const { return m_wave; }

private:
    struct Level
    {
        // Sparse infill on all layers within `k` of this one.
        ExPolygons                       area;
        BoundingBox                      bbox;
        AABBTreeLines::LinesDistancer<Line> distancer;
        double                           weight; // k / blend_layers
    };
    NonplanarInfillWave m_wave;
    size_t              m_layer_id;
    double              m_edge_blend; // mm
    std::vector<Level>  m_levels;
};

// Resample a sparse infill path to `segment_length` and give each point the field's offset. Returns false and leaves the
// path untouched when the whole path stays flat.
bool nonplanar_infill_contour_path(ExtrusionPath &path, const NonplanarInfillField &field, double segment_length);
// The same for every sparse infill path of a collection, recursively. Returns the number of paths contoured.
size_t nonplanar_infill_contour_collection(ExtrusionEntityCollection &collection, const NonplanarInfillField &field,
                                           double segment_length);

// Resampling step along the infill lines for a wave of this length.
double nonplanar_infill_segment_length(double wavelength);

} // namespace Slic3r

#endif // slic3r_NonplanarInfill_hpp_
