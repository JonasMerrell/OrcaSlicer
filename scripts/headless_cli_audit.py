#!/usr/bin/env python3
"""Headless CLI audit table: generate it and check that nothing in the GUI lacks a CLI path.

docs/headless-cli-audit.json has three parts:
  options      every config option -> preset ("process" | "filament" | "printer" | "project" | null),
               scopes (global/object/part/layer_range/plate) and how to set it without the GUI
  cli_options  every orca-slicer command line action/transform/misc option
  gui_actions  every GUI action that changes the output, with its CLI path and status

  generate --binary path/to/orca-slicer
      Rebuild "options" and "cli_options" from `orca-slicer --list-options -`, keeping hand-written
      notes and the whole gui_actions section. Run it after adding an option, then review the diff.

  check
      Static scan of the sources, no binary needed. Fails when a GUI action has no row in
      gui_actions: a gizmo (GLGizmosManager::EType), a calibration (CalibMode), a volume type
      (ModelVolumeType), a paint layer (ModelVolume::*_facets), or a Plater action that edits the
      model or exports output. Also fails on rows whose status isn't one of the known values.
      tests/libslic3r/test_cli_headless.cpp checks "options" against print_config_def.
"""
import argparse
import json
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
AUDIT = os.path.join(ROOT, "docs", "headless-cli-audit.json")
STATUSES = {"covered", "partial", "gui-only", "not-applicable", "excluded"}

# Plater methods that change the model or write output. Anything matching must be audited.
PLATER_ACTIONS = re.compile(
    r"^void Plater::((?:export|calib|cut|split|arrange|orient|fill_bed|mirror|scale|convert_unit|"
    r"increase_instances|decrease_instances|set_number_of_copies|set_global_filament_map|"
    r"replace_with|replace_all_with|reload_from_disk|reload_all_from_disk|send_gcode|send_to_printer|"
    r"load_gcode|add_file|load_project|simplify|merge|clone)[A-Za-z0-9_]*)\s*\(", re.M)


def read(rel):
    with open(os.path.join(ROOT, rel), encoding="utf-8") as fh:
        return fh.read()


def enum_body(text, header_regex):
    m = re.search(header_regex + r"[^{]*\{(.*?)\};", text, re.S)
    if not m:
        raise SystemExit(f"headless_cli_audit: can't find enum {header_regex!r}; update scripts/headless_cli_audit.py")
    body = re.sub(r"//[^\n]*", "", m.group(1))
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    return [n.strip().split("=")[0].strip() for n in body.split(",") if n.strip()]


def gui_sources():
    """All GUI action ids the sources define today."""
    ids = set()
    gizmos = enum_body(read("src/slic3r/GUI/Gizmos/GLGizmosManager.hpp"), r"enum EType\s*:\s*unsigned char")
    ids |= {f"gizmo:{g}" for g in gizmos if g != "Undefined"}
    calib = enum_body(read("src/libslic3r/calib.hpp"), r"enum class CalibMode\s*:\s*int")
    ids |= {f"calib:{c}" for c in calib if c != "Calib_None"}
    vols = enum_body(read("src/libslic3r/Model.hpp"), r"enum class ModelVolumeType\s*:\s*int")
    ids |= {f"volume:{v}" for v in vols if v != "INVALID"}
    ids |= {f"paint:{p}" for p in re.findall(r"FacetsAnnotation\s+(\w+_facets);", read("src/libslic3r/Model.hpp"))}
    ids |= {f"plater:{p}" for p in PLATER_ACTIONS.findall(read("src/slic3r/GUI/Plater.cpp"))}
    return ids


def check():
    audit = json.load(open(AUDIT, encoding="utf-8"))
    rows = audit.get("gui_actions", [])
    covered = set()
    problems = []
    for i, row in enumerate(rows):
        for field in ("id", "gui", "cli", "status"):
            if not row.get(field):
                problems.append(f"gui_actions[{i}] ({row.get('id')}): missing {field!r}")
        if row.get("status") not in STATUSES:
            problems.append(f"gui_actions[{i}] ({row.get('id')}): status must be one of {sorted(STATUSES)}")
        covered.update(row.get("sources", []))
    missing = sorted(gui_sources() - covered)
    for m in missing:
        problems.append(f"GUI action {m} has no row in docs/headless-cli-audit.json gui_actions "
                        f"(add one with its CLI path, or status gui-only and a reason)")
    stale = sorted(s for s in covered - gui_sources() if not s.startswith(("fork:", "menu:", "net:")))
    for s in stale:
        problems.append(f"gui_actions source {s} no longer exists in the code")
    if problems:
        print("\n".join(problems), file=sys.stderr)
        return 1
    print(json.dumps({"ok": True, "gui_actions": len(rows), "sources_checked": len(gui_sources()),
                      "options": len(audit.get("options", {}))}))
    return 0


def how_to_set(o):
    key, preset, scopes = o["key"], o.get("preset"), o.get("scopes", [])
    ways = []
    if preset == "process":
        ways.append(f"orca-slice --set {key}=V")
    elif preset == "filament":
        ways.append(f"orca-slice --set {key}=V (all filaments) / --set {key}[N]=V (filament N)")
    elif preset == "printer":
        ways.append(f"orca-slice --set {key}=V" + (" / --set {}[N]=V (extruder N)".format(key) if o.get("vector") else ""))
    elif preset == "project":
        ways.append(f"orca-slice --set {key}=V (passed as {o['cli']})")
    if "object" in scopes:
        ways.append("--set-object / job objects[].settings")
    if "part" in scopes:
        ways.append("job parts[].settings / --modifier")
    if "layer_range" in scopes:
        ways.append("--set-range / job height_ranges[].settings")
    if "plate" in scopes:
        ways.append("job plates[].settings")
    if not ways:
        ways.append(f"{o['cli']} V on orca-slicer (raw CLI only)")
    return "; ".join(ways)


def generate(binary):
    out = subprocess.run([binary, "--list-options", "-"], capture_output=True, text=True,
                         env=dict(os.environ, LC_ALL="C"), check=True).stdout
    start = out.find("{")
    cat = json.loads(out[start:])
    old = {}
    if os.path.exists(AUDIT):
        old = json.load(open(AUDIT, encoding="utf-8"))
    old_opts = old.get("options", {})
    options = {}
    for o in cat["options"]:
        row = {"preset": o.get("preset"), "scopes": o.get("scopes", []), "type": o["type"],
               "category": o.get("category", ""), "cli": how_to_set(o)}
        note = old_opts.get(o["key"], {}).get("note")
        if note:
            row["note"] = note
        options[o["key"]] = row
    cli = sorted({c["key"] for part in ("actions", "transforms", "misc") for c in cat["cli"][part]})
    audit = {
        "comment": "Generated by scripts/headless_cli_audit.py generate (options, cli_options); "
                   "gui_actions is hand-written. Checked by tests/libslic3r/test_cli_headless.cpp and "
                   "`scripts/headless_cli_audit.py check` (ctest headless_cli_audit).",
        "options": options,
        "cli_options": cli,
        "gui_actions": old.get("gui_actions", []),
    }
    with open(AUDIT, "w", encoding="utf-8") as fh:
        json.dump(audit, fh, indent=1, ensure_ascii=False)
        fh.write("\n")
    raw = [k for k, r in options.items() if r["preset"] is None and not r["scopes"] and not r.get("note")]
    print(json.dumps({"options": len(options), "cli_options": len(cli), "raw_cli_only_without_note": raw}, indent=1))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("generate")
    g.add_argument("--binary", required=True)
    sub.add_parser("check")
    sub.add_parser("sources", help="print the GUI action ids found in the sources")
    a = ap.parse_args()
    if a.cmd == "generate":
        return generate(a.binary)
    if a.cmd == "sources":
        print("\n".join(sorted(gui_sources())))
        return 0
    return check()


if __name__ == "__main__":
    sys.exit(main())
