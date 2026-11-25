#!/usr/bin/env bash
# End-to-end check of the toolhead clearance settings against the real orca-slicer binary.
#
#  - Slicing with toolhead_clearance_enable=1 and a profile writes plate_1.clearance.json next to
#    the G-code. A planar print never collides, so it reports 0 collisions and no warning.
#  - With the setting off (default) no report is written.
#  - --check-toolhead-clearance file.gcode checks existing G-code headless: a nozzle dip next to a
#    printed tower is reported (JSON on stdout, <name>.clearance.json and a toolhead_collision
#    warning in result.json); a planar file is clean; no profile or mesh is an error.
#  - The settings come in through the process file too (how orca-slice --set passes them).
#
# usage: test_cli_toolhead_clearance.sh <orca-slicer binary> <python3>
set -u

BIN="${1:-}"
PY="${2:-python3}"
# 77 is the test's SKIP_RETURN_CODE.
[ -x "$BIN" ] || { echo "SKIP: orca-slicer binary not found: $BIN"; exit 77; }

# From src/libslic3r/Utils.hpp; the shell sees them modulo 256.
CLI_SUCCESS=0
CLI_INVALID_PARAMS=-2

WORK="$(mktemp -d "${TMPDIR:-/tmp}/orca-cli-clearance.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/datadir"

PROFILE='["1.5x0", "12x1", "47x1.13", "1000x29.2"]'

machine() { # machine <file> <enable 0|1> [profile json]
    cat > "$1" <<EOF
{
    "type": "machine",
    "from": "User",
    "name": "CLI clearance test printer",
    "printable_area": ["0x0", "200x0", "200x200", "0x200"],
    "printable_height": "100",
    "layer_change_gcode": "G92 E0",
    "toolhead_clearance_enable": "$2",
    "toolhead_clearance_profile": ${3:-[]}
}
EOF
}
machine "$WORK/machine_on.json" 1 "$PROFILE"
machine "$WORK/machine_off.json" 0 "$PROFILE"
machine "$WORK/machine_noprofile.json" 1 "[]"
machine "$WORK/machine_plain.json" 0
cat > "$WORK/process.json" <<'EOF'
{
    "type": "process",
    "from": "User",
    "name": "CLI clearance test process",
    "enable_support": "0"
}
EOF
# Printer keys in the process file, as orca-slice --set writes them.
cat > "$WORK/process_set.json" <<EOF
{
    "type": "process",
    "from": "User",
    "name": "CLI clearance test process with printer overrides",
    "enable_support": "0",
    "toolhead_clearance_enable": "1",
    "toolhead_clearance_profile": $PROFILE,
    "toolhead_clearance_margin": "0.3"
}
EOF

"$PY" - "$WORK/cube.stl" "$WORK/dip.gcode" "$WORK/planar.gcode" <<'EOF'
import sys

def box(x0, y0, z0, x1, y1, z1):
    v = [(x, y, z) for z in (z0, z1) for y in (y0, y1) for x in (x0, x1)]
    for a, b, c, d in ((0, 2, 3, 1), (4, 5, 7, 6), (0, 1, 5, 4), (2, 6, 7, 3), (0, 4, 6, 2), (1, 3, 7, 5)):
        yield v[a], v[b], v[c]
        yield v[a], v[c], v[d]

with open(sys.argv[1], "w") as f:
    f.write("solid cube\n")
    for tri in box(0, 0, 0, 20, 20, 10):
        f.write("facet normal 0 0 0\nouter loop\n")
        for p in tri:
            f.write("vertex %g %g %g\n" % p)
        f.write("endloop\nendfacet\n")
    f.write("endsolid cube\n")

def tower(lines, top_layers):
    lines += ["G90", "M83", ";WIDTH:0.45"]
    for l in range(1, top_layers + 1):
        lines.append("G1 X95 Y100 Z%.2f F6000" % (0.2 * l))
        for i in range(23):
            y = 100 + i * 0.45
            lines += ["G1 X95 Y%.3f" % y, "G1 X105 Y%.3f E0.4" % y]

# A 4 mm tower, then a non-planar extrusion that dips to Z 2 just in front of it: the
# 1 mm step of the profile (out to r 12) is over the tower, 4.0 - 0.1 - 0.8 > 2.
dip = []
tower(dip, 20)
dip += ["G1 Z5", "G1 X100 Y80", "G1 X100 Y95 Z2.0 E0.5", "G1 Z10"]
open(sys.argv[2], "w").write("\n".join(dip) + "\n")
DIP_LINE = len(dip) - 1   # 1-based line of the dip move
open(sys.argv[2] + ".line", "w").write(str(DIP_LINE))

planar = []
tower(planar, 20)
planar += ["G1 Z4.2", "G1 X60 Y60", "G1 Z10"]
open(sys.argv[3], "w").write("\n".join(planar) + "\n")
EOF

fails=0
fail() { echo "FAIL: $*"; fails=$((fails + 1)); }

# slice <tag> <machine> <process>
slice() {
    local out="$WORK/$1"; mkdir -p "$out"
    timeout 300 "$BIN" --datadir "$WORK/datadir" --load-settings "$2;$3" --slice 0 --outputdir "$out" "$WORK/cube.stl" > "$out/log" 2>&1
    echo $? > "$out/status"
}
# check <tag> <gcode> <settings>
check() {
    local out="$WORK/$1"; mkdir -p "$out"
    timeout 120 "$BIN" --datadir "$WORK/datadir" --load-settings "$3" --check-toolhead-clearance "$2" --outputdir "$out" \
        > "$out/stdout" 2> "$out/log"
    echo $? > "$out/status"
}
expect_status() {
    local got; got="$(cat "$WORK/$1/status")"
    [ "$got" -eq $(( $2 & 255 )) ] || fail "$1: shell status $got, want $(( $2 & 255 ))"
}
# expect_report <tag> <file> <collisions: 0|some> [first line]
expect_report() {
    "$PY" - "$WORK/$1/$2" "$3" "${4:-}" <<'EOF' || fail "$1: $2"
import json, sys
path, want, line = sys.argv[1], sys.argv[2], sys.argv[3]
try:
    # stdout also carries the CLI's own exit line; the report is the line that is a JSON object
    lines = [l for l in open(path).read().splitlines() if l.startswith("{")]
    r = json.loads(lines[-1])
except (OSError, ValueError, IndexError) as e:
    sys.exit("cannot read %s: %s" % (path, e))
errs = []
if r.get("ran") is not True:
    errs.append("ran %r" % r.get("ran"))
n = r.get("collisions")
if (want == "0") != (n == 0):
    errs.append("collisions %r, want %s" % (n, want))
if line and (not r.get("first") or r["first"][0].get("line") != int(line)):
    errs.append("first %r, want line %s" % (r.get("first"), line))
for e in errs:
    print(e)
sys.exit(1 if errs else 0)
EOF
}
# expect_warning <tag> yes|no
expect_warning() {
    "$PY" - "$WORK/$1/result.json" "$2" <<'EOF' || fail "$1: result.json warnings"
import json, sys
try:
    r = json.load(open(sys.argv[1]))
except (OSError, ValueError) as e:
    sys.exit("cannot read %s: %s" % (sys.argv[1], e))
found = any(isinstance(w, dict) and w.get("class") == "toolhead_collision" for w in r.get("warnings", []))
if found != (sys.argv[2] == "yes"):
    sys.exit("toolhead_collision warning %s in %r" % ("present" if found else "missing", r.get("warnings")))
EOF
}

echo "== slicing with the check on writes a clean report"
slice on "$WORK/machine_on.json" "$WORK/process.json"
expect_status on $CLI_SUCCESS
expect_report on plate_1.clearance.json 0
expect_warning on no
grep -q '^; toolhead_clearance_profile = 1.5x0,12x1,47x1.13,1000x29.2' "$WORK/on/plate_1.gcode" || fail "on: profile not in the G-code config"

echo "== with the check off there is no report"
slice off "$WORK/machine_off.json" "$WORK/process.json"
expect_status off $CLI_SUCCESS
[ -e "$WORK/off/plate_1.clearance.json" ] && fail "off: a report was written"

echo "== printer keys in the process file (orca-slice --set) turn it on"
slice set "$WORK/machine_plain.json" "$WORK/process_set.json"
expect_status set $CLI_SUCCESS
expect_report set plate_1.clearance.json 0
grep -q '^; toolhead_clearance_margin = 0.3' "$WORK/set/plate_1.gcode" || fail "set: margin override not applied"

echo "== --check-toolhead-clearance finds the dip"
check dip "$WORK/dip.gcode" "$WORK/machine_on.json"
expect_status dip $CLI_SUCCESS
expect_report dip stdout some "$(cat "$WORK/dip.gcode.line")"
expect_report dip dip.clearance.json some "$(cat "$WORK/dip.gcode.line")"
expect_warning dip yes

echo "== --check-toolhead-clearance on planar G-code is clean, even with the setting off"
check planar "$WORK/planar.gcode" "$WORK/machine_off.json"
expect_status planar $CLI_SUCCESS
expect_report planar stdout 0
expect_warning planar no

echo "== --check-toolhead-clearance with process overrides"
check dipset "$WORK/dip.gcode" "$WORK/machine_plain.json;$WORK/process_set.json"
expect_status dipset $CLI_SUCCESS
expect_report dipset stdout some

echo "== no profile or mesh is an error"
check noprofile "$WORK/dip.gcode" "$WORK/machine_noprofile.json"
expect_status noprofile $CLI_INVALID_PARAMS
grep -q "no toolhead_clearance_profile or toolhead_clearance_mesh" "$WORK/noprofile/log" || fail "noprofile: message missing"

echo "== a missing G-code file is an error"
check missing "$WORK/nope.gcode" "$WORK/machine_on.json"
[ "$(cat "$WORK/missing/status")" -ne 0 ] || fail "missing: exit 0"

if [ "$fails" -ne 0 ]; then
    echo "$fails check(s) failed; work dir kept: $WORK"
    trap - EXIT
    exit 1
fi
echo "all toolhead clearance CLI checks passed"
