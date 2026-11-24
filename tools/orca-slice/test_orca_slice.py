#!/usr/bin/env python3
"""Tests for the orca-slice wrapper. Plain unittest: `python3 tools/orca-slice/test_orca_slice.py`.

Unit tests need nothing. The end-to-end tests slice with the fork binary at ORCA_DIR (default
~/OrcaSlicer/build/package) and are skipped when it is missing; the ones needing the headless-CLI
flags (--list-options, --load-job) are skipped on an older binary. The hub tests start a local
filament hub when HUB_SERVE (path to its `filament-hub` script) is set and never touch a real one.
Nothing here uploads to a printer.
"""
import argparse
import contextlib
import importlib.machinery
import importlib.util
import io
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
WRAPPER = os.path.join(HERE, "orca-slice")


def load_wrapper():
    loader = importlib.machinery.SourceFileLoader("orca_slice", WRAPPER)
    spec = importlib.util.spec_from_loader("orca_slice", loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod


os_ = load_wrapper()

CATALOG = {
    "wall_loops": {"key": "wall_loops", "preset": "process", "scopes": ["global", "object", "part", "layer_range"],
                   "type": "int", "vector": False},
    "sparse_infill_density": {"key": "sparse_infill_density", "preset": "process",
                              "scopes": ["global", "object", "part", "layer_range"], "type": "percent", "vector": False},
    "sparse_infill_pattern": {"key": "sparse_infill_pattern", "preset": "process",
                              "scopes": ["global", "object", "part", "layer_range"], "type": "enum", "vector": False,
                              "enum_values": ["grid", "gyroid", "honeycomb"]},
    "layer_height": {"key": "layer_height", "preset": "process", "scopes": ["global", "object", "layer_range"],
                     "type": "float", "vector": False},
    "nozzle_temperature": {"key": "nozzle_temperature", "preset": "filament", "scopes": ["global"], "type": "ints",
                           "vector": True},
    "retraction_length": {"key": "retraction_length", "preset": "printer", "scopes": ["global"], "type": "floats",
                          "vector": True},
    "printer_notes": {"key": "printer_notes", "preset": "printer", "scopes": ["global"], "type": "string",
                      "vector": False},
    "flush_volumes_matrix": {"key": "flush_volumes_matrix", "preset": "project", "scopes": ["global"],
                             "type": "floats", "vector": True},
    "__cli__": {},
}

LANE_DATA = {"result": {"namespace": "lane_data", "value": {
    "T0": {"lane": "0", "material": "ABS", "spool_id": 21, "name": "SimpleCore ABS Sunlu",
           "orca_preset": "SimpleCore ABS Sunlu", "color": "000000"},
    "T5": {"lane": "5", "material": "PET-CF", "spool_id": 11, "name": "SimpleCore Fiberon PET-CF",
           "orca_preset": "SimpleCore Fiberon PET-CF "},
    "T7": {"lane": "7", "material": "ABS", "orca_preset": "simplecore abs  polymaker"},
    "T8": {"lane": "8", "material": "TPU", "orca_preset": "Not A Preset"},
}}}


def parser():
    ap = argparse.ArgumentParser()
    os_.add_slice_args(ap)
    return ap


class SetRouting(unittest.TestCase):
    def route(self, items, catalog=CATALOG, nfil=2):
        machine = {"retraction_length": ["0.5", "0.5"], "printer_notes": ""}
        process = {"wall_loops": "2"}
        fils = [{"nozzle_temperature": ["250"]} for _ in range(nfil)]
        notes = []
        native = os_.route_sets([os_.parse_set(i) for i in items], catalog, machine, process, fils, notes)
        return machine, process, fils, native, notes

    def test_parse(self):
        self.assertEqual(os_.parse_set("a=b=c"), ("a", None, "b=c"))
        self.assertEqual(os_.parse_set("k[2]=5"), ("k", 2, "5"))
        for bad in ("novalue", "k[0]=1", "=1"):
            with self.assertRaises(os_.UsageError):
                os_.parse_set(bad)

    def test_process_filament_printer_project(self):
        m, p, f, native, notes = self.route(["wall_loops=5", "nozzle_temperature=231", "retraction_length[2]=0.9",
                                             "flush_volumes_matrix=0,100,100,0"])
        self.assertEqual(p["wall_loops"], "5")
        self.assertEqual([x["nozzle_temperature"] for x in f], [["231"], ["231"]])
        self.assertEqual(m["retraction_length"], ["0.5", "0.9"])
        self.assertEqual(native, ["--flush-volumes-matrix", "0,100,100,0"])
        self.assertEqual(notes, [])

    def test_filament_index(self):
        _, _, f, _, _ = self.route(["nozzle_temperature[2]=240"])
        self.assertEqual([x["nozzle_temperature"] for x in f], [["250"], ["240"]])
        with self.assertRaises(os_.UsageError):
            self.route(["nozzle_temperature[3]=240"])

    def test_errors(self):
        with self.assertRaisesRegex(os_.UsageError, "did you mean wall_loops"):
            self.route(["wall_loop=3"])
        with self.assertRaisesRegex(os_.UsageError, "allowed: grid, gyroid, honeycomb"):
            self.route(["sparse_infill_pattern=bogus"])
        with self.assertRaisesRegex(os_.UsageError, "not a per-extruder"):
            self.route(["printer_notes[1]=x"])
        with self.assertRaisesRegex(os_.UsageError, "project setting"):
            self.route(["flush_volumes_matrix[1]=3"])

    def test_fallback_without_catalog(self):
        m, p, f, native, notes = self.route(["nozzle_temperature=231", "retraction_length=1", "anything=3"], catalog=None)
        self.assertEqual(f[0]["nozzle_temperature"], ["231"])
        self.assertEqual(m["retraction_length"], ["1"])
        self.assertEqual(p["anything"], "3")
        self.assertEqual(len(notes), 1)


class JobFromFlags(unittest.TestCase):
    def job(self, args):
        return os_.job_from_flags(parser().parse_args(["part.stl"] + args), "/m/part.stl", CATALOG)

    def test_none(self):
        self.assertIsNone(self.job(["--set", "wall_loops=3", "--orient"]))

    def test_everything(self):
        j = self.job(["--set-object", "wall_loops=6", "--set-object", "part:sparse_infill_density=30%",
                      "--set-range", "0-3:sparse_infill_density=100%,wall_loops=4",
                      "--set-range", "0-3:layer_height=0.1",
                      "--modifier", "box@10,10,5:20,20,10:sparse_infill_density=80%,wall_loops=5",
                      "--negative", "cylinder@0,0,0:3,40", "--cut", "12:lower", "--copies", "3",
                      "--rotate-x", "90", "--lay-flat", "--orient"])
        o = j["objects"][0]
        self.assertEqual(o["file"], "/m/part.stl")
        self.assertEqual(o["settings"], {"wall_loops": "6", "sparse_infill_density": "30%"})
        self.assertEqual(o["height_ranges"], [{"min_z": 0.0, "max_z": 3.0, "settings": {
            "sparse_infill_density": "100%", "wall_loops": "4", "layer_height": "0.1"}}])
        self.assertEqual(o["parts"][0], {"position": [10.0, 10.0, 5.0], "type": "modifier",
                                         "primitive": {"shape": "box", "size": [20.0, 20.0, 10.0]},
                                         "settings": {"sparse_infill_density": "80%", "wall_loops": "5"}})
        self.assertEqual(o["parts"][1]["primitive"], {"shape": "cylinder", "radius": 3.0, "height": 40.0})
        self.assertEqual(o["parts"][1]["type"], "negative")
        self.assertEqual(o["cut"], {"z": 12.0, "keep": "lower"})
        self.assertEqual((o["count"], o["rotate"], o["lay_on"], o["orient"]), (3, [90.0, 0.0, 0.0], "largest_face", True))

    def test_vector_value_keeps_commas(self):
        d = os_._settings_list("wall_loops=3,flush_volumes_matrix=1,2,3", "x", None, "part")
        self.assertEqual(d, {"wall_loops": "3", "flush_volumes_matrix": "1,2,3"})

    def test_scope_errors(self):
        with self.assertRaisesRegex(os_.UsageError, "nozzle_temperature can't be set per object .*filament setting"):
            self.job(["--set-object", "nozzle_temperature=250"])
        with self.assertRaisesRegex(os_.UsageError, "layer_height can't be set per modifier"):
            self.job(["--modifier", "box@0,0,0:1,1,1:layer_height=0.1"])
        with self.assertRaisesRegex(os_.UsageError, "no object named"):
            self.job(["--set-object", "other:wall_loops=3"])
        with self.assertRaisesRegex(os_.UsageError, "take no settings"):
            self.job(["--negative", "box@0,0,0:1,1,1:wall_loops=2"])
        with self.assertRaisesRegex(os_.UsageError, "SHAPE@"):
            self.job(["--modifier", "pyramid@0,0,0:1"])
        with self.assertRaisesRegex(os_.UsageError, "allowed"):
            self.job(["--set-range", "0-1:sparse_infill_pattern=nope"])


class Lanes(unittest.TestCase):
    def test_parse(self):
        avail = {"SimpleCore ABS Sunlu": "", "SimpleCore Fiberon PET-CF": "", "SimpleCore ABS PolyMaker": ""}
        lanes = os_.parse_lanes(LANE_DATA, avail)
        self.assertEqual([l["tool"] for l in lanes], ["T0", "T5", "T7", "T8"])
        got = {l["tool"]: (l["preset"], l["preset_match"]) for l in lanes}
        self.assertEqual(got["T0"], ("SimpleCore ABS Sunlu", "exact"))
        self.assertEqual(got["T5"], ("SimpleCore Fiberon PET-CF", "fuzzy"))   # trailing space
        self.assertEqual(got["T7"], ("SimpleCore ABS PolyMaker", "fuzzy"))
        self.assertEqual(got["T8"], (None, None))

    def test_resolve(self):
        avail = {"SimpleCore ABS Sunlu": "", "SimpleCore ABS PolyMaker": ""}
        orig = os_.fetch_lanes
        os_.fetch_lanes = lambda: ("x", os_.parse_lanes(LANE_DATA, avail))
        try:
            self.assertEqual(os_.resolve_lane_filaments(["@T7"], False), (["SimpleCore ABS PolyMaker"], 7))
            self.assertEqual(os_.resolve_lane_filaments(["@T7"], True), (["SimpleCore ABS PolyMaker"], None))
            with self.assertRaisesRegex(os_.UsageError, "nothing is loaded in T3"):
                os_.resolve_lane_filaments(["@T3"], False)
            with self.assertRaisesRegex(os_.UsageError, "not one of the presets"):
                os_.resolve_lane_filaments(["@T8"], False)
            with contextlib.redirect_stderr(io.StringIO()):
                names, tool = os_.resolve_lane_filaments(["@printer"], False)
            self.assertEqual(len(names), 9)
            self.assertEqual(names[0], "SimpleCore ABS Sunlu")
            self.assertEqual(names[7], "SimpleCore ABS PolyMaker")
            self.assertEqual(names[1], os_.DEFAULT_FILAMENT)
            self.assertIsNone(tool)
            self.assertEqual(os_.resolve_lane_filaments(["A", "B"], False), (["A", "B"], None))
        finally:
            os_.fetch_lanes = orig


class HubConflict(unittest.TestCase):
    def test_conflict_exit_3(self):
        orig_call, orig_presets = os_.hub_call, os_.user_presets
        tmp = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False)
        json.dump({"name": "X", "filament_type": ["PLA"]}, tmp)
        tmp.close()

        def fake(path, method="GET", data=None, need_token=True):
            if path.startswith("/api/v1/profiles"):
                return 200, [{"id": "abc", "name": "X", "type": "filament", "updated_time": 5}]
            self.assertEqual((data["id"], data["original_updated_time"]), ("abc", 5))
            return 409, {"status": "conflict", "code": -1}
        os_.hub_call, os_.user_presets = fake, lambda kind: {"X": tmp.name}
        os.environ.setdefault("FILAMENT_HUB_URL", "http://127.0.0.1:9")
        try:
            with contextlib.redirect_stdout(io.StringIO()) as out, contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(os_.cmd_hub(["push", "X"]), 3)
            self.assertEqual(json.loads(out.getvalue())["status"], "conflict")
        finally:
            os_.hub_call, os_.user_presets = orig_call, orig_presets
            os.unlink(tmp.name)


def run(args, env=None, check=True):
    r = subprocess.run([sys.executable, WRAPPER] + args, capture_output=True, text=True,
                       env=dict(os.environ, **(env or {})), timeout=900)
    if check and r.returncode != 0:
        raise AssertionError(f"orca-slice {args} -> {r.returncode}\n{r.stderr[-3000:]}")
    return r


@unittest.skipUnless(os.environ.get("HUB_SERVE"), "set HUB_SERVE=/path/to/venv/bin/filament-hub for hub tests")
class HubLocal(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        db = os.path.join(self.tmp, "hub.sqlite")
        env = dict(os.environ, FILAMENT_HUB_DB=db)
        token = subprocess.run([os.environ["HUB_SERVE"], "token", "create", "t"], env=env, capture_output=True,
                               text=True, check=True).stdout.strip()
        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
        s.close()
        self.srv = subprocess.Popen([os.environ["HUB_SERVE"], "serve", "--host", "127.0.0.1", "--port", str(port)],
                                    env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.env = {"FILAMENT_HUB_URL": f"http://127.0.0.1:{port}", "FILAMENT_HUB_TOKEN": token,
                    "ORCA_HUB_CACHE": os.path.join(self.tmp, "cache")}
        for _ in range(50):
            if run(["hub", "status"], self.env, check=False).returncode == 0:
                break
            time.sleep(0.2)

    def tearDown(self):
        self.srv.terminate()
        self.srv.wait()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_push_pull(self):
        name = os_.DEFAULT_FILAMENT
        r = json.loads(run(["hub", "push", name], self.env).stdout)
        self.assertTrue(r["created"])
        r = json.loads(run(["hub", "push", name], self.env).stdout)
        self.assertFalse(r["created"])
        lst = json.loads(run(["hub", "list", "--type", "filament"], self.env).stdout)["presets"]
        self.assertEqual([p["name"] for p in lst], [name])
        pulled = json.loads(run(["hub", "pull"], self.env).stdout)["pulled"]
        self.assertTrue(os.path.isfile(pulled[0]["path"]))
        self.assertEqual(json.load(open(pulled[0]["path"]))["name"], name)
        bad = run(["hub", "list"], dict(self.env, FILAMENT_HUB_TOKEN="nope"), check=False)
        self.assertEqual(bad.returncode, 2)
        self.assertNotIn("nope", bad.stderr)
        nok = run(["hub", "list"], dict(self.env, FILAMENT_HUB_TOKEN=""), check=False)
        self.assertIn("FILAMENT_HUB_TOKEN", nok.stderr)


ORCA_BIN = os.path.join(os_.ORCA_DIR, "bin", "orca-slicer")
HAS_BIN = os.access(ORCA_BIN, os.X_OK)


def cube_stl(path, sx=20, sy=20, sz=10, ox=0.0):
    v = [(ox + a * sx, b * sy, c * sz) for a in (0, 1) for b in (0, 1) for c in (0, 1)]
    f = [(0, 1, 3), (0, 3, 2), (4, 6, 7), (4, 7, 5), (0, 4, 5), (0, 5, 1), (2, 3, 7), (2, 7, 6), (0, 2, 6), (0, 6, 4),
         (1, 5, 7), (1, 7, 3)]
    with open(path, "w") as fh:
        fh.write("solid c\n")
        for t in f:
            fh.write(" facet normal 0 0 0\n  outer loop\n")
            for i in t:
                fh.write("   vertex %g %g %g\n" % v[i])
            fh.write("  endloop\n endfacet\n")
        fh.write("endsolid c\n")


def gcode_config(path, key):
    with open(path, errors="replace") as fh:
        for line in fh:
            if line.startswith(f"; {key} = "):
                return line.split(" = ", 1)[1].strip()
    return None


@unittest.skipUnless(HAS_BIN, f"no slicer binary at {ORCA_BIN}")
class EndToEnd(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp()
        cls.cube = os.path.join(cls.tmp, "cube.stl")
        cube_stl(cls.cube)
        cls.new = os_.binary_has("--load-job") and os_.load_catalog() is not None

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def out(self, name):
        return os.path.join(self.tmp, name) + os.sep

    def test_set_filament_key(self):
        r = json.loads(run([self.cube, "-o", self.out("fil"), "--set", "nozzle_temperature=231"]).stdout)
        self.assertEqual(gcode_config(r["gcode"], "nozzle_temperature"), "231")
        self.assertEqual(gcode_config(r["gcode"], "brim_type"), "no_brim")

    def test_unknown_key_and_bad_enum(self):
        if not self.new:
            self.skipTest("binary has no --list-options")
        r = run([self.cube, "-o", self.out("bad"), "--set", "wall_loop=3"], check=False)
        self.assertEqual(r.returncode, 2)
        self.assertIn("did you mean wall_loops", r.stderr)
        r = run([self.cube, "-o", self.out("bad"), "--set", "sparse_infill_pattern=bogus"], check=False)
        self.assertEqual(r.returncode, 2)
        self.assertIn("allowed:", r.stderr)

    def test_set_object_and_range(self):
        if not self.new:
            self.skipTest("binary has no --load-job")
        r = json.loads(run([self.cube, "-o", self.out("obj"), "--set-object", "wall_loops=6",
                            "--set-range", "0-2:sparse_infill_density=100%", "--copies", "2"]).stdout)
        self.assertTrue(os.path.isfile(r["gcode"]))
        r2 = json.loads(run([self.cube, "-o", self.out("plain"), "--copies", "1"]).stdout)
        self.assertNotEqual(os.path.getsize(r["gcode"]), os.path.getsize(r2["gcode"]))

    def test_job_two_plates(self):
        if not self.new:
            self.skipTest("binary has no --load-job")
        job = os.path.join(self.tmp, "two.json")
        json.dump({"version": 1, "plates": [
            {"name": "A", "objects": [{"file": "cube.stl"}]},
            {"name": "B", "objects": [{"primitive": {"shape": "cylinder", "radius": 8, "height": 6},
                                       "settings": {"wall_loops": 4}}]}]}, open(job, "w"))
        r = json.loads(run([job, "-o", self.out("job")]).stdout)
        self.assertEqual([p["plate"] for p in r["plates"]], [1, 2])
        for p in r["plates"]:
            self.assertTrue(p["gcode"].endswith(f"_plate{p['plate']}.gcode"))
            self.assertTrue(os.path.isfile(p["gcode"]))
        self.assertEqual(r["gcode"], r["plates"][0]["gcode"])
        r1 = json.loads(run(["job", job, "-o", self.out("job1"), "--plate", "2"]).stdout)
        self.assertNotIn("plates", r1)
        self.assertEqual(r1["height_mm"], "6.00")

    def test_options(self):
        if not self.new:
            self.skipTest("binary has no --list-options")
        r = json.loads(run(["options", "--key", "nozzle_temperature"]).stdout)
        self.assertEqual(r[0]["preset"], "filament")
        r = json.loads(run(["options", "--scope", "plate"]).stdout)
        self.assertIn("curr_bed_type", [o["key"] for o in r])

    def test_export_3mf_no_slice(self):
        r = json.loads(run([self.cube, "-o", self.out("p3"), "--no-slice", "--export-3mf",
                            os.path.join(self.tmp, "p.3mf")]).stdout)
        self.assertTrue(os.path.isfile(r["project_3mf"]))
        self.assertIsNone(r["gcode"])


def package_dir(binary, resources, root):
    """A package-style ORCA_DIR (bin/orca-slicer + resources) around a build-tree binary."""
    os.makedirs(os.path.join(root, "bin"), exist_ok=True)
    sh = os.path.join(root, "bin", "orca-slicer")
    with open(sh, "w") as fh:
        fh.write(f'#!/bin/sh\nexec "{binary}" "$@"\n')
    os.chmod(sh, 0o755)
    if not os.path.exists(os.path.join(root, "resources")):
        os.symlink(resources, os.path.join(root, "resources"))
    return root


CLEARANCE_BIN = os.environ.get("CLEARANCE_BIN", os.path.expanduser("~/OrcaSlicer-clearance/build/src/Release/orca-slicer"))


@unittest.skipUnless(HAS_BIN, f"no slicer binary at {ORCA_BIN}")
class CheckGcode(unittest.TestCase):
    """--check-gcode (toolhead clearance on existing G-code, from the clearance thread)."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp()
        cube = os.path.join(cls.tmp, "cube.stl")
        cube_stl(cube)
        cls.gcode = json.loads(run([cube, "-o", cls.tmp + os.sep]).stdout)["gcode"]

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_parses(self):
        r = run(["--check-gcode", os.path.join(self.tmp, "missing.gcode")], check=False)
        self.assertEqual(r.returncode, 2)
        self.assertIn("no such file", r.stderr)
        self.assertIn("--check-gcode", run(["--help"]).stdout)

    def test_clear_error_without_flag(self):
        if os_.binary_has("--check-toolhead-clearance"):
            self.skipTest("this binary has --check-toolhead-clearance")
        r = run(["--check-gcode", self.gcode], check=False)
        self.assertEqual(r.returncode, 1)
        self.assertIn("clearance check FAILED", r.stderr)
        self.assertIn("check-toolhead-clearance", r.stderr)
        kept = [l.split(": ", 1)[1] for l in r.stderr.splitlines() if l.startswith("work dir kept: ")]
        for k in kept:
            shutil.rmtree(k, ignore_errors=True)

    @unittest.skipUnless(os.access(CLEARANCE_BIN, os.X_OK), f"no clearance build at {CLEARANCE_BIN}")
    def test_with_clearance_binary(self):
        res = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(CLEARANCE_BIN)))), "resources")
        pkg = package_dir(CLEARANCE_BIN, res, os.path.join(self.tmp, "clrpkg"))
        r = run(["--check-gcode", self.gcode, "--set", "toolhead_clearance_enable=1",
                 "--set", "toolhead_clearance_profile=1.5x0,12x1,47x1.13,1000x29.2"], env={"ORCA_DIR": pkg})
        out = json.loads(r.stdout)
        self.assertEqual(out["gcode"], self.gcode)
        self.assertTrue(out["clearance"]["ran"])
        self.assertEqual(out["clearance"]["report"], os.path.splitext(self.gcode)[0] + ".clearance.json")
        self.assertTrue(os.path.isfile(out["clearance"]["report"]))


if __name__ == "__main__":
    unittest.main(verbosity=2)
