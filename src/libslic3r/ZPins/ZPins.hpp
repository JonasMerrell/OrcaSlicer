// Z-pinning: vertical channels left open through several layers and filled with molten
// filament from above, so the pin fuses across the layer lines and the part gets much of its
// XY strength in Z as well.
//
// After Duty et al., "Z-pinning approach for 3D printing mechanically isotropic materials",
// Additive Manufacturing 27 (2019) 175-184: channels of about 3x the bead width, filled every
// few layers by holding the nozzle over the hole (no dip), alternate pins filled half an
// interval apart so the seams between pin segments are staggered.
//
// The work is split in two:
//  * plan_z_pin_columns() is pure: which pin is open on which layer, and when each one is filled.
//  * PrintObject::generate_z_pins() (ZPins.cpp) turns the plan into geometry after the walls are
//    generated: it cuts the channels out of the infill, clips the inner walls they cross, adds a
//    one-line wall around each channel and stores the injections on the layers for GCode.cpp.
#ifndef slic3r_ZPins_hpp_
#define slic3r_ZPins_hpp_

#include "../Point.hpp"
#include "../BoundingBox.hpp"

#include <cstdint>
#include <vector>

namespace Slic3r {

class ExPolygon;
using ExPolygons = std::vector<ExPolygon>;
class ExtrusionEntityCollection;

// One injection: after the layer that owns it is printed, park over `pos` and extrude `volume`.
struct ZPinInjection
{
    Point  pos;            // Object coordinates (scaled), like the layer's slices.
    double depth  { 0. };  // Depth of the open channel being filled (mm).
    double volume { 0. };  // Filament volume to inject, overfill included (mm^3).
};

struct ZPinColumnParams
{
    // Fill a pin every `interval` layers (and at the top of every pin).
    int  interval        { 10 };
    // Layers that must stay eligible above / below a layer for a pin to be open there. They keep
    // channels away from top and bottom surfaces (and, with the shell layers added, out of the shells).
    int  top_window      { 1 };
    int  bottom_window   { 1 };
    // Pins shorter than this are dropped, as are injections that would fill fewer layers.
    int  min_layers      { 2 };
};

struct ZPinEvent
{
    uint32_t pin;
    int      first_layer;  // Lowest layer of the channel part filled by this event.
};

struct ZPinPlan
{
    // [layer] -> pins whose channel is open (cut) on that layer.
    std::vector<std::vector<uint32_t>>  open;
    // [layer] -> pins filled right after that layer is printed.
    std::vector<std::vector<ZPinEvent>> fills;
};

// eligible[layer][pin]: the channel (with its wall) fits inside the allowed area of that layer.
// phase[pin]: offset of that pin's fill schedule in layers (0 or interval/2 for alternating sets).
ZPinPlan plan_z_pin_columns(const std::vector<std::vector<char>> &eligible, const std::vector<int> &phase, const ZPinColumnParams &params);

// Pin positions: a square grid, or with `staggered` rows offset by half the spacing and packed
// at spacing*sqrt(3)/2 (hexagonal). The grid is centred on `bbox` so that a symmetric part gets
// symmetric pins. `phase_set` gets 0/1 per pin so neighbours alternate (checkerboard / rows).
Points z_pin_grid(const BoundingBox &bbox, coord_t spacing, bool staggered, std::vector<int> *phase_set = nullptr);

// Circle approximation used for the channel cut, the channel wall and the injection path.
Polygon z_pin_circle(const Point &center, coord_t radius, double max_chord_error = 0.005);

// Remove the parts of every extrusion with an inner role (inner walls, gap fill) that run
// through `cut`. Outer walls are never touched. Returns true if anything was clipped.
bool z_pin_clip_inner_extrusions(ExtrusionEntityCollection &collection, const ExPolygons &cut, coord_t min_piece_length);

} // namespace Slic3r

#endif // slic3r_ZPins_hpp_
