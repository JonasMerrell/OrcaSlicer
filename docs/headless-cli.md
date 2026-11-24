# Headless command line (fork)

Everything that changes the sliced output can be done without a display: every setting, every
model edit the GUI offers (with the few exceptions listed under [Gaps](#gaps)), calibration prints,
exports, and the fork's own features. Agents and scripts drive it through `orca-slicer` directly or
through the `orca-slice` wrapper in `tools/orca-slice/` (presets, JSON output, printer lanes and the
filament hub).

The full table of what is reachable how is `docs/headless-cli-audit.json`. Two tests keep it honest:

- `libslic3r_tests "[HeadlessCLI]"` fails when an option of `print_config_def` has no row in the
  audit, or when its preset or scopes changed since the audit was made.
- `ctest -R headless_cli_audit` (`scripts/headless_cli_audit.py check`) scans the sources and fails
  when a gizmo, calibration mode, volume type, paint layer or model-editing / exporting `Plater`
  action has no row in the audit's `gui_actions`.

After adding an option: `scripts/headless_cli_audit.py generate --binary build/src/Release/orca-slicer`,
review the diff, and add a `note` for an option no preset stores. After adding a GUI action: add a
`gui_actions` row with its CLI path (or `"status": "gui-only"` and why).

No display is needed: `DISPLAY` and `WAYLAND_DISPLAY` may be unset. `LC_ALL=C` keeps number
formatting stable. Nothing here uploads to a printer or starts a print.

## 1. Settings

Every configuration option is also a command line option, highest priority:

```bash
orca-slicer --load-settings "machine.json;process.json" --load-filaments "f1.json;f2.json" \
    --sparse-infill-density 25% --nozzle-temperature 240,250 --slice 0 --outputdir out model.stl
```

`_` in a key becomes `-`. Vector options take comma-separated values, one per extruder/filament.
An unknown option fails with `Invalid option --foo`.

### Option catalog

```bash
orca-slicer --list-options -          # JSON to stdout
orca-slicer --list-options opts.json
```

```json
{"version": 1,
 "options": [{"key": "wall_loops", "cli": "--wall-loops", "type": "int", "vector": false,
              "default": "2", "label": "Wall loops", "category": "Strength", "tooltip": "...",
              "min": 0, "max": 1000, "mode": "simple", "preset": "process",
              "scopes": ["global", "object", "part", "layer_range"]}, ...],
 "cli": {"actions": [...], "transforms": [...], "misc": [...]}}
```

- `preset`: which preset stores the option: `process`, `filament`, `printer`, `project` (stored in
  the project/3MF, e.g. `flush_volumes_matrix`, `wipe_tower_x`) or `null`.
- `scopes`: `global` (any preset), `object` (object settings), `part` (part and modifier settings),
  `layer_range` (height range modifiers), `plate` (plate settings).
- `per_extruder`: a vector indexed by filament (filament options) or extruder (printer options).
- `enum_values` / `enum_labels`, `min` / `max`, `sidetext` (unit), `ratio_over` for percentages.

## 2. Job files: objects, parts, paint, cuts, plates

`--load-job job.json` builds the project from a JSON description instead of model files. It can't
be combined with model files, transforms or `--load-assemble-list` (it has all of that itself).
Everything it builds slices, exports (`--export-3mf` gives a project the GUI opens with all of it)
and inspects (`--inspect-mesh`, `--inspect-paint`) like a loaded 3MF.

```json
{
  "version": 1,
  "plates": [
    {
      "name": "brackets",
      "arrange": true,
      "settings": {"curr_bed_type": "Textured PEI Plate", "print_sequence": "by object"},
      "objects": [
        {
          "file": "bracket.stl",
          "name": "bracket",
          "count": 2,
          "rotate": [0, 0, 45],
          "filament": 1,
          "settings": {"wall_loops": 4, "sparse_infill_density": "40%"},
          "height_ranges": [{"min_z": 0, "max_z": 2, "settings": {"sparse_infill_density": "100%"}}],
          "parts": [
            {"type": "modifier", "primitive": {"shape": "box", "size": [10, 10, 5]},
             "position": [20, 0, 7.5], "settings": {"wall_loops": 8}},
            {"type": "negative", "primitive": {"shape": "cylinder", "radius": 1.6, "height": 30},
             "position": [0, 0, 10]},
            {"type": "part", "file": "insert.stl", "filament": 2}
          ],
          "paint": [
            {"layer": "support", "value": "enforce", "normal": [0, 0, -1], "max_angle": 20},
            {"layer": "seam", "value": "enforce", "box": {"min": [0, 0, 0], "max": [5, 50, 50]}},
            {"layer": "fuzzy_skin", "z": [10, 20]},
            {"layer": "color", "value": 2, "inside": "logo_mask.stl", "frame": "mesh"}
          ]
        }
      ]
    },
    {"name": "lids", "arrange": false,
     "objects": [{"file": "lid.3mf", "object": "lid", "position": [120, 120], "lay_on": "largest_face",
                  "cut": {"z": 12, "keep": "both"}}]}
  ]
}
```

A single plate can drop the `plates` level: `{"objects": [...]}`. Relative paths are relative to
the job file. Unknown fields, unknown keys, keys set at a level the GUI wouldn't allow, and values
an option can't parse (including unknown enum values) are errors naming the field, e.g.
`plates[0].objects[1].settings.nozzle_temperature: can't be set per object (it is a filament setting: set it globally)`.
The CLI exits with code -5 (`CLI_CONFIG_FILE_ERROR`), prints that message on stderr and puts it in
`result.json` `error_string`.

### Plates

| field | meaning |
|---|---|
| `name` | plate name |
| `arrange` | arrange the plate's objects (default `true`; default `false` when every object has a `position`). With `false` every object needs a `position`. |
| `settings` | plate settings: `curr_bed_type`, `print_sequence`, `first_layer_print_sequence`, `other_layers_print_sequence`, `other_layers_print_sequence_nums`, `spiral_mode`, `filament_map_mode`, `filament_map`, ... (the options with scope `plate`) |
| `objects` | the plate's objects |

Each plate slices to its own G-code (`--slice 0` all, `--slice N` plate N).

### Objects

| field | meaning |
|---|---|
| `file` | STL, OBJ, 3MF, STEP, AMF or DRC. A multi-object file becomes one object with all of them as parts unless `object` picks one (index or name). Paint and settings in a 3MF are kept. |
| `primitive` | instead of a file: `{"shape": "box", "size": [x,y,z]}`, `{"shape": "cylinder"|"cone", "radius", "height"}`, `{"shape": "sphere", "radius"}`; optional `segments` |
| `name` | object name |
| `count` | number of copies (instances) |
| `position` / `positions` | plate coordinates `[x, y]` of the bounding box centre, one per copy |
| `rotate` | `[rx, ry, rz]` degrees |
| `scale` | factor or `[sx, sy, sz]` |
| `mirror` | `"x"` or a list of axes |
| `lay_on` | `"largest_face"` or a direction `[nx, ny, nz]`: lay on that face, like the Lay on face tool |
| `orient` | `true`: auto-orient this object |
| `filament` | filament number (1-based) for the whole object |
| `settings` | object settings (options with scope `object`) |
| `height_ranges` | `[{"min_z", "max_z", "settings"}]`, mm above the bed; settings with scope `layer_range` |
| `parts` | extra parts and modifiers, see below |
| `paint` | painting rules, see below |
| `boolean` | `[{"op": "minus"|"plus"|"intersect", "file"|"primitive", "position", "rotate", "scale", "part"}]`, like the Mesh boolean tool, in the source mesh frame, on model part `part` (default 0) |
| `simplify` | `{"ratio": 0.3}` (fraction of triangles kept) or `{"triangles": 5000}`, like the Simplify tool. Drops paint on the simplified parts. |
| `cut` | `{"z": mm above the bed, "keep": "both"|"upper"|"lower", "flip_upper", "keep_as_parts", "place_on_cut"}`: a horizontal planar cut, like the Cut tool |
| `split` | `true`: split into one object per connected shell (Split to objects); arrange the result |

Order of operations: load, `boolean`, `simplify`, `parts`, then `scale`, `mirror`, `rotate`,
`lay_on`, `height_ranges`, `paint`, `cut`, `split`, and finally copies and placement on the plate
(every copy drops onto the bed).

### Parts

| field | meaning |
|---|---|
| `type` | `part`, `modifier` (default), `negative`, `support_enforcer`, `support_blocker`, `seam_center`, `seam_left`, `seam_right`, `seam_enforced`, `seam_blocked`, `seam_neutral` |
| `file` / `primitive` / `object` | as for objects. Primitives are centred on `position`. |
| `position`, `rotate`, `scale`, `mirror` | in the object's source mesh frame (the coordinates of its file), so a modifier exported from the same CAD model needs no `position` |
| `name` | part name |
| `filament` | filament for a part or modifier |
| `settings` | part/modifier settings (options with scope `part`) |

### Paint

Each rule paints the facets that match all of its criteria, in order; later rules win.

| field | meaning |
|---|---|
| `layer` | `support`, `seam`, `fuzzy_skin`, `color` |
| `value` | `enforce` (default) / `block` / `none` for support and seam; `on` (default) / `none` for fuzzy skin; the filament number for colour (0 clears) |
| `parts` | model part indices to paint (default: all model parts) |
| `all` | `true`: every facet |
| `normal` + `max_angle` | facets whose outward normal is within `max_angle` degrees (default 30) of the direction, as the object will print. `[0,0,-1]` = downward facing. |
| `z` | `[min, max]` facet centre height |
| `box` | `{"min": [x,y,z], "max": [x,y,z]}` around the facet centre |
| `sphere` | `{"center": [x,y,z], "radius": r}` |
| `facets` | facet indices of the part's mesh |
| `inside` | a closed mesh file: facets whose centre is inside it (a paint mask) |
| `frame` | coordinates of `z`, `box`, `sphere`, `inside`: `object` (default: as printed, origin at the object's bounding box minimum, so `z` is height above the bed) or `mesh` (the source file's coordinates, e.g. a mask modelled next to the part in CAD) |
| `invert` | `true`: paint the facets that don't match |

Check the result with `--inspect-paint` or `--export-3mf` and the GUI.

## 3. Transforms on model files

For plain model input (not with `--load-job`): `--rotate`, `--rotate-x`, `--rotate-y`, `--scale`,
`--scale-to-fit`, `--ground-largest-face`, `--ground-face-normal`, `--ground-face-point`,
`--orient 1`, `--arrange 1` (+ `--allow-rotations`, `--align-to-y-axis`), `--repair`,
`--convert-unit`, `--assemble`, `--repetitions`, `--clone-objects`, `--cut-grid`. They run in
command line order. `--inspect-mesh` prints bounding boxes and the faces the `--ground-*` options
choose from.

## 4. Exports and reports

`--slice N --outputdir D` writes `plate_N.gcode` and `result.json`; `--export-3mf F` the project
(with `--min-save`); `--export-stl` / `--export-stls D`; `--export-gcode-html` a self-contained
toolpath preview per plate; `--export-statistics` `plate_N.statistics.json`; `--export-settings F|-`
the merged settings; `--strict` turns slicing warnings into a non-zero exit (they are listed in
`result.json` `warnings` either way, e.g. `config_value_substituted` for an unknown enum value).

## 5. Calibration prints

Every generator of the Calibration menu runs headless (`src/CLICalibration.cpp`):

```bash
orca-slicer --load-settings "machine.json;process.json" --load-filaments f.json \
    --calib temp_tower --calib-params "start=260,end=230" --slice 0 --outputdir out
```

`--calib` replaces model files (it loads the calibration model itself); `--arrange`/`--orient` may
come along, other transforms, model files and job files may not. Lists inside `--calib-params`
use `:`. Defaults are the GUI dialog's.

| mode | parameters (defaults) |
|---|---|
| `pa_line` | start=0, end=0.1, step=0.002, print_numbers=1, extruder, bowden=1 (Bowden ranges) |
| `pa_tower` | as pa_line, print_numbers=0 |
| `pa_pattern` | start=0, end=0.08, step=0.005, print_numbers=1, speeds=60:120, accels=3000:6000 |
| `flow_rate` | pass=1\|2, pattern=archimedean\|monotonic |
| `flow_rate_yolo` | pass=1 (YOLO) \| 2 (perfectionist), pattern |
| `temp_tower` | start=230, end=190 (5 degree blocks, 155-500), resize=1 |
| `vol_speed` | start=5, end=20, step=0.5 (mm3/s) |
| `vfa` | start=40, end=200, step=10, resize=1, layer_height (auto) |
| `retraction` | start=0, end=2, step=0.1 |
| `input_shaping_freq` | freq_x=15:110, freq_y=15:110, damp=0.15, shaper, model=0 (ringing) \| 1 (fast) |
| `input_shaping_damp` | freq_x=30, freq_y=30, start=0, end=0.4, shaper, model |
| `cornering` | start=1, end=15 (jerk; 0..0.25 for Marlin junction deviation), model=0\|1\|2 |

- Settings typed on the command line win over the calibration's own changes; they are listed in
  `result.json` `calibration.user_overrides`.
- With `brim_type=no_brim` the brims some calibrations add are skipped (`calibration.brim_skipped`).
- Questions the GUI would ask become warnings on stderr and in `calibration.warnings`.
- `result.json` `calibration` holds the resolved parameters.
- Not headless: Auto PA line (Bambu printer-side), belt-printer calibrations (refused with a
  message), the VFA dialog's automatic layer-height pick (use `layer_height=`).

## 6. The orca-slice wrapper

`tools/orca-slice/orca-slice` slices with named presets from the desktop's user presets, prints one
JSON object, never uploads or prints and always turns brims off. `orca-slice --help` has every
flag; the main ones:

```bash
orca-slice part.stl -P "SimpleCore Parts 0.25" -f "SimpleCore ASA" -o out/
orca-slice part.stl --set nozzle_temperature=245 --set 'nozzle_temperature[2]=250' --set wall_loops=4
orca-slice part.stl --set-object wall_loops=6 --set-range 0-2:sparse_infill_density=100%
orca-slice part.stl --modifier box@10,10,5:8,8,4:wall_loops=1 --negative cylinder@0,0,5:1.6,30 --cut 12:both
orca-slice part.stl --orient | --rotate 90 | --lay-flat | --scale 0.5 | --copies 4
orca-slice job.json -f "SimpleCore ABS Sunlu" -f "SimpleCore ASA"      # job file, see section 2
orca-slice part.stl -f @T7                                               # filament loaded in T7, retooled to T7
orca-slice part.stl --export-3mf out/part.3mf --stats --html --strict
orca-slice calib pa_tower --params "start=0,end=0.08,step=0.002"
orca-slice options --search fuzzy            # option catalog, filtered
orca-slice inspect part.3mf --paint
orca-slice --check-gcode out/part.gcode --set toolhead_clearance_enable=1 ...
```

- `--set KEY=VALUE` goes to the preset that owns the key (from `--list-options`): process,
  every filament (or `KEY[i]` for filament i), printer (`KEY[i]` = extruder i), and project keys
  straight to the slicer. Unknown keys and bad enum values stop with exit 2 and the allowed
  values; `--allow-substitutions` lets the slicer substitute instead.
- `--set-object`, `--set-range`, `--modifier`, `--negative`, `--cut`, `--copies` build a job file
  around the model.
- Several plates give `<name>_plate<N>.gcode` and a `plates` list in the JSON; `--plate N`
  slices one.

## 7. Printer lanes and the filament hub

```bash
orca-slice lanes                       # Moonraker lane_data (GET only): tool, material, colour, spool, preset
orca-slice part.stl -f @T7             # the preset loaded in T7
orca-slice part.3mf -f @printer        # one filament per lane, T0..Tn
FILAMENT_HUB_TOKEN=... orca-slice hub status|list|inventory
FILAMENT_HUB_TOKEN=... orca-slice hub pull [--type filament] [--name NAME]
FILAMENT_HUB_TOKEN=... orca-slice hub push "SimpleCore ASA" [--type filament] [--force]
```

`MOONRAKER_URL` (default the SimpleCore printer) and `MOONRAKER_API_KEY` select the printer.
`FILAMENT_HUB_URL` (default: `filament_hub_url` from OrcaSlicer.conf) selects the hub; the token is
read from `FILAMENT_HUB_TOKEN` only, never from a file, and never written or printed. `hub pull`
writes to `~/.cache/orca-slice/hub/`, which preset lookup searches after the desktop presets; it
never touches the desktop's preset folders. A push conflict exits 3 with the server's version in
the JSON; `--force` overwrites it.

## Gaps

What still needs the GUI (see `gui_actions` rows with status `gui-only` / `partial`):

- Cut tool: angled planes, connectors (plugs, dowels, snaps) and groove cuts (horizontal planar
  cuts are headless).
- Painted modifiers, texture displacement, text and SVG emboss (import the text/SVG as a mesh part
  instead).
- Free-hand paint strokes (rule-based paint and painted 3MFs are headless).
- Split to parts as a flag (give the parts as `parts[]`), "fill bed" (give `count`).
- Toolpaths as OBJ (the HTML preview is headless).
- Uploading and printing are left out on purpose.
