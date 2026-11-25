#!/usr/bin/env bash
# End-to-end check of --calib (the Calibration menu without the GUI) against the real orca-slicer binary.
#
# Every calibration mode is generated and sliced headless with a standalone Klipper printer, and the
# G-code is checked for what the test is about: the pressure advance / temperature / speed / retraction /
# input shaper / cornering steps, the PA pattern lines, the flow-rate objects. Also checked: the
# calibration report in result.json, that a setting typed on the command line wins over the
# calibration's, that brim_type = no_brim keeps the calibration's object brims out, and that bad
# input fails with a clear message.
#
# usage: test_cli_calibration.sh <orca-slicer binary> <python3>
set -u

BIN="${1:-}"
PY="${2:-python3}"
# 77 is the test's SKIP_RETURN_CODE.
[ -x "$BIN" ] || { echo "SKIP: orca-slicer binary not found: $BIN"; exit 77; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/orca-cli-calib.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/datadir"
export LC_ALL=C
unset DISPLAY WAYLAND_DISPLAY

cat > "$WORK/machine.json" <<'EOF'
{
    "type": "machine", "from": "User", "name": "CLI calib test printer",
    "gcode_flavor": "klipper",
    "printable_area": ["0x0", "250x0", "250x250", "0x250"],
    "printable_height": "250",
    "nozzle_diameter": ["0.4"],
    "machine_max_speed_x": ["500"], "machine_max_speed_y": ["500"],
    "machine_max_acceleration_extruding": ["10000"],
    "machine_max_jerk_x": ["9"], "machine_max_jerk_y": ["9"],
    "layer_change_gcode": "G92 E0"
}
EOF
cat > "$WORK/process.json" <<'EOF'
{
    "type": "process", "from": "User", "name": "CLI calib test process",
    "layer_height": "0.2", "initial_layer_print_height": "0.2",
    "brim_type": "no_brim", "skirt_loops": "0", "enable_support": "0"
}
EOF
cat > "$WORK/filament.json" <<'EOF'
{
    "type": "filament", "from": "User", "name": "CLI calib test filament",
    "filament_max_volumetric_speed": ["20"], "nozzle_temperature": ["220"],
    "enable_pressure_advance": ["1"], "pressure_advance": ["0.04"]
}
EOF

fail=0
run() { # run <name> <args...>: slice into $WORK/<name>, keep the exit code in $WORK/<name>/rc
    local name="$1"; shift
    mkdir -p "$WORK/$name"
    "$BIN" --datadir "$WORK/datadir" --load-settings "$WORK/machine.json;$WORK/process.json" \
        --load-filaments "$WORK/filament.json" --slice 0 --outputdir "$WORK/$name" "$@" \
        > "$WORK/$name/log" 2>&1
    echo $? > "$WORK/$name/rc"
}

run pa_line            --calib pa_line
run pa_pattern         --calib pa_pattern --calib-params "speeds=60:120,accels=3000"
run pa_tower           --calib pa_tower --calib-params "start=0,end=0.05,step=0.005"
run flow_rate          --calib flow_rate --calib-params "pass=1"
run flow_rate_yolo     --calib flow_rate_yolo --calib-params "pass=2,pattern=monotonic"
run temp_tower         --calib temp_tower --calib-params "start=230,end=210"
run vol_speed          --calib vol_speed --calib-params "start=5,end=10,step=1"
run vfa                --calib vfa --calib-params "start=40,end=100,step=20"
run retraction         --calib retraction --calib-params "start=0,end=1,step=0.5" --wall-loops 3
run is_freq            --calib input_shaping_freq --calib-params "freq_x=20:60,shaper=mzv"
run is_damp            --calib input_shaping_damp --calib-params "freq_x=40,start=0.05,end=0.3"
run cornering          --calib cornering --calib-params "start=2,end=10"
run bad_mode           --calib pa_spiral
run bad_param          --calib temp_tower --calib-params "step=7"
run bad_value          --calib pa_tower --calib-params "end=abc"
run with_model         --calib pa_line "$WORK/machine.json"

"$PY" - "$WORK" <<'EOF'
import glob, json, os, re, sys

work = sys.argv[1]
failures = []

def check(cond, msg):
    if not cond:
        failures.append(msg)
    return cond

def load(name):
    d = os.path.join(work, name)
    rc = int(open(os.path.join(d, "rc")).read())
    res = {}
    if os.path.exists(os.path.join(d, "result.json")):
        res = json.load(open(os.path.join(d, "result.json")))
    gcodes = sorted(glob.glob(os.path.join(d, "*.gcode")))
    gcode = open(gcodes[0], errors="replace").read() if gcodes else ""
    log = open(os.path.join(d, "log"), errors="replace").read()
    return rc, res, gcode, log

def ok(name):
    rc, res, gcode, log = load(name)
    if not check(rc == 0 and gcode, f"{name}: slice failed (exit {rc}): {res.get('error_string')} {log[-800:]}"):
        return None, None
    check(res.get("calibration", {}).get("mode") is not None, f"{name}: no calibration report in result.json")
    return res, gcode

def values(gcode, pattern):
    return [float(m) for m in re.findall(pattern, gcode)]

# Feed rates set inside outer wall sections (spiral vase sets F on a line of its own).
def outer_wall_speeds(gcode):
    speeds, in_wall = set(), False
    for line in gcode.splitlines():
        if line.startswith(";TYPE:"):
            in_wall = line.strip() == ";TYPE:Outer wall"
        elif in_wall and line.startswith("G1"):
            m = re.search(r"F(\d+(\.\d+)?)", line)
            if m:
                speeds.add(round(float(m.group(1))))
    return speeds

res, g = ok("pa_line")
if g:
    pa = set(values(g, r"SET_PRESSURE_ADVANCE ADVANCE=([\d.]+)"))
    check(len(pa) >= 10, f"pa_line: expected many PA values, got {sorted(pa)[:20]}")

res, g = ok("pa_pattern")
if g:
    check(g.count("; start pressure advance pattern for layer") >= 2, "pa_pattern: no pattern custom G-code")
    check(len(res["calibration"]["objects"]) == 2, f"pa_pattern: expected 2 tests (2 speeds x 1 accel), got {res['calibration']['objects']}")
    pa = set(values(g, r"SET_PRESSURE_ADVANCE ADVANCE=([\d.]+)"))
    check(len(pa) >= 10, f"pa_pattern: expected the PA steps, got {sorted(pa)[:20]}")

res, g = ok("pa_tower")
if g:
    pa = sorted(set(values(g, r"SET_PRESSURE_ADVANCE ADVANCE=([\d.]+)")))
    check(len(pa) >= 5 and abs(pa[-1] - 0.05) < 0.0051, f"pa_tower: expected PA 0..0.05, got {pa}")
    check(";TYPE:Brim" not in g and "; FEATURE: Brim" not in g, "pa_tower: printed a brim although brim_type is no_brim")
    check("brim_type" in res["calibration"]["brim_skipped"], f"pa_tower: brim_skipped = {res['calibration']['brim_skipped']}")

for name, n_min in (("flow_rate", 5), ("flow_rate_yolo", 5)):
    res, g = ok(name)
    if g:
        ratios = res["calibration"].get("flow_ratios", {})
        check(len(ratios) >= n_min and len(set(round(v, 4) for v in ratios.values())) >= n_min,
              f"{name}: expected one flow ratio per test object, got {ratios}")

res, g = ok("temp_tower")
if g:
    temps = sorted(set(values(g, r"M104 S(\d+)")))
    check(210 in temps and 230 in temps and 220 in temps, f"temp_tower: expected 230..210 in 5 degree steps, got {temps}")
    check(";TYPE:Brim" not in g, "temp_tower: printed a brim although brim_type is no_brim")

res, g = ok("vol_speed")
if g:
    sp = outer_wall_speeds(g)
    check(len(sp) >= 5, f"vol_speed: expected the outer wall speed to step up, got {sorted(sp)}")
    check(res["calibration"].get("volumetric_end") == 10, "vol_speed: volumetric_end not reported")

res, g = ok("vfa")
if g:
    sp = outer_wall_speeds(g)
    want = {40 * 60, 60 * 60, 80 * 60, 100 * 60}
    check(want <= sp, f"vfa: expected outer wall at 40/60/80/100 mm/s, got {sorted(sp)}")

res, g = ok("retraction")
if g:
    lengths = sorted(set(values(g, r"; Calib_Retraction_tower: Z_HEIGHT: [\d.]+, length:([\d.]+)")))
    check(lengths[:1] == [0.0] and 1.0 in lengths, f"retraction: expected lengths 0..1, got {lengths}")
    check("wall_loops" in res["calibration"]["user_overrides"], "retraction: --wall-loops 3 not listed in user_overrides")
    check("; wall_loops = 3" in g, "retraction: --wall-loops 3 was not kept")

res, g = ok("is_freq")
if g:
    check("SHAPER_TYPE=MZV" in g, "is_freq: no SET_INPUT_SHAPER SHAPER_TYPE=MZV")
    f = sorted(set(values(g, r"SET_INPUT_SHAPER.*SHAPER_FREQ_X=([\d.]+)")))
    check(len(f) >= 5 and f[0] >= 19.9 and f[-1] <= 60.1, f"is_freq: expected X frequencies 20..60, got {f[:5]}..{f[-5:]}")

res, g = ok("is_damp")
if g:
    d = sorted(set(values(g, r"SET_INPUT_SHAPER.*DAMPING_RATIO_X=([\d.]+)")))
    check(len(d) >= 5 and d[0] >= 0.049 and d[-1] <= 0.301, f"is_damp: expected damping 0.05..0.3, got {d[:5]}..{d[-5:]}")
    check("SHAPER_FREQ_X=40.00" in g, "is_damp: fixed frequency 40 Hz not set")

res, g = ok("cornering")
if g:
    scv = sorted(set(values(g, r"SQUARE_CORNER_VELOCITY=([\d.]+)")))
    check(len(scv) >= 5 and scv[0] >= 1.99 and scv[-1] <= 10.01, f"cornering: expected square corner velocity 2..10, got {scv}")

for name, needle in (("bad_mode", "unknown --calib mode"), ("bad_param", "not a parameter"), ("bad_value", "not a number"),
                     ("with_model", "cannot be combined with model files")):
    rc, res, gcode, log = load(name)
    check(rc != 0, f"{name}: expected a failure")
    check(not gcode, f"{name}: wrote G-code although it failed")
    check(needle in res.get("error_string", "") and needle in log, f"{name}: error should say '{needle}', got {res.get('error_string')!r}")

for f in failures:
    print("FAIL:", f)
print(f"{len(failures)} failure(s)")
sys.exit(1 if failures else 0)
EOF
