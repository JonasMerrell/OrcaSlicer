# Wave Overhang Settings Reference

This document describes every config option added by the wave-overhangs fork of OrcaSlicer, what it does, and how to tune it. For an overview of what wave overhangs are and why they exist, see the [main README](../README.md). For the algorithm itself, see [ALGORITHMS.md](ALGORITHMS.md); for known problems (mostly warping), see [LIMITATIONS.md](LIMITATIONS.md).

Sections below follow the **Print Settings → Wave overhangs** page top to bottom, so the order here matches the order in the GUI.

## Contents

1. [What wave overhangs are](#what-wave-overhangs-are)
2. [How to enable](#how-to-enable)
3. [General](#general)
4. [Detection](#detection)
5. [Pattern](#pattern)
6. [Corner reinforcement](#corner-reinforcement)
7. [Motion](#motion)
8. [Cooling](#cooling)
9. [Floor layers](#floor-layers)
10. [Staircase (multi-layer waves)](#staircase-multi-layer-waves)
11. [Pin supports](#pin-supports)
12. [Debug](#debug)
13. [Known limitations](#known-limitations)
14. [G-code markers](#g-code-markers)

---

## What wave overhangs are

Wave overhangs let you print steep cantilevered overhangs without supports. Instead of dropping support columns from the bed, each ring of extrusion anchors to the one before it and the nozzle marches outward into empty space one fused-plastic rung at a time. The generator emits expanding wavefronts seeded at the supported edge of the overhang.

**Wave overhangs are angle-agnostic.** The trigger is geometric, not an angle threshold: any perimeter Orca's *Strength > Detect overhang walls* + *Overhang reverse threshold* pipeline flags as overhang becomes a candidate for wave. So angled overhangs (slopes anywhere between vertical and fully horizontal) are supported, with the wave only filling the part of each layer that protrudes past the previous one. There is no minimum angle setting; the old `wave_overhang_min_angle` key is only kept so older profiles still load (see [Reserved settings](#reserved-settings)).

## How to enable

1. Open a model with an overhang.
2. Go to **Print Settings → Wave overhangs**.
3. Toggle **Use wave overhangs (Experimental)** on.
4. Slice. Wave extrusions will appear in the G-code preview wherever overhangs are detected.

Simple mode only shows the master toggle. Switch the top-right mode selector to **Advanced** to see individual tunables.

> **No presets yet.** The tunable space is large and the "right" bundles depend on printer, material, and geometry. Rather than ship opinionated defaults now, we want community test prints to surface what actually works. Expect presets to return once there's real data.

---

## General

### `wave_overhangs`

Master on/off switch. When off, none of the other wave-overhang settings have any effect and overhangs are printed with Orca's normal perimeter generator.

- **Type:** bool · **Default:** `false` · **Mode:** Simple

### `wave_overhangs_instead_of_bridges`

Skip the bridge-vs-wave decision and wave every overhang in the region. Off by default: a flat span stays a regular bridge only when its unsupported distance is below 20% of the overhang's perimeter and it has no holes; longer, narrower, one-sided (cantilevered), concave, or holed overhangs get waves. Turn on to guarantee no bridge patterns anywhere in the region; bottom bridges and internal bridges both become solid infill on top of wave.

- **Type:** bool · **Default:** `false`
- **When to use:** an overhang that grows *inward* (a lip on the inside of a ring, a phone-case rim) is anchored on all sides, so the bridgeability check hands it to Orca's normal bridge pipeline. Turn this on to force wave there.

### `support_remaining_areas_after_wave_overhangs`

When wave overhangs and supports are both on, generate supports only for overhang areas the wave toolpaths did *not* cover. Explicit support enforcers still apply normally.

- **Type:** bool · **Default:** `true`
- **Off** = the support generator ignores wave coverage entirely and treats every overhang as if waves weren't there.

---

## Detection

### `wave_overhang_min_length`

Overhang regions whose outline (outer contour length) is shorter than this get no wave and print as normal overhangs. `0` = no limit.

- **Type:** float (mm) · **Default:** `0.0` · **Range:** `0 to 50`
- **Tuning:** raise to 2 to 5 mm to avoid waving tiny overhang fragments (curved corners, small notches) where the travel/seam overhead isn't worth it.

### `wave_overhang_max_iterations`

Safety cap on the generator's main loop: max wavefronts per overhang region.

- **Type:** int · **Default:** `0` (= unlimited) · **Range:** `0 to 500`
- The generator stops naturally when it can't grow further; this is a hard cap for pathological cases or to bound print time on very large overhangs.

---

## Pattern

### `wave_overhang_pattern`

Travel pattern between wave lines.

- **Type:** enum (`monotonic` | `zigzag` | `smart`) · **Default:** `smart`

| Choice | Behavior |
|---|---|
| `monotonic` | Each line printed independently, same direction. Safest flow consistency, most travel. |
| `zigzag` | Lines connected into a back-and-forth meander. Minimum travel, but start-of-line is sometimes on the unsupported side. |
| `smart` | Starts each line from the better-supported end. Best default. |

### `wave_overhang_outer_perimeters`

Number of outer perimeters preserved inside the overhang region. Everything inward of that becomes wave pattern. Independent of *Strength > Walls* (`wall_loops`): if you set this to `1`, the overhang zone always has one outer wall + wave regardless of how many walls the rest of the object prints.

- **Type:** int · **Default:** `1` · **Range:** `0 to 20`
- **How it works:** the overhang region of the layer is computed from the island geometry (island minus lower_slices). The outermost `N` normal perimeters in that region are kept untouched; inner perimeters are clipped away and replaced by wave pattern. The wave starts exactly where the preserved walls end, so there is no gap between them.
- **Interaction with `wall_loops`:** the setting is capped at the effective wall count for the layer. Two cases matter:
  - If you set `wave_overhang_outer_perimeters = 5` but `wall_loops = 2`, only 2 walls exist, so effectively 2 are preserved and wave fills everything inside.
  - On topmost layers with `only_one_wall_top` enabled, only 1 wall is generated regardless of `wall_loops`, so the cap pulls effective preservation down to 1.
  Without the cap the wave region would be over-shrunk and the slicer would fill the leftover band with bridge paths instead of wave.
- **Tuning:** `1` is usually plenty. Raise to `2` or `3` for thicker outer shells on structural parts. Set to `0` for pure wave from the model boundary inward (no outer perimeter, experimental).

### `wave_overhang_line_spacing`

Center-to-center distance between adjacent wave extrusions.

- **Type:** float (mm) · **Default:** `0.35` · **Min:** `0.01`
- **Tuning:** tighter (0.28 to 0.30) = denser fill, stronger, slower, risk of over-extrusion on cantilevers. Wider (0.40 to 0.50) = faster, visible gaps between rings, weaker.
- **Other nozzles:** the default is tuned for 0.4 mm. On a 0.2 mm nozzle bring it down toward your line width (0.15 to 0.20); on 0.6 mm you can go to 0.5.

### `wave_overhang_perimeter_overlap`

Extends the wave propagation boundary outward toward kept perimeter lines so the outermost wave sits closer to the shell.

- **Type:** float (mm) · **Default:** `0.1` · **Range:** `0 to 20`
- **Tuning:** raise toward `0.2 to 0.3` if you see a visible gap between the last wave ring and the outer perimeter. Too high can cause wave lines to print *into* the perimeter.

### `wave_overhang_minimum_width`

If a neck in the wave region is narrower than this width, insert a thin split there before propagation so fragile wave branches do not form.

- **Type:** float (mm) · **Default:** `0.7` · **Range:** `0 to 20`
- **Tuning:** raise to split more aggressively if wave rings skip over thin necks between lobes. Lower to `0` to disable splitting.

### `wave_overhang_min_new_area`

Terminate propagation when a new wavefront adds less than this much new area.

- **Type:** float (mm²) · **Default:** `0.01` · **Range:** `0 to 100` · **Mode:** Develop (hidden in Advanced)
- **Tuning:** lower (toward `0.0001`) to keep propagating deep into tight regions; raise to terminate early on diminishing returns.

### `wave_overhang_flow_mm3_per_mm`

Absolute volume of plastic extruded per millimetre of wave-overhang line.

- **Type:** float (mm³/mm) · **Default:** `0.15` · **Range:** `0.02 to 1.5`
- **Why absolute (not a ratio):** a wave-overhang line hangs in air, not squished against a layer below. Nothing to squish into means layer height has no effect on the bead's cross-section; the bead size is set by nozzle bore and mm³/mm extrusion rate alone. An absolute mm³/mm captures that directly.
- **Why 0.15:** the calibrated reference value for a 0.4 mm nozzle from the underlying research.
- **For other nozzle sizes:** scale as `0.15 × (nozzle / 0.4)²`. 0.2 mm → 0.04, 0.3 mm → 0.09, 0.5 mm → 0.23, 0.6 mm → 0.34, 0.8 mm → 0.60. The setting is **not** adjusted automatically when you change nozzle; it is a per-material, per-nozzle calibration surface, so update it by hand.

**Tuning guide:**
- Start at `0.15` (scaled for your nozzle) and print.
- If wave lines look thin / starved / broken: raise by 10 to 20 %.
- If wave lines blob or merge: lower by 10 to 20 %.

---

## Corner reinforcement

Opt-in. Sharp convex corners of an overhang warp worst: their cantilevered wave lines are short and have little neighbouring material to fuse with, so they curl as the plastic cools. Corner reinforcement packs more lines into a small radius around each corner so every short line has a neighbour to bond with.

All three sub-options are no-ops unless the master toggle ("Corner reinforcement") is on, **and** `wave_overhang_line_spacing_corner` is non-zero and smaller than `wave_overhang_line_spacing`, **and** `wave_overhang_corner_taper_distance` ("Corner reinforcement radius") is non-zero. The defaults (0.18 mm and 3.0 mm) satisfy this, so switching the toggle on is enough.

### `wave_overhang_corner_taper_enable`

Master toggle for corner reinforcement. Reveals the three sub-options below.

- **Type:** bool · **Default:** `false`

### `wave_overhang_line_spacing_corner`

Denser line spacing used inside the corner zone.

- **Type:** float (mm) · **Default:** `0.18` · **Range:** `0 to 2`
- Must be smaller than `wave_overhang_line_spacing`. `0` = reinforcement off.
- **Tuning:** start at roughly half the main spacing (e.g. `0.18` for a `0.35` main spacing).

### `wave_overhang_corner_taper_distance`

Radius around each detected corner vertex over which the denser spacing is applied.

- **Type:** float (mm) · **Default:** `3.0` · **Range:** `0 to 20`
- `0` = reinforcement off. Larger values widen the reinforced zone at the cost of extrusion time. `2 to 4 mm` is a sensible starting range.

### `wave_overhang_corner_angle_threshold`

A vertex on the overhang contour counts as a corner when its interior angle is smaller than this.

- **Type:** float (°) · **Default:** `90` · **Range:** `10 to 180`
- Smaller values catch only very sharp corners; larger values also flag gentler turns.

---

## Motion

### `wave_overhang_print_speed`

Print speed for wave extrusions.

- **Type:** float (mm/s) · **Default:** `2.0` · **Min:** `0.1`
- **Tuning:** slower = better cooling and adhesion of cantilever rings. `1.5 to 2.5` mm/s is the usual useful range. Going above ~5 mm/s defeats the point of wave overhangs.

### `wave_overhang_perimeter_speed`

Print speed for the walls (perimeters) on layers that contain wave traces. The walls in those layers anchor onto the cantilevered wave, so printing them at the regular wall speed can pull the wave loose or starve adhesion.

- **Type:** float (mm/s) · **Default:** `0` (= inherit normal wall speed) · **Range:** `0 to 1000`
- **Tuning:** try `10 to 20` mm/s if the wave strip detaches when the outer wall lands next to it.

### `wave_overhang_travel_speed`

Travel speed within wave regions between non-extruding hops.

- **Type:** float (mm/s) · **Default:** `40.0` · **Min:** `1.0`
- Applied via a per-move override in `GCodeWriter::travel_to_*` around wave extrusions. Slower travels reduce the chance of the nozzle catching a still-soft ring.

### `wave_overhang_end_retract_length`

Forced retraction (in mm) emitted at the end of every wave-overhang line. Independent of the filament's normal retraction settings.

- **Type:** float (mm) · **Default:** `0.0` · **Range:** `0 to 10`
- **Why:** wave lines finish in mid-air, so residual nozzle pressure dribbles a small blob that sticks to the next travel path or piles up against the enclosing perimeter. Orca's normal retraction is travel-distance-gated; short inter-wave travels often don't cross the threshold, so most wave line ends never retract.
- **What it does:** after each wave line, emits `G1 E-<length> F...` (labelled `; WAVE_OVERHANG_END` in the G-code). The next wave line's lead-in travel automatically unretracts via the filament's existing state tracking.
- **Tuning:** `0` = disabled, use normal retraction heuristic. Start at `0.4 to 0.8 mm` if you see end-of-line blobs or over-extrusion piled against the outer perimeter; raise until the artifacts go away. Above `1.5 mm` you risk under-extrusion at the start of the next line.

---

## Cooling

### `wave_overhang_fan_speed`

Part-cooling fan percentage forced during wave extrusions.

- **Type:** int (%) · **Default:** `100` · **Range:** `0 to 100`
- Implemented with `;_WAVE_OVERHANG_FAN_START/END` markers handled in `CoolingBuffer`.
- **Tuning:** keep at 100 % for PLA/PETG. Drop to 40 to 60 % for ABS/ASA to reduce warping, but note cantilever rings benefit hugely from fast cooling.

### `wave_overhang_aux_fan_speed`

Auxiliary (chamber/side) fan percentage forced during wave extrusions. Strong sideways airflow on the unsupported tracks without ramping the printhead fan (which can create a thermal gradient near the nozzle).

- **Type:** int (%) · **Default:** `-1` (= inherit normal aux fan) · **Range:** `-1 to 100`
- Requires the printer profile's auxiliary fan to be enabled.

### `wave_overhang_nozzle_temp`

Override the hotend temperature specifically for wave extrusions. An `M104` is emitted at the start of each wave region and the filament default is restored at the end.

- **Type:** int (°C) · **Default:** `0` (= keep filament default) · **Range:** `0 to 350`
- **Tuning:** try 5 to 15 °C below the material's normal temperature to reduce sagging and reheating of already-printed rings. See [LIMITATIONS.md](LIMITATIONS.md#practical-mitigations).

### `wave_overhang_min_wave_time`

Minimum time in seconds each wave region must take. If a region's extrusion would finish faster than this, a `G4` dwell pads the difference before the next region, giving each ring time to solidify.

- **Type:** float (s) · **Default:** `0` (= off) · **Range:** `0 to 60`

### `wave_overhang_min_layer_time`

Minimum time in seconds a wave layer must take, measured from the first wave extrusion to the end of the layer. If the layer would finish faster, a `G4` dwell pads the difference before the next layer starts, so the wave bed can solidify before solid infill lands on top. Targets warping.

- **Type:** float (s) · **Default:** `0` (= off) · **Range:** `0 to 300`

---

## Floor layers

Everything in this group affects the solid layers stacked *on top of* a wave region. Per [LIMITATIONS.md](LIMITATIONS.md), those first layers above the wave do most of the pulling that warps a cantilever, which is why they get their own pattern, speed, and fan controls.

### `wave_overhang_floor_layers`

Number of solid floor layers placed directly above a wave region. **Authoritative**: this value overrides Orca's `bottom_shell_layers` behavior within the wave shadow. `N` means *exactly* N solid layers above the wave, not N-plus-whatever-bottom-shell-layers-adds.

- **Type:** int · **Default:** `2` · **Range:** `0 to 20`
- `N = 0` = zero solid layers above the wave footprint. The layer directly above the wave strip goes straight to sparse infill. Use for max material savings on purely aesthetic overhangs.
- `N = 2` (default) = two solid-infill layers above the wave before sparse infill resumes. Standard mechanical backing.
- `N = 3+` = heavier structural cap (slower, more filament, but stiffer).
- All N layers are `stInternalSolid` (regular solid infill); no bridge classification is used, since these layers sit on top of the wave extrusions (which are solid material) rather than spanning air.
- **Interaction with `bottom_shell_layers`:** inside the wave shadow, the floor_layers value wins. Outside the wave shadow, Orca's normal shell rules still apply. Implementation: each affected layer gets a `wave_overhang_shadow_polygons` mask that is subtracted from the bottom-shell seed set in both `discover_vertical_shells` and `discover_horizontal_shells`, and `apply_wave_overhang_floor_layer_authority()` runs after all surface classification to enforce the count.

### `wave_overhang_floor_use_hilbert`

Force the solid floor layers above wave regions to use a Hilbert-curve infill pattern instead of the region's normal solid-infill pattern. Fractal scan paths leave the smallest residual thermal stress, which reduces the upward curl of long cantilevers as the layers above cool. Reveals the Hilbert sub-options below.

- **Type:** bool · **Default:** `false`

### `wave_overhang_floor_hilbert_layers`

Of the N floor layers, only the bottom M get the Hilbert pattern; the rest use the default solid pattern. The lowest layers warp most, so capping the Hilbert band saves time without losing much benefit.

- **Type:** int · **Default:** `0` (= all floor layers) · **Range:** `0 to 20`

### `wave_overhang_floor_hilbert_density`

Infill percentage for the Hilbert floor layers. `100` = solid Hilbert (matches the cited research). Below 100 turns the floor into a sparse Hilbert: faster, less material, weaker backing. Experimental.

- **Type:** int (%) · **Default:** `100` · **Range:** `1 to 100`

### `wave_overhang_floor_print_speed`

Print speed for the Hilbert floor layers. Slower gives each line time to shed heat before its neighbour lands, reducing residual stress.

- **Type:** float (mm/s) · **Default:** `0` (= inherit solid-infill speed) · **Range:** `0 to 1000`

### `wave_overhang_floor_perimeter_speed`

Print speed for the walls on the floor layers above wave regions. Fast walls landing on the fragile wave shadow warp it; slowing them keeps the substrate closer to bed temperature.

- **Type:** float (mm/s) · **Default:** `0` (= inherit wall speed) · **Range:** `0 to 1000`

### `wave_overhang_floor_speed_ramp`

Number of layers over which the two floor speed overrides ramp linearly back up to normal speed.

- **Type:** int (layers) · **Default:** `0` · **Range:** `0 to 64`
- `0` = step: every floor layer prints at the override speed, then snaps back to normal on the next layer.
- `1..N` = layer 1 above the wave prints at the override speed, layer N (or the last floor layer, whichever comes first) prints at full normal speed, linear interpolation in between. Softens the thermal-stress step at the speed transition.

### `wave_overhang_floor_fan_speed`

Part-cooling fan percentage forced during the Hilbert floor layers. Lower fan keeps the layer warm longer so stress can relax before it locks in. Counter-intuitive but matches the cited research.

- **Type:** int (%) · **Default:** `-1` (= inherit) · **Range:** `-1 to 100`

### `wave_overhang_floor_aux_fan_speed`

Auxiliary fan percentage forced during the Hilbert floor layers. Pair with a low printhead fan to keep the layer warm near the nozzle while still moving chamber air.

- **Type:** int (%) · **Default:** `-1` (= inherit) · **Range:** `-1 to 100`
- Requires the printer profile's auxiliary fan to be enabled.

---

## Staircase (multi-layer waves)

A wave overhang is one layer printed in mid air, so a long one is flimsy and its free edge curls while it cools. With the staircase on, the wave layer and the next `wave_overhang_staircase_layers - 1` layers above the overhang are printed together, inside the wave layer. The wave front advances a few rings, then the layer above follows one stagger behind it at Z+h, the next one a stagger behind that at Z+2h, and so on. The band grows outward together, so the free edge is held down while it is still warm. The cantilever being built is N layers thick, and stiffness goes with thickness cubed, so 3 layers are about 27 times stiffer than 1.

How it works (`src/libslic3r/WaveOverhangs/WaveStaircase.{hpp,cpp}`, pass in `WaveStaircasePrint.cpp`):

- **Step regions.** Step k (layer L+k) lies on step k-1, inside the fill area of L+k (its walls are printed normally later). It stays `stagger + line width` away from everything layer L+k-1 prints later at the lower height, so the nozzle tip never meets a higher step. That makes the steps taper toward the free edge and the sides. They also reach back over the supported side, so the root of the cantilever is full height. The band prints these areas, and they are removed from the fill of L+1 .. L+N-1.
- **Order.** A distance field from the supported edge (geodesic, like the wave fronts) cuts the band into bands of `wave_overhang_staircase_rings_per_step` rings. Each round the wave front advances one band. Then each step prints whatever no lower, still unprinted material lies within the stagger of, nearest first. The band prints after the island's infill (like walls that wait for their infill), so nothing else on layer L lands next to the steps afterwards.
- **G-code.** Step paths are z-contoured paths at +dz, extruded at their own layer's flow (no contour flow scaling). The wave fan and nozzle temperature overrides cover the whole band. A travel that passes within the stagger of a printed step higher than both of its ends is lifted `wave_overhang_staircase_travel_lift` above the step first, crosses at that height, and comes down only at its destination (`; wave staircase: clear the higher steps` with G-code comments on). This holds on the following layers too, until they reach the steps.
- **Height.** The toolhead passes over the band at the wave layer's height, so (N-1)·h must stay below the lowest toolhead part above the nozzle tip minus a margin. `wave_overhang_staircase_max_height` caps it. Layers beyond the cap are left out of the band.

| Setting | Default | |
|---|---|---|
| `wave_overhang_staircase` | off | turns it on (needs `wave_overhangs`) |
| `wave_overhang_staircase_layers` | 3 | layers per band, wave layer included (2-8) |
| `wave_overhang_staircase_stagger` | 2.0 mm | nozzle centre to the edge of higher material: the tip's flat radius plus a margin |
| `wave_overhang_staircase_rings_per_step` | 3 | wave rings the front advances per round |
| `wave_overhang_staircase_pattern` | waves | upper steps: waves (rings along the front), rectilinear, hilbert, concentric |
| `wave_overhang_staircase_max_height` | 0.6 mm | cap on the top step's height above the wave layer |
| `wave_overhang_staircase_travel_lift` | 0.4 mm | travel height above the highest nearby step |
| `wave_overhang_staircase_speed` | 0 | upper step speed, 0 = solid infill speed |
| `wave_overhang_staircase_min_round_time` | 0 s | a shorter round ends lifted, with a dwell |

Headless: all of them are region settings, so `orca-slice --set wave_overhang_staircase=1` and per-object `print_params` in `--load-assemble-list` work. Tests: `[WaveOverhangs][Staircase]` (libslic3r and fff_print) and `ctest -R cli_wave_staircase`.

Limits: support material is generated after this pass and is not kept away from the steps (travels are still lifted). The G-code preview groups the steps with the wave layer. Combining it with Z anti-aliasing, brick layers or non-planar infill is untested.

---

## Pin supports

Resin printers hold an overhang down with many thin pins. The pin supports do the same for wave overhangs: a row of separate, tapered pins under the free edge of each wave strip, each fused into the underside of the overhang. A wave layer curls up at its free edge as it cools; a fused pin holds the edge down in tension, which a normal support with a Z gap cannot do. A test shelf (36 mm ABS cantilever, 2026-11-26) with seven such pins modelled as cones printed flat where the same shelf without them curled.

What the pins are (`src/libslic3r/Support/TreeSupportPins.{hpp,cpp}`, run from the support step after any other support):

- **Placement.** On every layer with wave paths, the strip as it hangs over air (the layer minus the layer below, the pieces the wave filled) is shrunk by `wave_overhang_pin_edge_inset`. Its outline is the row of pins, kept only where the strip reaches at least `wave_overhang_pin_min_overhang` out from the layer below, so the part of the edge near the supported side gets none. Each row piece gets a pin at both ends and at every sharp corner, and pins at most `wave_overhang_pin_spacing` apart in between. `wave_overhang_pin_rows` adds rows further in, `wave_overhang_pin_row_spacing` apart. Pins closer than half a spacing to another are dropped.
- **Shape.** A pin is a cone: `wave_overhang_pin_tip_diameter` at the top, widening at `wave_overhang_pin_taper_angle` from the vertical down to `wave_overhang_pin_base_diameter`. Neighbouring pins are kept `wave_overhang_pin_gap` apart, so closely spaced pins come out thinner than the base diameter rather than merging into a wall. Each layer of a pin prints solid, as concentric loops.
- **Contact.** The top of a pin is the layer right under the wave layer: no Z gap, no interface. The wave layer prints over it unchanged and fuses to it. Pins drop straight down (they never move sideways or merge, unlike tree branches) to the bed, or stand fused on the model where it is below them. Beside the model they keep the pin gap away from it.
- **Waves unchanged.** The pins are support extrusions only. The wave generator and the bridge/bottom surface detection only look at the object's own slices, so the overhang still prints as waves, with or without the staircase. That is what tree supports with support enforcers did not do: they turned the overhang into a bottom surface on support, and merged their branches into one wall.
- **Supports on or off.** With supports off, only the pins print, on support layers at the object's layer heights. With tree supports on, the pins go into the same support layers (a layer is added where none sits at the right height). With `support_remaining_areas_after_wave_overhangs` on (the default), the other supports leave the wave strip to the pins. The pins print with the support filament and line width, at `wave_overhang_pin_speed`.

| Setting | Default | |
|---|---|---|
| `wave_overhang_pin_supports` | off | turns the pins on (needs `wave_overhangs`; works with `enable_support` off) |
| `wave_overhang_pin_tip_diameter` | 0.8 mm | where the pin fuses into the overhang (the test: ~0.85 mm) |
| `wave_overhang_pin_base_diameter` | 5.0 mm | largest diameter, below the taper (the test: 5.4 mm) |
| `wave_overhang_pin_taper_angle` | 16° | half angle of the cone; 0 = a cylinder of the tip diameter |
| `wave_overhang_pin_spacing` | 4.0 mm | along a row (the test: 6.25 mm; denser holds better) |
| `wave_overhang_pin_edge_inset` | 2.5 mm | outer row's distance in from the strip's edge (the test: 2.5 mm) |
| `wave_overhang_pin_rows` | 1 | rows of pins, outer row first |
| `wave_overhang_pin_row_spacing` | 0 | between rows, 0 = the pin spacing |
| `wave_overhang_pin_min_overhang` | 5.0 mm | pins only where the strip reaches this far out from the layer below |
| `wave_overhang_pin_gap` | 1.0 mm | smallest gap between two pins, and between a pin and the model |
| `wave_overhang_pin_speed` | 30 mm/s | print speed of the pins (never faster than the first-layer speed on layer 1); 0 = support speed |

Headless: they are object settings, so `orca-slice --set wave_overhang_pin_supports=1 --set wave_overhang_pin_spacing=3` and per-object settings (`--set-object`, job files, `print_params` in `--load-assemble-list`) work. Tests: `[WaveOverhangs][PinSupports]` (libslic3r and fff_print) and `ctest -R cli_wave_pins`.

Removing them: snap or cut each pin at its tip. A pin standing on the model leaves a small fused nub there too.

Limits: organic tree supports and normal supports put their own layers at their own heights; the pins add their layers between them, which is untested beyond tree (slim/strong/hybrid) supports. Pins only go under wave overhangs, not under bridges or plain overhangs. Rafts and `support_on_build_plate_only` are not considered (pins always stand on the model where it is below them).

---

## Debug

### `wave_overhang_debug_gcode`

Emit `; WAVE_OVERHANG_START` / `; WAVE_OVERHANG_END` comments around wave extrusions and the `; WAVE_OVERHANG_BUILD` / `; WAVE_OVERHANG_CONFIG` banners in the file header.

- **Type:** bool · **Default:** `true`
- Comments only; no effect on the actual print. See [G-code markers](#g-code-markers) for format. Leave it on when filing a bug report or uploading to [waveoverhangs.com](https://waveoverhangs.com); the banners are what the parser reads.

---

## Reserved settings

These keys exist so that older profiles and 3MF files still load. They have no effect on slicing. `wave_overhang_seam_mode` and `wave_overhang_spacing_mode` are only visible in Develop mode, `wave_overhang_min_angle` is not shown at all.

| Key | Status |
|---|---|
| `wave_overhang_min_angle` | Not used. Which overhangs get waves is decided by Orca's *Detect overhang walls* + *Overhang reverse threshold* (Strength tab). |
| `wave_overhang_seam_mode` | Not used. Ring direction comes from `wave_overhang_pattern`. |
| `wave_overhang_spacing_mode` | Not used. Ring step is always uniform. |

## Known limitations

| Topic | Status |
|---|---|
| `wave_overhang_min_new_area` | Develop mode only. |
| Nozzle sizes other than 0.4 mm | Flow and line spacing defaults are tuned for 0.4 mm and are **not** auto-scaled. Scale by hand (see `wave_overhang_flow_mm3_per_mm`). |
| Overhangs anchored on all sides (inward lips, rings) | Classified as bridgeable and handed to Orca's bridge pipeline unless `wave_overhangs_instead_of_bridges` is on. |
| Large spans | Warping grows with span; see [LIMITATIONS.md](LIMITATIONS.md). Wave overhangs are a tool for smaller, self-contained overhangs. |

Everything else on the page is plumbed end-to-end (speed, travel, fan, aux fan, nozzle temp, dwell times, floor layers, Hilbert floor, support integration).

---

## G-code markers

When `wave_overhang_debug_gcode = true` (the default), the following comments appear in the output. They are pure comments (no motion, no effect on the print), but they're invaluable for verifying that waves were emitted and for bug reports.

**Build context** (once, in the file header, after Orca's `HEADER_BLOCK`):

```
; WAVE_OVERHANG_BUILD wave_overhangs_version=<x.y.z> orca_base=<orca version>
```

Only the fork version and the Orca base version are carried here. Printer, filament, layer height, temperatures and flow ratio are already present in Orca's own trailing `; key = value` config block at the end of the file, so they are not duplicated.

**Region banner** (once per print region with wave overhangs enabled, right after the BUILD line):

```
; WAVE_OVERHANG_CONFIG region=<N> outer_perim=<int>
  spacing=<mm> flow_mm3_per_mm=<x> speed=<mm/s> travel=<mm/s> fan=<%>
  floor_layers=<int> min_angle=<deg> min_length=<mm> max_iterations=<int>
  pattern=<smart|monotonic|zigzag> spacing_mode=<uniform|progressive> seam_mode=<alternating|aligned|random>
  perimeter_overlap=<mm> minimum_wave_width=<mm>
  min_new_area=<mm²>
  corner_enable=<0|1> corner_spacing=<mm> corner_taper=<mm> corner_angle=<deg>
  end_retract=<mm>
  nozzle_temp_override=<C> min_wave_time=<s> min_layer_time=<s>
  wall_loops=<int> top_shell_layers=<int> bottom_shell_layers=<int>
  infill_density=<%> infill_pattern=<name>
  support_remainder=<0|1> instead_of_bridges=<0|1>
```

(Emitted as a single line in the G-code; split across lines here only for readability.)

**Extrusion block markers** (wrap every wave extrusion):

```
; WAVE_OVERHANG_START
G1 X... Y... E...
...
; WAVE_OVERHANG_END
```

Fan overrides additionally emit `;_WAVE_OVERHANG_FAN_START` / `;_WAVE_OVERHANG_FAN_END` (consumed by the cooling post-processor and stripped from the final file).

**Verifying waves were emitted:** slice your model, then grep the output for `WAVE_OVERHANG_START`. No matches means either Orca's upstream overhang-wall detection didn't flag any regions (check *Strength > Detect overhang walls* / *Overhang reverse threshold*), `wave_overhangs` is off, your `min_length` threshold filtered everything, or the overhang was judged bridgeable (see `wave_overhangs_instead_of_bridges`). If you want the markers off for a production print, set `wave_overhang_debug_gcode = false`.
