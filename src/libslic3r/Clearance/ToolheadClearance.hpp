///|/ Toolhead clearance for non-planar printing.
///|/
///|/ A planar slicer never lowers the nozzle below the layer it is printing, so the
///|/ toolhead (heater block, duct, probe, gantry) can't touch the part. Non-planar
///|/ features (Z anti-aliasing of top surfaces, non-planar infill, brick layers) do
///|/ move the nozzle down or sideways next to taller, already printed material. This
///|/ module answers "how low may the nozzle go here?" and "does this path hit the part?".
///|/
///|/ Three pieces:
///|/  - ToolheadShape : the toolhead's underside in the nozzle frame (a stepped cone from
///|/                    printer settings, or a height field rasterised from a mesh).
///|/  - PrintedVolume : what is already printed, as a height field over the bed
///|/                    (layers below plus earlier paths on the current layer).
///|/  - ClearanceChecker : the queries feature code calls (min safe Z, max dip, path check).
///|/  - check_gcode()  : a post-slice scan of finished G-code against the volume it builds.
///|/
///|/ Conventions
///|/  - Units are mm, unscaled, in plate (bed) coordinates, the same XY as the G-code.
///|/  - The nozzle frame has its origin at the nozzle tip and printer X/Y/Z axes.
///|/  - Printed tops are stored as the nozzle Z that laid the material. A bead is assumed
///|/    to stand `allowance` below that, so a nozzle on its own layer never collides.
///|/  - Everything is off unless toolhead_clearance_enable is set. Planar prints never
///|/    collide by construction (all printed tops <= nozzle Z, toolhead heights >= 0).
///|/
///|/ Printer settings (PrintConfig, machine tab, all off by default):
///|/  - toolhead_clearance_enable  (bool)   turns the API and the post-slice check on.
///|/  - toolhead_clearance_profile (points) stepped cone as "radius x height" pairs (mm),
///|/                                       e.g. "0.5x0,4x1.6,13x1.13,45x1.13,1000x30".
///|/  - toolhead_clearance_mesh    (string) optional STL/OBJ of the toolhead in the nozzle frame.
///|/  - toolhead_clearance_margin  (float)  safety gap, default 0.2 mm.
///|/
///|/ Typical use from a feature (e.g. non-planar infill on one layer):
///|/
///|/     auto shape = Clearance::ToolheadShape::from_config(print.config());
///|/     if (!shape.empty()) {
///|/         Clearance::ClearanceChecker chk(std::move(shape), plate_bbox);
///|/         chk.printed().add_layers_below(object, layer->print_z, instance_shift);
///|/         for (path : planned_paths) {
///|/             auto r = chk.check_path(path, layer->print_z);   // per-point min safe Z
///|/             // clamp: z_i = std::max(z_i, r.min_z[i]); or drop the path if r.collides
///|/             chk.printed().add_path(final_path, line_width);   // same-layer paths count too
///|/         }
///|/     }
///|/
///|/ Keep this header stable: the Z anti-aliasing, non-planar infill and brick layers
///|/ branches code against it. Add, don't change.
#ifndef slic3r_ToolheadClearance_hpp_
#define slic3r_ToolheadClearance_hpp_

#include "../libslic3r.h"
#include "../Point.hpp"
#include "../BoundingBox.hpp"
#include "../ExPolygon.hpp"

#include <admesh/stl.h>

#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace Slic3r {

class PrintConfig;
class PrintObject;

namespace Clearance {

constexpr double INF = std::numeric_limits<double>::infinity();

// One step of a stepped-cone profile: for every point whose horizontal distance from the
// nozzle axis is below `radius` (and at or beyond the previous step's radius), the
// toolhead's lowest point is `height` mm above the nozzle tip. Beyond the last step's
// radius nothing hangs down (height +inf); add a last step with a large radius to model
// a gantry or X beam that spans the whole bed.
struct ProfileStep
{
    double radius = 0.;
    double height = 0.;
};

// The underside of the toolhead in the nozzle frame, as heights above the nozzle tip.
class ToolheadShape
{
public:
    ToolheadShape() = default;

    // Stepped cone. Steps are sorted by radius; heights need not be monotonic.
    static ToolheadShape from_profile(std::vector<ProfileStep> steps, double cell = 0.25);
    // Rasterise a mesh given in the nozzle frame (tip at the origin, printer axes): for each
    // XY cell, the lowest Z of the mesh over that cell (+inf where the mesh doesn't reach).
    // Parts below the tip (z < 0, e.g. a probe's sensing volume) are clamped to 0.
    static ToolheadShape from_mesh(const indexed_triangle_set &its, double cell = 0.25);
    // STL / OBJ / 3MF file through the usual loaders. Empty shape and *err set on failure.
    static ToolheadShape from_mesh_file(const std::string &path, double cell = 0.25, std::string *err = nullptr);
    // From the printer settings toolhead_clearance_*: empty when disabled or unset.
    // When both a mesh and a profile are given, the lower of the two wins at each point.
    static ToolheadShape from_config(const PrintConfig &cfg, std::string *err = nullptr);

    bool empty() const { return m_cells.empty() && m_far >= INF; }

    // Toolhead underside height above the nozzle tip at offset (dx, dy) from the nozzle
    // axis. +inf where nothing of the toolhead is over that point.
    double height_at(double dx, double dy) const;
    // Height the toolhead keeps everywhere outside its rasterised extent (gantry), +inf if none.
    double far_height() const { return m_far; }
    // Radius (from the nozzle axis) of the rasterised extent.
    double extent() const { return m_half * m_cell; }
    double cell() const { return m_cell; }

    // A conservative stepped-cone summary: for each radius band the lowest height found
    // anywhere in that band. Bands are `band` mm wide; consecutive equal heights merge.
    std::vector<ProfileStep> radial_profile(double band = 0.5) const;

    // Lowest toolhead height within `radius` of the nozzle axis.
    double min_height_within(double radius) const;

    // Cells in the nozzle frame, sorted by height (ascending), for the checker.
    struct Cell { int ix, iy; double h; };
    const std::vector<Cell> &cells_by_height() const { return m_sorted; }
    // Lowest height over the square of half-size TILE/2+1 cells around (dx, dy): a lower
    // bound for a whole printed-volume tile centred there.
    double box_min(double dx, double dy) const;

private:
    void finish();
    double m_cell = 0.25;
    int    m_half = 0;               // grid is (2*m_half+1)^2, centred on the tip
    std::vector<double> m_cells;     // row-major, +inf = empty
    std::vector<Cell>   m_sorted;    // finite cells by height
    double m_far = INF;
    int    m_box_r = 0, m_box_half = 0;
    std::vector<double> m_box;       // min-filtered m_cells, padded by m_box_r
};

// Height field of the printed part over the plate: for each cell, the highest nozzle Z
// that deposited material there (-inf where nothing is printed yet). Material is treated
// as solid from the bed up to that top, which is conservative for overhangs.
class PrintedVolume
{
public:
    PrintedVolume() = default;
    PrintedVolume(const BoundingBoxf &area, double cell = 0.25);

    bool   valid() const { return !m_top.empty(); }
    double cell() const { return m_cell; }
    const BoundingBoxf &area() const { return m_area; }

    // A whole layer's footprint (scaled coordinates, e.g. Layer::lslices shifted to the
    // plate by the instance offset) at nozzle height top_z.
    void add_layer(const ExPolygons &slices, double top_z);
    // Every layer of `object` whose print_z <= max_print_z (all of them if INF), lslices
    // shifted by `shift` (scaled, an instance's shift on the plate).
    void add_layers_below(const PrintObject &object, double max_print_z, const Point &shift);
    // Layers (and support layers) with min_print_z < print_z <= max_print_z. Lets a
    // feature build its volume incrementally, or one band at a time.
    void add_layers_between(const PrintObject &object, double min_print_z, double max_print_z, const Point &shift);
    // An extrusion path (nozzle positions, mm) laying a bead `width` wide. Z may vary.
    void add_path(const std::vector<Vec3d> &path, double width);
    void add_segment(const Vec3d &a, const Vec3d &b, double width);

    // Highest printed top at a plate point, -inf if none / outside the area.
    double top_at(double x, double y) const;
    double max_top() const { return m_max; }
    // Highest printed top over an axis-aligned box (fast, may round the box out to tiles).
    double max_top_in(const BoundingBoxf &box) const;

    // Raw cell access for the checker.
    int  nx() const { return m_nx; }
    int  ny() const { return m_ny; }
    double top_cell(int ix, int iy) const { return (ix < 0 || iy < 0 || ix >= m_nx || iy >= m_ny) ? -INF : m_top[size_t(iy) * m_nx + ix]; }
    // Cell index of a plate coordinate (may be outside the grid).
    int  ix_of(double x) const;
    int  iy_of(double y) const;

    // Tiles of TILE x TILE cells keep their maximum, to skip low regions quickly.
    static constexpr int TILE = 16;
    double tile_max(int tx, int ty) const { return (tx < 0 || ty < 0 || tx >= m_tx || ty >= m_ty) ? -INF : m_tile[size_t(ty) * m_tx + tx]; }

private:
    void raise(int ix, int iy, double z);
    BoundingBoxf m_area;
    double m_cell = 0.25;
    int    m_nx = 0, m_ny = 0;
    std::vector<float> m_top;
    int    m_tx = 0, m_ty = 0;
    std::vector<float> m_tile;
    double m_max = -INF;
};

struct Options
{
    // Extra safety gap kept between the toolhead (any part above the tip) and the part.
    // Applied as height - margin, floored at 0, so it never affects the nozzle tip itself.
    double margin = 0.2;
    // Printed beads stand this far below the nozzle Z that laid them (0.1 like rCAD's
    // simulate_gcode print_allowance). Lets the nozzle ride its own layer.
    double allowance = 0.1;
    // Path sampling step along segments (mm). 0 = the printed volume's cell size.
    double step = 0.;
    // check_path computes min_z exactly only down to `depth` below the lower of the point's
    // Z and reference_z; anything lower is reported as that floor (i.e. "at least this much
    // room"). Keeps planar neighbourhoods cheap.
    double depth = 1.0;
};

// Result of checking one planned path.
struct PathResult
{
    // Lowest safe nozzle Z at each input point (-inf where nothing constrains it).
    std::vector<double> min_z;
    // How far each point may dip below `reference_z` (reference_z - min_z, +inf if free).
    std::vector<double> max_dip;
    // Overall: the smallest of max_dip over the path, and the worst sample between points.
    double max_dip_path = INF;
    // True if some point (or sample between points) is below its min safe Z.
    bool   collides = false;
    // First offending input point index (segment start if between points), or size_t(-1).
    size_t first_collision = size_t(-1);
    // Deepest penetration (mm, > 0 when colliding).
    double worst_penetration = 0.;
};

class ClearanceChecker
{
public:
    ClearanceChecker(ToolheadShape shape, const BoundingBoxf &plate, Options opt = {}, double cell = 0.25);

    const ToolheadShape &shape() const { return m_shape; }
    PrintedVolume       &printed() { return m_printed; }
    const PrintedVolume &printed() const { return m_printed; }
    const Options       &options() const { return m_opt; }

    // Lowest nozzle Z at plate point (x, y) that keeps the whole toolhead clear of the
    // printed volume (with margin). -inf when nothing constrains it.
    double min_nozzle_z(double x, double y) const;
    // reference_z - min_nozzle_z: how far the nozzle may dip below reference_z here.
    double max_dip(double x, double y, double reference_z) const { return reference_z - min_nozzle_z(x, y); }
    // Gap between the toolhead and the part for a nozzle at p (negative = collision).
    double gap(const Vec3d &p) const { return p.z() - min_nozzle_z(p.x(), p.y()); }

    // Check a planned path (nozzle positions, mm), sampling between points.
    PathResult check_path(const std::vector<Vec3d> &path, double reference_z) const;

    // min_nozzle_z, but only resolved above `floor`: returns max(floor, min safe Z).
    // Much cheaper when the floor is close to the answer.
    double required_z(double x, double y, double floor) const;

private:
    ToolheadShape m_shape;
    PrintedVolume m_printed;
    Options       m_opt;
};

// ---- post-slice G-code check -------------------------------------------------------

struct GCodeCollision
{
    size_t line = 0;          // 1-based G-code line
    Vec3d  nozzle;            // nozzle position at the worst sample of the move
    double penetration = 0.;  // mm the toolhead is below its safe Z
    bool   travel = false;    // the move was a travel (no extrusion)
};

struct GCodeCheckReport
{
    bool   ran = false;
    std::string error;
    size_t moves_total = 0;                 // moves replayed after the first extrusion
    size_t moves_checked = 0;               // moves with printed material above the nozzle tip nearby
    size_t collisions = 0;                  // moves that collide
    double min_gap = INF;                   // smallest gap (mm) over checked moves, measured below 2 mm
    size_t min_gap_line = 0;
    std::vector<GCodeCollision> first;      // first `max_report` colliding moves
    std::string to_json() const;
};

// Replay finished G-code (G0-G3, G90/G91, M82/M83, G92; ;WIDTH: comments) on a plate of
// `plate` (mm), building the printed volume from its extrusions, and check every move
// against what was printed before it.
GCodeCheckReport check_gcode(const std::string &gcode_path, const ToolheadShape &shape, const BoundingBoxf &plate,
                             const Options &opt = {}, double default_width = 0.45, size_t max_report = 50,
                             double cell = 0.25);

} // namespace Clearance
} // namespace Slic3r

#endif // slic3r_ToolheadClearance_hpp_
