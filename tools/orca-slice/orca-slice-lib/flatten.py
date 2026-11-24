#!/usr/bin/env python3
"""
Resolve OrcaSlicer preset inheritance into self-contained machine / process /
filament JSON files the CLI can load with --load-settings / --load-filaments.

Why flatten at all: the CLI resolves `inherits` against the bundled system
library, but the user's exported bundle is missing a few intermediate user
presets ("Voron Generic ABS", ...). We resolve the chain here and fall back to
the system generic of the same filament type when a parent link is gone.

Two fields the CLI is picky about (see OrcaSlicer.cpp CLI::run):
  * `from` must be "system" / "User" / "Default" - anything else -> err -5.
  * process<->printer compat check compares the process's `compatible_printers`
    against the *machine's `inherits` value* (its system parent name), NOT the
    machine preset name. So the flattened machine must keep its real `inherits`
    ("Voron 0.1 0.4 nozzle") and the process's compatible_printers must contain
    that string (it already does, via fdm_process_voron_common) - err -17 else.
"""
import io
import json
import os
import re
import sys
import glob
import zipfile

import params

# orca-slice copy: defaults point at the OrcaSlicer fork build + the desktop
# user presets (~/.config/OrcaSlicer/user/default/{machine,process,filament}).
ORCA = os.environ.get("ORCA_DIR") or os.path.expanduser("~/OrcaSlicer/build/package")
SYS_PROFILES = os.path.join(ORCA, "resources", "profiles")
BUNDLE = os.environ.get("ORCA_PRESETS") or os.path.expanduser("~/.config/OrcaSlicer/user/default")
# presets pulled from the filament hub by `orca-slice hub pull`; the desktop bundle wins on a name clash
HUB_CACHE = os.environ.get("ORCA_HUB_CACHE") or os.path.expanduser("~/.cache/orca-slice/hub")

# meta keys stripped from a flattened preset (they only make sense on a stored
# user preset). `inherits` and `from` are handled explicitly in finalize().
DROP_KEYS = {
    "setting_id", "base_id", "filament_id", "user_id", "updated_time",
    "instantiation", "different_settings_to_system",
}

FILAMENT_TYPE_FALLBACK = {
    "PLA": "Generic PLA @System",
    "PLA-CF": "Generic PLA-CF @System",
    "ABS": "Generic ABS @System",
    "ASA": "Generic ASA @System",
    "PETG": "Generic PETG @System",
    "PET": "Generic PETG @System",
    "PCTG": "Generic PCTG @System",
    "PC": "Generic PC @System",
    "TPU": "Generic TPU @System",
    "PA": "Generic PA @System",
    "PA-CF": "Generic PA-CF @System",
    "PVA": "Generic PVA @System",
    "HIPS": "Generic HIPS @System",
}


def _load(path):
    with open(path, "r", encoding="utf-8") as fh:
        return json.load(fh)


def build_index():
    """name -> (filepath, is_system). System library first, user bundle wins."""
    idx = {}
    for path in glob.glob(os.path.join(SYS_PROFILES, "**", "*.json"), recursive=True):
        p = path + os.sep
        if not any(s in p for s in ("/machine/", "/process/", "/filament/")):
            continue
        try:
            d = _load(path)
        except Exception:
            continue
        n = d.get("name")
        if n and d.get("type") in ("machine", "process", "filament"):
            idx.setdefault(n, (path, True))
    for sub in ("printer", "machine", "process", "filament"):
        for path in glob.glob(os.path.join(HUB_CACHE, sub, "*.json")):
            try:
                d = _load(path)
            except Exception:
                continue
            n = d.get("name")
            if n:
                idx[n] = (path, False)  # hub copy overrides system, desktop bundle overrides it
    for sub in ("printer", "machine", "process", "filament"):
        for path in glob.glob(os.path.join(BUNDLE, sub, "*.json")):
            try:
                d = _load(path)
            except Exception:
                continue
            n = d.get("name")
            if n:
                idx[n] = (path, False)  # user bundle overrides system
    return idx


INDEX = build_index()


def configure(orca_dir):
    """Point at a different OrcaSlicer tree (app.py --orca-dir) and re-index."""
    global ORCA, SYS_PROFILES, INDEX
    if orca_dir and os.path.abspath(orca_dir) != os.path.abspath(ORCA):
        ORCA = orca_dir
        SYS_PROFILES = os.path.join(ORCA, "resources", "profiles")
        INDEX = build_index()


def reindex():
    """Unconditionally re-scan SYS_PROFILES + BUNDLE. configure() only rebuilds
    on a directory change; call this after writing new files into BUNDLE
    (e.g. orcacloud.sync_presets()) so resolve()/INDEX see them immediately."""
    global INDEX
    INDEX = build_index()


BUNDLE_SUBDIR = {"printer": "printer", "machine": "printer",
                 "process": "process", "print": "process", "filament": "filament"}


def _find_preset_file(sub, name):
    """An existing BUNDLE/<sub>/*.json whose `name` field matches, if any -
    looked up by content rather than by re-deriving a filename, so a preset
    is always overwritten in place regardless of how its file happened to
    be named (real preset names routinely contain spaces/@/etc that a naive
    filename sanitizer would mangle differently run to run)."""
    for path in glob.glob(os.path.join(BUNDLE, sub, "*.json")):
        try:
            if _load(path).get("name") == name:
                return path
        except Exception:
            continue
    return None


def write_preset(kind, name, config):
    """Write a preset dict into BUNDLE/<subdir>/<name>.json (subdir chosen by
    `kind`, one of printer/machine/process/print/filament). Overwrites any
    existing file for the same name. Does NOT reindex - call reindex() once
    after a batch of writes. Returns the path written."""
    sub = BUNDLE_SUBDIR.get(kind)
    if not sub:
        raise ValueError(f"unknown preset kind {kind!r}")
    out = dict(config)
    out["name"] = name
    out.setdefault("type", "machine" if sub == "printer" else sub)
    d = os.path.join(BUNDLE, sub)
    os.makedirs(d, exist_ok=True)
    path = _find_preset_file(sub, name)
    if not path:
        safe = re.sub(r"[^A-Za-z0-9 ._@()+-]", "_", name) or "preset"
        path = os.path.join(d, safe + ".json")
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(out, fh, indent=1, ensure_ascii=False)
    return path


def _kind_from_path(path):
    """A real OrcaSlicer export's preset JSON almost never carries its own
    `type` key (checked against actual exports) - the zip's folder name is
    the only reliable signal, for both a single-preset export
    (.orca_printer/.orca_filament/.orca_print, which is a zip too) and a
    full "Export Configs" bundle (printer/process/filament subfolders)."""
    for part in path.replace("\\", "/").split("/")[:-1]:
        if part in BUNDLE_SUBDIR:
            return part
    return None


def _kind_from_content(d):
    """Last-resort guess for a bare (non-zip) preset JSON with neither a
    `type` key nor a folder to go by - by a handful of kind-specific keys."""
    if "nozzle_diameter" in d or "printable_area" in d:
        return "printer"
    if "filament_type" in d or "filament_diameter" in d:
        return "filament"
    if "layer_height" in d or "sparse_infill_density" in d:
        return "process"
    return None


def _import_one(d, kind_hint=None):
    """Write one preset dict if it looks like a printer/process/filament
    preset. Returns [(kind, name)] or [] if `d` isn't a recognisable preset
    (e.g. a stray non-preset JSON entry inside an export zip)."""
    name = d.get("name")
    raw_kind = kind_hint or d.get("type") or _kind_from_content(d) or ""
    sub = BUNDLE_SUBDIR.get(raw_kind)
    if not name or not sub:
        return []
    write_preset(raw_kind, name, d)
    return [(sub, name)]


def import_bundle(data):
    """Import one uploaded preset export. `data` is the raw file bytes -
    either a bare preset JSON, or a zip (OrcaSlicer's `.orca_printer` /
    `.orca_filament` / `.orca_print` single-preset export, or a full "Export
    Configs" bundle with printer/process/filament subfolders - both are zips
    with the preset JSON(s) inside, told apart by their folder names, not
    the outer filename). Writes each recognised preset into BUNDLE via
    write_preset(). Does NOT reindex - call reindex() once after. Returns
    [(kind, name), ...]; raises ValueError if nothing importable was found."""
    imported = []
    if zipfile.is_zipfile(io.BytesIO(data)):
        with zipfile.ZipFile(io.BytesIO(data)) as zf:
            for info in zf.infolist():
                if info.is_dir() or not info.filename.lower().endswith(".json"):
                    continue
                try:
                    d = json.loads(zf.read(info).decode("utf-8"))
                except (ValueError, UnicodeDecodeError):
                    continue
                if isinstance(d, dict):
                    imported += _import_one(d, kind_hint=_kind_from_path(info.filename))
    else:
        try:
            d = json.loads(data.decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            raise ValueError("not a valid OrcaSlicer preset - expected a "
                              ".json export or a .orca_printer/.orca_filament/.zip bundle")
        if isinstance(d, dict):
            imported += _import_one(d)
    if not imported:
        raise ValueError("no printer/filament/process preset found in that file")
    return imported


class MissingParent(Exception):
    pass


# populated during a flatten_set() call, drained by the caller
WARNINGS = []


def _merge(parent, child):
    out = dict(parent)
    for k, v in child.items():
        out[k] = v  # Orca merge == full key replacement
    return out


def resolve(name, _seen=None):
    """Return the fully merged config dict for a preset name."""
    _seen = _seen or set()
    if name in _seen:
        raise ValueError(f"inheritance loop at {name}")
    _seen.add(name)
    entry = INDEX.get(name)
    if not entry:
        raise MissingParent(name)
    d = _load(entry[0])
    parent_name = d.get("inherits")
    if parent_name:
        try:
            base = resolve(parent_name, _seen)
        except MissingParent:
            base = _fallback(d, parent_name)
        return _merge(base, d)
    return dict(d)


def _fallback(child, missing_name):
    """A parent user-preset is gone. Substitute the system generic by type."""
    ftype = None
    ft = child.get("filament_type")
    if isinstance(ft, list) and ft:
        ftype = ft[0]
    elif isinstance(ft, str):
        ftype = ft
    if not ftype:
        for key in FILAMENT_TYPE_FALLBACK:
            if key.lower() in missing_name.lower():
                ftype = key
                break
    target = FILAMENT_TYPE_FALLBACK.get(ftype or "", "Generic PLA @System")
    msg = (f"filament preset '{missing_name}' isn't in the exported bundle - "
           f"using system '{target}' as its base (tuning may differ)")
    WARNINGS.append(msg)
    sys.stderr.write(f"[flatten] {msg}\n")
    return resolve(target)


def system_root(name):
    """Walk the inherits chain; return the first ancestor marked from==system."""
    seen = set()
    cur = name
    while cur and cur not in seen:
        seen.add(cur)
        entry = INDEX.get(cur)
        if not entry:
            return None
        path, is_system = entry
        d = _load(path)
        if is_system or d.get("from") == "system":
            return d.get("name", cur)
        cur = d.get("inherits")
    return None


def leaf_inherits(name):
    entry = INDEX.get(name)
    return _load(entry[0]).get("inherits") if entry else None


TYPE_FIELD = {"printer": "machine", "machine": "machine",
              "process": "process", "filament": "filament"}


def finalize(d, kind, orig_name):
    d = {k: v for k, v in d.items() if k not in DROP_KEYS}
    d["type"] = TYPE_FIELD[kind]
    d["name"] = orig_name
    d["from"] = "User"
    d["version"] = "2.3.2.0"
    d["is_custom_defined"] = "0"
    inh = leaf_inherits(orig_name)
    if inh:
        d["inherits"] = inh
    else:
        d.pop("inherits", None)
    return d


SUPPORT_PRESETS = {
    "off": {"enable_support": "0"},
    "normal": {"enable_support": "1", "support_type": "normal(auto)"},
    "tree": {"enable_support": "1", "support_type": "tree(auto)"},
}

# Orca bed-type name -> the filament temp key it reads. The exported .orca_printer
# never carries the plate selection (it lives in app/project state), and with it
# unset the CLI falls back to "Cool Plate" -> cool_plate_temp (0 for ABS/PETG),
# so BED_TEMP=0 reaches START_PRINT. We pick one explicitly instead.
BED_TYPES = {
    "Textured PEI Plate": "textured_plate_temp",
    "Smooth PEI Plate": "hot_plate_temp",
    "High Temp Plate": "hot_plate_temp",
    "Engineering Plate": "eng_plate_temp",
    "Cool Plate": "cool_plate_temp",
    "Textured Cool Plate": "textured_cool_plate_temp",
}
DEFAULT_BED_TYPE = "Textured PEI Plate"


def _first_num(v, default=0.0):
    if isinstance(v, list) and v:
        v = v[0]
    try:
        return float(v)
    except (TypeError, ValueError):
        return default


def _fix_temps(filament):
    """Keep chamber_temperature(s) consistent - fdm_filament_common pins the
    plural to 0 and user presets only touch the singular."""
    singular = _first_num(filament.get("chamber_temperature"))
    plural = _first_num(filament.get("chamber_temperatures"))
    val = str(int(max(singular, plural)))
    filament["chamber_temperature"] = [val]
    filament["chamber_temperatures"] = [val]


def _prep_filament(name, bed_type):
    fil = finalize(resolve(name), "filament", name)
    fil["compatible_printers"] = []
    fil["compatible_printers_condition"] = ""
    fil.pop("compatible_prints", None)
    fil.pop("compatible_prints_condition", None)
    _fix_temps(fil)
    # force the chosen plate's initial-layer bed temp to something non-zero if
    # the filament left it at 0
    temp_key = BED_TYPES[bed_type]
    if _first_num(fil.get(temp_key)) <= 0:
        best = max([_first_num(fil.get(k)) for k in
                    ("hot_plate_temp", "textured_plate_temp",
                     "eng_plate_temp", "cool_plate_temp")] + [0])
        if best > 0:
            fil[temp_key] = [str(int(best))]
            fil[temp_key + "_initial_layer"] = [str(int(best))]
    return fil


# form field -> Orca process key. Values pass straight through as strings.
PROCESS_OVERRIDES = {
    "layer_height": "layer_height",
    "wall_loops": "wall_loops",
    "sparse_infill_density": "sparse_infill_density",
    "sparse_infill_pattern": "sparse_infill_pattern",
}


def flatten_set(machine_name, process_name, filaments, support, outdir,
                bed_type=DEFAULT_BED_TYPE, prime_tower=False, overrides=None):
    """`filaments` may be a single preset name or a list (multi-material).
    `overrides` is a {form-key: value} dict of process tweaks (PROCESS_OVERRIDES).
    Returns {'machine':path,'process':path,'filaments':[path,...]}."""
    os.makedirs(outdir, exist_ok=True)
    WARNINGS.clear()
    if isinstance(filaments, str):
        filaments = [filaments]
    if bed_type not in BED_TYPES:
        bed_type = DEFAULT_BED_TYPE

    machine = finalize(resolve(machine_name), "machine", machine_name)
    process = finalize(resolve(process_name), "process", process_name)

    # process<->printer compat: process.compatible_printers must contain the
    # machine's system-parent name.
    root = machine.get("inherits") or system_root(machine_name)
    cp = process.get("compatible_printers")
    if not isinstance(cp, list):
        cp = []
    if root and root not in cp:
        cp = cp + [root]
    process["compatible_printers"] = cp + [machine_name]
    process.pop("compatible_printers_condition", None)

    machine["curr_bed_type"] = bed_type
    machine["default_bed_type"] = bed_type

    if support:  # legacy web-form radio; the API leaves the preset alone
        process.update(SUPPORT_PRESETS.get(support, SUPPORT_PRESETS["off"]))

    # overrides: {orca process key: value}. Legacy form keys map 1:1 (see
    # PROCESS_OVERRIDES); anything else must be a key the params schema knows.
    for field, val in (overrides or {}).items():
        key = PROCESS_OVERRIDES.get(field, field)
        if val in (None, ""):
            continue
        if key not in PROCESS_OVERRIDES.values() and key not in params.BY_KEY:
            continue
        process[key] = str(val)
    if "layer_height" in process and "initial_layer_print_height" not in \
            (overrides or {}):
        # keep the first layer at least as tall as the layers
        lh = _first_num(process.get("layer_height"))
        if _first_num(process.get("initial_layer_print_height")) < lh:
            process["initial_layer_print_height"] = f"{lh:g}"

    if prime_tower or len(filaments) > 1:
        process["enable_prime_tower"] = "1"
        # the printer preset parks the wipe tower at x=15, which is off this
        # bed (printable x starts at 26) - move it to the back-right corner.
        process["wipe_tower_x"] = "250"
        process["wipe_tower_y"] = "250"

    paths = {"filaments": []}
    for kind, obj in (("machine", machine), ("process", process)):
        p = os.path.join(outdir, f"{kind}.json")
        with open(p, "w", encoding="utf-8") as fh:
            json.dump(obj, fh, indent=1, ensure_ascii=False)
        paths[kind] = p

    fils = [_prep_filament(name, bed_type) for name in filaments]
    if len(fils) > 1:
        # Orca merges the N filament configs into per-value vectors of length N;
        # if one filament is missing a key the others define, the vector comes
        # out short and the CLI aborts with "ConfigOptionVector ... invalid size
        # found". The user's thin presets (few keys) trip this next to fleshed-
        # out ones. Union the keys - the gaps are hardware params (loading /
        # ramming / cooling) that are the same across every Tradrack lane anyway.
        union = {}
        for f in fils:
            for k, v in f.items():
                union.setdefault(k, v)
        for f in fils:
            for k, v in union.items():
                f.setdefault(k, v)

    for i, fil in enumerate(fils, 1):
        p = os.path.join(outdir, f"filament_{i}.json")
        with open(p, "w", encoding="utf-8") as fh:
            json.dump(fil, fh, indent=1, ensure_ascii=False)
        paths["filaments"].append(p)
    # legacy single-file alias
    paths["filament"] = paths["filaments"][0]
    return paths


if __name__ == "__main__":
    import argparse

    ap = argparse.ArgumentParser()
    ap.add_argument("--machine", default="SimpleCore")
    ap.add_argument("--process", required=True)
    ap.add_argument("--filament", required=True, action="append",
                    help="repeat for multi-material")
    ap.add_argument("--support", default="off", choices=list(SUPPORT_PRESETS))
    ap.add_argument("--bed-type", default=DEFAULT_BED_TYPE, choices=list(BED_TYPES))
    ap.add_argument("--prime-tower", action="store_true")
    ap.add_argument("--outdir", required=True)
    a = ap.parse_args()
    got = flatten_set(a.machine, a.process, a.filament, a.support, a.outdir,
                      a.bed_type, a.prime_tower)
    print(json.dumps(got, indent=2))
