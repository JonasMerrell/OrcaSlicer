#!/usr/bin/env bash
# End-to-end check of staircase wave overhangs (wave_overhang_staircase*) through the headless CLI.
#
# Slices a 30 mm cantilevered shelf with no display (DISPLAY and WAYLAND_DISPLAY unset) and checks
# the G-code:
#  1. Defaults with wave_overhangs=1: staircase on, wave layer + 3 steps, floor speed 50%. 1b: staircase off:
#     every extrusion stays at its layer's Z.
#  2. wave_overhang_staircase=1 with 3 layers: some layers also extrude at +1 and +2 layer heights (3 layers per
#     band), never higher; travels over the higher steps are lifted; the nozzle never comes within
#     the stagger of material higher than itself.
#  3. wave_overhang_staircase_layers=2 and the Hilbert upper pattern: only +1 layer height.
#  4. wave_overhang_staircase_max_height=0.25 caps a 4-layer band at +0.2 (one layer up).
#  5. A per-object override through --load-assemble-list (print_params): two shelves, the
#     staircase on for the first only.
#
# usage: test_cli_wave_staircase.sh <orca-slicer binary> <python3>
set -u

BIN="${1:-}"
PY="${2:-python3}"
# 77 is the test's SKIP_RETURN_CODE.
[ -x "$BIN" ] || { echo "SKIP: orca-slicer binary not found: $BIN"; exit 77; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/orca-cli-staircase.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/datadir"

cat > "$WORK/machine.json" <<'EOF'
{
    "type": "machine",
    "from": "User",
    "name": "CLI staircase test printer",
    "printable_area": ["0x0", "200x0", "200x200", "0x200"],
    "printable_height": "100",
    "gcode_flavor": "klipper",
    "layer_change_gcode": "G92 E0"
}
EOF
cat > "$WORK/filament.json" <<'EOF'
{
    "type": "filament",
    "from": "User",
    "name": "CLI staircase test filament",
    "filament_diameter": ["1.75"]
}
EOF
process() {
    "$PY" - "$@" <<'EOF'
import json, sys

cfg = {"type": "process", "from": "User", "name": "CLI staircase test process",
       "layer_height": "0.2", "initial_layer_print_height": "0.2", "brim_type": "no_brim",
       "enable_arc_fitting": "0", "use_relative_e_distances": "1", "enable_support": "0",
       "gcode_comments": "1", "wall_loops": "2",
       "wave_overhangs": "1", "wave_overhangs_instead_of_bridges": "1",
       "wave_overhang_print_speed": "10"}
for kv in sys.argv[2:]:
    k, v = kv.split("=", 1)
    cfg[k] = v
with open(sys.argv[1], "w") as f:
    json.dump(cfg, f, indent=4)
EOF
}

# A 10 x 20 x 6 mm tower with a 30 mm shelf (1.2 mm thick) out of one side at z 3, as two boxes in
# one ASCII STL, so the test needs no fixture.
"$PY" - "$WORK/shelf.stl" <<'EOF'
import sys

def box(x0, y0, z0, x1, y1, z1):
    v = [(x, y, z) for x in (x0, x1) for y in (y0, y1) for z in (z0, z1)]
    q = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)]
    t = []
    for a, b, c, d in q:
        t += [(v[a], v[b], v[c]), (v[a], v[c], v[d])]
    return t

tris = box(0, 0, 0, 10, 20, 6) + box(9.5, 0, 3, 40, 20, 4.2)
with open(sys.argv[1], "w") as f:
    f.write("solid shelf\n")
    for t in tris:
        f.write("facet normal 0 0 0\nouter loop\n")
        for p in t:
            f.write("vertex %.4f %.4f %.4f\n" % p)
        f.write("endloop\nendfacet\n")
    f.write("endsolid shelf\n")
EOF

fails=0
fail() { echo "FAIL: $*"; fails=$((fails + 1)); }

slice() {
    local out="$WORK/$1"; shift
    mkdir -p "$out"
    env -u DISPLAY -u WAYLAND_DISPLAY timeout 300 "$BIN" --datadir "$WORK/datadir" --outputdir "$out" \
        --load-filaments "$WORK/filament.json" --slice 0 "$@" > "$out/log" 2>&1
    echo $? > "$out/status"
    [ "$(cat "$out/status")" = "0" ] || fail "$1: CLI exited with $(cat "$out/status"): $(tail -n 5 "$out/log")"
    [ -s "$out/plate_1.gcode" ] || fail "$1: no G-code written"
}

# check <tag> <checks as JSON>
check() {
    "$PY" - "$WORK/$1/plate_1.gcode" "$1" "$2" <<'EOF' || fails=$((fails + 1))
import json, math, re, sys
from collections import defaultdict

path, tag, want = sys.argv[1], sys.argv[2], json.loads(sys.argv[3])
header = {}
layer_z, x, y, z = 0.0, 0.0, 0.0, 0.0
moves = []   # (layer_z, x0, y0, x1, y1, z, extruding)
text = open(path).read()
for line in text.splitlines():
    if line.startswith(";Z:"):
        layer_z = float(line[3:])
    elif line.startswith("; wave_overhang_"):
        k, v = line[2:].split(" = ", 1)
        header[k] = v.strip()
    elif line.startswith("G1 ") or line.startswith("G0 "):
        c = dict((m[0], float(m[1:])) for m in re.findall(r"[XYZE]-?[\d.]+", line.split(";")[0]))
        nx, ny, nz = c.get("X", x), c.get("Y", y), c.get("Z", z)
        if layer_z > 0:
            moves.append((layer_z, x, y, nx, ny, nz, c.get("E", 0.0) > 0 and ("X" in c or "Y" in c)))
        x, y, z = nx, ny, nz

errors = []
err = errors.append
for k, v in want.get("header", {}).items():
    if header.get(k) != v:
        err("header %s = %r, expected %r" % (k, header.get(k), v))
ext = [m for m in moves if m[6]]
dzs = defaultdict(int)
for m in ext:
    dzs[round(m[5] - m[0], 2)] += 1
steps = sorted(d for d, n in dzs.items() if d > 0.01 and n >= 10)
if steps != want["steps"]:
    err("extrusion heights above the layer %s, expected %s (counts %s)" % (steps, want["steps"], dict(dzs)))
high = sum(n for d, n in dzs.items() if d > max(want["steps"] + [0]) + 0.01)
if high:
    err("%d extrusions higher than the top step" % high)
if "lifted" in want and (("wave staircase: clear the higher steps" in text) != want["lifted"]):
    err("lifted travels: %s, expected %s" % (not want["lifted"], want["lifted"]))
if "stepped_x_below" in want:
    split = want["stepped_x_below"]
    right = [m for m in ext if m[5] - m[0] > 0.01 and m[3] > split]
    left = [m for m in ext if m[5] - m[0] > 0.01 and m[3] < split]
    if right:
        err("%d stepped extrusions on the object without the override" % len(right))
    if len(left) < 20:
        err("only %d stepped extrusions on the object with the override" % len(left))
# Clearance: the nozzle (on any move) never within `stagger` of the centre line of an earlier
# extrusion that is higher than the nozzle, less that line's half width (0.25 allowed for widths).
stagger = want.get("stagger")
if stagger:
    cell = 1.0
    grid = defaultdict(list)
    worst = math.inf
    def samples(m, step=0.2):
        L = math.hypot(m[3] - m[1], m[4] - m[2])
        n = max(1, int(L / step))
        return [(m[1] + (m[3] - m[1]) * i / n, m[2] + (m[4] - m[2]) * i / n) for i in range(n + 1)]
    cur = None
    for m in moves:
        if m[0] != cur:
            # Steps the layers have reached are no longer above anything.
            cur = m[0]
            for key in list(grid):
                grid[key] = [q for q in grid[key] if q[2] > cur + 0.01]
                if not grid[key]:
                    del grid[key]
        z = m[5]
        if grid:
            for (px, py) in samples(m):
                gx, gy = int(px // cell), int(py // cell)
                for dx in range(-3, 4):
                    for dy in range(-3, 4):
                        for (qx, qy, qz) in grid.get((gx + dx, gy + dy), ()):
                            if qz > z + 0.01:
                                worst = min(worst, math.hypot(px - qx, py - qy) - 0.25)
        if m[6] and m[5] > m[0] + 0.01:
            for (px, py) in samples(m):
                grid[(int(px // cell), int(py // cell))].append((px, py, m[5]))
    if worst < stagger - 0.15:
        err("nozzle %.2f mm from higher material, stagger %.2f" % (worst, stagger))
    print("  clearance to higher material: %.2f mm" % worst)
if errors:
    print("FAIL: %s: %s" % (tag, "; ".join(errors)))
    sys.exit(1)
print("ok: %s: %d extrusions, steps %s" % (tag, len(ext), steps))
EOF
}

echo "== 1. staircase on by default (wave layer + 3 steps)"
process "$WORK/def.json"
slice def --load-settings "$WORK/machine.json;$WORK/def.json" "$WORK/shelf.stl"
check def '{"header": {"wave_overhang_staircase": "1", "wave_overhang_staircase_layers": "4", "wave_overhang_floor_print_speed": "50%"}, "steps": [0.2, 0.4, 0.6], "lifted": true}'

echo "== 1b. wave_overhang_staircase=0"
process "$WORK/off.json" wave_overhang_staircase=0
slice off --load-settings "$WORK/machine.json;$WORK/off.json" "$WORK/shelf.stl"
check off '{"header": {"wave_overhang_staircase": "0"}, "steps": [], "lifted": false}'

echo "== 2. wave_overhang_staircase=1, 3 layers"
process "$WORK/on.json" wave_overhang_staircase=1 wave_overhang_staircase_layers=3 wave_overhang_staircase_stagger=1.6
slice on --load-settings "$WORK/machine.json;$WORK/on.json" "$WORK/shelf.stl"
check on '{"header": {"wave_overhang_staircase": "1", "wave_overhang_staircase_layers": "3"}, "steps": [0.2, 0.4], "lifted": true, "stagger": 1.6}'

echo "== 3. two layers, Hilbert upper step"
process "$WORK/two.json" wave_overhang_staircase=1 wave_overhang_staircase_layers=2 wave_overhang_staircase_pattern=hilbert
slice two --load-settings "$WORK/machine.json;$WORK/two.json" "$WORK/shelf.stl"
check two '{"steps": [0.2], "stagger": 2.0}'

echo "== 4. max height caps the band"
process "$WORK/cap.json" wave_overhang_staircase=1 wave_overhang_staircase_layers=4 wave_overhang_staircase_max_height=0.25
slice cap --load-settings "$WORK/machine.json;$WORK/cap.json" "$WORK/shelf.stl"
check cap '{"steps": [0.2]}'

echo "== 5. per-object override through --load-assemble-list"
"$PY" - "$WORK/assemble.json" "$WORK/shelf.stl" <<'EOF'
import json, sys

obj = lambda x, params: {"path": sys.argv[2], "count": 1, "filaments": [1], "pos_x": [x], "pos_y": [100.0],
                         "pos_z": [0.0], "print_params": params}
json.dump({"plates": [{"plate_name": "staircase", "need_arrange": False,
                       "objects": [obj(50.0, {"wave_overhang_staircase": "1"}), obj(130.0, {})]}]},
          open(sys.argv[1], "w"), indent=4)
EOF
slice per_object --load-settings "$WORK/machine.json;$WORK/off.json" --load-assemble-list "$WORK/assemble.json"
check per_object '{"steps": [0.2, 0.4, 0.6], "stepped_x_below": 100.0}'

if [ "$fails" -ne 0 ]; then
    echo "FAIL: $fails check(s) failed"
    exit 1
fi
echo "PASS: staircase wave overhangs from the headless CLI: on by default, off, 3 layers, 2 layers, height cap, per-object override"
