#include "CliJob.hpp"

#include "../AABBMesh.hpp"
#include "../CliOptionCatalog.hpp"
#include "../CutUtils.hpp"
#include "../Geometry.hpp"
#include "../LayOnFace.hpp"
#include "../MeshBoolean.hpp"
#include "../QuadricEdgeCollapse.hpp"
#include "../Model.hpp"
#include "../PrintConfig.hpp"
#include "../TriangleSelector.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/algorithm/string/case_conv.hpp>

#include <nlohmann/json.hpp>

#include <cmath>
#include <set>
#include <sstream>
#include <stdexcept>

namespace Slic3r {

namespace {

using json = nlohmann::json;
namespace fs = boost::filesystem;

struct JobError : std::runtime_error
{
    JobError(const std::string &where, const std::string &what) : std::runtime_error(where + ": " + what) {}
};

// ---------------------------------------------------------------- helpers

void check_fields(const json &j, const std::string &where, std::initializer_list<const char *> allowed)
{
    if (!j.is_object())
        throw JobError(where, "must be a JSON object");
    for (auto it = j.begin(); it != j.end(); ++it) {
        bool ok = false;
        for (const char *a : allowed)
            ok |= it.key() == a;
        if (!ok) {
            std::string list;
            for (const char *a : allowed)
                list += std::string(list.empty() ? "" : ", ") + a;
            throw JobError(where + "." + it.key(), "unknown field (allowed: " + list + ")");
        }
    }
}

double num(const json &j, const std::string &where)
{
    if (!j.is_number())
        throw JobError(where, "must be a number");
    const double v = j.get<double>();
    if (!std::isfinite(v))
        throw JobError(where, "must be finite");
    return v;
}

Vec3d vec3(const json &j, const std::string &where, size_t min_size = 3, double fill = 0.)
{
    Vec3d v(fill, fill, fill);
    if (!j.is_array() || j.size() < min_size || j.size() > 3)
        throw JobError(where, min_size == 3 ? "must be [x, y, z]" : "must be [x, y] or [x, y, z]");
    for (size_t i = 0; i < j.size(); ++i)
        v[i] = num(j[i], where + "[" + std::to_string(i) + "]");
    return v;
}

std::string str(const json &j, const std::string &where)
{
    if (!j.is_string())
        throw JobError(where, "must be a string");
    return j.get<std::string>();
}

// JSON value -> the string form ConfigOption::deserialize() reads.
std::string config_value_string(const json &v, const std::string &where)
{
    if (v.is_string())
        return v.get<std::string>();
    if (v.is_boolean())
        return v.get<bool>() ? "1" : "0";
    if (v.is_number_integer())
        return std::to_string(v.get<long long>());
    if (v.is_number()) {
        std::ostringstream ss;
        ss.precision(12);
        ss << v.get<double>();
        return ss.str();
    }
    if (v.is_array()) {
        std::string out;
        for (size_t i = 0; i < v.size(); ++i)
            out += (i ? "," : "") + config_value_string(v[i], where + "[" + std::to_string(i) + "]");
        return out;
    }
    throw JobError(where, "must be a string, number, bool or array");
}

enum class Scope { Object, Part, LayerRange, Plate };

const char *scope_name(Scope s)
{
    switch (s) {
    case Scope::Object: return "object";
    case Scope::Part: return "part/modifier";
    case Scope::LayerRange: return "height range";
    default: return "plate";
    }
}

// Validate every key against the option catalog (the GUI only offers these keys at that level)
// and deserialize strictly: a value the option can't parse, or an enum value it would substitute,
// is an error rather than a silent default.
void apply_settings(const json &settings, const std::string &where, Scope scope, DynamicPrintConfig &out)
{
    if (!settings.is_object())
        throw JobError(where, "must be an object of {\"key\": value}");
    for (auto it = settings.begin(); it != settings.end(); ++it) {
        const std::string  key  = it.key();
        const std::string  at   = where + "." + key;
        const CliOptionInfo info = cli_option_info(key);
        if (info.key.empty())
            throw JobError(at, "unknown setting (see orca-slicer --list-options)");
        const bool ok = scope == Scope::Object ? info.per_object :
                        scope == Scope::Part ? info.per_part :
                        scope == Scope::LayerRange ? info.per_layer_range : info.per_plate;
        if (!ok)
            throw JobError(at, std::string("can't be set per ") + scope_name(scope) +
                                   (info.preset.empty() ? "" : " (it is a " + info.preset + " setting: set it globally)"));
        ConfigSubstitutionContext ctx(ForwardCompatibilitySubstitutionRule::Enable);
        const std::string value = config_value_string(it.value(), at);
        try {
            out.set_deserialize(key, value, ctx);
        } catch (const std::exception &e) {
            throw JobError(at, std::string("bad value \"") + value + "\": " + e.what());
        }
        if (!ctx.substitutions.empty())
            throw JobError(at, "bad value \"" + value + "\" (would be replaced by \"" +
                                   ctx.substitutions.front().new_value->serialize() + "\")");
    }
}

TriangleMesh primitive_mesh(const json &p, const std::string &where)
{
    check_fields(p, where, {"shape", "size", "radius", "height", "segments"});
    if (!p.contains("shape"))
        throw JobError(where + ".shape", "missing (box, cylinder, cone, sphere)");
    const std::string shape = str(p["shape"], where + ".shape");
    const double      fa    = p.contains("segments") ? 2. * PI / std::max(3., num(p["segments"], where + ".segments")) : 2. * PI / 90.;
    auto positive = [&](const char *k) {
        if (!p.contains(k))
            throw JobError(where + "." + k, std::string("missing (needed for ") + shape + ")");
        const double v = num(p[k], where + "." + k);
        if (v <= 0)
            throw JobError(where + "." + k, "must be > 0");
        return v;
    };
    TriangleMesh mesh;
    if (shape == "box") {
        const Vec3d s = vec3(p.contains("size") ? p["size"] : json(), where + ".size");
        if (s.minCoeff() <= 0)
            throw JobError(where + ".size", "must be > 0");
        mesh = make_cube(s.x(), s.y(), s.z());
    } else if (shape == "cylinder")
        mesh = make_cylinder(positive("radius"), positive("height"), fa);
    else if (shape == "cone")
        mesh = make_cone(positive("radius"), positive("height"), fa);
    else if (shape == "sphere")
        mesh = make_sphere(positive("radius"), fa);
    else
        throw JobError(where + ".shape", "unknown shape \"" + shape + "\" (box, cylinder, cone, sphere)");
    // Primitives are placed by their bounding box centre.
    mesh.translate(-mesh.bounding_box().center().cast<float>());
    return mesh;
}

std::string resolve(const std::string &base_dir, const std::string &file)
{
    fs::path p(file);
    if (p.is_relative() && !base_dir.empty())
        p = fs::path(base_dir) / p;
    return p.string();
}

// Load one object from a mesh/project file. `pick` selects an object of a multi-object file.
ModelObject *load_object_file(Model &dst, const std::string &file, const json *pick, const std::string &where)
{
    if (!fs::exists(file))
        throw JobError(where, "no such file: " + file);
    Model src;
    try {
        DynamicPrintConfig        cfg;
        ConfigSubstitutionContext ctx(ForwardCompatibilitySubstitutionRule::Enable);
        src = Model::read_from_file(file, &cfg, &ctx, LoadStrategy::LoadModel);
    } catch (const std::exception &e) {
        throw JobError(where, "can't read " + file + ": " + e.what());
    }
    if (src.objects.empty())
        throw JobError(where, "no objects in " + file);
    ModelObject *chosen = nullptr;
    if (pick != nullptr) {
        if (pick->is_number_integer()) {
            const int i = pick->get<int>();
            if (i < 0 || i >= int(src.objects.size()))
                throw JobError(where, "object index " + std::to_string(i) + " out of range (file has " +
                                          std::to_string(src.objects.size()) + " objects)");
            chosen = src.objects[i];
        } else if (pick->is_string()) {
            for (ModelObject *o : src.objects)
                if (o->name == pick->get<std::string>())
                    chosen = o;
            if (chosen == nullptr)
                throw JobError(where, "no object named \"" + pick->get<std::string>() + "\" in " + file);
        } else
            throw JobError(where, "\"object\" must be an index or a name");
    } else if (src.objects.size() == 1)
        chosen = src.objects.front();
    else {
        // Several objects and no pick: one object with all of them as parts (as "Assemble" does).
        ModelObject *merged = dst.add_object();
        merged->name        = fs::path(file).stem().string();
        merged->input_file  = file;
        for (ModelObject *o : src.objects)
            for (const ModelVolume *v : o->volumes) {
                ModelVolume *nv = merged->add_volume(*v);
                // bake the source instance placement so the parts keep their relative positions
                if (!o->instances.empty())
                    nv->set_transformation(Geometry::Transformation(o->instances.front()->get_matrix() * v->get_matrix()));
            }
        return merged;
    }
    ModelObject *obj = dst.add_object(*chosen);
    obj->clear_instances();
    if (obj->input_file.empty())
        obj->input_file = file;
    if (obj->name.empty())
        obj->name = fs::path(file).stem().string();
    return obj;
}

ModelVolumeType volume_type(const std::string &t, const std::string &where)
{
    static const std::map<std::string, ModelVolumeType> types = {
        {"part", ModelVolumeType::MODEL_PART},
        {"negative", ModelVolumeType::NEGATIVE_VOLUME},
        {"modifier", ModelVolumeType::PARAMETER_MODIFIER},
        {"support_blocker", ModelVolumeType::SUPPORT_BLOCKER},
        {"support_enforcer", ModelVolumeType::SUPPORT_ENFORCER},
        {"seam_center", ModelVolumeType::PRECISE_SEAM_CENTER},
        {"seam_left", ModelVolumeType::PRECISE_SEAM_LEFT},
        {"seam_right", ModelVolumeType::PRECISE_SEAM_RIGHT},
        {"seam_enforced", ModelVolumeType::PRECISE_SEAM_ENFORCED},
        {"seam_blocked", ModelVolumeType::PRECISE_SEAM_BLOCKED},
        {"seam_neutral", ModelVolumeType::PRECISE_SEAM_NEUTRAL},
    };
    auto it = types.find(t);
    if (it == types.end()) {
        std::string list;
        for (auto &kv : types)
            list += (list.empty() ? "" : ", ") + kv.first;
        throw JobError(where, "unknown part type \"" + t + "\" (" + list + ")");
    }
    return it->second;
}

Vec3d degrees_to_radians(const Vec3d &d) { return d * (PI / 180.); }

Vec3d scale_vec(const json &j, const std::string &where)
{
    Vec3d s = j.is_number() ? Vec3d::Constant(num(j, where)) : vec3(j, where);
    if (s.minCoeff() <= 0)
        throw JobError(where, "must be > 0");
    return s;
}

Vec3d mirror_vec(const json &j, const std::string &where)
{
    Vec3d m = Vec3d::Ones();
    auto axis = [&](const std::string &a, const std::string &w) {
        if (a == "x") m.x() = -1;
        else if (a == "y") m.y() = -1;
        else if (a == "z") m.z() = -1;
        else throw JobError(w, "mirror axis must be x, y or z");
    };
    if (j.is_string())
        axis(j.get<std::string>(), where);
    else if (j.is_array())
        for (size_t i = 0; i < j.size(); ++i)
            axis(str(j[i], where + "[" + std::to_string(i) + "]"), where + "[" + std::to_string(i) + "]");
    else
        throw JobError(where, "must be \"x\" or a list like [\"x\", \"z\"]");
    return m;
}

int filament_index(const json &j, const std::string &where)
{
    if (!j.is_number_integer() || j.get<int>() < 1 || j.get<int>() > int(EnforcerBlockerType::ExtruderMax))
        throw JobError(where, "must be a filament number from 1");
    return j.get<int>();
}

// ---------------------------------------------------------------- paint

struct PaintRule
{
    std::string                 layer;   // support, seam, fuzzy_skin, color
    EnforcerBlockerType         state = EnforcerBlockerType::NONE;
    std::vector<int>            parts;   // model-part indices (among MODEL_PART volumes); empty = all
    bool                        all = false;
    std::optional<Vec3d>        normal;
    double                      max_angle = 30.;
    std::optional<Vec2d>        z;
    std::optional<std::pair<Vec3d, Vec3d>> box;
    std::optional<std::pair<Vec3d, double>> sphere;
    std::set<int>               facets;
    std::unique_ptr<AABBMesh>   inside;
    bool                        mesh_frame = false; // geometric criteria in the source mesh frame, not object frame
    bool                        invert = false;
};

PaintRule parse_paint(const json &p, const std::string &where, const std::string &base_dir)
{
    check_fields(p, where, {"layer", "value", "parts", "all", "normal", "max_angle", "z", "box", "sphere", "facets", "inside", "frame", "invert"});
    PaintRule r;
    if (!p.contains("layer"))
        throw JobError(where + ".layer", "missing (support, seam, fuzzy_skin, color)");
    r.layer = str(p["layer"], where + ".layer");
    const json        value = p.contains("value") ? p["value"] : json();
    const std::string vwhere = where + ".value";
    auto enforce_block = [&]() {
        const std::string v = value.is_null() ? "enforce" : str(value, vwhere);
        if (v == "enforce") return EnforcerBlockerType::ENFORCER;
        if (v == "block") return EnforcerBlockerType::BLOCKER;
        if (v == "none") return EnforcerBlockerType::NONE;
        throw JobError(vwhere, "must be enforce, block or none");
    };
    if (r.layer == "support" || r.layer == "seam")
        r.state = enforce_block();
    else if (r.layer == "fuzzy_skin") {
        const std::string v = value.is_null() ? "on" : (value.is_boolean() ? (value.get<bool>() ? "on" : "none") : str(value, vwhere));
        if (v != "on" && v != "none")
            throw JobError(vwhere, "must be on or none");
        r.state = v == "on" ? EnforcerBlockerType::FUZZY_SKIN : EnforcerBlockerType::NONE;
    } else if (r.layer == "color") {
        if (value.is_null())
            throw JobError(vwhere, "missing: the filament number to paint with (0 clears)");
        if (value.is_number_integer() && value.get<int>() == 0)
            r.state = EnforcerBlockerType::NONE;
        else
            r.state = EnforcerBlockerType(filament_index(value, vwhere));
    } else
        throw JobError(where + ".layer", "unknown paint layer \"" + r.layer + "\" (support, seam, fuzzy_skin, color)");

    if (p.contains("parts")) {
        if (!p["parts"].is_array())
            throw JobError(where + ".parts", "must be a list of part indices");
        for (size_t i = 0; i < p["parts"].size(); ++i) {
            if (!p["parts"][i].is_number_integer() || p["parts"][i].get<int>() < 0)
                throw JobError(where + ".parts[" + std::to_string(i) + "]", "must be a part index from 0");
            r.parts.push_back(p["parts"][i].get<int>());
        }
    }
    int criteria = 0;
    if (p.contains("all")) {
        r.all = p["all"].is_boolean() && p["all"].get<bool>();
        criteria += r.all;
    }
    if (p.contains("normal")) {
        Vec3d n = vec3(p["normal"], where + ".normal");
        if (n.norm() < EPSILON)
            throw JobError(where + ".normal", "must not be zero");
        r.normal = n.normalized();
        ++criteria;
    }
    if (p.contains("max_angle")) {
        if (!p.contains("normal"))
            throw JobError(where + ".max_angle", "only valid with \"normal\"");
        r.max_angle = num(p["max_angle"], where + ".max_angle");
    }
    if (p.contains("z")) {
        if (!p["z"].is_array() || p["z"].size() != 2)
            throw JobError(where + ".z", "must be [min_z, max_z]");
        r.z = Vec2d(num(p["z"][0], where + ".z[0]"), num(p["z"][1], where + ".z[1]"));
        ++criteria;
    }
    if (p.contains("box")) {
        const json &b = p["box"];
        check_fields(b, where + ".box", {"min", "max"});
        if (!b.contains("min") || !b.contains("max"))
            throw JobError(where + ".box", "needs min and max");
        r.box = std::make_pair(vec3(b["min"], where + ".box.min"), vec3(b["max"], where + ".box.max"));
        ++criteria;
    }
    if (p.contains("sphere")) {
        const json &s = p["sphere"];
        check_fields(s, where + ".sphere", {"center", "radius"});
        if (!s.contains("center") || !s.contains("radius"))
            throw JobError(where + ".sphere", "needs center and radius");
        r.sphere = std::make_pair(vec3(s["center"], where + ".sphere.center"), num(s["radius"], where + ".sphere.radius"));
        ++criteria;
    }
    if (p.contains("facets")) {
        if (!p["facets"].is_array())
            throw JobError(where + ".facets", "must be a list of facet indices");
        for (const json &f : p["facets"]) {
            if (!f.is_number_integer() || f.get<int>() < 0)
                throw JobError(where + ".facets", "facet indices must be integers from 0");
            r.facets.insert(f.get<int>());
        }
        ++criteria;
    }
    if (p.contains("inside")) {
        const std::string file = resolve(base_dir, str(p["inside"], where + ".inside"));
        if (!fs::exists(file))
            throw JobError(where + ".inside", "no such file: " + file);
        Model mask;
        try {
            mask = Model::read_from_file(file, nullptr, nullptr, LoadStrategy::LoadModel | LoadStrategy::AddDefaultInstances);
        } catch (const std::exception &e) {
            throw JobError(where + ".inside", "can't read " + file + ": " + e.what());
        }
        if (mask.objects.empty())
            throw JobError(where + ".inside", "no mesh in " + file);
        r.inside = std::make_unique<AABBMesh>(mask.mesh());
        ++criteria;
    }
    if (p.contains("frame")) {
        const std::string f = str(p["frame"], where + ".frame");
        if (f != "object" && f != "mesh")
            throw JobError(where + ".frame", "must be object or mesh");
        r.mesh_frame = f == "mesh";
    }
    if (p.contains("invert"))
        r.invert = p["invert"].is_boolean() && p["invert"].get<bool>();
    if (criteria == 0)
        throw JobError(where, "needs a selection: all, normal, z, box, sphere, facets or inside");
    return r;
}

FacetsAnnotation &paint_layer(ModelVolume &v, const std::string &layer)
{
    if (layer == "support") return v.supported_facets;
    if (layer == "seam") return v.seam_facets;
    if (layer == "fuzzy_skin") return v.fuzzy_skin_facets;
    return v.mmu_segmentation_facets;
}

// Paints the object's model parts; returns the number of facets painted per rule.
std::vector<size_t> apply_paint(ModelObject &obj, const std::vector<PaintRule> &rules, const std::string &where)
{
    std::vector<size_t> counts(rules.size(), 0);
    if (rules.empty())
        return counts;
    const Transform3d inst = obj.instances.front()->get_matrix();
    // Object frame: oriented as on the bed, origin at the object's bounding box minimum.
    const Vec3d       origin = obj.instance_bounding_box(0).min;

    std::vector<ModelVolume *> parts;
    for (ModelVolume *v : obj.volumes)
        if (v->is_model_part())
            parts.push_back(v);

    for (size_t ri = 0; ri < rules.size(); ++ri)
        for (int pi : rules[ri].parts)
            if (pi >= int(parts.size()))
                throw JobError(where + "[" + std::to_string(ri) + "].parts", "part " + std::to_string(pi) + " doesn't exist (object has " +
                                                                                 std::to_string(parts.size()) + " parts)");

    for (size_t pi = 0; pi < parts.size(); ++pi) {
        ModelVolume       &vol   = *parts[pi];
        const TriangleMesh &mesh = vol.mesh();
        const Transform3d   world = inst * vol.get_matrix();
        const Transform3d   to_mesh_frame = vol.get_matrix(); // volume frame -> source mesh (object) frame
        const Matrix3d      normal_m = world.linear().inverse().transpose();
        const auto         &its  = mesh.its;

        std::map<std::string, std::unique_ptr<TriangleSelector>> selectors;
        for (size_t ri = 0; ri < rules.size(); ++ri) {
            const PaintRule &r = rules[ri];
            if (!r.parts.empty() && std::find(r.parts.begin(), r.parts.end(), int(pi)) == r.parts.end())
                continue;
            std::unique_ptr<TriangleSelector> &sel = selectors[r.layer];
            if (!sel) {
                sel = std::make_unique<TriangleSelector>(mesh);
                FacetsAnnotation &fa = paint_layer(vol, r.layer);
                if (!fa.empty())
                    sel->deserialize(fa.get_data(), false);
            }
            if (r.facets.size() && *r.facets.rbegin() >= int(its.indices.size()))
                throw JobError(where + "[" + std::to_string(ri) + "].facets",
                               "facet " + std::to_string(*r.facets.rbegin()) + " out of range (part " + std::to_string(pi) + " has " +
                                   std::to_string(its.indices.size()) + " facets)");
            for (int f = 0; f < int(its.indices.size()); ++f) {
                const Vec3i32 &t  = its.indices[f];
                const Vec3d    a  = its.vertices[t[0]].cast<double>(), b = its.vertices[t[1]].cast<double>(), c = its.vertices[t[2]].cast<double>();
                const Vec3d    cl = (a + b + c) / 3.;
                const Vec3d    cw = world * cl - origin;           // object frame
                const Vec3d    cm = to_mesh_frame * cl;            // mesh frame
                const Vec3d    cp = r.mesh_frame ? cm : cw;
                bool hit = true;
                if (r.normal) {
                    Vec3d n = normal_m * (b - a).cross(c - a);
                    hit &= n.norm() > 0 && n.normalized().dot(*r.normal) >= std::cos(r.max_angle * PI / 180.);
                }
                if (r.z)
                    hit &= cp.z() >= r.z->x() && cp.z() <= r.z->y();
                if (r.box)
                    hit &= (cp.array() >= r.box->first.array()).all() && (cp.array() <= r.box->second.array()).all();
                if (r.sphere)
                    hit &= (cp - r.sphere->first).norm() <= r.sphere->second;
                if (!r.facets.empty())
                    hit &= r.facets.count(f) > 0;
                if (r.inside && hit) {
                    // Inside test: the first surface hit along a ray is seen from behind.
                    const Vec3d dir = Vec3d(0.0123, 0.0071, 1.).normalized();
                    auto        h   = r.inside->query_ray_hit(cp, dir);
                    hit &= h.is_inside();
                }
                if (r.invert)
                    hit = !hit;
                if (hit) {
                    sel->set_facet(f, r.state);
                    ++counts[ri];
                }
            }
        }
        for (auto &[layer, sel] : selectors)
            paint_layer(vol, layer).set(*sel);
    }
    return counts;
}

// ---------------------------------------------------------------- objects

struct BuiltObject
{
    std::vector<ModelObject *> objects; // more than one after a cut
};

void set_extruder(ModelConfig &cfg, int filament) { cfg.set_key_value("extruder", new ConfigOptionInt(filament)); }

BuiltObject build_object(const json &o, const std::string &where, const std::string &base_dir, Model &model, CliJobResult &result,
                         std::set<int> &used_filaments, int plate_idx)
{
    check_fields(o, where, {"file", "primitive", "object", "name", "count", "position", "positions", "rotate", "scale", "mirror", "lay_on",
                            "orient", "filament", "settings", "height_ranges", "parts", "paint", "cut", "simplify", "boolean", "split"});
    if (o.contains("file") == o.contains("primitive"))
        throw JobError(where, "needs exactly one of \"file\" or \"primitive\"");

    ModelObject *obj = nullptr;
    if (o.contains("file"))
        obj = load_object_file(model, resolve(base_dir, str(o["file"], where + ".file")), o.contains("object") ? &o["object"] : nullptr,
                               where + ".file");
    else {
        if (o.contains("object"))
            throw JobError(where + ".object", "only valid with \"file\"");
        obj = model.add_object("primitive", "", primitive_mesh(o["primitive"], where + ".primitive"));
    }
    if (o.contains("name"))
        obj->name = str(o["name"], where + ".name");

    int filament = 0;
    if (o.contains("filament")) {
        filament = filament_index(o["filament"], where + ".filament");
        set_extruder(obj->config, filament);
        // the object's own parts follow the object
        for (ModelVolume *v : obj->volumes)
            v->config.erase("extruder");
        used_filaments.insert(filament);
    } else
        used_filaments.insert(obj->config.has("extruder") ? std::max(1, obj->config.extruder()) : 1);

    if (o.contains("settings")) {
        DynamicPrintConfig cfg;
        apply_settings(o["settings"], where + ".settings", Scope::Object, cfg);
        obj->config.apply(cfg);
    }

    // Mesh edits on the object's model parts, as the Simplify and Mesh boolean gizmos do.
    if (o.contains("boolean")) {
        if (!o["boolean"].is_array())
            throw JobError(where + ".boolean", "must be a list");
        for (size_t i = 0; i < o["boolean"].size(); ++i) {
            const json        &b  = o["boolean"][i];
            const std::string  bw = where + ".boolean[" + std::to_string(i) + "]";
            check_fields(b, bw, {"op", "file", "primitive", "object", "position", "rotate", "scale", "part"});
            if (b.contains("file") == b.contains("primitive"))
                throw JobError(bw, "needs exactly one of \"file\" or \"primitive\"");
            const std::string op = b.contains("op") ? str(b["op"], bw + ".op") : "minus";
            if (op != "minus" && op != "plus" && op != "intersect")
                throw JobError(bw + ".op", "must be minus, plus or intersect");
            TriangleMesh tool;
            if (b.contains("file")) {
                Model tmp;
                tool = load_object_file(tmp, resolve(base_dir, str(b["file"], bw + ".file")), b.contains("object") ? &b["object"] : nullptr, bw + ".file")->raw_mesh();
            } else
                tool = primitive_mesh(b["primitive"], bw + ".primitive");
            Geometry::Transformation t;
            if (b.contains("rotate"))
                t.set_rotation(degrees_to_radians(vec3(b["rotate"], bw + ".rotate")));
            if (b.contains("scale"))
                t.set_scaling_factor(scale_vec(b["scale"], bw + ".scale"));
            if (b.contains("position"))
                t.set_offset(vec3(b["position"], bw + ".position"));
            tool.transform(t.get_matrix());
            int part = b.contains("part") ? (b["part"].is_number_integer() ? b["part"].get<int>() : -1) : 0;
            std::vector<ModelVolume *> parts;
            for (ModelVolume *v : obj->volumes)
                if (v->is_model_part())
                    parts.push_back(v);
            if (part < 0 || part >= int(parts.size()))
                throw JobError(bw + ".part", "no model part " + std::to_string(part));
            ModelVolume *v = parts[part];
            // Work in the source mesh frame: bake the volume transform, then restore it.
            TriangleMesh mesh = v->mesh();
            mesh.transform(v->get_matrix());
            try {
                if (op == "minus") MeshBoolean::cgal::minus(mesh, tool);
                else if (op == "plus") MeshBoolean::cgal::plus(mesh, tool);
                else MeshBoolean::cgal::intersect(mesh, tool);
            } catch (const std::exception &e) {
                throw JobError(bw, std::string("boolean failed (are both meshes closed?): ") + e.what());
            }
            if (mesh.empty())
                throw JobError(bw, "the boolean left an empty mesh");
            mesh.transform(v->get_matrix().inverse());
            v->set_mesh(std::move(mesh));
            v->calculate_convex_hull();
            v->set_new_unique_id();
        }
        obj->invalidate_bounding_box();
    }
    if (o.contains("simplify")) {
        const json        &sj = o["simplify"];
        const std::string  sw = where + ".simplify";
        check_fields(sj, sw, {"ratio", "triangles"});
        if (sj.contains("ratio") == sj.contains("triangles"))
            throw JobError(sw, "needs exactly one of ratio (0..1 of the triangles kept) or triangles");
        for (ModelVolume *v : obj->volumes) {
            if (!v->is_model_part())
                continue;
            indexed_triangle_set its = v->mesh().its;
            uint32_t target = 0;
            if (sj.contains("ratio")) {
                const double r = num(sj["ratio"], sw + ".ratio");
                if (r <= 0 || r > 1)
                    throw JobError(sw + ".ratio", "must be in (0, 1]");
                target = uint32_t(std::max(4.0, std::round(r * its.indices.size())));
            } else {
                if (!sj["triangles"].is_number_integer() || sj["triangles"].get<int>() < 4)
                    throw JobError(sw + ".triangles", "must be an integer >= 4");
                target = uint32_t(sj["triangles"].get<int>());
            }
            if (target >= its.indices.size())
                continue;
            its_quadric_edge_collapse(its, target);
            v->set_mesh(std::move(its));
            v->calculate_convex_hull();
            v->set_new_unique_id();
            // painting refers to the old facets
            v->supported_facets.reset();
            v->seam_facets.reset();
            v->mmu_segmentation_facets.reset();
            v->fuzzy_skin_facets.reset();
        }
        obj->invalidate_bounding_box();
    }

    // Extra parts, modifiers, negative volumes, support enforcers/blockers: in the source mesh frame.
    if (o.contains("parts")) {
        if (!o["parts"].is_array())
            throw JobError(where + ".parts", "must be a list");
        for (size_t i = 0; i < o["parts"].size(); ++i) {
            const json        &p  = o["parts"][i];
            const std::string  pw = where + ".parts[" + std::to_string(i) + "]";
            check_fields(p, pw, {"type", "file", "primitive", "object", "name", "position", "rotate", "scale", "mirror", "filament", "settings"});
            if (p.contains("file") == p.contains("primitive"))
                throw JobError(pw, "needs exactly one of \"file\" or \"primitive\"");
            const ModelVolumeType type = volume_type(p.contains("type") ? str(p["type"], pw + ".type") : "modifier", pw + ".type");
            TriangleMesh          mesh;
            std::string           name;
            if (p.contains("file")) {
                Model        tmp;
                ModelObject *src = load_object_file(tmp, resolve(base_dir, str(p["file"], pw + ".file")), p.contains("object") ? &p["object"] : nullptr,
                                                    pw + ".file");
                mesh = src->raw_mesh();
                name = src->name;
            } else {
                mesh = primitive_mesh(p["primitive"], pw + ".primitive");
                name = std::string(p["primitive"].value("shape", "primitive"));
            }
            if (mesh.empty())
                throw JobError(pw, "empty mesh");
            ModelVolume *v = obj->add_volume(std::move(mesh), type);
            v->name        = p.contains("name") ? str(p["name"], pw + ".name") : name;
            Geometry::Transformation t = v->get_transformation();
            if (p.contains("rotate"))
                t.set_rotation(degrees_to_radians(vec3(p["rotate"], pw + ".rotate")));
            if (p.contains("scale"))
                t.set_scaling_factor(scale_vec(p["scale"], pw + ".scale"));
            if (p.contains("mirror"))
                t.set_mirror(mirror_vec(p["mirror"], pw + ".mirror"));
            if (p.contains("position"))
                t.set_offset(t.get_offset() + vec3(p["position"], pw + ".position"));
            v->set_transformation(t);
            if (p.contains("filament")) {
                if (type != ModelVolumeType::MODEL_PART && type != ModelVolumeType::PARAMETER_MODIFIER)
                    throw JobError(pw + ".filament", "only parts and modifiers take a filament");
                const int f = filament_index(p["filament"], pw + ".filament");
                set_extruder(v->config, f);
                used_filaments.insert(f);
            }
            if (p.contains("settings")) {
                if (type != ModelVolumeType::MODEL_PART && type != ModelVolumeType::PARAMETER_MODIFIER)
                    throw JobError(pw + ".settings", "only parts and modifiers take settings");
                DynamicPrintConfig cfg;
                apply_settings(p["settings"], pw + ".settings", Scope::Part, cfg);
                v->config.apply(cfg);
            }
        }
        obj->sort_volumes(true);
    }

    // Transforms of the whole object go on its instance, as the GUI's move/rotate/scale do.
    ModelInstance *inst = obj->add_instance();
    if (o.contains("scale"))
        inst->set_scaling_factor(scale_vec(o["scale"], where + ".scale"));
    if (o.contains("mirror"))
        inst->set_mirror(mirror_vec(o["mirror"], where + ".mirror"));
    if (o.contains("rotate"))
        inst->set_rotation(degrees_to_radians(vec3(o["rotate"], where + ".rotate")));
    if (o.contains("lay_on")) {
        const json  &l  = o["lay_on"];
        const std::string lw = where + ".lay_on";
        const auto   planes = lay_on_face_planes(*obj, inst->get_matrix_no_offset());
        if (planes.empty())
            throw JobError(lw, "the object has no face large enough to lay on");
        int idx = -1;
        if (l.is_string() && l.get<std::string>() == "largest_face")
            idx = find_largest_plane(planes);
        else if (l.is_array())
            idx = find_plane_by_normal(planes, vec3(l, lw));
        else
            throw JobError(lw, "must be \"largest_face\" or a direction [nx, ny, nz] in object coordinates");
        if (idx < 0)
            throw JobError(lw, "no face found");
        lay_on_face(*obj, 0, planes[idx].normal);
    }
    if (o.contains("orient")) {
        if (!o["orient"].is_boolean())
            throw JobError(where + ".orient", "must be true or false");
        result.orient[obj->id().id] = o["orient"].get<bool>();
    }
    obj->invalidate_bounding_box();

    if (o.contains("height_ranges")) {
        if (!o["height_ranges"].is_array())
            throw JobError(where + ".height_ranges", "must be a list");
        for (size_t i = 0; i < o["height_ranges"].size(); ++i) {
            const json        &r  = o["height_ranges"][i];
            const std::string  rw = where + ".height_ranges[" + std::to_string(i) + "]";
            check_fields(r, rw, {"min_z", "max_z", "settings"});
            if (!r.contains("min_z") || !r.contains("max_z"))
                throw JobError(rw, "needs min_z and max_z (mm above the bed)");
            const double lo = num(r["min_z"], rw + ".min_z"), hi = num(r["max_z"], rw + ".max_z");
            if (lo < 0 || hi <= lo)
                throw JobError(rw, "needs 0 <= min_z < max_z");
            DynamicPrintConfig cfg;
            if (r.contains("settings"))
                apply_settings(r["settings"], rw + ".settings", Scope::LayerRange, cfg);
            // A range without layer_height gets the object's layer height in the CLI once the
            // presets are known (the slicer needs it in every range; the GUI always stores it).
            if (cfg.has("extruder"))
                used_filaments.insert(cfg.opt_int("extruder"));
            obj->layer_config_ranges[{lo, hi}].assign_config(std::move(cfg));
        }
    }

    // Paint, evaluated with the object oriented as it will print.
    std::vector<PaintRule> rules;
    if (o.contains("paint")) {
        if (!o["paint"].is_array())
            throw JobError(where + ".paint", "must be a list");
        for (size_t i = 0; i < o["paint"].size(); ++i) {
            rules.push_back(parse_paint(o["paint"][i], where + ".paint[" + std::to_string(i) + "]", base_dir));
            if (rules.back().layer == "color" && int(rules.back().state) > 0)
                used_filaments.insert(int(rules.back().state));
        }
    }
    const std::vector<size_t> painted = apply_paint(*obj, rules, where + ".paint");

    BuiltObject built;
    if (o.contains("cut")) {
        const json        &c  = o["cut"];
        const std::string  cw = where + ".cut";
        check_fields(c, cw, {"z", "keep", "flip_upper", "keep_as_parts", "place_on_cut"});
        if (!c.contains("z"))
            throw JobError(cw + ".z", "missing (cut height in mm above the bed)");
        const BoundingBoxf3 bb = obj->instance_bounding_box(0);
        const double        z  = num(c["z"], cw + ".z");
        if (z <= 0 || z >= bb.size().z())
            throw JobError(cw + ".z", "must be inside the object (0 < z < " + std::to_string(bb.size().z()) + ")");
        const std::string keep = c.contains("keep") ? str(c["keep"], cw + ".keep") : "both";
        ModelObjectCutAttributes attrs = ModelObjectCutAttribute::KeepPaint;
        if (keep == "both" || keep == "upper")
            attrs = attrs | ModelObjectCutAttribute::KeepUpper;
        if (keep == "both" || keep == "lower")
            attrs = attrs | ModelObjectCutAttribute::KeepLower;
        if (keep != "both" && keep != "upper" && keep != "lower")
            throw JobError(cw + ".keep", "must be both, upper or lower");
        if (c.value("flip_upper", false))
            attrs = attrs | ModelObjectCutAttribute::FlipUpper;
        if (c.value("keep_as_parts", false))
            attrs = attrs | ModelObjectCutAttribute::KeepAsParts;
        if (c.value("place_on_cut", false))
            attrs = attrs | ModelObjectCutAttribute::PlaceOnCutUpper | ModelObjectCutAttribute::PlaceOnCutLower;
        const Vec3d off = obj->instances.front()->get_offset();
        Cut         cut(obj, 0, Geometry::translation_transform((bb.min.z() + z) * Vec3d::UnitZ() - off), attrs);
        const ModelObjectPtrs &pieces = cut.perform_with_plane();
        if (pieces.empty())
            throw JobError(cw, "the cut produced nothing");
        for (const ModelObject *piece : pieces) {
            ModelObject *n = model.add_object(*piece);
            built.objects.push_back(n);
        }
        model.delete_object(obj);
    } else
        built.objects.push_back(obj);

    // Split to objects, as the GUI's "Split to objects" does: one object per connected shell.
    if (o.contains("split")) {
        if (!o["split"].is_boolean())
            throw JobError(where + ".split", "must be true or false");
        if (o["split"].get<bool>()) {
            std::vector<ModelObject *> split;
            for (ModelObject *b : built.objects) {
                ModelObjectPtrs pieces;
                b->split(&pieces, true);
                if (pieces.size() <= 1) {
                    // nothing to split: split() still made a copy
                    for (ModelObject *pc : pieces)
                        model.delete_object(pc);
                    split.push_back(b);
                    continue;
                }
                model.delete_object(b);
                for (ModelObject *pc : pieces) {
                    if (pc->instances.empty())
                        pc->add_instance();
                    split.push_back(pc);
                }
            }
            built.objects = std::move(split);
        }
    }

    // Copies and placement. Positions are plate coordinates of each copy's bounding box centre;
    // without them the plate's arrange places the copies.
    int count = 1;
    if (o.contains("count")) {
        if (!o["count"].is_number_integer() || o["count"].get<int>() < 1)
            throw JobError(where + ".count", "must be an integer >= 1");
        count = o["count"].get<int>();
    }
    std::vector<Vec3d> positions;
    if (o.contains("position") && o.contains("positions"))
        throw JobError(where, "use either position or positions");
    if (o.contains("position"))
        positions.push_back(vec3(o["position"], where + ".position", 2));
    if (o.contains("positions")) {
        if (!o["positions"].is_array())
            throw JobError(where + ".positions", "must be a list of [x, y]");
        for (size_t i = 0; i < o["positions"].size(); ++i)
            positions.push_back(vec3(o["positions"][i], where + ".positions[" + std::to_string(i) + "]", 2));
        count = std::max<int>(count, int(positions.size()));
    }
    for (ModelObject *b : built.objects) {
        while (int(b->instances.size()) < count)
            b->add_instance(*b->instances.front());
        for (size_t i = 0; i < b->instances.size(); ++i) {
            ModelInstance *mi = b->instances[i];
            BoundingBoxf3  ib = b->instance_bounding_box(i);
            Vec3d          offset = mi->get_offset();
            offset.z() -= ib.min.z();
            if (!positions.empty()) {
                const Vec3d &p = positions[std::min(i, positions.size() - 1)];
                offset.x() += p.x() - ib.center().x();
                offset.y() += p.y() - ib.center().y();
            } else {
                offset.x() -= ib.center().x();
                offset.y() -= ib.center().y();
            }
            mi->set_offset(offset);
        }
        b->invalidate_bounding_box();

        std::map<std::string, std::string> rep;
        rep["name"]    = b->name;
        rep["plate"]   = std::to_string(plate_idx + 1);
        rep["volumes"] = std::to_string(b->volumes.size());
        rep["instances"] = std::to_string(b->instances.size());
        const BoundingBoxf3 bb = b->instance_bounding_box(0);
        std::ostringstream  size;
        size << bb.size().x() << "," << bb.size().y() << "," << bb.size().z();
        rep["size"] = size.str();
        std::string pc;
        for (size_t i = 0; i < painted.size(); ++i)
            pc += (i ? "," : "") + std::to_string(painted[i]);
        if (!painted.empty())
            rep["painted_facets"] = pc;
        result.report.push_back(std::move(rep));
    }
    return built;
}

} // namespace

bool load_cli_job(const nlohmann::json &job, const std::string &base_dir, Model &model, CliJobResult &result)
{
    try {
        check_fields(job, "job", {"version", "plates", "objects", "comment"});
        if (job.contains("version") && (!job["version"].is_number_integer() || job["version"].get<int>() != 1))
            throw JobError("job.version", "only version 1 is supported");
        json plates;
        if (job.contains("plates") && job.contains("objects"))
            throw JobError("job", "use either \"plates\" or top-level \"objects\" (one plate)");
        if (job.contains("plates"))
            plates = job["plates"];
        else if (job.contains("objects"))
            plates = json::array({json{{"objects", job["objects"]}}});
        else
            throw JobError("job", "needs \"plates\" or \"objects\"");
        if (!plates.is_array() || plates.empty())
            throw JobError("job.plates", "must be a non-empty list");

        for (size_t pi = 0; pi < plates.size(); ++pi) {
            const json        &pl = plates[pi];
            const std::string  pw = "plates[" + std::to_string(pi) + "]";
            check_fields(pl, pw, {"name", "arrange", "settings", "objects"});
            assemble_plate_info_t plate;
            plate.plate_name   = pl.contains("name") ? str(pl["name"], pw + ".name") : std::string();
            plate.need_arrange = true;
            if (pl.contains("arrange")) {
                if (!pl["arrange"].is_boolean())
                    throw JobError(pw + ".arrange", "must be true or false");
                plate.need_arrange = pl["arrange"].get<bool>();
            }
            if (pl.contains("settings")) {
                DynamicPrintConfig cfg;
                apply_settings(pl["settings"], pw + ".settings", Scope::Plate, cfg);
                for (const std::string &k : cfg.keys())
                    plate.plate_params[k] = cfg.opt_serialize(k);
            }
            if (!pl.contains("objects") || !pl["objects"].is_array() || pl["objects"].empty())
                throw JobError(pw + ".objects", "must be a non-empty list");
            std::set<int> used;
            bool          positioned_all = true;
            for (size_t oi = 0; oi < pl["objects"].size(); ++oi) {
                const json &o = pl["objects"][oi];
                positioned_all &= o.is_object() && (o.contains("position") || o.contains("positions"));
                BuiltObject b = build_object(o, pw + ".objects[" + std::to_string(oi) + "]", base_dir, model, result, used, int(pi));
                for (ModelObject *m : b.objects)
                    plate.loaded_obj_list.push_back(m);
            }
            if (!pl.contains("arrange") && positioned_all)
                plate.need_arrange = false; // every object placed by hand: keep it there
            if (!plate.need_arrange && !positioned_all)
                throw JobError(pw, "arrange is false, so every object needs a position");
            plate.filaments_count = int(used.size());
            result.plates.push_back(std::move(plate));
        }
    } catch (const JobError &e) {
        result.error = e.what();
        return false;
    } catch (const std::exception &e) {
        result.error = std::string("job: ") + e.what();
        return false;
    }
    return true;
}

bool load_cli_job(const std::string &path, Model &model, CliJobResult &result)
{
    json job;
    try {
        boost::nowide::ifstream in(path);
        if (!in) {
            result.error = path + ": can't open the job file";
            return false;
        }
        job = json::parse(in);
    } catch (const std::exception &e) {
        result.error = path + ": not valid JSON: " + e.what();
        return false;
    }
    return load_cli_job(job, fs::absolute(fs::path(path)).parent_path().string(), model, result);
}

} // namespace Slic3r
