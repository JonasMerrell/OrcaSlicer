# Z-pins (Experimental)

Z-pins make printed parts stronger across the layer lines. The slicer leaves small vertical channels
open through several layers. Every few layers the nozzle parks over each channel and fills it with
molten filament from above. A pin fuses across all the layer lines it crosses, so a crack between two
layers has to break the pins too.

It is off by default. Turn it on under **Strength → Z-pins**, or from the command line (below).

## Command line (headless)

Everything works headless: no display, no GUI dialogs. Every setting in the table below is an ordinary
process key.

**`orca-slice` (netdash wrapper):** `--set key=value`, repeatable.

```
orca-slice part.stl --set z_pins=1
orca-slice part.stl --set z_pins=1 --set z_pin_diameter=1.8 --set z_pin_spacing=5 \
    --set z_pin_pattern=grid --set z_pin_interval=8 --set z_pin_overfill=120% \
    --set z_pin_in_inner_walls=0 --set z_pin_dwell=0.1
orca-slice part.stl --set z_pins=1 --html     # check the channels in the toolpath preview
```

**The fork's own CLI:** put the keys in the process JSON passed to `--load-settings`. The file must be
self-contained, with no `inherits`, or the CLI can't resolve it:

```
orca-slicer --load-settings "machine.json;process.json" --load-filaments filament.json \
    --slice 0 --outputdir out/ part.stl            # process.json has "z_pins": "1", ...
```

**Per object:** all z-pin keys are object settings. In a 3MF, add them to the object in
`Metadata/model_settings.config`, then slice the 3MF with its own settings
(`orca-slice part.3mf --as-is`, or `orca-slicer --slice 0 part.3mf`):

```xml
<object id="2">
    <metadata key="name" value="bar_pinned"/>
    <metadata key="z_pins" value="1"/>
    <metadata key="z_pin_diameter" value="1.2"/>
```

**Checking the output:**
- Each fill is a `; Z_PIN_INJECT X.. Y.. depth .. volume .. mm3 flow .. mm3/s` line, and each fill layer
  starts with `; Z_PINS layer N z Z: k injections`.
- The settings used are in the G-code's config footer (`; z_pin_diameter = 1.8`).

**Bad values:**
- An out-of-range value stops the slice with exit code −18 and a message such as
  `z_pin_interval: 1 not in range [2, 100]`, without writing G-code.
- An unknown `z_pin_pattern` falls back to `staggered` with a warning, like every Orca enum.

`tests/cli/test_cli_z_pins.sh` covers all of this. It sets every key to a non-default value, slices with
no display, reads each value's effect back out of the G-code, and checks the per-object 3MF path and the
range errors. Run it with `ctest -C Release -R cli_z_pins`.

## Background

- Duty, Failla, Kim, Smith, Lindahl, Kunc, *Z-pinning approach for 3D printing mechanically isotropic
  materials*, Additive Manufacturing 27 (2019) 175–184 (open copy: https://www.osti.gov/pages/servlets/purl/1808415).
  The setup: about 1.3 mm holes in a sparse grid of 0.53 mm beads, pins 8 layers of 0.3 mm deep, and two
  pin sets filled 4 layers apart. The nozzle was held still over each hole, with no dip. At 120% fill the Z
  strength rose 3.5× in PLA and CF-PLA, and PLA's anisotropy fell from 52% to under 10%.
- UT/ORNL SFF papers 2017–2019. In fully solid parts, the time the nozzle spends on the pins lets the
  layer cool. In their tests that cost more strength than the pins added (a 55 s pause every 4th layer:
  33 → 12 MPa, and pins brought it back only to 16–17 MPa). Pins pay off in sparse parts and thick walls;
  keep injections short.
- MAGMA (MGunlogson/OrcaSlicer, AGPL-3.0) is a separate, much larger take on injected infill. Its tuning
  notes inform the defaults here: inject at the filament's max volumetric speed, keep each injection
  under about 1.5 s, and use no dwell.

## How it works

1. **Grid.** Pin positions form a grid (square, or staggered/hexagonal) centred on the object, so the
   channels line up on every layer.
2. **Where a pin fits.** After the walls are generated, a pin is allowed on a layer when the channel and
   its wall fit in the allowed area of that layer. The allowed area is the infill, or the whole inside
   of the outer wall when *Pins in inner walls* is on.
3. **Columns.** A pin is open on a layer only if it also fits on the layers around it: the top and
   bottom shell layers plus the pin-free margins. Pins therefore never break through a top or bottom
   surface and never open under a cavity or an overhang. Pins shorter than 2 layers are dropped.
4. **Geometry.** On every layer where a pin is open, the channel is cut out of the infill and gets a
   one-line inner-wall loop (the channel wall). Inner walls it crosses are cut where the channel wall
   joins them. The outer wall is never cut.
5. **Injection.** After printing the layer of a fill, the nozzle goes to each due channel and stops
   `z_pin_injection_height` above the top of that layer. It extrudes `π r² × open depth × overfill` while
   circling inside the channel, optionally dwells, then always retracts (and wipes along the circle when
   wipe is on). The injection is inside the object's EXCLUDE_OBJECT block.
   - The flow is `z_pin_injection_speed`, capped by the filament's max volumetric speed. The circle is
     run at no more than 25 mm/s, which can lower a very high flow. With a 0.5 mm nozzle, 1.5 mm
     channels and 2 mm fills, that works out to 0.15–0.22 s per pin at 12–16 mm³/s.
   - The injection circles, rather than extruding in place, because the G-code processor counts only
     moves with XY motion as extrusion. That way the preview, the time estimate and the filament
     estimate all include the pins (they show as sparse infill).
   - Circling also keeps each move under Klipper's default `max_extrude_cross_section` of 4 × nozzle².
6. **Alternate sets.** With *Alternate pin sets*, neighbouring pins (checkerboard, or alternate rows on
   the staggered grid) are filled half an interval apart, as in Duty et al.

Each injection is marked in the G-code:

```
; Z_PINS layer 57 z 11.600: 6 injections
; Z_PIN_INJECT X128.000 Y131.196 depth 2.000 volume 3.888 mm3 flow 20.00 mm3/s
```

## Settings

| Key | Default | Meaning |
|---|---|---|
| `z_pins` | off | Master switch. |
| `z_pin_diameter` | 1.5 mm | Open channel diameter. About 3× the line width works; the nozzle's flat must cover it. |
| `z_pin_spacing` | 6 mm | Distance between pins. |
| `z_pin_pattern` | staggered | `grid` or `staggered` (hexagonal). |
| `z_pin_in_sparse_infill` | on | Pins in the infill. |
| `z_pin_in_inner_walls` | on | Pins may cross inner walls (they are cut around the channel). |
| `z_pin_in_solid_infill` | off | Let pins run into the top/bottom shells (up to the margins). |
| `z_pin_interval` | 10 layers | Fill every N layers. Keep N × layer height around 2–3 mm. |
| `z_pin_alternate` | on | Fill neighbouring pins half an interval apart. |
| `z_pin_overfill` | 110 % | Injected volume / channel volume. |
| `z_pin_injection_speed` | 0 (= filament max) | Volumetric rate, capped by `filament_max_volumetric_speed`. |
| `z_pin_injection_height` | 0.1 mm | Nozzle tip height above the layer while injecting. Never negative: no dip. |
| `z_pin_dwell` | 0 s | Wait after each injection. |
| `z_pin_top_margin` | 1 layer | Extra pin-free layers under top surfaces (on top of the shells unless pins may enter them). |
| `z_pin_bottom_margin` | 1 layer | Same above bottom surfaces. |

## Limits

- Pins are object settings: a height range or part modifier can't switch them on for part of an object.
  To compare pinned and plain parts on one plate, set `z_pins` per object (object settings in the GUI,
  or `<metadata key="z_pins" value="1"/>` on the object in a 3MF's `Metadata/model_settings.config`).
- The nozzle stays above the layer, so the channels must be wide enough for the melt to fall in.
  Narrow, deep channels bridge over and leave voids (Duty et al.). Use about 3× the line width and
  intervals of 2–3 mm.
- Multi-material: the pins are filled with whatever filament is active when the object's layer is
  done.
- Spiral vase mode disables pins.
- Each fill layer adds a short stop per pin. On large parts, consider a larger spacing.
