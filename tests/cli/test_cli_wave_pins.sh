#!/usr/bin/env bash
# End-to-end check of pin supports under wave overhangs (wave_overhang_pin_*) through the headless CLI.
#
# Slices a 30 mm cantilevered shelf (the wave layer is z 3.2, the layer under it z 3.0) with no
# display (DISPLAY and WAYLAND_DISPLAY unset) and supports off, and checks the G-code:
#  1. pins off (the default): no support extrusions at all.
#  2. wave_overhang_pin_supports=1: support extrusions on every layer from the bed up to z 3.0, the
#     layer right under the wave layer (no gap), none above; the wave layer and the layers above
#     print the same extrusions as without pins.
#  3. A denser wave_overhang_pin_spacing gives more support extrusions on the tip layer.
#  4. A per-object override through --load-assemble-list (print_params): two shelves, pins on the
#     first only.
#
# usage: test_cli_wave_pins.sh <orca-slicer binary> <python3>
set -u

BIN="${1:-}"
PY="${2:-python3}"
# 77 is the test's SKIP_RETURN_CODE.
[ -x "$BIN" ] || { echo "SKIP: orca-slicer binary not found: $BIN"; exit 77; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/orca-cli-wave-pins.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/datadir"

cat > "$WORK/machine.json" <<'EOF'
{
    "type": "machine",
    "from": "User",
    "name": "CLI wave pins test printer",
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
    "name": "CLI wave pins test filament",
    "filament_diameter": ["1.75"]
}
EOF
process() {
    "$PY" - "$@" <<'EOF'
import json, sys

cfg = {"type": "process", "from": "User", "name": "CLI wave pins test process",
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

# stats <tag>: JSON with the support extrusions and all extrusions per layer Z, the X range of the
# support, and the pin header line.
stats() {
    "$PY" - "$WORK/$1/plate_1.gcode" <<'EOF'
import json, re, sys
from collections import defaultdict
layer_z, typ, x = 0.0, "", 0.0
support, ext, xs, header = defaultdict(int), defaultdict(int), [], {}
for line in open(sys.argv[1]):
    if line.startswith(";Z:"):
        layer_z = round(float(line[3:]), 2)
    elif line.startswith(";TYPE:"):
        typ = line[6:].strip()
    elif line.startswith("; wave_overhang_pin_supports ="):
        header["pins"] = line.split("=", 1)[1].strip()
    elif line.startswith("G1 "):
        c = dict((m[0], float(m[1:])) for m in re.findall(r"[XYE]-?[\d.]+", line.split(";")[0]))
        x = c.get("X", x)
        if c.get("E", 0) > 0 and ("X" in c or "Y" in c):
            ext[str(layer_z)] += 1
            if typ == "Support":
                support[str(layer_z)] += 1
                xs.append(x)
print(json.dumps({"support": support, "ext": ext, "header": header,
                  "xmin": min(xs) if xs else None, "xmax": max(xs) if xs else None}))
EOF
}

echo "== 1. pins off by default"
process "$WORK/off.json"
slice off --load-settings "$WORK/machine.json;$WORK/off.json" "$WORK/shelf.stl"
OFF="$(stats off)"
"$PY" -c 'import json,sys; s=json.loads(sys.argv[1]); sys.exit(0 if not s["support"] and s["header"].get("pins")=="0" else 1)' "$OFF" \
    || fail "off: support extrusions or header wrong: $OFF"

echo "== 2. wave_overhang_pin_supports=1"
process "$WORK/on.json" wave_overhang_pin_supports=1
slice on --load-settings "$WORK/machine.json;$WORK/on.json" "$WORK/shelf.stl"
ON="$(stats on)"
"$PY" - "$OFF" "$ON" <<'EOF' || fails=$((fails + 1))
import json, sys
off, on = json.loads(sys.argv[1]), json.loads(sys.argv[2])
errors = []
zs = sorted(float(z) for z in on["support"])
want = [round(0.2 * i, 2) for i in range(1, 16)]
if zs != want:
    errors.append("support on layers %s, expected z 0.2 .. 3.0 every layer" % zs)
if on["header"].get("pins") != "1":
    errors.append("header wave_overhang_pin_supports = %r" % on["header"].get("pins"))
# The wave layer (z 3.2) and the layers above print the same extrusions as without pins.
for z in ("3.2", "3.4", "4.2"):
    if on["ext"].get(z) != off["ext"].get(z):
        errors.append("layer %s: %s extrusions, %s without pins" % (z, on["ext"].get(z), off["ext"].get(z)))
if errors:
    print("FAIL: on: " + "; ".join(errors))
    sys.exit(1)
print("ok: on: support z %.1f..%.1f, %d moves on the tip layer, wave layer unchanged" % (zs[0], zs[-1], on["support"]["3.0"]))
EOF

echo "== 3. denser spacing"
process "$WORK/dense.json" wave_overhang_pin_supports=1 wave_overhang_pin_spacing=2.5
slice dense --load-settings "$WORK/machine.json;$WORK/dense.json" "$WORK/shelf.stl"
DENSE="$(stats dense)"
"$PY" -c 'import json,sys; a,b=json.loads(sys.argv[1]),json.loads(sys.argv[2]); sys.exit(0 if b["support"].get("3.0",0) > a["support"].get("3.0",0) else 1)' "$ON" "$DENSE" \
    || fail "dense: no more support on the tip layer than at 4 mm spacing"

echo "== 4. per-object override through --load-assemble-list"
"$PY" - "$WORK/assemble.json" "$WORK/shelf.stl" <<'EOF'
import json, sys

obj = lambda x, params: {"path": sys.argv[2], "count": 1, "filaments": [1], "pos_x": [x], "pos_y": [100.0],
                         "pos_z": [0.0], "print_params": params}
json.dump({"plates": [{"plate_name": "pins", "need_arrange": False,
                       "objects": [obj(50.0, {"wave_overhang_pin_supports": "1"}), obj(130.0, {})]}]},
          open(sys.argv[1], "w"), indent=4)
EOF
slice per_object --load-settings "$WORK/machine.json;$WORK/off.json" --load-assemble-list "$WORK/assemble.json"
PER="$(stats per_object)"
"$PY" -c 'import json,sys; s=json.loads(sys.argv[1]); sys.exit(0 if s["support"] and s["xmax"] < 100 else 1)' "$PER" \
    || fail "per_object: pins missing on the first shelf or present on the second: $PER"

if [ "$fails" -ne 0 ]; then
    echo "FAIL: $fails check(s) failed"
    exit 1
fi
echo "PASS: wave overhang pin supports from the headless CLI: off by default, on, denser spacing, per-object override"
