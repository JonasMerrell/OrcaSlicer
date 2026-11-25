#!/usr/bin/env python3
"""End-to-end CLI check of Z-pinning: every z_pin_* setting works headless from a process file and
from a 3MF's per-object settings, and shows up in the G-code as configured.

Runs the real orca-slicer binary with no DISPLAY/WAYLAND_DISPLAY, the way agents run it. Each case
slices a small box and reads the `; Z_PIN_INJECT` blocks back out of the G-code.

usage: test_cli_z_pins.py <orca-slicer binary> <resources/profiles/BBL>
"""
import glob
import json
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

BIN, PROFILES = sys.argv[1], sys.argv[2]
MACHINE = os.path.join(PROFILES, "machine", "Bambu Lab X1 Carbon 0.4 nozzle.json")
PROCESS = "0.20mm Standard @BBL X1C"
H = 0.2
WORK = tempfile.mkdtemp(prefix="orca-cli-zpins.")
ENV = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
ENV["LC_ALL"] = "C"
failures = []


def check(cond, msg):
    if not cond:
        failures.append(msg)
        print("FAIL: " + msg)
    return cond


def box(path, sx, sy, sz):
    """An ASCII STL box, so the test needs no fixture."""
    v = [(x, y, z) for z in (0, sz) for y in (0, sy) for x in (0, sx)]
    with open(path, "w") as f:
        f.write("solid box\n")
        for a, b, c, d in ((0, 2, 3, 1), (4, 5, 7, 6), (0, 1, 5, 4), (2, 6, 7, 3), (0, 4, 6, 2), (1, 3, 7, 5)):
            for tri in ((v[a], v[b], v[c]), (v[a], v[c], v[d])):
                f.write("facet normal 0 0 0\nouter loop\n")
                for p in tri:
                    f.write("vertex %g %g %g\n" % p)
                f.write("endloop\nendfacet\n")
        f.write("endsolid box\n")


def flat_process(overrides):
    """The system process with its `inherits` chain merged in: the CLI resolves `inherits` only
    for presets of its own bundle, so a process file from elsewhere has to be self-contained.
    Writing keys into it is what `orca-slice --set key=value` does."""
    by_name = {}
    for f in glob.glob(os.path.join(PROFILES, "process", "*.json")):
        try:
            d = json.load(open(f, encoding="utf-8"))
            by_name[d.get("name")] = d
        except Exception:
            pass

    def flat(name):
        d = by_name[name]
        out = flat(d["inherits"]) if d.get("inherits") else {}
        out.update(d)
        return out

    d = flat(PROCESS)
    d.pop("inherits", None)
    d.update({"from": "User", "name": "z-pin cli test", "layer_height": str(H),
              "initial_layer_print_height": str(H), "wall_loops": "2",
              "top_shell_layers": "3", "bottom_shell_layers": "3",
              "top_shell_thickness": "0", "bottom_shell_thickness": "0",
              "sparse_infill_density": "20%", "brim_type": "no_brim", "skirt_loops": "0"})
    d.update(overrides)
    return d


def run(tag, args):
    out = os.path.join(WORK, tag)
    os.makedirs(os.path.join(out, "datadir"), exist_ok=True)
    try:
        p = subprocess.run([BIN, "--datadir", os.path.join(out, "datadir")] + args + ["--outputdir", out],
                           env=ENV, stdin=subprocess.DEVNULL, capture_output=True, text=True,
                           errors="replace", timeout=300)
    except subprocess.TimeoutExpired:
        check(False, f"{tag}: the CLI hung (GUI dialog?)")
        return None, "", out
    rc = p.returncode - 256 if p.returncode > 127 else p.returncode
    return rc, p.stdout + p.stderr, out


def slice_process(tag, overrides, models):
    proc = os.path.join(WORK, tag + ".json")
    json.dump(flat_process(overrides), open(proc, "w"), indent=1)
    rc, log, out = run(tag, ["--load-settings", f"{MACHINE};{proc}", "--load-defaultfila",
                             "--arrange", "1", "--orient", "0", "--slice", "0"] + models)
    gcode = os.path.join(out, "plate_1.gcode")
    return rc, log, (open(gcode, errors="replace").read() if os.path.isfile(gcode) else None)


INJ = re.compile(r"; Z_PIN_INJECT X([-\d.]+) Y([-\d.]+) depth ([\d.]+) volume ([\d.]+) mm3 flow ([\d.]+)")
WORD = re.compile(r"([XYZEF])([-\d.]+)")


def injections(gcode):
    """Every Z_PIN_INJECT block: position, layer z, nozzle z, planned depth/volume/flow, the
    filament extruded in it, the dwell after it and the object it belongs to."""
    out, cur, layer_z, obj = [], None, None, None
    pos = {"X": 0., "Y": 0., "Z": 0.}
    for line in gcode.splitlines():
        s = line.strip()
        m = re.match(r"; Z_PINS layer \d+ z ([\d.]+)", s)
        if m:
            layer_z = float(m[1])
        if s.startswith("; printing object "):
            obj = s[len("; printing object "):].split(" id:")[0]
        m = INJ.match(s)
        if m:
            cur = dict(x=float(m[1]), y=float(m[2]), depth=float(m[3]), volume=float(m[4]), flow=float(m[5]),
                       layer_z=layer_z, nozzle_z=pos["Z"], e=0., dwell_ms=0, obj=obj)
            out.append(cur)
            continue
        if cur is not None and s.startswith("G4"):
            p = re.search(r"P(\d+)", s)
            cur["dwell_ms"] += int(p[1]) if p else 0
            continue
        if not s.startswith(("G1", "G0")):
            if cur is not None and s and not s.startswith((";", "M")):
                cur = None
            continue
        w = {k: float(v) for k, v in WORD.findall(s.split(";")[0])}
        if cur is not None:
            if w.get("E", 0.) > 0 and ("X" in w or "Y" in w):
                cur["e"] += w["E"]
            elif "X" in w or "Y" in w or w.get("E", 0.) < 0:
                cur = None
        for k in "XYZ":
            if k in w:
                pos[k] = w[k]
    return out


def footer(gcode, key):
    m = re.search(r"^; %s = (.*)$" % re.escape(key), gcode, re.M)
    return m[1].strip() if m else None


def pins(inj):
    cols = {}
    for i in inj:
        cols.setdefault((round(i["x"], 3), round(i["y"], 3)), []).append(i)
    for lst in cols.values():
        lst.sort(key=lambda i: i["layer_z"])
    return cols


def near_multiple(v, step, tol=0.01):
    return abs(v / step - round(v / step)) * step < tol


cube = os.path.join(WORK, "cube.stl")
box(cube, 20, 20, 10)
fil_area = math.pi * 1.75 ** 2 / 4

# 1. Off by default: the system process slices with no pins.
rc, log, gc = slice_process("off", {}, [cube])
if check(rc == 0 and gc, f"off: slice failed ({rc}): {log[-800:]}"):
    check(not injections(gc), "off: Z_PIN_INJECT in the G-code although z_pins is off")
    check(footer(gc, "z_pins") == "0", "off: z_pins is not 0 by default")

# 2. Every setting at a non-default value on a square grid, read back from the G-code.
SETTINGS = {
    "z_pins": "1", "z_pin_diameter": "2", "z_pin_spacing": "5", "z_pin_pattern": "grid",
    "z_pin_in_sparse_infill": "1", "z_pin_in_inner_walls": "1", "z_pin_in_solid_infill": "0",
    "z_pin_interval": "6", "z_pin_alternate": "1", "z_pin_overfill": "125%",
    "z_pin_injection_speed": "1.5", "z_pin_injection_height": "0.15", "z_pin_dwell": "0.25",
    "z_pin_top_margin": "2", "z_pin_bottom_margin": "2",
}
rc, log, gc = slice_process("grid", SETTINGS, [cube])
if check(rc == 0 and gc, f"grid: slice failed ({rc}): {log[-800:]}"):
    for k, v in SETTINGS.items():
        check(footer(gc, k) == v, f"grid: {k} = {footer(gc, k)!r} in the G-code, expected {v!r}")
    inj = injections(gc)
    if check(len(inj) > 0, "grid: no injections"):
        cols = pins(inj)
        xs = sorted({x for x, _ in cols})
        ys = sorted({y for _, y in cols})
        check(len(cols) >= 4, f"grid: only {len(cols)} pins")
        # z_pin_pattern=grid, z_pin_spacing=5: every pin offset is a multiple of 5 mm in X and in Y.
        check(all(near_multiple(x - xs[0], 5.) for x in xs) and all(near_multiple(y - ys[0], 5.) for y in ys),
              f"grid: pins are not on a 5 mm square grid: {sorted(cols)}")
        for i in inj:
            # z_pin_diameter=2, z_pin_overfill=125%: volume = pi * 1^2 * depth * 1.25.
            check(abs(i["volume"] - math.pi * i["depth"] * 1.25) < 0.01 * i["volume"] + 1e-3,
                  f"grid: volume {i['volume']} for depth {i['depth']}, expected {math.pi * i['depth'] * 1.25:.3f}")
            # z_pin_injection_speed=1.5, under the default filament's max volumetric speed (2).
            check(abs(i["flow"] - 1.5) < 1e-3, f"grid: flow {i['flow']}, expected 1.5")
            # z_pin_injection_height=0.15: the nozzle stays 0.15 mm above the layer, never below.
            check(abs(i["nozzle_z"] - i["layer_z"] - 0.15) < 1e-3,
                  f"grid: nozzle at {i['nozzle_z']} over layer {i['layer_z']}, expected +0.15")
            # z_pin_dwell=0.25 s after each injection.
            check(i["dwell_ms"] == 250, f"grid: dwell {i['dwell_ms']} ms, expected 250")
            # The extruded filament carries the planned volume (times the filament flow ratio).
            fr = float(footer(gc, "filament_flow_ratio") or 1)
            check(abs(i["e"] * fil_area - i["volume"] * fr) < 0.02 * i["volume"] + 1e-3,
                  f"grid: extruded {i['e'] * fil_area:.3f} mm3, planned {i['volume'] * fr:.3f}")
            # z_pin_interval=6: no fill spans more than interval + 1 layers.
            check(i["depth"] <= 7 * H + 1e-6, f"grid: fill {i['depth']} mm deep, interval is 6 layers")
        ends = {}
        for xy, lst in cols.items():
            for k in range(1, len(lst)):
                check(abs(lst[k]["layer_z"] - lst[k]["depth"] - lst[k - 1]["layer_z"]) < 1e-3,
                      f"grid: pin {xy} fill at {lst[k]['layer_z']} does not start where the last ended")
            # 3 bottom shells + z_pin_bottom_margin 2, 3 top shells + z_pin_top_margin 2.
            check(lst[0]["layer_z"] - lst[0]["depth"] >= 5 * H - 1e-3,
                  f"grid: pin {xy} starts at {lst[0]['layer_z'] - lst[0]['depth']}, below the bottom margin")
            check(lst[-1]["layer_z"] <= 10 - 5 * H + 1e-3, f"grid: pin {xy} reaches {lst[-1]['layer_z']}, into the top margin")
            ends[xy] = {round(i["layer_z"], 3) for i in lst[:-1]}
        # z_pin_alternate=1: neighbouring pins do not share joint layers.
        keys = sorted(ends)
        shared = [(a, b) for a in keys for b in keys if a < b and abs(a[0] - b[0]) + abs(a[1] - b[1]) < 5.01
                  and ends[a] & ends[b]]
        check(not shared, f"grid: neighbouring pins with joints on the same layer: {shared[:3]}")

# 3. Staggered pattern, solid infill allowed, sets not alternated.
rc, log, gc = slice_process("stagger", {"z_pins": "1", "z_pin_pattern": "staggered", "z_pin_spacing": "5",
                                        "z_pin_in_solid_infill": "1", "z_pin_alternate": "0",
                                        "z_pin_top_margin": "1", "z_pin_bottom_margin": "1"}, [cube])
if check(rc == 0 and gc, f"stagger: slice failed ({rc}): {log[-800:]}"):
    cols = pins(injections(gc))
    if check(len(cols) >= 2, f"stagger: {len(cols)} pins"):
        ys = sorted({y for _, y in cols})
        # Rows s*sqrt(3)/2 apart; neighbouring rows offset by s/2.
        check(all(near_multiple(y - ys[0], 5 * math.sqrt(3) / 2, 0.02) for y in ys), f"stagger: row spacing {ys}")
        rows = {}
        for x, y in cols:
            rows.setdefault(y, []).append(x)
        if len(rows) >= 2:
            r0, r1 = rows[ys[0]], rows[ys[1]]
            check(all(near_multiple(x - r0[0] - 2.5, 5.) for x in r1), f"stagger: rows not offset by 2.5 mm: {r0} {r1}")
        # z_pin_in_solid_infill: pins go down to the 1-layer margin, through the bottom shells.
        lowest = min(lst[0]["layer_z"] - lst[0]["depth"] for lst in cols.values())
        check(lowest <= 2 * H + 1e-3, f"stagger: with solid infill allowed the lowest pin starts at {lowest}")
        # z_pin_alternate=0: all pins are filled on the same layers.
        layers = [tuple(round(i["layer_z"], 3) for i in lst) for lst in cols.values()]
        check(len(set(layers)) == 1, "stagger: pins filled on different layers with z_pin_alternate=0")

# 4. No region allowed: no pins, no error.
rc, log, gc = slice_process("noregion", {"z_pins": "1", "z_pin_in_sparse_infill": "0", "z_pin_in_inner_walls": "0"},
                            [cube])
if check(rc == 0 and gc, f"noregion: slice failed ({rc}): {log[-800:]}"):
    check(not injections(gc), "noregion: injections with every region switched off")

# 5. Out-of-range values stop the CLI with an error instead of slicing or opening a dialog.
for key, bad in (("z_pin_interval", "1"), ("z_pin_diameter", "-1"), ("z_pin_overfill", "5%")):
    rc, log, gc = slice_process("bad_" + key, {"z_pins": "1", key: bad}, [cube])
    check(rc != 0 and gc is None and key in log, f"{key}={bad}: expected a clean error, got rc {rc}")

# 6. Per-object settings in a 3MF: pins on one of two objects, with its own diameter.
pinned, plain = os.path.join(WORK, "pinned.stl"), os.path.join(WORK, "plain.stl")
box(pinned, 20, 20, 10)
box(plain, 20, 20, 10)
proc = os.path.join(WORK, "project.json")
json.dump(flat_process({}), open(proc, "w"), indent=1)
# --export-3mf takes a name, written into --outputdir.
rc, log, out = run("export", ["--load-settings", f"{MACHINE};{proc}", "--load-defaultfila", "--arrange", "1",
                              "--orient", "0", "--export-3mf", "raw.3mf", pinned, plain])
raw = os.path.join(out, "raw.3mf")
if check(os.path.isfile(raw), f"export: no 3MF written ({rc}): {log[-800:]}"):
    project = os.path.join(WORK, "project.3mf")
    with zipfile.ZipFile(raw) as zin, zipfile.ZipFile(project, "w", zipfile.ZIP_DEFLATED) as zout:
        for item in zin.infolist():
            data = zin.read(item.filename)
            if item.filename == "Metadata/model_settings.config":
                txt, n = re.subn(r'(<object id="\d+"[^>]*>\s*<metadata key="name" value="pinned[^"]*"/>)',
                                 r'\1\n    <metadata key="z_pins" value="1"/>'
                                 r'\n    <metadata key="z_pin_diameter" value="1.2"/>', data.decode())
                check(n == 1, "export: object 'pinned' not found in model_settings.config")
                data = txt.encode()
            zout.writestr(item, data)
    rc, log, out = run("perobject", ["--slice", "0", project])
    gcode = os.path.join(out, "plate_1.gcode")
    if check(rc == 0 and os.path.isfile(gcode), f"perobject: slice failed ({rc}): {log[-800:]}"):
        inj = injections(open(gcode, errors="replace").read())
        if check(inj, "perobject: no injections"):
            objs = {i["obj"] for i in inj}
            check(objs == {"pinned.stl"} or objs == {"pinned"}, f"perobject: injections in {objs}, expected pinned only")
            for i in inj:
                check(abs(i["volume"] - math.pi * 0.36 * i["depth"] * 1.1) < 0.01 * i["volume"] + 1e-3,
                      f"perobject: volume {i['volume']} does not use the object's 1.2 mm diameter")

if failures:
    print(f"work dir kept: {WORK}")
    sys.exit(1)
shutil.rmtree(WORK, ignore_errors=True)
print("PASS: z-pin settings work headless from a process file and per object in a 3MF")
