#ifndef slic3r_BrickLayers_hpp_
#define slic3r_BrickLayers_hpp_

// Orca: brick layers (interlocking walls).
//
// Every second inner wall is raised by half a layer height, so the walls of neighbouring layers
// overlap like bricks in a wall instead of stacking in planes. The idea is the one GeekDetour's
// BrickLayers post-processor popularised; here it runs inside the slicer on the wall paths, so it
// knows the real wall order, the layer heights and the geometry of the layers around each wall.
//
// For a layer L with height h whose top is z (its nominal print Z), a raised wall piece is printed
// with its top at z + h/2. Its bottom is the top of whatever lies under it:
//  * the raised piece of layer L-1 (top at z - h + h(L-1)/2): a full brick, h tall, normal flow;
//  * an ordinary wall of layer L-1 (top at z - h): the first brick, 1.5 h tall, 1.5x flow.
// A candidate wall piece that has no candidate wall above it in layer L+1 (the top of the part, a
// slope, a layer where the wall stops) is not raised, because layer L+1 would print over it at its
// nominal height. If it sits on a raised piece of layer L-1 it is the last brick: printed at the
// nominal z, h/2 tall, 0.5x flow. Pieces are cut wherever these conditions change along a wall.
//
// Heights are taken per layer, so variable layer heights and a first layer of its own height work.

#include "libslic3r.h"
#include "Polyline.hpp"
#include "Polygon.hpp"

#include <utility>
#include <vector>

namespace Slic3r {

class ExtrusionEntityCollection;
class ExtrusionPath;
class PrintObject;

namespace BrickLayers {

struct Params
{
    // 1-based index of the first raised wall counted from the outer wall (1); every second wall
    // inwards from it is raised as well.
    int    first_wall = 2;
    // Extra flow multiplier applied to raised and end-of-run pieces.
    double flow_ratio = 1.;
};

// Is the wall with this inset index (0 = outer wall) one of the raised ones?
bool is_raised_wall(int inset_idx, int first_wall);

// One object layer as the brick pass sees it.
struct LayerInput
{
    // Layer thickness in mm.
    double height = 0.;
    // False for layers that must not have raised walls: below the start layer, wave-overhang
    // layers and their floor layers.
    bool   enabled = false;
    // Wall collections of the regions that have brick layers on, with their parameters.
    std::vector<std::pair<ExtrusionEntityCollection*, Params>> perimeters;
};

// Interval [first, second] of arc length (scaled units) along a polyline.
using Interval  = std::pair<double, double>;
using Intervals = std::vector<Interval>;

// The parts of `polyline` that lie inside `region`, as arc-length intervals in path order.
// Runs (inside or outside) shorter than `min_run` are absorbed into their neighbours, so a wall
// does not change height for a tiny stretch.
Intervals inside_intervals(const Polyline &polyline, const Polygons &region, double min_run);

// The part of `polyline` between arc lengths `from` and `to`.
Polyline sub_polyline(const Polyline &polyline, double from, double to);

// The flow the G-code writer emits for `path`, including the (height + z) / height factor it
// applies to Z-contoured points (GCode::_extrude_piece). Raised pieces are stored pre-divided by it.
double emitted_mm3_per_mm(const ExtrusionPath &path);

// Raise the walls of `layers` (bottom to top, one object) in place. Returns the number of path
// pieces that were raised.
size_t apply(std::vector<LayerInput> &layers);

// Build the layer inputs of `object` and apply. Called at the end of PrintObject::make_perimeters().
void apply(PrintObject &object);

} // namespace BrickLayers
} // namespace Slic3r

#endif // slic3r_BrickLayers_hpp_
