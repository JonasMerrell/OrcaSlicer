///|/ Resin-style pin supports under the free edge of wave overhangs.
///|/
///|/ Released under the terms of the AGPLv3 or higher.
///|/
#ifndef slic3r_TreeSupportPins_hpp_
#define slic3r_TreeSupportPins_hpp_

#include <vector>

#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Point.hpp"

namespace Slic3r {

class PrintObject;
class PrintObjectConfig;

// Pin supports are a tree support style for wave overhangs: every branch is a single straight,
// tapered pin that is never merged with its neighbours. A pin stands on the bed or on the model
// below and its tip fuses into the underside of the wave overhang with no Z gap and no interface,
// so it holds the free edge down in tension instead of just propping it up. The pins are support
// extrusions only: they do not change how the overhang above is detected or printed, so it still
// prints as waves (the wave generator only looks at the object's own slices).
namespace PinSupports {

struct Params
{
    double tip_diameter  = 0.8;  // mm, where the pin fuses into the overhang
    double base_diameter = 5.0;  // mm, largest diameter of the cone
    double taper_angle   = 16.;  // degrees, half angle of the cone from the vertical
    double spacing       = 4.0;  // mm, between pins along a row
    double edge_inset    = 2.5;  // mm, outer row's distance in from the edge of the overhang
    int    rows          = 1;
    double row_spacing   = 0.;   // mm, 0 = spacing
    double min_overhang  = 5.0;  // mm, pins only where the overhang reaches this far out from the layer below
    double gap           = 1.0;  // mm, smallest gap between pins and between a pin and the model

    static Params from_config(const PrintObjectConfig &config);
};

// Pin centres for one wave overhang. `overhang` is the part of the layer that hangs over air (the
// wave strip), `below` the layer under it. Rows follow the strip's edge `edge_inset` in (and then a
// row spacing further in per row) wherever the strip reaches at least `min_overhang` out from
// `below`; each row gets a pin at both of its ends and at every sharp corner, and pins at most
// `spacing` apart in between. Pins closer than half a spacing to an earlier one are dropped.
Points place_pins(const ExPolygons &overhang, const ExPolygons &below, const Params &params);

// Largest base radius each pin may have so that no two pins come closer than `gap` (scaled).
std::vector<coord_t> base_radius_caps(const Points &pins, coord_t base_radius, coord_t tip_radius, coord_t gap);

// Radius of a pin `depth` mm below its tip, in mm (base_radius_cap in mm too).
double pin_radius(const Params &params, double depth, double base_radius_cap);

// One pin of a sliced object.
struct Pin
{
    Point    pos;
    size_t   top_layer;    // object layer of the tip: the layer just under the wave layer
    size_t   bottom_layer; // lowest object layer the pin prints on (0 = it stands on the bed)
    coord_t  base_radius_cap;
};

// Plan the pins of a sliced object (after the wall step, which records the wave overhangs) and
// return the area each pin covers on each object layer (indexed by object layer).
std::vector<ExPolygons> plan_object(const PrintObject &object, std::vector<Pin> *pins_out = nullptr);

// Add the pins of `object` to its support layers, making support layers at the object's layer
// heights where there are none. Called from the support step, after any other support.
void generate(PrintObject &object);

} // namespace PinSupports
} // namespace Slic3r

#endif
