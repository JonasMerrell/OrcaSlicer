"""
The process parameters the phone app exposes, grouped the way desktop
OrcaSlicer's sidebar does (tab -> group -> option). Each entry is validated
here before it is written into the flattened process JSON.

  kind: "float" | "int" | "percent" | "bool" | "enum" | "floatpct"
        floatpct = a float that may also carry a "%" suffix (line widths)
  choices: [(value, label), ...] for enums
"""

TABS = ["Quality", "Strength", "Speed", "Support", "Others"]


def P(key, label, tab, group, kind, unit="", choices=None, lo=None, hi=None,
      hint=""):
    d = {"key": key, "label": label, "tab": tab, "group": group, "kind": kind,
         "unit": unit}
    if choices:
        d["choices"] = [list(c) for c in choices]
    if lo is not None:
        d["min"] = lo
    if hi is not None:
        d["max"] = hi
    if hint:
        d["hint"] = hint
    return d


INFILL_PATTERNS = [
    ("concentric", "Concentric"), ("zig-zag", "Rectilinear"), ("grid", "Grid"),
    ("line", "Line"), ("cubic", "Cubic"), ("triangles", "Triangles"),
    ("tri-hexagon", "Tri-hexagon"), ("gyroid", "Gyroid"),
    ("honeycomb", "Honeycomb"), ("adaptivecubic", "Adaptive Cubic"),
    ("alignedrectilinear", "Aligned Rectilinear"),
    ("3dhoneycomb", "3D Honeycomb"), ("hilbertcurve", "Hilbert Curve"),
    ("archimedeanchords", "Archimedean Chords"),
    ("octagramspiral", "Octagram Spiral"), ("supportcubic", "Support Cubic"),
    ("lightning", "Lightning"), ("crosshatch", "Cross Hatch"),
]
SURFACE_PATTERNS = [
    ("concentric", "Concentric"), ("zig-zag", "Rectilinear"),
    ("monotonic", "Monotonic"), ("monotonicline", "Monotonic line"),
    ("alignedrectilinear", "Aligned Rectilinear"),
    ("hilbertcurve", "Hilbert Curve"), ("archimedeanchords", "Archimedean Chords"),
    ("octagramspiral", "Octagram Spiral"),
]
SUPPORT_PATTERNS = [
    ("default", "Default"), ("rectilinear", "Rectilinear"),
    ("rectilinear-grid", "Rectilinear grid"), ("honeycomb", "Honeycomb"),
    ("lightning", "Lightning"), ("hollow", "Hollow"),
]
INTERFACE_PATTERNS = [
    ("auto", "Default"), ("rectilinear", "Rectilinear"),
    ("concentric", "Concentric"), ("rectilinear_interlaced", "Rectilinear Interlaced"),
    ("grid", "Grid"),
]

PARAMS = [
    # ---------------------------------------------------------- Quality --- #
    P("layer_height", "Layer height", "Quality", "Layer height", "float", "mm",
      lo=0.05, hi=0.45),
    P("initial_layer_print_height", "Initial layer height", "Quality",
      "Layer height", "float", "mm", lo=0.05, hi=0.6),
    P("line_width", "Default", "Quality", "Line width", "floatpct", "mm or %",
      lo=0.1, hi=2),
    P("initial_layer_line_width", "First layer", "Quality", "Line width",
      "floatpct", "mm or %", lo=0.1, hi=2),
    P("outer_wall_line_width", "Outer wall", "Quality", "Line width",
      "floatpct", "mm or %", lo=0.1, hi=2),
    P("inner_wall_line_width", "Inner wall", "Quality", "Line width",
      "floatpct", "mm or %", lo=0.1, hi=2),
    P("top_surface_line_width", "Top surface", "Quality", "Line width",
      "floatpct", "mm or %", lo=0.1, hi=2),
    P("sparse_infill_line_width", "Sparse infill", "Quality", "Line width",
      "floatpct", "mm or %", lo=0.1, hi=2),
    P("seam_position", "Seam position", "Quality", "Seam", "enum",
      choices=[("nearest", "Nearest"), ("aligned", "Aligned"),
               ("back", "Back"), ("random", "Random")]),
    P("seam_gap", "Seam gap", "Quality", "Seam", "floatpct", "mm or %",
      lo=0, hi=100),
    P("wall_generator", "Wall generator", "Quality", "Precision", "enum",
      choices=[("classic", "Classic"), ("arachne", "Arachne")]),
    P("precise_wall", "Precise wall", "Quality", "Precision", "bool"),
    P("precise_outer_wall", "Precise Z height", "Quality", "Precision", "bool"),
    P("only_one_wall_top", "Only one wall on top surfaces", "Quality",
      "Precision", "bool"),
    P("elefant_foot_compensation", "Elephant foot compensation", "Quality",
      "Precision", "float", "mm", lo=0, hi=2),
    P("xy_hole_compensation", "X-Y hole compensation", "Quality", "Precision",
      "float", "mm", lo=-2, hi=2),
    P("xy_contour_compensation", "X-Y contour compensation", "Quality",
      "Precision", "float", "mm", lo=-2, hi=2),
    P("resolution", "Resolution", "Quality", "Precision", "float", "mm",
      lo=0, hi=0.5),
    P("enable_arc_fitting", "Arc fitting", "Quality", "Precision", "bool"),
    P("ironing_type", "Ironing type", "Quality", "Ironing", "enum",
      choices=[("no ironing", "No ironing"), ("top", "Top surfaces"),
               ("topmost", "Topmost surface"), ("solid", "All solid layers")]),
    P("ironing_flow", "Ironing flow", "Quality", "Ironing", "percent", "%",
      lo=0, hi=100),
    P("ironing_speed", "Ironing speed", "Quality", "Ironing", "float", "mm/s",
      lo=1, hi=300),
    P("wall_sequence", "Wall printing order", "Quality", "Wall generator",
      "enum", choices=[("inner wall/outer wall", "inner/outer"),
                       ("outer wall/inner wall", "outer/inner"),
                       ("inner-outer-inner wall", "inner/outer/inner")]),
    P("detect_thin_wall", "Detect thin wall", "Quality", "Advanced", "bool"),
    P("detect_overhang_wall", "Detect overhang wall", "Quality", "Advanced",
      "bool"),
    P("reduce_crossing_wall", "Avoid crossing wall", "Quality", "Advanced",
      "bool"),

    # --------------------------------------------------------- Strength --- #
    P("wall_loops", "Wall loops", "Strength", "Walls", "int", lo=0, hi=20),
    P("top_shell_layers", "Top shell layers", "Strength", "Top/bottom shells",
      "int", lo=0, hi=50),
    P("top_shell_thickness", "Top shell thickness", "Strength",
      "Top/bottom shells", "float", "mm", lo=0, hi=20),
    P("top_surface_pattern", "Top surface pattern", "Strength",
      "Top/bottom shells", "enum", choices=SURFACE_PATTERNS),
    P("bottom_shell_layers", "Bottom shell layers", "Strength",
      "Top/bottom shells", "int", lo=0, hi=50),
    P("bottom_shell_thickness", "Bottom shell thickness", "Strength",
      "Top/bottom shells", "float", "mm", lo=0, hi=20),
    P("bottom_surface_pattern", "Bottom surface pattern", "Strength",
      "Top/bottom shells", "enum", choices=SURFACE_PATTERNS),
    P("sparse_infill_density", "Sparse infill density", "Strength", "Infill",
      "percent", "%", lo=0, hi=100),
    P("sparse_infill_pattern", "Sparse infill pattern", "Strength", "Infill",
      "enum", choices=INFILL_PATTERNS),
    P("infill_direction", "Infill direction", "Strength", "Infill", "float",
      "°", lo=0, hi=360),
    P("infill_wall_overlap", "Infill/wall overlap", "Strength", "Infill",
      "percent", "%", lo=0, hi=100),
    P("infill_combination", "Infill combination", "Strength", "Infill", "bool"),
    P("minimum_sparse_infill_area", "Minimum sparse infill threshold",
      "Strength", "Infill", "float", "mm²", lo=0, hi=1000),
    P("reduce_infill_retraction", "Reduce infill retraction", "Strength",
      "Infill", "bool"),
    P("infill_anchor", "Length of infill anchor", "Strength", "Infill",
      "floatpct", "mm or %", lo=0, hi=1000),
    P("infill_anchor_max", "Maximum length of infill anchor", "Strength",
      "Infill", "floatpct", "mm or %", lo=0, hi=1000),

    # ------------------------------------------------------------ Speed --- #
    P("initial_layer_speed", "Initial layer", "Speed", "Initial layer speed",
      "float", "mm/s", lo=1, hi=1000),
    P("initial_layer_infill_speed", "Initial layer infill", "Speed",
      "Initial layer speed", "float", "mm/s", lo=1, hi=1000),
    P("outer_wall_speed", "Outer wall", "Speed", "Other layers speed", "float",
      "mm/s", lo=1, hi=1000),
    P("inner_wall_speed", "Inner wall", "Speed", "Other layers speed", "float",
      "mm/s", lo=1, hi=1000),
    P("sparse_infill_speed", "Sparse infill", "Speed", "Other layers speed",
      "float", "mm/s", lo=1, hi=1000),
    P("internal_solid_infill_speed", "Internal solid infill", "Speed",
      "Other layers speed", "float", "mm/s", lo=1, hi=1000),
    P("top_surface_speed", "Top surface", "Speed", "Other layers speed",
      "float", "mm/s", lo=1, hi=1000),
    P("gap_infill_speed", "Gap infill", "Speed", "Other layers speed", "float",
      "mm/s", lo=1, hi=1000),
    P("support_speed", "Support", "Speed", "Other layers speed", "float",
      "mm/s", lo=1, hi=1000),
    P("support_interface_speed", "Support interface", "Speed",
      "Other layers speed", "float", "mm/s", lo=1, hi=1000),
    P("bridge_speed", "Bridge", "Speed", "Overhang speed", "float", "mm/s",
      lo=1, hi=500),
    P("enable_overhang_speed", "Slow down for overhang", "Speed",
      "Overhang speed", "bool"),
    P("travel_speed", "Travel", "Speed", "Travel speed", "float", "mm/s",
      lo=1, hi=1500),
    P("default_acceleration", "Normal printing", "Speed", "Acceleration",
      "float", "mm/s²", lo=100, hi=100000),
    P("outer_wall_acceleration", "Outer wall", "Speed", "Acceleration",
      "float", "mm/s²", lo=100, hi=100000),
    P("inner_wall_acceleration", "Inner wall", "Speed", "Acceleration",
      "float", "mm/s²", lo=100, hi=100000),
    P("initial_layer_acceleration", "Initial layer", "Speed", "Acceleration",
      "float", "mm/s²", lo=100, hi=100000),
    P("top_surface_acceleration", "Top surface", "Speed", "Acceleration",
      "float", "mm/s²", lo=100, hi=100000),
    P("sparse_infill_acceleration", "Sparse infill", "Speed", "Acceleration",
      "floatpct", "mm/s² or %", lo=1, hi=100000),
    P("travel_acceleration", "Travel", "Speed", "Acceleration", "float",
      "mm/s²", lo=100, hi=100000),

    # ---------------------------------------------------------- Support --- #
    P("enable_support", "Enable support", "Support", "Support", "bool"),
    P("support_type", "Type", "Support", "Support", "enum",
      choices=[("normal(auto)", "normal(auto)"), ("tree(auto)", "tree(auto)"),
               ("normal(manual)", "normal(manual)"),
               ("tree(manual)", "tree(manual)")]),
    P("support_threshold_angle", "Threshold angle", "Support", "Support",
      "int", "°", lo=0, hi=90),
    P("support_on_build_plate_only", "On build plate only", "Support",
      "Support", "bool"),
    P("raft_layers", "Raft layers", "Support", "Raft", "int", lo=0, hi=100),
    P("support_top_z_distance", "Top Z distance", "Support", "Advanced",
      "float", "mm", lo=0, hi=1),
    P("support_bottom_z_distance", "Bottom Z distance", "Support", "Advanced",
      "float", "mm", lo=0, hi=1),
    P("support_object_xy_distance", "Support/object xy distance", "Support",
      "Advanced", "float", "mm", lo=0, hi=10),
    P("support_base_pattern", "Base pattern", "Support", "Advanced", "enum",
      choices=SUPPORT_PATTERNS),
    P("support_base_pattern_spacing", "Base pattern spacing", "Support",
      "Advanced", "float", "mm", lo=0, hi=20),
    P("support_interface_pattern", "Interface pattern", "Support", "Advanced",
      "enum", choices=INTERFACE_PATTERNS),
    P("support_interface_top_layers", "Top interface layers", "Support",
      "Advanced", "int", lo=0, hi=20),
    P("support_interface_bottom_layers", "Bottom interface layers", "Support",
      "Advanced", "int", lo=-1, hi=20),
    P("support_interface_spacing", "Top interface spacing", "Support",
      "Advanced", "float", "mm", lo=0, hi=20),
    P("tree_support_branch_angle", "Tree support branch angle", "Support",
      "Tree supports", "float", "°", lo=0, hi=60),
    P("tree_support_branch_distance", "Tree support branch distance",
      "Support", "Tree supports", "float", "mm", lo=1, hi=20),
    P("tree_support_branch_diameter", "Tree support branch diameter",
      "Support", "Tree supports", "float", "mm", lo=1, hi=20),
    P("tree_support_wall_count", "Tree support wall loops", "Support",
      "Tree supports", "int", lo=-1, hi=10),

    # ----------------------------------------------------------- Others --- #
    P("skirt_loops", "Skirt loops", "Others", "Skirt", "int", lo=0, hi=20),
    P("skirt_distance", "Skirt distance", "Others", "Skirt", "float", "mm",
      lo=0, hi=60),
    P("skirt_height", "Skirt height", "Others", "Skirt", "int", "layers",
      lo=1, hi=1000),
    P("brim_type", "Brim type", "Others", "Brim", "enum",
      choices=[("auto_brim", "Auto"), ("outer_only", "Outer brim only"),
               ("inner_only", "Inner brim only"),
               ("outer_and_inner", "Outer and inner brim"),
               ("no_brim", "No-brim")]),
    P("brim_width", "Brim width", "Others", "Brim", "float", "mm", lo=0,
      hi=100),
    P("brim_object_gap", "Brim-object gap", "Others", "Brim", "float", "mm",
      lo=0, hi=2),
    P("spiral_mode", "Spiral vase", "Others", "Special mode", "bool"),
    P("slicing_mode", "Slicing mode", "Others", "Special mode", "enum",
      choices=[("regular", "Regular"), ("even_odd", "Even-odd"),
               ("close_holes", "Close holes")]),
    P("print_sequence", "Print sequence", "Others", "Special mode", "enum",
      choices=[("by layer", "By layer"), ("by object", "By object")]),
    P("enable_prime_tower", "Enable prime tower", "Others", "Prime tower",
      "bool"),
    P("prime_tower_width", "Prime tower width", "Others", "Prime tower",
      "float", "mm", lo=2, hi=100),
    P("prime_tower_brim_width", "Prime tower brim width", "Others",
      "Prime tower", "float", "mm", lo=-1, hi=50),
    P("flush_into_infill", "Flush into objects' infill", "Others",
      "Flush options", "bool"),
    P("flush_into_objects", "Flush into objects", "Others", "Flush options",
      "bool"),
    P("flush_into_support", "Flush into objects' support", "Others",
      "Flush options", "bool"),
    P("timelapse_type", "Timelapse", "Others", "G-code output", "enum",
      choices=[("0", "Traditional"), ("1", "Smooth")]),
    P("exclude_object", "Exclude objects", "Others", "G-code output", "bool"),
]

BY_KEY = {p["key"]: p for p in PARAMS}


class BadValue(ValueError):
    pass


def _num(s):
    try:
        return float(s)
    except (TypeError, ValueError):
        raise BadValue(f"not a number: {s!r}")


def validate(key, value):
    """Normalise `value` for `key`; raise BadValue if it's out of spec.
    Returns the string OrcaSlicer expects in the process JSON."""
    p = BY_KEY.get(key)
    if not p:
        raise BadValue(f"unknown setting {key}")
    v = str(value).strip()
    if v == "":
        raise BadValue(f"empty value for {key}")
    kind = p["kind"]
    lo, hi = p.get("min"), p.get("max")

    def check_range(n):
        if lo is not None and n < lo:
            raise BadValue(f"{p['label']}: {n:g} < {lo}")
        if hi is not None and n > hi:
            raise BadValue(f"{p['label']}: {n:g} > {hi}")

    if kind == "bool":
        if v.lower() in ("1", "true", "on", "yes"):
            return "1"
        if v.lower() in ("0", "false", "off", "no"):
            return "0"
        raise BadValue(f"{p['label']}: not a boolean")
    if kind == "enum":
        ok = {c[0] for c in p["choices"]}
        if v not in ok:
            raise BadValue(f"{p['label']}: {v!r} not one of {sorted(ok)}")
        return v
    if kind == "int":
        n = _num(v)
        if n != int(n):
            raise BadValue(f"{p['label']}: must be a whole number")
        check_range(n)
        return str(int(n))
    if kind == "float":
        n = _num(v)
        check_range(n)
        return f"{n:g}"
    if kind == "percent":
        n = _num(v.rstrip("%"))
        check_range(n)
        return f"{n:g}%"
    if kind == "floatpct":
        if v.endswith("%"):
            n = _num(v[:-1])
            if n < 0 or n > 1000:
                raise BadValue(f"{p['label']}: bad percentage")
            return f"{n:g}%"
        n = _num(v)
        check_range(n)
        return f"{n:g}"
    raise BadValue(f"bad kind for {key}")


def validate_all(overrides):
    """{key: raw} -> {key: normalised}; raises BadValue on the first problem."""
    out = {}
    for k, v in (overrides or {}).items():
        if v in (None, ""):
            continue
        out[k] = validate(k, v)
    return out


def extract(config):
    """Pull the exposed keys out of a resolved process config."""
    out = {}
    for p in PARAMS:
        v = config.get(p["key"])
        if isinstance(v, list):
            v = v[0] if v else None
        out[p["key"]] = None if v is None else str(v)
    return out
