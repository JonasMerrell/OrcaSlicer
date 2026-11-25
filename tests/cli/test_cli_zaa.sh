#!/usr/bin/env bash
# End-to-end check of Z contouring (Z anti-aliasing, zaa_*) through the headless CLI.
#
# Slices a shallow dome with no display (DISPLAY and WAYLAND_DISPLAY unset) and checks the G-code:
#  1. zaa_enabled=0 (the default): every extrusion stays at its layer's Z.
#  2. zaa_enabled=1: extrusions follow the dome inside their layer, from one layer height below the
#     layer's Z up to zaa_min_z above it, and top surface flow scales with the local layer thickness.
#  3. zaa_max_dz=0.05 and zaa_inner_walls=0: no extrusion moves more than 0.05 mm, and inner walls
#     stay planar.
#  4. A per-object override through --load-assemble-list (print_params): two domes, Z contouring on
#     for the first only, so only that one is contoured.
#
# usage: test_cli_zaa.sh <orca-slicer binary> <python3>
set -u

BIN="${1:-}"
PY="${2:-python3}"
# 77 is the test's SKIP_RETURN_CODE.
[ -x "$BIN" ] || { echo "SKIP: orca-slicer binary not found: $BIN"; exit 77; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/orca-cli-zaa.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/datadir"

# Standalone presets: without "inherits" the CLI loads them as-is, with no preset bundle.
cat > "$WORK/machine.json" <<'EOF'
{
    "type": "machine",
    "from": "User",
    "name": "CLI ZAA test printer",
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
    "name": "CLI ZAA test filament",
    "filament_diameter": ["1.75"]
}
EOF
# process <file> [key=value...]: a standalone process with brims off, plus the given settings.
process() {
    "$PY" - "$@" <<'EOF'
import json, sys

cfg = {"type": "process", "from": "User", "name": "CLI ZAA test process",
       "layer_height": "0.2", "initial_layer_print_height": "0.2", "brim_type": "no_brim",
       "enable_arc_fitting": "0", "use_relative_e_distances": "1", "enable_support": "0"}
for kv in sys.argv[2:]:
    k, v = kv.split("=", 1)
    cfg[k] = v
with open(sys.argv[1], "w") as f:
    json.dump(cfg, f, indent=4)
EOF
}

# A shallow spherical-cap dome (30 mm across, 4 mm cap on a 1 mm plinth, 29 degrees at the rim) as an
# ASCII STL, so the test needs no fixture.
"$PY" - "$WORK/dome.stl" <<'EOF'
import math, sys

R, h_plinth, h_cap = 15.0, 1.0, 4.0
Rs = (R * R + h_cap * h_cap) / (2 * h_cap)
zc = h_plinth + h_cap - Rs
NA, NR = 96, 30
P = lambda r, a, z: (r * math.cos(a), r * math.sin(a), z)
cap = lambda r: zc + math.sqrt(Rs * Rs - r * r)
tris = []
for j in range(NA):
    a0, a1 = 2 * math.pi * j / NA, 2 * math.pi * (j + 1) / NA
    for i in range(NR):
        r0, r1 = R * i / NR, R * (i + 1) / NR
        p00, p01, p10, p11 = P(r0, a0, cap(r0)), P(r0, a1, cap(r0)), P(r1, a0, cap(r1)), P(r1, a1, cap(r1))
        tris.append((p00, p10, p11))
        if i:
            tris.append((p00, p11, p01))
    b0, b1, t0, t1 = P(R, a0, 0), P(R, a1, 0), P(R, a0, h_plinth), P(R, a1, h_plinth)
    tris += [(b0, b1, t1), (b0, t1, t0), ((0, 0, 0), b1, b0)]
with open(sys.argv[1], "w") as f:
    f.write("solid dome\n")
    for t in tris:
        f.write("facet normal 0 0 0\nouter loop\n")
        for p in t:
            f.write("vertex %.5f %.5f %.5f\n" % p)
        f.write("endloop\nendfacet\n")
    f.write("endsolid dome\n")
EOF

fails=0
fail() { echo "FAIL: $*"; fails=$((fails + 1)); }

# slice <tag> [option...]: run headless into $WORK/<tag>, keeping the log and the shell status there.
slice() {
    local out="$WORK/$1"; shift
    mkdir -p "$out"
    env -u DISPLAY -u WAYLAND_DISPLAY timeout 300 "$BIN" --datadir "$WORK/datadir" --outputdir "$out" \
        --load-filaments "$WORK/filament.json" --slice 0 "$@" > "$out/log" 2>&1
    echo $? > "$out/status"
    [ "$(cat "$out/status")" = "0" ] || fail "$1: CLI exited with $(cat "$out/status"): $(tail -n 5 "$out/log")"
    [ -s "$out/plate_1.gcode" ] || fail "$1: no G-code written"
}

# check <tag> <checks as JSON>: parse the G-code and test it, see the checks below.
check() {
    "$PY" - "$WORK/$1/plate_1.gcode" "$1" "$2" <<'EOF' || fails=$((fails + 1))
import json, math, re, sys

path, tag, want = sys.argv[1], sys.argv[2], json.loads(sys.argv[3])
h = 0.2
header = {}
layer_z, x, y, z, typ = 0.0, 0.0, 0.0, 0.0, ""
moves = []  # (type, layer_z, x0, y0, x1, y1, z1, e)
for line in open(path):
    if line.startswith(";Z:"):
        layer_z = float(line[3:])
    elif line.startswith(";TYPE:"):
        typ = line[6:].strip()
    elif line.startswith("; zaa_"):
        k, v = line[2:].split(" = ", 1)
        header[k] = v.strip()
    elif line.startswith("G1 ") or line.startswith("G0 "):
        c = dict((m[0], float(m[1:])) for m in re.findall(r"[XYZE]-?[\d.]+", line.split(";")[0]))
        nx, ny, nz = c.get("X", x), c.get("Y", y), c.get("Z", z)
        e = c.get("E", 0.0)
        # Only object extrusions: none before the first layer (start G-code purge lines).
        if e > 0 and ("X" in c or "Y" in c) and layer_z > 0:
            moves.append((typ, layer_z, x, y, nx, ny, nz, e))
        x, y, z = nx, ny, nz

errors = []
def err(msg):
    errors.append(msg)

for k, v in want.get("header", {}).items():
    if header.get(k) != v:
        err("header %s = %r, expected %r" % (k, header.get(k), v))
if len(moves) < 100:
    err("only %d extrusion moves" % len(moves))

dz = lambda m: m[6] - m[1]
off_plane = [m for m in moves if abs(dz(m)) > 1e-3]
if want.get("planar"):
    if off_plane:
        err("%d extrusions off their layer's Z, e.g. %s" % (len(off_plane), off_plane[0]))
if "min_off_plane" in want and len(off_plane) < want["min_off_plane"]:
    err("only %d extrusions follow the surface, expected at least %d" % (len(off_plane), want["min_off_plane"]))
if "band" in want:
    lo, hi = want["band"]
    bad = [m for m in moves if not (lo - 1e-3 <= dz(m) <= hi + 1e-3)]
    if bad:
        err("%d extrusions outside dz [%g, %g], e.g. dz %.3f %s" % (len(bad), lo, hi, dz(bad[0]), bad[0][0]))
for t in want.get("planar_types", []):
    bad = [m for m in off_plane if m[0] == t]
    if bad:
        err("%d %s extrusions off their layer's Z" % (len(bad), t))
if want.get("flow"):
    # mm of filament per mm of path per mm of local layer thickness, on top surfaces.
    k = sorted(m[7] / (math.hypot(m[4] - m[2], m[5] - m[3]) * (h + dz(m)))
               for m in moves if m[0] == "Top surface" and math.hypot(m[4] - m[2], m[5] - m[3]) > 0.3)
    if len(k) < 10:
        err("only %d top surface moves to check the flow on" % len(k))
    else:
        med = k[len(k) // 2]
        within = sum(abs(v / med - 1) < 0.05 for v in k) / len(k)
        if within < 0.9:
            err("top surface flow follows the layer thickness on only %.0f%% of moves" % (100 * within))
if "contoured_x_below" in want:
    # Per object: contoured moves only left of the given X (the first dome), and some there.
    split = want["contoured_x_below"]
    right = [m for m in off_plane if m[4] > split]
    left = [m for m in off_plane if m[4] < split]
    if right:
        err("%d extrusions contoured on the object without the override, e.g. %s" % (len(right), right[0]))
    if len(left) < 100:
        err("only %d extrusions contoured on the object with the override" % len(left))

if errors:
    print("FAIL: %s: %s" % (tag, "; ".join(errors)))
    sys.exit(1)
print("ok: %s: %d extrusions, %d off-plane" % (tag, len(moves), len(off_plane)))
EOF
}

echo "== 1. Z contouring off by default"
process "$WORK/off.json"
slice off --load-settings "$WORK/machine.json;$WORK/off.json" "$WORK/dome.stl"
check off '{"header": {"zaa_enabled": "0"}, "planar": true}'

echo "== 2. zaa_enabled=1"
process "$WORK/on.json" zaa_enabled=1
slice on --load-settings "$WORK/machine.json;$WORK/on.json" "$WORK/dome.stl"
check on '{"header": {"zaa_enabled": "1"}, "min_off_plane": 500, "band": [-0.2, 0.05], "flow": true}'

echo "== 3. zaa_max_dz=0.05, zaa_inner_walls=0"
process "$WORK/limited.json" zaa_enabled=1 zaa_max_dz=0.05 zaa_inner_walls=0
slice limited --load-settings "$WORK/machine.json;$WORK/limited.json" "$WORK/dome.stl"
check limited '{"header": {"zaa_max_dz": "0.05", "zaa_inner_walls": "0"}, "min_off_plane": 200, "band": [-0.05, 0.05], "planar_types": ["Inner wall"]}'

echo "== 4. per-object override through --load-assemble-list"
"$PY" - "$WORK/assemble.json" "$WORK/dome.stl" <<'EOF'
import json, sys

obj = lambda x, params: {"path": sys.argv[2], "count": 1, "filaments": [1], "pos_x": [x], "pos_y": [100.0],
                         "pos_z": [0.0], "print_params": params}
json.dump({"plates": [{"plate_name": "zaa", "need_arrange": False,
                       "objects": [obj(60.0, {"zaa_enabled": "1"}), obj(140.0, {})]}]},
          open(sys.argv[1], "w"), indent=4)
EOF
slice per_object --load-settings "$WORK/machine.json;$WORK/off.json" --load-assemble-list "$WORK/assemble.json"
check per_object '{"contoured_x_below": 100.0}'

if [ "$fails" -ne 0 ]; then
    echo "FAIL: $fails check(s) failed"
    exit 1
fi
echo "PASS: Z contouring from the headless CLI: off by default, on, limits, per-object override"
