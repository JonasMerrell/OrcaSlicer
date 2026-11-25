#!/usr/bin/env bash
# End-to-end check of brick layers through the real orca-slicer binary, headless (no DISPLAY or
# WAYLAND_DISPLAY, so no GUI and no dialogs).
#
# 1. brick_layers set in the process preset (what `orca-slice --set brick_layers=1` writes): only
#    inner walls leave the layer plane, by exactly half the layer height; the first and the top layer
#    are not raised; raised walls print after the layer's infill; the first raised layer has 1.5x the
#    flow of a full brick and the top layer's squashed wall 0.5x.
# 2. The same with layer_height 0.3: the offset follows the setting (+0.15).
# 3. brick_layers off: every extrusion is in its layer plane.
# 4. Per-object override (--load-assemble-list print_params) with brick_layers off in the process:
#    only the object that turns it on is raised.
#
# usage: test_cli_brick_layers.sh <orca-slicer binary> <python3>
set -u

BIN="${1:-}"
PY="${2:-python3}"
# 77 is the test's SKIP_RETURN_CODE.
[ -x "$BIN" ] || { echo "SKIP: orca-slicer binary not found: $BIN"; exit 77; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/orca-cli-brick-layers.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/datadir"

# Standalone presets: without "inherits" the CLI loads them as-is, with no preset bundle.
cat > "$WORK/machine.json" <<'EOF'
{
    "type": "machine",
    "from": "User",
    "name": "CLI brick layers test printer",
    "printable_area": ["0x0", "200x0", "200x200", "0x200"],
    "printable_height": "100",
    "layer_change_gcode": "G92 E0"
}
EOF
# process <file> <brick_layers> <layer_height>
process() {
    cat > "$1" <<EOF
{
    "type": "process",
    "from": "User",
    "name": "CLI brick layers test process $2 $3",
    "layer_height": "$3",
    "initial_layer_print_height": "$3",
    "wall_loops": "3",
    "sparse_infill_density": "20%",
    "only_one_wall_top": "0",
    "seam_slope_type": "none",
    "enable_support": "0",
    "gcode_label_objects": "1",
    "brick_layers": "$2"
}
EOF
}
process "$WORK/brick.json" 1 0.2
process "$WORK/brick_03.json" 1 0.3
process "$WORK/plain.json" 0 0.2

# A 10 x 10 x 6 mm box: 30 layers at 0.2 mm.
"$PY" - "$WORK/box.stl" <<'EOF'
import sys
v = [(x, y, z) for z in (0, 6) for y in (0, 10) for x in (0, 10)]
with open(sys.argv[1], "w") as f:
    f.write("solid box\n")
    for a, b, c, d in ((0, 2, 3, 1), (4, 5, 7, 6), (0, 1, 5, 4), (2, 6, 7, 3), (0, 4, 6, 2), (1, 3, 7, 5)):
        for tri in ((v[a], v[b], v[c]), (v[a], v[c], v[d])):
            f.write("facet normal 0 0 0\nouter loop\n")
            for p in tri:
                f.write("vertex %g %g %g\n" % p)
            f.write("endloop\nendfacet\n")
    f.write("endsolid box\n")
EOF

fails=0
fail() { echo "FAIL: $*"; fails=$((fails + 1)); }

# run <tag> <process.json> [option...] [model...]: slice headless into $WORK/<tag>.
run() {
    local out="$WORK/$1" proc="$2"; shift 2
    mkdir -p "$out"
    env -u DISPLAY -u WAYLAND_DISPLAY timeout 300 "$BIN" --datadir "$WORK/datadir" \
        --load-settings "$WORK/machine.json;$proc" "$@" --slice 0 --outputdir "$out" > "$out/log" 2>&1
    local rc=$?
    [ "$rc" -eq 0 ] || fail "$tag: orca-slicer exited $rc: $(tail -n 5 "$out/log")"
    [ -s "$out/plate_1.gcode" ] || fail "$tag: no g-code written"
}

# check <tag> <mode> [args]: analyse $WORK/<tag>/plate_1.gcode.
check() {
    "$PY" - "$WORK/$1/plate_1.gcode" "${@:2}" <<'EOF'
import math, re, sys
from collections import defaultdict

path, mode, args = sys.argv[1], sys.argv[2], sys.argv[3:]
num = re.compile(r"([XYZE])(-?[\d.]+)")
x = y = z = e = 0.0
rel = False
typ, layer_z, obj = "", None, None
moves = []  # (index, object, type, layer_z, z, e_per_mm)
for i, line in enumerate(open(path)):
    s = line.strip()
    if s.startswith(";TYPE:"):
        typ = s[6:]; continue
    if s.startswith(";Z:"):
        layer_z = float(s[3:]); continue
    if s.startswith("; printing object"):
        obj = re.search(r"id:(\d+)", s).group(1); continue
    if s.startswith("; stop printing object"):
        obj = None; continue
    code = s.split(";")[0].strip()
    if code.startswith("M83"): rel = True
    elif code.startswith("M82"): rel = False
    elif code.startswith("G92"):
        m = dict(num.findall(code))
        if "E" in m: e = float(m["E"])
    elif code.startswith(("G1", "G2", "G3")):
        m = {k: float(v) for k, v in num.findall(code)}
        nx, ny, nz = m.get("X", x), m.get("Y", y), m.get("Z", z)
        de = (m["E"] if rel else m["E"] - e) if "E" in m else 0.0
        if "E" in m and not rel: e = m["E"]
        d = math.hypot(nx - x, ny - y)
        if de > 0 and d > 0.5 and layer_z is not None:
            moves.append((i, obj, typ, layer_z, nz, de / d))
        x, y, z = nx, ny, nz

errors = []
def raised(m): return m[4] > m[3] + 1e-3

if mode == "planar":
    off = [m for m in moves if abs(m[4] - m[3]) > 1e-3]
    if off: errors.append("%d extrusions off the layer plane, first: %s" % (len(off), off[0]))

elif mode == "brick":
    h = float(args[0])
    layers = sorted({m[3] for m in moves})
    up = [m for m in moves if raised(m)]
    if not up: errors.append("no raised wall")
    for m in up:
        if m[2] != "Inner wall": errors.append("raised %s at Z %.2f" % (m[2], m[3])); break
        if abs(m[4] - m[3] - h / 2) > 2e-3: errors.append("offset %.3f, want %.3f" % (m[4] - m[3], h / 2)); break
    low = [m for m in moves if m[4] < m[3] - 1e-3]
    if low: errors.append("extrusion below its layer: %s" % (low[0],))
    raised_layers = {m[3] for m in up}
    if layers[0] in raised_layers: errors.append("first layer raised")
    if layers[-1] in raised_layers: errors.append("top layer raised")
    if len(raised_layers) != len(layers) - 2:
        errors.append("%d raised layers, want %d" % (len(raised_layers), len(layers) - 2))
    for lz in raised_layers:
        ms = [m for m in moves if m[3] == lz]
        last_infill = max([m[0] for m in ms if "infill" in m[2].lower()] or [-1])
        if min(m[0] for m in ms if raised(m)) < last_infill:
            errors.append("raised wall before infill at Z %.2f" % lz); break
    def median(v):
        v = sorted(v); return v[len(v) // 2] if v else float("nan")
    mid = layers[len(layers) // 2]
    full = median([m[5] for m in up if m[3] == mid])
    first = median([m[5] for m in up if m[3] == layers[1]])
    top = min([m[5] for m in moves if m[3] == layers[-1] and m[2] == "Inner wall"] or [float("nan")])
    if not abs(first / full - 1.5) < 0.05: errors.append("first brick flow x%.3f, want x1.5" % (first / full))
    if not abs(top / full - 0.5) < 0.05: errors.append("top wall flow x%.3f, want x0.5" % (top / full))

elif mode == "per_object":
    by_obj = defaultdict(int)
    objs = {m[1] for m in moves if m[1] is not None}
    for m in moves:
        if raised(m): by_obj[m[1]] += 1
    if len(objs) != 2: errors.append("expected 2 labelled objects, got %s" % sorted(objs))
    if sorted(by_obj) != [args[0]]: errors.append("raised walls by object %s, want only object %s" % (dict(by_obj), args[0]))

for err in errors: print("  " + err)
sys.exit(1 if errors else 0)
EOF
}

tag=brick;    run brick    "$WORK/brick.json"    "$WORK/box.stl"
check brick brick 0.2 || fail "brick_layers=1, layer_height 0.2"

tag=brick_03; run brick_03 "$WORK/brick_03.json" "$WORK/box.stl"
check brick_03 brick 0.3 || fail "brick_layers=1, layer_height 0.3"

tag=plain;    run plain    "$WORK/plain.json"    "$WORK/box.stl"
check plain planar || fail "brick_layers=0 left the layer plane"

# Per-object: the process has brick layers off, the first object turns them on.
cat > "$WORK/assemble.json" <<EOF
{"plates": [{"plate_name": "brick layers", "need_arrange": true, "objects": [
    {"path": "$WORK/box.stl", "count": 1, "filaments": [1], "print_params": {"brick_layers": "1"}},
    {"path": "$WORK/box.stl", "count": 1, "filaments": [1]}
]}]}
EOF
tag=per_object; run per_object "$WORK/plain.json" --load-assemble-list "$WORK/assemble.json"
check per_object per_object 0 || fail "per-object brick_layers override"

[ "$fails" -eq 0 ] || { echo "$fails brick layers CLI check(s) failed"; exit 1; }
echo "PASS: brick layers from the process preset, per layer height, off, and per object, headless"
