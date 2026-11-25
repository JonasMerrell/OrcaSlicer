#!/usr/bin/env bash
# End-to-end check of non-planar sparse infill through the real orca-slicer binary, run headless
# (no DISPLAY / WAYLAND_DISPLAY).
#
#  1. Every nonplanar_infill_* key set in a process JSON (what `orca-slice --set key=value` writes)
#     is taken as given: echoed in the G-code's config block and not substituted. The sparse
#     infill leaves the layer plane by up to the effective amplitude (here capped by
#     nonplanar_infill_max_dz); every other extrusion stays on the plane.
#  2. Off by default: the same model with no nonplanar keys is flat.
#  3. Per object and per height range (--load-assemble-list print_params / range_params): only
#     the object, or only the Z range, that turns it on gets waves.
#  4. A value the parser cannot accept is reported as a config_value_substituted warning.
#
# usage: test_cli_nonplanar_infill.sh <orca-slicer binary> <python3>
set -u

BIN="${1:-}"
PY="${2:-python3}"
[ -x "$BIN" ] || { echo "SKIP: orca-slicer binary not found: $BIN"; exit 77; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/orca-cli-nonplanar.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/datadir"

cat > "$WORK/machine.json" <<'EOF'
{
    "type": "machine",
    "from": "User",
    "name": "CLI nonplanar test printer",
    "printable_area": ["0x0", "200x0", "200x200", "0x200"],
    "printable_height": "100",
    "layer_change_gcode": "G92 E0"
}
EOF
# The common part of the processes below.
base_process() {
    cat <<EOF
    "type": "process",
    "from": "User",
    "name": "$1",
    "layer_height": "0.2",
    "initial_layer_print_height": "0.2",
    "sparse_infill_density": "25%",
    "sparse_infill_pattern": "rectilinear",
    "top_shell_layers": "3",
    "bottom_shell_layers": "3",
    "enable_support": "0",
    "brim_type": "no_brim",
    "gcode_comments": "1"
EOF
}
{ echo "{"; base_process "CLI nonplanar off"; echo "}"; } > "$WORK/process_off.json"
# Every key with a value other than its default. The amplitude (0.08 mm) is above the maximum Z
# offset (0.07 mm), so the waves must stop at 0.07 mm.
{ echo "{"; base_process "CLI nonplanar on"; cat <<'EOF'
    ,
    "nonplanar_infill": "1",
    "nonplanar_infill_amplitude": "0.08",
    "nonplanar_infill_wavelength": "6",
    "nonplanar_infill_phase_shift": "45",
    "nonplanar_infill_angle": "30",
    "nonplanar_infill_pattern": "egg_crate",
    "nonplanar_infill_blend_layers": "3",
    "nonplanar_infill_edge_blend": "1.5",
    "nonplanar_infill_max_dz": "0.07"
}
EOF
} > "$WORK/process_on.json"
{ echo "{"; base_process "CLI nonplanar bad enum"; cat <<'EOF'
    ,
    "nonplanar_infill": "1",
    "nonplanar_infill_pattern": "zigzag"
}
EOF
} > "$WORK/process_bad_enum.json"

"$PY" - "$WORK" <<'EOF'
import sys

def box(x0, y0, z0, x1, y1, z1):
    v = [(x, y, z) for z in (z0, z1) for y in (y0, y1) for x in (x0, x1)]
    for a, b, c, d in ((0, 2, 3, 1), (4, 5, 7, 6), (0, 1, 5, 4), (2, 6, 7, 3), (0, 4, 6, 2), (1, 3, 7, 5)):
        yield v[a], v[b], v[c]
        yield v[a], v[c], v[d]

def write_stl(path, name, triangles):
    with open(path, "w") as f:
        f.write("solid %s\n" % name)
        for tri in triangles:
            f.write("facet normal 0 0 0\nouter loop\n")
            for p in tri:
                f.write("vertex %g %g %g\n" % p)
            f.write("endloop\nendfacet\n")
        f.write("endsolid %s\n" % name)

for name in ("cube", "wavy", "plain", "ranged"):
    write_stl("%s/%s.stl" % (sys.argv[1], name), name, box(0, 0, 0, 20, 20, 20))
EOF

cat > "$WORK/assemble.json" <<EOF
{
    "plates": [{
        "plate_name": "nonplanar",
        "need_arrange": false,
        "objects": [
            { "path": "$WORK/wavy.stl",  "count": 1, "filaments": [1], "pos_x": [40],  "pos_y": [40],
              "print_params": { "nonplanar_infill": "1", "nonplanar_infill_amplitude": "0.1" } },
            { "path": "$WORK/plain.stl", "count": 1, "filaments": [1], "pos_x": [90],  "pos_y": [40] },
            { "path": "$WORK/ranged.stl", "count": 1, "filaments": [1], "pos_x": [140], "pos_y": [40],
              "height_ranges": [{ "min_z": 0, "max_z": 10,
                                  "range_params": { "nonplanar_infill": "1", "nonplanar_infill_amplitude": "0.1" } }] }
        ]
    }]
}
EOF

fails=0
fail() { echo "FAIL: $*"; fails=$((fails + 1)); }

# run <tag> <process json> [option...] [model]: slice headless into $WORK/<tag>.
run() {
    local out="$WORK/$1"; local process="$2"; shift 2
    mkdir -p "$out"
    env -u DISPLAY -u WAYLAND_DISPLAY timeout 600 "$BIN" --datadir "$WORK/datadir" \
        --load-settings "$WORK/machine.json;$process" "$@" --slice 0 --outputdir "$out" > "$out/log" 2>&1
    echo $? > "$out/status"
    [ "$(cat "$out/status")" -eq 0 ] || { fail "$(basename "$out"): exit status $(cat "$out/status")"; tail -20 "$out/log"; }
    compgen -G "$out/*.gcode" > /dev/null || fail "$(basename "$out"): no G-code"
}

# check <tag> <python checks...>: parse the tag's G-code and result.json, run the checks.
check() {
    local tag="$1"; shift
    "$PY" - "$WORK/$tag" "$@" <<'EOF' || fail "$tag"
import glob, json, math, re, sys
from collections import defaultdict

out, checks = sys.argv[1], sys.argv[2:]
gcode = sorted(glob.glob(out + "/*.gcode"))[0]
AX = re.compile(r"([XYZE])(-?[\d.]+)")
config, moves = {}, []
role, obj, layer_z = "", "", 0.0
x = y = z = e_abs = 0.0
relative = False
for raw in open(gcode, errors="replace"):
    m = re.match(r"; (\w+) = (.*)$", raw)
    if m:
        config[m.group(1)] = m.group(2).strip()
    if raw.startswith(";TYPE:"):
        role = raw[6:].strip(); continue
    if raw.startswith(";Z:"):
        layer_z = float(raw[3:]); continue
    if raw.startswith("; printing object "):
        obj = raw[len("; printing object "):].split()[0]; continue
    if raw.startswith("M83"): relative = True
    if raw.startswith("M82"): relative = False
    if raw.startswith("G92") and "E" in raw: e_abs = 0.0
    if not raw.startswith(("G1 ", "G0 ")):
        continue
    a = dict((k, float(v)) for k, v in AX.findall(raw.split(";", 1)[0]))
    nx, ny, nz = a.get("X", x), a.get("Y", y), a.get("Z", z)
    de = 0.0
    if "E" in a:
        de = a["E"] if relative else a["E"] - e_abs
        if not relative: e_abs = a["E"]
    if de > 0 and math.hypot(nx - x, ny - y) > 1e-4 and role != "Custom":
        moves.append((obj, role, layer_z, nz - layer_z))
    x, y, z = nx, ny, nz

result = json.load(open(out + "/result.json"))
errors = []
subs = [w for w in result.get("warnings", []) if isinstance(w, dict) and w.get("class") == "config_value_substituted"]

def sparse_dz(obj_prefix="", zmin=-1e9, zmax=1e9):
    return [abs(d) for o, r, lz, d in moves if r == "Sparse infill" and o.startswith(obj_prefix) and zmin <= lz <= zmax]

for c in checks:
    kind, _, arg = c.partition(":")
    if kind == "config":                      # config:key=value echoed in the G-code
        k, v = arg.split("=", 1)
        if config.get(k) != v:
            errors.append("config %s = %r, want %r" % (k, config.get(k), v))
    elif kind == "no_substitution":
        if subs:
            errors.append("unexpected substitutions %r" % subs)
    elif kind == "substituted":               # substituted:key
        if not any(w.get("key") == arg for w in subs):
            errors.append("no config_value_substituted warning for %s in %r" % (arg, subs))
    elif kind == "flat_others":               # nothing but sparse infill leaves the plane
        bad = [(o, r, lz, d) for o, r, lz, d in moves if r != "Sparse infill" and abs(d) > 1e-3]
        if bad:
            errors.append("%d non-sparse moves off the plane, e.g. %r" % (len(bad), bad[:3]))
    elif kind == "waves":                     # waves:obj,zmin,zmax,min_peak,max_peak
        o, zmin, zmax, lo, hi = arg.split(",")
        d = sparse_dz(o, float(zmin), float(zmax))
        if not d:
            errors.append("waves %s: no sparse infill" % arg)
        elif not (float(lo) <= max(d) <= float(hi)):
            errors.append("waves %s: peak %.4f outside [%s, %s]" % (arg, max(d), lo, hi))
        elif sum(1 for v in d if v > 0.01) < 50:
            errors.append("waves %s: only %d waved moves" % (arg, sum(1 for v in d if v > 0.01)))
    elif kind == "flat":                      # flat:obj,zmin,zmax
        o, zmin, zmax = arg.split(",")
        d = sparse_dz(o, float(zmin), float(zmax))
        if not d:
            errors.append("flat %s: no sparse infill" % arg)
        elif max(d) > 1e-3:
            errors.append("flat %s: sparse infill leaves the plane by %.4f" % (arg, max(d)))
    else:
        errors.append("unknown check %r" % c)
for e in errors:
    print(e)
sys.exit(1 if errors else 0)
EOF
}

echo "== every nonplanar key is taken from the process, headless, and shapes the sparse infill"
run on "$WORK/process_on.json" "$WORK/cube.stl"
check on no_substitution flat_others \
    config:nonplanar_infill=1 config:nonplanar_infill_amplitude=0.08 config:nonplanar_infill_wavelength=6 \
    config:nonplanar_infill_phase_shift=45 config:nonplanar_infill_angle=30 config:nonplanar_infill_pattern=egg_crate \
    config:nonplanar_infill_blend_layers=3 config:nonplanar_infill_edge_blend=1.5 config:nonplanar_infill_max_dz=0.07 \
    "waves:,0,100,0.06,0.0705"

echo "== off by default"
run off "$WORK/process_off.json" "$WORK/cube.stl"
check off no_substitution flat_others config:nonplanar_infill=0 "flat:,0,100"

echo "== per object and per height range through an assemble list"
run objects "$WORK/process_off.json" --load-assemble-list "$WORK/assemble.json"
check objects no_substitution flat_others \
    "waves:wavy,0,100,0.09,0.1005" "flat:plain,0,100" \
    "waves:ranged,0,10,0.09,0.1005" "flat:ranged,10.5,100"

echo "== a pattern the parser cannot accept is reported"
run bad_enum "$WORK/process_bad_enum.json" "$WORK/cube.stl"
check bad_enum substituted:nonplanar_infill_pattern

if [ "$fails" -ne 0 ]; then
    echo "$fails check(s) failed"
    exit 1
fi
echo "all nonplanar infill CLI checks passed"
