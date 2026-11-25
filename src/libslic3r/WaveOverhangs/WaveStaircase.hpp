///|/ Staircase wave overhangs (multi-layer wave bands).
///|/
///|/ A wave overhang is one layer printed in mid air, so a long one is flimsy and its free
///|/ edge curls up while it cools. The staircase prints the overhang region N layers at once
///|/ instead: the wave layer's front leads, the layer above follows a stagger behind it at
///|/ Z+h, the next one at Z+2h, and so on. The band grows outward together, so the free edge
///|/ is held down while it is still warm, and the part being built is N layers thick (a
///|/ three layer cantilever is about 27 times stiffer than one layer).
///|/
///|/ Released under the terms of the AGPLv3 or higher.
///|/
#ifndef slic3r_WaveStaircase_hpp_
#define slic3r_WaveStaircase_hpp_

#include <limits>
#include <vector>

#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Polyline.hpp"

namespace Slic3r {
class PrintObject;
}

namespace Slic3r::WaveOverhangs::Staircase {

// Pattern of the upper steps (layers L+1 .. L+N-1 of the band).
enum class StepPattern { Waves, Rectilinear, Hilbert, Concentric };

struct Params
{
    // Layers printed together, the wave layer included (N). 2 or more.
    int         layers         = 3;
    // Smallest horizontal distance (mm) from the nozzle centre to the edge of material that
    // is higher than the nozzle tip: the tip's flat radius plus a margin.
    double      stagger        = 2.0;
    // Wave rings the front advances per round before the upper steps follow.
    int         rings_per_step = 3;
    // Ring spacing (mm) of the distance field the band advances along (the wave line spacing).
    double      ring_spacing   = 0.35;
    StepPattern pattern        = StepPattern::Waves;
};

// One upper layer L+k (k >= 1) of the band.
struct StepLayer
{
    double   dz     = 0.;   // print_z(L+k) - print_z(L), mm
    Polygons fill;          // fill area of the region on L+k (inside its walls)
    Polygons slices;        // the whole layer L+k (all islands, all regions)
    Flow     flow;          // flow of the step's lines (solid infill of L+k)
    float    angle  = 0.f;  // fill angle (radians) of the line patterns
};

struct BandInput
{
    ExtrusionPaths         waves;      // the wave paths of the island, in print order (step 0)
    Polygons               footprint;  // area the waves fill on layer L (fill_carve), this island
    Polygons               island;     // the island on layer L
    Polygons               anchor;     // layer L-1 under and around the island (supported area)
    Polygons               rest0;      // material of layer L printed after the band (other islands, ironing)
    std::vector<StepLayer> steps;      // k = 1 .. N-1, already capped by the caller
};

struct BandResult
{
    // S_1 .. S_{N-1}: the areas the band prints on L+1 .. L+N-1. The caller removes them from
    // the fill of those layers. Empty vector: no staircase on this island.
    std::vector<Polygons> step_regions;
    // The whole band in print order. Step 0 paths are the wave pieces; step k >= 1 paths are
    // z_contoured with every point at +dz_k and staircase_step = k.
    ExtrusionPaths        paths;
    int                   rounds = 0;
};

// Areas of the upper steps. Each step lies on the step below it (or on layer L, which is
// printed before the band) and keeps `stagger + line width` away from anything printed
// later at a lower height, so the nozzle tip never meets a higher step: when layer L+j is
// printed normally next to a step of L+k (k > j), there is room for the tip.
std::vector<Polygons> step_regions(const BandInput &in, const Params &params);

BandResult build_band(const BandInput &in, const Params &params);

// Post-hoc check of a print sequence: for each path, the smallest horizontal distance from
// its centreline to the edge of material laid earlier that is higher than it. Paths carry
// their height as the z of their points (z_contoured) or 0.
struct ClearanceReport
{
    double min_clearance = std::numeric_limits<double>::infinity(); // mm, +inf: never next to higher material
    size_t violations    = 0;                                         // samples closer than `stagger - tolerance`
};
ClearanceReport check_sequence(const ExtrusionPaths &sequence, double stagger, double tolerance = 0.05);

// The slicing pass (PrintObject::prepare_infill, after the wave floor passes): for every wave
// layer of a region with wave_overhang_staircase on, compute the steps, remove them from the
// fill of the layers above and replace the island's wave paths with one band collection that
// the G-code writer prints after the island's infill. Re-running it first undoes the previous
// run, so prepare_infill can run again without regenerating the perimeters.
void apply(PrintObject &object);

// True for the band collection apply() adds to LayerRegion::perimeters.
bool is_band(const ExtrusionEntity *entity);

} // namespace Slic3r::WaveOverhangs::Staircase

#endif
