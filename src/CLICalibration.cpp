// ORCA headless-cli: the calibration generators of the Calibration menu, without the GUI.
//
// Each cli_calib_* function below mirrors one Plater::calib_*() in src/slic3r/GUI/Plater.cpp (and the
// defaults / checks of its dialog in calib_dlg.cpp). The desktop app edits the selected print, filament
// and printer presets; the CLI has one merged config, so those edits land there, and per-object edits
// stay on the object. Keep the two in sync when one changes.
//
// Differences from the desktop app, all deliberate:
//  * A key typed on the command line (--wall-loops 3 ...) is not overridden by the calibration, globally
//    or on the calibration objects. It is listed in result.json's calibration.user_overrides.
//  * When the merged config has brim_type = no_brim, the brims the app adds on the calibration objects
//    (PA tower, temperature tower, ...) are left out. Listed in calibration.brim_skipped.
//  * No dialogs: a value the app would question (VFA end speed above the volumetric limit, ...) gives a
//    warning on stderr and in calibration.warnings instead, and the run goes on.
//  * Belt printers are refused: their calibration layouts depend on the GUI's belt plumbing.

#include "CLICalibration.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <locale>
#include <sstream>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include "libslic3r/Arrange.hpp"
#include "libslic3r/CutUtils.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/ParameterUtils.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"

namespace Slic3r {

namespace {

struct ModeInfo
{
    const char* name;
    CalibMode   mode;
};

const std::vector<ModeInfo>& mode_table()
{
    static const std::vector<ModeInfo> table = {
        {"pa_line", CalibMode::Calib_PA_Line},
        {"pa_pattern", CalibMode::Calib_PA_Pattern},
        {"pa_tower", CalibMode::Calib_PA_Tower},
        {"flow_rate", CalibMode::Calib_Flow_Rate},
        {"flow_rate_yolo", CalibMode::Calib_Flow_Rate},
        {"temp_tower", CalibMode::Calib_Temp_Tower},
        {"vol_speed", CalibMode::Calib_Vol_speed_Tower},
        {"vfa", CalibMode::Calib_VFA_Tower},
        {"retraction", CalibMode::Calib_Retraction_tower},
        {"input_shaping_freq", CalibMode::Calib_Input_shaping_freq},
        {"input_shaping_damp", CalibMode::Calib_Input_shaping_damp},
        {"cornering", CalibMode::Calib_Cornering},
    };
    return table;
}

std::string num(double v)
{
    std::ostringstream ss;
    ss.imbue(std::locale::classic());
    ss.precision(12);
    ss << v;
    return ss.str();
}

bool parse_double(const std::string& s, double& out)
{
    std::istringstream ss(s);
    ss.imbue(std::locale::classic());
    ss >> out;
    return !ss.fail() && ss.eof() && std::isfinite(out);
}

bool parse_list(const std::string& s, std::vector<double>& out)
{
    out.clear();
    std::vector<std::string> parts;
    boost::split(parts, s, boost::is_any_of(":;/ "), boost::token_compress_on);
    for (std::string p : parts) {
        boost::trim(p);
        if (p.empty())
            continue;
        double v;
        if (!parse_double(p, v))
            return false;
        out.push_back(v);
    }
    return true;
}

// The edits the desktop app makes, against the CLI's merged config.
struct Editor
{
    CliCalibration&              calib;
    DynamicPrintConfig&          cfg;
    const std::set<std::string>& user_keys;

    bool user(const std::string& key)
    {
        if (user_keys.count(key) == 0)
            return false;
        if (std::find(calib.user_overrides.begin(), calib.user_overrides.end(), key) == calib.user_overrides.end())
            calib.user_overrides.push_back(key);
        return true;
    }

    // print_config->set_key_value(key, opt)
    void set(const std::string& key, ConfigOption* opt)
    {
        std::unique_ptr<ConfigOption> holder(opt);
        if (user(key) || print_config_def.get(key) == nullptr)
            return;
        cfg.set_key_value(key, holder.release());
    }

    // set_config_values<T, OptionType>(config, key, value): every entry of a per-extruder / per-filament
    // vector. The app's preset always has the key; the merged config may not, so it is created here.
    template<typename T, typename OptionType> void set_all(const std::string& key, T value)
    {
        if (user(key) || print_config_def.get(key) == nullptr)
            return;
        OptionType* opt = cfg.option<OptionType>(key, true);
        if (opt == nullptr) {
            BOOST_LOG_TRIVIAL(warning) << "calibration: " << key << " has an unexpected type, left as it is";
            return;
        }
        if (opt->values.empty())
            opt->values.resize(1);
        for (auto& v : opt->values)
            v = value;
    }

    // model().objects[i]->config.set_key_value(key, opt)
    void set_obj(ModelObject* obj, const std::string& key, ConfigOption* opt)
    {
        std::unique_ptr<ConfigOption> holder(opt);
        if (user(key))
            return;
        if (boost::starts_with(key, "brim_")) {
            const auto* brim = cfg.option<ConfigOptionEnum<BrimType>>("brim_type");
            if (brim != nullptr && brim->value == btNoBrim) {
                if (std::find(calib.brim_skipped.begin(), calib.brim_skipped.end(), key) == calib.brim_skipped.end())
                    calib.brim_skipped.push_back(key);
                return;
            }
        }
        obj->config.set_key_value(key, holder.release());
    }

    double nozzle_diameter(size_t idx = 0) const
    {
        const auto* opt = cfg.option<ConfigOptionFloats>("nozzle_diameter");
        if (opt == nullptr || opt->values.empty())
            return 0.4;
        double d = opt->get_at(std::min(idx, opt->values.size() - 1));
        return d > 0. ? d : 0.4;
    }

    double float_at(const std::string& key, double fallback, size_t idx = 0) const
    {
        if (const auto* opt = dynamic_cast<const ConfigOptionVectorBase*>(cfg.option(key))) {
            if (const auto* f = dynamic_cast<const ConfigOptionFloats*>(cfg.option(key)); f && !f->values.empty())
                return f->get_at(idx);
            if (const auto* f = dynamic_cast<const ConfigOptionFloatsNullable*>(cfg.option(key)); f && !f->values.empty())
                return f->get_at(idx);
            (void) opt;
        }
        if (const auto* f = cfg.option<ConfigOptionFloat>(key))
            return f->value;
        return fallback;
    }

    GCodeFlavor flavor() const
    {
        const auto* opt = cfg.option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor");
        return opt ? opt->value : gcfMarlinLegacy;
    }

    // Plater.cpp has_junction_deviation()
    bool has_junction_deviation() const
    {
        const auto* jd = cfg.option<ConfigOptionFloats>("machine_max_junction_deviation");
        return flavor() == gcfMarlinFirmware && jd && !jd->values.empty() && jd->values.front() > 0.0;
    }

    void warn(const std::string& msg)
    {
        calib.warnings.push_back(msg);
        BOOST_LOG_TRIVIAL(warning) << "calibration: " << msg;
    }
};

// The merged CLI config only has the keys the presets carry; the app's presets have all of them. Code
// that reads many keys (the PA pattern generator, arrange) gets the defaults filled in.
DynamicPrintConfig full_of(const DynamicPrintConfig& cfg)
{
    DynamicPrintConfig full = DynamicPrintConfig::full_print_config();
    full.apply(cfg, true);
    return full;
}

// Make sure the keys a calibration reads exist, with their defaults when the presets do not set them.
void ensure_keys(DynamicPrintConfig& cfg)
{
    static const char* keys[] = {"layer_height", "initial_layer_print_height", "line_width", "outer_wall_line_width", "nozzle_diameter",
                                 "outer_wall_acceleration", "inner_wall_acceleration", "default_acceleration", "default_jerk",
                                 "outer_wall_jerk", "inner_wall_jerk", "outer_wall_speed", "inner_wall_speed", "initial_layer_speed",
                                 "filament_max_volumetric_speed", "filament_flow_ratio", "printable_area", "max_layer_height",
                                 "internal_solid_infill_speed", "top_surface_speed", "brim_type", "gcode_flavor"};
    for (const char* k : keys)
        if (!cfg.has(k) && print_config_def.get(k) != nullptr)
            cfg.option(k, true);
}

int extruder_param_index(const DynamicPrintConfig& cfg, const std::string& key, int extruder)
{
    const auto* et = cfg.option<ConfigOptionEnumsGeneric>("extruder_type");
    const auto* nv = cfg.option<ConfigOptionEnumsGeneric>("nozzle_volume_type");
    if (et && nv && extruder < int(et->values.size()) && extruder < int(nv->values.size()))
        return std::max(0, get_index_for_extruder_parameter(cfg, key, extruder, ExtruderType(et->values[extruder]),
                                                            NozzleVolumeType(nv->values[extruder])));
    return extruder;
}

int extruder_count(const DynamicPrintConfig& cfg)
{
    const auto* nd = cfg.option<ConfigOptionFloats>("nozzle_diameter");
    return nd ? std::max<int>(1, int(nd->values.size())) : 1;
}

// CalibUtils.cpp update_speed_parameter(): the speed the volumetric limit allows at the profile's line
// width and layer height, written into the global speed `key`.
void update_speed_parameter(Editor& ed, const std::string& key)
{
    if (ed.user(key))
        return;
    DynamicPrintConfig& cfg             = ed.cfg;
    const double        nozzle_diameter = ed.nozzle_diameter();
    const double        layer_height    = cfg.opt_float("layer_height");
    double              line_width      = cfg.get_abs_value("line_width", nozzle_diameter);
    if (line_width <= 0.)
        line_width = Flow::auto_extrusion_width(frPerimeter, float(nozzle_diameter));
    const Flow flow{float(line_width), float(layer_height), float(nozzle_diameter)};

    auto* speed_opt = cfg.option<ConfigOptionFloatsNullable>(key, true);
    auto* mvs       = cfg.option<ConfigOptionFloats>("filament_max_volumetric_speed");
    if (speed_opt == nullptr || mvs == nullptr || mvs->values.empty())
        return;
    for (int i = 0; i < extruder_count(cfg); ++i) {
        int          index     = extruder_param_index(cfg, "filament_max_volumetric_speed", i);
        const double max_speed = mvs->get_at(index) / flow.mm3_per_mm();
        index                  = extruder_param_index(cfg, key, i);
        if (index >= int(speed_opt->values.size()))
            speed_opt->values.resize(index + 1, speed_opt->values.empty() ? max_speed : speed_opt->values.front());
        speed_opt->values[index] = max_speed;
    }
}

// CalibUtils.cpp generate_max_speed_parameter_value()
std::vector<double> max_speed_parameter_value(const DynamicPrintConfig& cfg, const std::string& key, float line_width,
                                              float layer_height, bool linear, int pass, double nozzle_diameter)
{
    const Flow  flow{line_width, layer_height, float(nozzle_diameter)};
    const auto* mvs        = cfg.option<ConfigOptionFloats>("filament_max_volumetric_speed");
    const auto* flow_ratio = cfg.option<ConfigOptionFloats>("filament_flow_ratio");
    const auto* speed_opt  = cfg.option<ConfigOptionFloatsNullable>(key);
    const auto* nv         = cfg.option<ConfigOptionEnumsGeneric>("nozzle_volume_type");
    const size_t variants  = nv && !nv->values.empty() ? nv->values.size() : 1;
    std::vector<double> out;
    for (int i = 0; i < extruder_count(cfg); ++i) {
        int          index        = extruder_param_index(cfg, "filament_max_volumetric_speed", i);
        const double max_vol      = mvs ? mvs->get_at(index) : 0.;
        const double cur_flowrate = flow_ratio ? flow_ratio->get_at(index) : 1.;
        const double max_speed    = linear ? max_vol / (flow.mm3_per_mm() * (cur_flowrate + (pass == 2 ? 0.035 : 0.05)) / cur_flowrate) :
                                             max_vol / (flow.mm3_per_mm() * (pass == 1 ? 1.2 : 1));
        index                     = extruder_param_index(cfg, key, i);
        const double cur          = speed_opt && !speed_opt->values.empty() ? speed_opt->get_at(index) : max_speed;
        const double speed_value  = std::floor(std::min(cur, max_speed));
        for (size_t v = 0; v < variants; ++v)
            out.push_back(speed_value);
    }
    return out;
}

// Plater.cpp zero_pressure_advance_where_disabled()
void zero_pressure_advance_where_disabled(Editor& ed)
{
    auto enable_pa   = ed.cfg.option<ConfigOptionBools>("enable_pressure_advance");
    auto pa          = ed.cfg.option<ConfigOptionFloats>("pressure_advance");
    auto adaptive_pa = ed.cfg.option<ConfigOptionBools>("adaptive_pressure_advance");
    if (!enable_pa || !pa || !adaptive_pa || pa->empty() || adaptive_pa->empty())
        return;
    if (ed.user("enable_pressure_advance") || ed.user("pressure_advance"))
        return;
    if (pa->size() < enable_pa->size())
        pa->resize(enable_pa->size());
    if (adaptive_pa->size() < enable_pa->size())
        adaptive_pa->resize(enable_pa->size());
    for (size_t variant = 0; variant < enable_pa->size(); ++variant) {
        if (enable_pa->get_at(variant))
            continue;
        enable_pa->values[variant]   = true;
        pa->values[variant]          = 0.0;
        adaptive_pa->values[variant] = false;
    }
}

BoundingBoxf bed_extents(const DynamicPrintConfig& cfg)
{
    const auto* area = cfg.option<ConfigOptionPoints>("printable_area");
    if (area == nullptr || area->values.empty())
        return BoundingBoxf(Vec2d(0, 0), Vec2d(200, 200));
    return get_extents(area->values);
}

// Plater::add_model(): load a resources/calib model, centred on the bed and resting on it, as the app
// places a freshly added object.
std::string add_model(Model& model, const DynamicPrintConfig& cfg, const std::string& rel_path)
{
    const std::string path = resources_dir() + rel_path;
    if (!boost::filesystem::exists(path))
        return "calibration model not found: " + path;
    Model loaded;
    try {
        DynamicPrintConfig        file_config;
        ConfigSubstitutionContext ctx(ForwardCompatibilitySubstitutionRule::Enable);
        loaded = Model::read_from_file(path, &file_config, &ctx, LoadStrategy::LoadModel | LoadStrategy::AddDefaultInstances);
    } catch (const std::exception& e) {
        return "cannot load calibration model " + path + ": " + e.what();
    }
    if (loaded.objects.empty())
        return "calibration model " + path + " has no objects";

    const size_t first = model.objects.size();
    for (ModelObject* o : loaded.objects)
        model.add_object(*o);
    BoundingBoxf3 group;
    for (size_t i = first; i < model.objects.size(); ++i) {
        model.objects[i]->ensure_on_bed();
        group.merge(model.objects[i]->instance_bounding_box(0));
    }
    const Vec2d bed_center = bed_extents(cfg).center();
    const Vec3d shift(bed_center.x() - group.center().x(), bed_center.y() - group.center().y(), 0.);
    for (size_t i = first; i < model.objects.size(); ++i) {
        model.objects[i]->translate_instances(shift);
        model.objects[i]->invalidate_bounding_box();
    }
    return {};
}

// Plater::cut_horizontal(): cut object `obj_idx` at height `z` and put the kept part(s) at the end of
// the object list, as Plater::apply_cut_object_to_model() does.
void cut_horizontal(Model& model, size_t obj_idx, double z, ModelObjectCutAttributes attributes)
{
    if (obj_idx >= model.objects.size())
        return;
    if (!attributes.has(ModelObjectCutAttribute::KeepUpper) && !attributes.has(ModelObjectCutAttribute::KeepLower))
        return;
    ModelObject* object          = model.objects[obj_idx];
    const Vec3d  instance_offset = object->instances[0]->get_offset();
    Cut          cut(object, 0, Geometry::translation_transform(z * Vec3d::UnitZ() - instance_offset), attributes);
    const auto&  new_objects = cut.perform_with_plane();
    for (const ModelObject* o : new_objects)
        model.add_object(*o);
    model.delete_object(obj_idx);
}

bool is_belt_printer(const DynamicPrintConfig& cfg)
{
    const auto* belt = cfg.option<ConfigOptionBool>("belt_printer");
    return belt != nullptr && belt->value;
}

// ---------------------------------------------------------------------------------------- modes

void calib_pa_common(Editor& ed)
{
    // Plater::calib_pa()
    ed.set("overhang_reverse", new ConfigOptionBool(false));
    ed.set("precise_z_height", new ConfigOptionBool(false));
    ed.set("wipe_inward", new ConfigOptionBool(false));
    ed.set("retract_after_outer_wall", new ConfigOptionBool(false));
    ed.set("resonance_avoidance", new ConfigOptionBool(false));
}

std::string calib_pa_line(Editor& ed, Model& model)
{
    calib_pa_common(ed);
    return add_model(model, ed.cfg, "/calib/pressure_advance/pressure_advance_test.drc");
}

std::string calib_pa_tower(Editor& ed, Model& model)
{
    calib_pa_common(ed);
    if (std::string err = add_model(model, ed.cfg, "/calib/pressure_advance/tower_with_seam.drc"); !err.empty())
        return err;
    const Calib_Params& params = ed.calib.params;
    ed.set("enable_wrapping_detection", new ConfigOptionBool(false));
    ed.set_all<double, ConfigOptionFloats>("slow_down_layer_time", 1.0);

    ModelObject* obj = model.objects[0];
    ed.set_obj(obj, "alternate_extra_wall", new ConfigOptionBool(false));
    update_speed_parameter(ed, "outer_wall_speed");
    update_speed_parameter(ed, "inner_wall_speed");
    ed.set_obj(obj, "seam_position", new ConfigOptionEnum<SeamPosition>(spRear));
    ed.set_obj(obj, "wall_loops", new ConfigOptionInt(2));
    ed.set_obj(obj, "top_shell_layers", new ConfigOptionInt(0));
    ed.set_obj(obj, "bottom_shell_layers", new ConfigOptionInt(0));
    ed.set_obj(obj, "sparse_infill_density", new ConfigOptionPercent(0));
    ed.set_obj(obj, "brim_type", new ConfigOptionEnum<BrimType>(btEar));
    ed.set_obj(obj, "brim_object_gap", new ConfigOptionFloat(.0f));
    ed.set_obj(obj, "brim_ears_max_angle", new ConfigOptionFloat(135.f));
    ed.set_obj(obj, "brim_width", new ConfigOptionFloat(6.f));
    ed.set_obj(obj, "seam_slope_type", new ConfigOptionEnum<SeamScarfType>(SeamScarfType::None));
    ed.set("max_volumetric_extrusion_rate_slope", new ConfigOptionFloat(0));

    const double new_height = std::ceil((params.end - params.start) / params.step) + 1;
    if (new_height < obj->bounding_box_exact().size().z())
        cut_horizontal(model, 0, new_height, ModelObjectCutAttribute::KeepLower);
    return {};
}

std::string calib_pa_pattern(Editor& ed, Model& model)
{
    calib_pa_common(ed);
    CliCalibration&     calib  = ed.calib;
    Calib_Params&       params = calib.params;
    DynamicPrintConfig& cfg    = ed.cfg;
    std::vector<double> speeds{params.speeds};
    std::vector<double> accels{params.accelerations};

    const double nozzle_diameter = ed.nozzle_diameter();
    ed.set_all<bool, ConfigOptionBoolsNullable>("filament_retract_when_changing_layer", false);
    ed.set_all<bool, ConfigOptionBoolsNullable>("filament_wipe", false);
    ed.set_all<bool, ConfigOptionBools>("wipe", false);
    ed.set_all<bool, ConfigOptionBools>("retract_when_changing_layer", false);
    ed.set("resonance_avoidance", new ConfigOptionBool(false));

    // Acceleration of the test: the outer wall's, else the inner wall's, else the default.
    double accel = cfg.get_abs_value_at("outer_wall_acceleration", params.extruder_id);
    if (accel == 0)
        accel = cfg.get_abs_value_at("inner_wall_acceleration", params.extruder_id);
    if (accel == 0)
        accel = cfg.get_abs_value_at("default_acceleration", params.extruder_id);
    if (accels.empty()) {
        accels.assign({accel});
        ed.warn("no accelerations given, using " + num(std::lround(accel)) + " mm/s^2");
    } else
        accel = *std::max_element(accels.begin(), accels.end());
    ed.set_all<double, ConfigOptionFloatsNullable>("outer_wall_acceleration", accel);
    ed.set("print_sequence", new ConfigOptionEnum<PrintSequence>(PrintSequence::ByLayer));

    if (!ed.has_junction_deviation() && cfg.get_abs_value_at("default_jerk", params.extruder_id) > 0) {
        double jerk = cfg.get_abs_value_at("outer_wall_jerk", params.extruder_id);
        if (jerk == 0)
            jerk = cfg.get_abs_value_at("inner_wall_jerk", params.extruder_id);
        if (jerk == 0)
            jerk = cfg.get_abs_value_at("default_jerk", params.extruder_id);
        for (const char* key : {"default_jerk", "outer_wall_jerk", "inner_wall_jerk", "top_surface_jerk", "infill_jerk", "travel_jerk"})
            ed.set_all<double, ConfigOptionFloatsNullable>(key, jerk);
    }
    if (ed.has_junction_deviation())
        ed.set_all<double, ConfigOptionFloatsNullable>("default_junction_deviation", 0);

    const SuggestedConfigCalibPAPattern suggested;
    for (const auto& opt : suggested.floats_pairs)
        ed.set_all<double, ConfigOptionFloatsNullable>(opt.first, opt.second[0]);
    for (const auto& opt : suggested.nozzle_ratio_pairs)
        ed.set(opt.first, new ConfigOptionFloatOrPercent(nozzle_diameter * opt.second / 100, false));
    for (const auto& opt : suggested.int_pairs)
        ed.set(opt.first, new ConfigOptionInt(opt.second));
    ed.set(suggested.brim_pair.first, new ConfigOptionEnum<BrimType>(suggested.brim_pair.second));
    ed.set("enable_wrapping_detection", new ConfigOptionBool(false));

    if (speeds.empty()) {
        const double speed = CalibPressureAdvance::find_optimal_PA_speed(full_of(cfg), cfg.get_abs_value("line_width", nozzle_diameter),
                                                                         cfg.get_abs_value("layer_height"), 0, 0);
        ed.set_all<double, ConfigOptionFloatsNullable>("outer_wall_speed", speed);
        speeds.assign({speed});
        ed.warn("no speeds given, using the optimal speed " + num(std::lround(speed)) + " mm/s");
    } else if (speeds.size() == 1)
        ed.set_all<double, ConfigOptionFloatsNullable>("outer_wall_speed", speeds.front());
    params.speeds        = speeds;
    params.accelerations = accels;

    // The "handle" cube the app adds with load_generic_subobject("Cube"); each test gets a copy.
    // A centred cube, as ObjectList::load_generic_subobject() makes it (handle_pos_offset() assumes that).
    TriangleMesh cube_mesh(its_make_cube(10., 10., 10.));
    cube_mesh.translate(-5.f, -5.f, -5.f);
    ModelObject* cube = model.add_object("Cube", "", std::move(cube_mesh));
    cube->add_instance();
    const Vec3d origin = Vec3d::Zero(); // plate 1
    const DynamicPrintConfig    full = full_of(cfg);
    CalibPressureAdvancePattern pa_pattern(params, full, /*is_bbl_machine*/ false, *cube, origin);

    auto cube_bb = cube->raw_bounding_box();
    cube->scale((pa_pattern.print_size_x() + 4) / cube_bb.size().x(), (pa_pattern.print_size_y() + 4) / cube_bb.size().y(),
                pa_pattern.max_layer_z() / cube_bb.size().z());

    arrangement::ArrangePolygons arranged_items;
    {
        arrangement::ArrangeParams ap;
        Points                     bedpts = arrangement::get_shrink_bedpts(&full, ap);
        for (size_t i = 0; i < speeds.size() * accels.size(); i++) {
            arrangement::ArrangePolygon p;
            cube->instances[0]->get_arrange_polygon(&p);
            p.bed_idx = 0;
            arranged_items.emplace_back(p);
        }
        arrangement::arrange(arranged_items, bedpts, ap);
    }
    for (const auto& ai : arranged_items)
        if (ai.bed_idx != 0)
            return std::to_string(speeds.size() * accels.size()) +
                   " PA patterns (speeds x accelerations) do not fit on one plate; give fewer speeds or accelerations";

    cube_bb = cube->raw_bounding_box();
    cube->scale(pa_pattern.handle_xy_size() / cube_bb.size().x(), pa_pattern.handle_xy_size() / cube_bb.size().y(),
                pa_pattern.max_layer_z() / cube_bb.size().z());

    for (size_t test_idx = 0; test_idx < arranged_items.size(); test_idx++) {
        const auto& ai   = arranged_items[test_idx];
        const auto  tspd = speeds[test_idx % speeds.size()];
        const auto  tacc = accels[test_idx / speeds.size()];
        ModelObject* obj = test_idx == 0 ? cube : model.add_object(*cube);
        obj->name.assign(std::string("pa_pattern_") + std::to_string(int(tspd)) + std::string("_") + std::to_string(int(tacc)));
        if (speeds.size() > 1)
            obj->config.set_key_value("outer_wall_speed", new ConfigOptionFloatsNullable(1, tspd));
        if (accels.size() > 1)
            obj->config.set_key_value("outer_wall_acceleration", new ConfigOptionFloatsNullable(1, tacc));
        const Vec3d obj_offset{unscale<double>(ai.translation(X)), unscale<double>(ai.translation(Y)), 0};
        obj->instances[0]->set_offset(origin + obj_offset + pa_pattern.handle_pos_offset());
        obj->ensure_on_bed();
    }
    model.calib_pa_pattern = std::make_unique<CalibPressureAdvancePattern>(pa_pattern);
    calib.keep_layout      = true;
    calib.extra["speeds"]        = speeds;
    calib.extra["accelerations"] = accels;
    return {};
}

// Plater.cpp adjust_settings_for_flowrate_calib()
void adjust_settings_for_flowrate_calib(Editor& ed, Model& model, bool linear, int pass, InfillPattern pattern)
{
    DynamicPrintConfig& cfg             = ed.cfg;
    const float         nozzle_diameter = float(ed.nozzle_diameter());
    const float         xyScale         = nozzle_diameter / 0.6f;
    double              first_layer_height = cfg.opt_float("initial_layer_print_height");
    const double        layer_height       = nozzle_diameter / 2.0;
    first_layer_height                      = std::max(first_layer_height, layer_height);

    // The app scales the selection (all test objects) around its centre.
    const float zscale = float((first_layer_height + 9 * layer_height) / 2);
    const Vec3d scale  = xyScale > 1.2 ? Vec3d(xyScale, xyScale, zscale) : Vec3d(1, 1, zscale);
    BoundingBoxf3 group;
    for (ModelObject* o : model.objects)
        group.merge(o->instance_bounding_box(0));
    const Vec3d c = group.center();
    for (ModelObject* o : model.objects) {
        for (ModelInstance* inst : o->instances) {
            inst->set_scaling_factor(inst->get_scaling_factor().cwiseProduct(scale));
            Vec3d off = inst->get_offset();
            off.x()   = c.x() + (off.x() - c.x()) * scale.x();
            off.y()   = c.y() + (off.y() - c.y()) * scale.y();
            inst->set_offset(off);
        }
        o->invalidate_bounding_box();
    }

    const auto*  flow_ratio   = cfg.option<ConfigOptionFloats>("filament_flow_ratio");
    const double cur_flowrate = flow_ratio && !flow_ratio->values.empty() ? flow_ratio->get_at(0) : 1.0;
    const float  infill_line_width = nozzle_diameter * 1.2f;
    std::vector<double> internal_solid_speeds = max_speed_parameter_value(cfg, "internal_solid_infill_speed", infill_line_width,
                                                                          float(layer_height), linear, pass, nozzle_diameter);
    std::vector<double> top_surface_speeds    = max_speed_parameter_value(cfg, "top_surface_speed", infill_line_width,
                                                                          float(layer_height), linear, pass, nozzle_diameter);

    for (ModelObject* _obj : model.objects) {
        _obj->ensure_on_bed();
        ed.set_obj(_obj, "wall_loops", new ConfigOptionInt(1));
        ed.set_obj(_obj, "only_one_wall_top", new ConfigOptionBool(true));
        ed.set_obj(_obj, "thick_internal_bridges", new ConfigOptionBool(false));
        ed.set_obj(_obj, "enable_extra_bridge_layer", new ConfigOptionEnum<EnableExtraBridgeLayer>(eblDisabled));
        ed.set_obj(_obj, "internal_bridge_density", new ConfigOptionPercent(100));
        ed.set_obj(_obj, "sparse_infill_density", new ConfigOptionPercent(35));
        ed.set_obj(_obj, "min_width_top_surface", new ConfigOptionFloatOrPercent(100, true));
        ed.set_obj(_obj, "bottom_shell_layers", new ConfigOptionInt(2));
        ed.set_obj(_obj, "top_shell_layers", new ConfigOptionInt(5));
        ed.set_obj(_obj, "top_shell_thickness", new ConfigOptionFloat(0));
        ed.set_obj(_obj, "bottom_shell_thickness", new ConfigOptionFloat(0));
        ed.set_obj(_obj, "detect_thin_wall", new ConfigOptionBool(true));
        ed.set_obj(_obj, "filter_out_gap_fill", new ConfigOptionFloat(0));
        ed.set_obj(_obj, "sparse_infill_pattern", new ConfigOptionEnum<InfillPattern>(ipRectilinear));
        ed.set_obj(_obj, "top_surface_line_width", new ConfigOptionFloatOrPercent(infill_line_width, false));
        ed.set_obj(_obj, "internal_solid_infill_line_width", new ConfigOptionFloatOrPercent(infill_line_width, false));
        ed.set_obj(_obj, "top_surface_pattern", new ConfigOptionEnum<InfillPattern>(pattern));
        const auto* top_solid_flow = dynamic_cast<const ConfigOptionFloatsNullable*>(_obj->config.option("top_solid_infill_flow_ratio"));
        ed.set_obj(_obj, "top_solid_infill_flow_ratio", new ConfigOptionFloatsNullable(top_solid_flow ? top_solid_flow->size() : 1, 1.0f));
        ed.set_obj(_obj, "infill_direction", new ConfigOptionFloat(45));
        ed.set_obj(_obj, "solid_infill_direction", new ConfigOptionFloat(135));
        ed.set_obj(_obj, "center_of_surface_pattern", new ConfigOptionEnum<CenterOfSurfacePattern>(CenterOfSurfacePattern::Each_Surface));
        ed.set_obj(_obj, "separated_infills", new ConfigOptionBool(false));
        ed.set_obj(_obj, "align_infill_direction_to_model", new ConfigOptionBool(true));
        ed.set_obj(_obj, "ironing_type", new ConfigOptionEnum<IroningType>(IroningType::NoIroning));
        ed.set_obj(_obj, "internal_solid_infill_speed", new ConfigOptionFloatsNullable(internal_solid_speeds));
        ed.set_obj(_obj, "top_surface_speed", new ConfigOptionFloatsNullable(top_surface_speeds));
        ed.set_obj(_obj, "seam_slope_type", new ConfigOptionEnum<SeamScarfType>(SeamScarfType::None));
        ed.set_obj(_obj, "gap_fill_target", new ConfigOptionEnum<GapFillTarget>(GapFillTarget::gftNowhere));
        ed.set("max_volumetric_extrusion_rate_slope", new ConfigOptionFloat(0));
        ed.set_obj(_obj, "calib_flowrate_topinfill_special_order", new ConfigOptionBool(true));
        ed.set_obj(_obj, "top_surface_fill_order", new ConfigOptionEnum<SurfaceFillOrder>(SurfaceFillOrder::Default));

        // the flow modifier is in the object name: flowrate_<n>, a leading 'm' for minus
        std::string obj_name = _obj->name.size() > 9 ? _obj->name.substr(9) : std::string();
        if (!obj_name.empty() && obj_name[0] == 'm')
            obj_name[0] = '-';
        double modifier = 1.0;
        if (!parse_double(obj_name, modifier))
            modifier = 1.0;
        const double ratio = linear ? (cur_flowrate + modifier) / cur_flowrate : 1.0 + modifier / 100.;
        ed.set_obj(_obj, "print_flow_ratio", new ConfigOptionFloat(ratio));
        ed.calib.extra["flow_ratios"][_obj->name] = ratio;
    }

    ed.set("layer_height", new ConfigOptionFloat(layer_height));
    ed.set("alternate_extra_wall", new ConfigOptionBool(false));
    ed.set("initial_layer_print_height", new ConfigOptionFloat(first_layer_height));
    ed.set("reduce_crossing_wall", new ConfigOptionBool(true));
    ed.set("enable_wrapping_detection", new ConfigOptionBool(false));
}

std::string calib_flowrate(Editor& ed, Model& model, bool is_linear)
{
    const int pass = ed.calib.flow_pass;
    const char* file = is_linear ? (pass == 1 ? "/calib/filament_flow/Orca-LinearFlow.3mf" : "/calib/filament_flow/Orca-LinearFlow_fine.3mf") :
                                   (pass == 1 ? "/calib/filament_flow/flowrate-test-pass1.3mf" : "/calib/filament_flow/flowrate-test-pass2.3mf");
    if (std::string err = add_model(model, ed.cfg, file); !err.empty())
        return err;
    const InfillPattern pattern = ed.calib.flow_pattern == "monotonic" ? ipMonotonic : ipArchimedeanChords;
    adjust_settings_for_flowrate_calib(ed, model, is_linear, pass, pattern);
    ed.set("resonance_avoidance", new ConfigOptionBool(false));
    // Re-centre: the scaling above moved the group around its own centre only.
    BoundingBoxf3 group;
    for (ModelObject* o : model.objects)
        group.merge(o->instance_bounding_box(0));
    const Vec2d bed_center = bed_extents(ed.cfg).center();
    for (ModelObject* o : model.objects) {
        o->translate_instances(Vec3d(bed_center.x() - group.center().x(), bed_center.y() - group.center().y(), 0.));
        o->invalidate_bounding_box();
    }
    return {};
}

std::string calib_temp(Editor& ed, Model& model)
{
    constexpr double base_temp_tower_nozzle_diameter = 0.4;
    constexpr double base_temp_tower_block_height    = 10.0;
    constexpr int    base_temp_tower_temp_step       = 5;
    const Calib_Params& params = ed.calib.params;

    if (std::string err = add_model(model, ed.cfg, "/calib/temperature_tower/temperature_tower.drc"); !err.empty())
        return err;
    const long   start_temp      = std::lround(params.start);
    const double nozzle_diameter = ed.nozzle_diameter(std::max(params.extruder_id, 0));
    const double nozzle_scale    = nozzle_diameter / base_temp_tower_nozzle_diameter;
    const double block_height    = base_temp_tower_block_height;

    // cut upper
    auto obj_bb      = model.objects[0]->bounding_box_exact();
    auto block_count = std::lround((500 - params.end) / base_temp_tower_temp_step + 1);
    if (block_count > 0) {
        auto new_height = block_count * block_height - EPSILON;
        if (new_height < obj_bb.size().z())
            cut_horizontal(model, 0, new_height, ModelObjectCutAttribute::KeepLower);
    }
    // cut bottom
    obj_bb      = model.objects[0]->bounding_box_exact();
    block_count = std::lround((500 - params.start) / base_temp_tower_temp_step);
    if (block_count > 0) {
        auto new_height = block_count * block_height + EPSILON;
        if (new_height < obj_bb.size().z())
            cut_horizontal(model, 0, new_height, ModelObjectCutAttribute::KeepUpper);
    }

    ModelObject* obj = model.objects[0];
    if (params.nozzle_based_resize && std::abs(nozzle_scale - 1.0) > EPSILON)
        obj->scale(nozzle_scale, nozzle_scale, nozzle_scale);
    obj->ensure_on_bed();

    ed.set("resonance_avoidance", new ConfigOptionBool(false));
    ed.set_all<int, ConfigOptionInts>("nozzle_temperature_initial_layer", int(start_temp));
    ed.set_all<int, ConfigOptionInts>("nozzle_temperature", int(start_temp));
    if (params.nozzle_based_resize)
        ed.set_obj(obj, "layer_height", new ConfigOptionFloat(nozzle_diameter / 2));
    ed.set_obj(obj, "brim_type", new ConfigOptionEnum<BrimType>(btOuterOnly));
    ed.set_obj(obj, "brim_width", new ConfigOptionFloat(5.0));
    ed.set_obj(obj, "brim_object_gap", new ConfigOptionFloat(0.0));
    ed.set_obj(obj, "alternate_extra_wall", new ConfigOptionBool(false));
    ed.set_obj(obj, "seam_slope_type", new ConfigOptionEnum<SeamScarfType>(SeamScarfType::None));
    ed.set_obj(obj, "overhang_reverse", new ConfigOptionBool(false));
    ed.set_obj(obj, "precise_z_height", new ConfigOptionBool(false));
    ed.set("enable_wrapping_detection", new ConfigOptionBool(false));
    if (params.nozzle_based_resize)
        ed.set("initial_layer_print_height", new ConfigOptionFloat(nozzle_diameter / 2));
    return {};
}

std::string calib_max_vol_speed(Editor& ed, Model& model)
{
    if (std::string err = add_model(model, ed.cfg, "/calib/volumetric_speed/SpeedTestStructure.drc"); !err.empty())
        return err;
    DynamicPrintConfig& cfg    = ed.cfg;
    Calib_Params&       params = ed.calib.params;
    ModelObject*        obj    = model.objects[0];

    const BoundingBoxf bed_ext   = bed_extents(cfg);
    const double       scale_obj = (bed_ext.size().x() - 10) / obj->bounding_box_exact().size().x();
    if (scale_obj < 1.0)
        obj->scale(scale_obj, 1, 1);

    const double nozzle_diameter = ed.nozzle_diameter();
    const double line_width      = nozzle_diameter * 1.75;
    const double layer_height    = nozzle_diameter * 0.8;

    if (auto* max_lh = cfg.option<ConfigOptionFloats>("max_layer_height"); max_lh && !ed.user("max_layer_height"))
        for (double& v : max_lh->values)
            v = std::max(v, layer_height);

    const auto*  mvs_opt                       = cfg.option<ConfigOptionFloats>("filament_max_volumetric_speed");
    const double filament_max_volumetric_speed = mvs_opt && !mvs_opt->values.empty() ? mvs_opt->get_at(0) : 0.;
    ed.set_all<double, ConfigOptionFloats>("filament_max_volumetric_speed", std::max(filament_max_volumetric_speed, 200.0));
    ed.set_all<double, ConfigOptionFloats>("slow_down_layer_time", 0.0);
    ed.set("resonance_avoidance", new ConfigOptionBool(false));
    ed.set_obj(obj, "enable_overhang_speed", new ConfigOptionBoolsNullable(1, false));
    ed.set_obj(obj, "wall_loops", new ConfigOptionInt(1));
    ed.set_obj(obj, "alternate_extra_wall", new ConfigOptionBool(false));
    ed.set_obj(obj, "top_shell_layers", new ConfigOptionInt(0));
    ed.set_obj(obj, "bottom_shell_layers", new ConfigOptionInt(0));
    ed.set_obj(obj, "sparse_infill_density", new ConfigOptionPercent(0));
    ed.set_obj(obj, "outer_wall_line_width", new ConfigOptionFloatOrPercent(line_width, false));
    ed.set_obj(obj, "layer_height", new ConfigOptionFloat(layer_height));
    ed.set_obj(obj, "brim_type", new ConfigOptionEnum<BrimType>(btOuterAndInner));
    ed.set_obj(obj, "brim_width", new ConfigOptionFloat(5.0));
    ed.set_obj(obj, "brim_object_gap", new ConfigOptionFloat(0.0));
    ed.set_obj(obj, "precise_z_height", new ConfigOptionBool(false));
    ed.set("timelapse_type", new ConfigOptionEnum<TimelapseType>(tlTraditional));
    ed.set("spiral_mode", new ConfigOptionBool(true));
    ed.set("max_volumetric_extrusion_rate_slope", new ConfigOptionFloat(0));
    ed.set("enable_wrapping_detection", new ConfigOptionBool(false));

    // cut upper
    const auto obj_bb = obj->bounding_box_exact();
    const auto height = (params.end - params.start + 1) / params.step;
    if (height < obj_bb.size().z())
        cut_horizontal(model, 0, height, ModelObjectCutAttribute::KeepLower);

    // The tower steps the outer wall speed; the dialog takes volumetric speeds.
    const auto* flow_ratio = cfg.option<ConfigOptionFloats>("filament_flow_ratio");
    const double mm3_per_mm = Flow(float(line_width), float(layer_height), float(nozzle_diameter)).mm3_per_mm() *
                              (flow_ratio && !flow_ratio->values.empty() ? flow_ratio->get_at(0) : 1.0);
    ed.calib.extra["volumetric_start"] = params.start;
    ed.calib.extra["volumetric_end"]   = params.end;
    ed.calib.extra["volumetric_step"]  = params.step;
    params.end   = params.end / mm3_per_mm;
    params.start = params.start / mm3_per_mm;
    params.step  = params.step / mm3_per_mm;
    return {};
}

std::string calib_retraction(Editor& ed, Model& model)
{
    if (std::string err = add_model(model, ed.cfg, "/calib/retraction/retraction_tower.drc"); !err.empty())
        return err;
    DynamicPrintConfig& cfg    = ed.cfg;
    const Calib_Params& params = ed.calib.params;
    ModelObject*        obj    = model.objects[0];

    ed.set("enable_wrapping_detection", new ConfigOptionBool(false));
    ed.set("wipe_inward", new ConfigOptionBool(false));
    ed.set("retract_after_outer_wall", new ConfigOptionBool(false));

    const float nozzle_diameter = float(ed.nozzle_diameter());
    const float layer_height    = nozzle_diameter <= 0.1f ? 0.05f : nozzle_diameter <= 0.2f ? 0.1f : 0.2f;
    if (auto* max_lh = cfg.option<ConfigOptionFloats>("max_layer_height"); max_lh && !ed.user("max_layer_height"))
        for (double& v : max_lh->values)
            v = std::max(v, double(layer_height));

    ed.set("resonance_avoidance", new ConfigOptionBool(false));
    ed.set("use_firmware_retraction", new ConfigOptionBool(false));
    ed.set_obj(obj, "wall_loops", new ConfigOptionInt(2));
    ed.set_obj(obj, "top_shell_layers", new ConfigOptionInt(0));
    ed.set_obj(obj, "bottom_shell_layers", new ConfigOptionInt(3));
    ed.set_obj(obj, "sparse_infill_density", new ConfigOptionPercent(0));
    ed.set("initial_layer_print_height", new ConfigOptionFloat(layer_height));
    ed.set_obj(obj, "layer_height", new ConfigOptionFloat(layer_height));
    ed.set_obj(obj, "alternate_extra_wall", new ConfigOptionBool(false));
    ed.set_obj(obj, "seam_position", new ConfigOptionEnum<SeamPosition>(spAligned));
    ed.set_obj(obj, "wall_sequence", new ConfigOptionEnum<WallSequence>(WallSequence::InnerOuter));
    ed.set_obj(obj, "overhang_reverse", new ConfigOptionBool(false));
    ed.set_obj(obj, "precise_z_height", new ConfigOptionBool(false));
    ed.set_obj(obj, "seam_slope_type", new ConfigOptionEnum<SeamScarfType>(SeamScarfType::None));

    const auto obj_bb = obj->bounding_box_exact();
    const auto height = 1.0 + 0.4 + ((params.end - params.start)) / params.step - EPSILON;
    if (height < obj_bb.size().z())
        cut_horizontal(model, 0, height, ModelObjectCutAttribute::KeepLower);
    return {};
}

std::string calib_vfa(Editor& ed, Model& model)
{
    if (std::string err = add_model(model, ed.cfg, "/calib/vfa/vfa.drc"); !err.empty())
        return err;
    DynamicPrintConfig& cfg    = ed.cfg;
    Calib_Params&       params = ed.calib.params;

    const double nozzle_diameter = ed.nozzle_diameter(std::max(params.extruder_id, 0));
    const double layer_height    = params.vfa_layer_height > 0.0 ? params.vfa_layer_height : nozzle_diameter / 2.0;

    // VFA_Test_Dlg::on_start(): the end speed against the filament's volumetric limit. The dialog offers
    // to lower the layer height (from the compatible process presets) or asks to continue; here it warns.
    {
        double preset_lh = nozzle_diameter / 2.0;
        if (const auto* lh_opt = cfg.option<ConfigOptionFloat>("layer_height"); lh_opt && lh_opt->value > 0.0)
            preset_lh = lh_opt->value;
        const double used_lh       = params.nozzle_based_resize ? layer_height : preset_lh;
        const double max_vol_speed = ed.float_at("filament_max_volumetric_speed", 0.0);
        double       line_width    = cfg.get_abs_value("outer_wall_line_width", nozzle_diameter);
        if (line_width <= 0.0)
            line_width = cfg.get_abs_value("line_width", nozzle_diameter);
        if (line_width <= 0.0)
            line_width = nozzle_diameter;
        const double mm3_per_mm = Flow(float(line_width), float(used_lh), float(nozzle_diameter)).mm3_per_mm();
        const double limit      = mm3_per_mm > 0.0 ? max_vol_speed / mm3_per_mm : 1e9;
        if (max_vol_speed > 0.0 && params.end > limit)
            ed.warn("the end speed " + num(params.end) + " mm/s exceeds the filament's maximum volumetric speed (" + num(max_vol_speed) +
                    " mm3/s), which limits the outer wall to about " + num(std::floor(limit)) + " mm/s at layer height " + num(used_lh) +
                    " mm: the upper blocks will be clamped. Lower end= or pass layer_height= in --calib-params.");
    }

    auto obj_bb = model.objects[0]->bounding_box_exact();
    auto height = vfa_base_block_height * ((params.end - params.start) / params.step + 1) - EPSILON;
    if (height < obj_bb.size().z())
        cut_horizontal(model, 0, height, ModelObjectCutAttribute::KeepLower);

    ModelObject* obj = model.objects[0];
    if (params.nozzle_based_resize) {
        const double xy_scale = nozzle_diameter / vfa_base_nozzle_diameter;
        const double z_scale  = (vfa_layers_per_block * layer_height) / vfa_base_block_height;
        if (std::abs(xy_scale - 1.0) > EPSILON || std::abs(z_scale - 1.0) > EPSILON)
            obj->scale(xy_scale, xy_scale, z_scale);
    }
    obj->ensure_on_bed();

    ed.set("resonance_avoidance", new ConfigOptionBool(false));
    ed.set_all<double, ConfigOptionFloats>("slow_down_layer_time", 0.0);
    ed.set_all<bool, ConfigOptionBoolsNullable>("enable_overhang_speed", false);
    ed.set("timelapse_type", new ConfigOptionEnum<TimelapseType>(tlTraditional));
    ed.set("wall_loops", new ConfigOptionInt(1));
    ed.set("alternate_extra_wall", new ConfigOptionBool(false));
    ed.set("top_shell_layers", new ConfigOptionInt(0));
    ed.set("bottom_shell_layers", new ConfigOptionInt(1));
    ed.set("sparse_infill_density", new ConfigOptionPercent(0));
    ed.set("detect_thin_wall", new ConfigOptionBool(false));
    ed.set("spiral_mode", new ConfigOptionBool(true));
    ed.set("enable_wrapping_detection", new ConfigOptionBool(false));
    ed.set("precise_z_height", new ConfigOptionBool(false));
    if (params.nozzle_based_resize) {
        ed.set("initial_layer_print_height", new ConfigOptionFloat(layer_height));
        ed.set_obj(obj, "layer_height", new ConfigOptionFloat(layer_height));
    }
    ed.set_obj(obj, "brim_type", new ConfigOptionEnum<BrimType>(btOuterOnly));
    ed.set_obj(obj, "brim_width", new ConfigOptionFloat(3.0));
    ed.set_obj(obj, "brim_object_gap", new ConfigOptionFloat(0.0));
    params.vfa_layer_height = params.nozzle_based_resize ? layer_height : 0.0;
    return {};
}

// ConfigManipulation::update_print_fff_config(): turning spiral vase mode on asks whether to adjust the
// settings it needs; the calibrations rely on the "Yes" answer, which the CLI's config check enforces.
void spiral_vase_fix(Editor& ed)
{
    const auto* spiral = ed.cfg.option<ConfigOptionBool>("spiral_mode");
    if (spiral == nullptr || !spiral->value)
        return;
    ed.set("wall_loops", new ConfigOptionInt(1));
    ed.set("top_shell_layers", new ConfigOptionInt(0));
    ed.set("sparse_infill_density", new ConfigOptionPercent(0));
    ed.set("enable_support", new ConfigOptionBool(false));
    ed.set("enforce_support_layers", new ConfigOptionInt(0));
    ed.set("detect_thin_wall", new ConfigOptionBool(false));
    ed.set("overhang_reverse", new ConfigOptionBool(false));
    ed.set("timelapse_type", new ConfigOptionEnum<TimelapseType>(tlTraditional));
    ed.set("enable_wrapping_detection", new ConfigOptionBool(false));
}

// The shared part of the input shaping (frequency, damping) and cornering tests.
void motion_test_common(Editor& ed, ModelObject* obj, bool cornering)
{
    DynamicPrintConfig& cfg    = ed.cfg;
    const Calib_Params& params = ed.calib.params;
    if (cornering) {
        if (ed.has_junction_deviation()) {
            ed.set("machine_max_junction_deviation", new ConfigOptionFloats{params.end});
            ed.set_all<double, ConfigOptionFloatsNullable>("default_junction_deviation", 0);
        } else {
            ed.set("machine_max_jerk_x", new ConfigOptionFloats{params.end});
            ed.set("machine_max_jerk_y", new ConfigOptionFloats{params.end});
            ed.set_all<double, ConfigOptionFloatsNullable>("default_jerk", 0);
        }
    } else if (ed.has_junction_deviation()) {
        const double jd = ed.float_at("machine_max_junction_deviation", 0.);
        ed.set("machine_max_junction_deviation", new ConfigOptionFloats{std::max(jd, 0.25)});
        ed.set_all<double, ConfigOptionFloatsNullable>("default_junction_deviation", 0);
    } else {
        const double jerk_value = ed.flavor() == gcfKlipper ? 5.0 : 10.0;
        ed.set("machine_max_jerk_x", new ConfigOptionFloats{std::max(ed.float_at("machine_max_jerk_x", 0.), jerk_value)});
        ed.set("machine_max_jerk_y", new ConfigOptionFloats{std::max(ed.float_at("machine_max_jerk_y", 0.), jerk_value)});
        ed.set_all<double, ConfigOptionFloatsNullable>("default_jerk", 0);
    }

    zero_pressure_advance_where_disabled(ed);

    ed.set("resonance_avoidance", new ConfigOptionBool(false));
    if (cornering) {
        ed.set("input_shaping_emit", new ConfigOptionBool(true));
        ed.set("input_shaping_type", new ConfigOptionEnum<InputShaperType>(InputShaperType::Disable));
    } else
        ed.set("input_shaping_emit", new ConfigOptionBool(false));
    ed.set_all<double, ConfigOptionFloats>("slow_down_layer_time", 0.0);
    ed.set_all<double, ConfigOptionFloats>("slow_down_min_speed", 0.0);
    ed.set_all<bool, ConfigOptionBools>("slow_down_for_layer_cooling", false);
    if (cornering) {
        const double mvs = ed.float_at("filament_max_volumetric_speed", 0.);
        ed.set_all<double, ConfigOptionFloats>("filament_max_volumetric_speed", std::max(mvs, 200.0));
    }
    if (ed.calib.params.mode == CalibMode::Calib_Input_shaping_freq)
        ed.set("layer_height", new ConfigOptionFloat(0.2));
    ed.set_all<bool, ConfigOptionBools>("enable_overhang_speed", false);
    if (!cornering) {
        ed.set("accel_to_decel_enable", new ConfigOptionBool(true));
        ed.set("accel_to_decel_factor", new ConfigOptionPercent(100));
    }
    ed.set("timelapse_type", new ConfigOptionEnum<TimelapseType>(tlTraditional));
    ed.set("wall_loops", new ConfigOptionInt(1));
    ed.set("top_shell_layers", new ConfigOptionInt(0));
    ed.set("bottom_shell_layers", new ConfigOptionInt(1));
    ed.set("sparse_infill_density", new ConfigOptionPercent(0));
    ed.set("detect_thin_wall", new ConfigOptionBool(false));
    ed.set("spiral_mode", new ConfigOptionBool(true));
    ed.set("spiral_mode_smooth", new ConfigOptionBool(false));
    ed.set("bottom_surface_pattern", new ConfigOptionEnum<InfillPattern>(ipRectilinear));
    const double machine_max_speed        = std::min(ed.float_at("machine_max_speed_x", 500.), ed.float_at("machine_max_speed_y", 500.));
    const double machine_max_acceleration = ed.float_at("machine_max_acceleration_extruding", 10000.);
    ed.set_all<double, ConfigOptionFloatsNullable>("outer_wall_speed", machine_max_speed);
    ed.set_all<double, ConfigOptionFloatsNullable>("default_acceleration", machine_max_acceleration);
    ed.set_all<double, ConfigOptionFloatsNullable>("outer_wall_acceleration", machine_max_acceleration);
    ed.set("precise_z_height", new ConfigOptionBool(false));
    ed.set_obj(obj, "brim_type", new ConfigOptionEnum<BrimType>(btOuterOnly));
    ed.set_obj(obj, "brim_width", new ConfigOptionFloat(3.0));
    ed.set_obj(obj, "brim_object_gap", new ConfigOptionFloat(0.0));
    (void) cfg;
}

std::string calib_input_shaping(Editor& ed, Model& model)
{
    const int test_model = ed.calib.params.test_model;
    if (std::string err = add_model(model, ed.cfg, test_model < 1 ? "/calib/input_shaping/ringing_tower.drc" : "/calib/input_shaping/fast_tower_test.drc");
        !err.empty())
        return err;
    motion_test_common(ed, model.objects[0], false);
    return {};
}

std::string calib_cornering(Editor& ed, Model& model)
{
    const int         test_model = ed.calib.params.test_model;
    const std::string path       = test_model == 0 ? "/calib/input_shaping/ringing_tower.drc" :
                                   (test_model == 1 ? "/calib/input_shaping/fast_tower_test.drc" : "/calib/cornering/SCV-V2.drc");
    if (std::string err = add_model(model, ed.cfg, path); !err.empty())
        return err;
    motion_test_common(ed, model.objects[0], true);
    return {};
}

// Tab.cpp input_shaper_types_for_flavor(), without Disable: the shaper types the dialog offers.
std::vector<InputShaperType> shaper_types_for_flavor(GCodeFlavor flavor)
{
    switch (flavor) {
    case gcfKlipper:
        return {InputShaperType::Default, InputShaperType::ZV, InputShaperType::MZV, InputShaperType::ZVD,
                InputShaperType::EI, InputShaperType::TwoHumpEI, InputShaperType::ThreeHumpEI};
    case gcfRepRapFirmware:
        return {InputShaperType::Default, InputShaperType::MZV, InputShaperType::ZVD, InputShaperType::ZVDD,
                InputShaperType::ZVDDD, InputShaperType::EI2, InputShaperType::EI3, InputShaperType::DAA};
    case gcfMarlinFirmware: return {InputShaperType::ZV};
    default: return {InputShaperType::Default};
    }
}

std::string shaper_name(InputShaperType t)
{
    const ConfigOptionDef* def = print_config_def.get("input_shaping_type");
    const size_t           idx = size_t(t);
    if (def && idx < def->enum_values.size())
        return def->enum_values[idx];
    return std::to_string(int(t));
}

// Parsed --calib-params, before the printer-dependent defaults are filled in.
struct RawParams
{
    std::map<std::string, std::string> kv;
    bool has(const char* k) const { return kv.count(k) > 0; }
};

RawParams g_raw; // filled by cli_calib_parse, read by cli_calib_setup

} // namespace

const std::vector<std::string>& cli_calib_modes()
{
    static std::vector<std::string> names;
    if (names.empty())
        for (const auto& m : mode_table())
            names.emplace_back(m.name);
    return names;
}

std::string cli_calib_parse(const std::string& mode_in, const std::string& params_in, CliCalibration& out)
{
    std::string mode = boost::algorithm::to_lower_copy(boost::trim_copy(mode_in));
    std::replace(mode.begin(), mode.end(), '-', '_');
    const ModeInfo* info = nullptr;
    for (const auto& m : mode_table())
        if (mode == m.name)
            info = &m;
    if (info == nullptr)
        return "unknown --calib mode \"" + mode_in + "\"; use one of: " + boost::algorithm::join(cli_calib_modes(), ", ");

    out              = CliCalibration();
    out.active       = true;
    out.mode_name    = mode;
    out.params.mode  = info->mode;
    g_raw            = RawParams();

    static const std::map<std::string, std::set<std::string>> allowed = {
        {"pa_line", {"start", "end", "step", "print_numbers", "bowden", "extruder"}},
        {"pa_pattern", {"start", "end", "step", "print_numbers", "bowden", "speeds", "accels", "extruder"}},
        {"pa_tower", {"start", "end", "step", "bowden", "extruder"}},
        {"flow_rate", {"pass", "pattern"}},
        {"flow_rate_yolo", {"pass", "pattern"}},
        {"temp_tower", {"start", "end", "resize", "extruder"}},
        {"vol_speed", {"start", "end", "step"}},
        {"vfa", {"start", "end", "step", "resize", "layer_height", "extruder"}},
        {"retraction", {"start", "end", "step"}},
        {"input_shaping_freq", {"freq_x", "freq_y", "damp", "shaper", "model"}},
        {"input_shaping_damp", {"freq_x", "freq_y", "start", "end", "shaper", "model"}},
        {"cornering", {"start", "end", "model"}},
    };
    const auto& ok = allowed.at(mode);

    std::vector<std::string> fields;
    boost::split(fields, params_in, boost::is_any_of(","), boost::token_compress_on);
    for (std::string f : fields) {
        boost::trim(f);
        if (f.empty())
            continue;
        const size_t eq = f.find('=');
        if (eq == std::string::npos)
            return "--calib-params: \"" + f + "\" is not key=value";
        std::string k = boost::algorithm::to_lower_copy(boost::trim_copy(f.substr(0, eq)));
        std::string v = boost::trim_copy(f.substr(eq + 1));
        if (k == "accelerations")
            k = "accels";
        if (ok.count(k) == 0)
            return "--calib-params: \"" + k + "\" is not a parameter of --calib " + mode + " (it takes: " +
                   boost::algorithm::join(std::vector<std::string>(ok.begin(), ok.end()), ", ") + ")";
        g_raw.kv[k] = v;
    }

    // Numeric fields are checked here, so a typo fails before any work is done.
    Calib_Params& p = out.params;
    for (const char* k : {"start", "end", "step", "damp", "layer_height"})
        if (g_raw.has(k)) {
            double v;
            if (!parse_double(g_raw.kv[k], v))
                return std::string("--calib-params: ") + k + "=" + g_raw.kv[k] + " is not a number";
        }
    auto get_int = [&](const char* k, int& dst, int lo, int hi) -> std::string {
        if (!g_raw.has(k))
            return {};
        double v;
        if (!parse_double(g_raw.kv[k], v) || v != std::floor(v) || v < lo || v > hi)
            return std::string("--calib-params: ") + k + "=" + g_raw.kv[k] + " must be a whole number from " + std::to_string(lo) + " to " +
                   std::to_string(hi);
        dst = int(v);
        return {};
    };
    int flag = 0;
    if (std::string e = get_int("print_numbers", flag, 0, 1); !e.empty())
        return e;
    if (std::string e = get_int("extruder", p.extruder_id, 0, 63); !e.empty())
        return e;
    if (std::string e = get_int("pass", out.flow_pass, 1, 2); !e.empty())
        return e;
    if (std::string e = get_int("model", p.test_model, 0, mode == "cornering" ? 2 : 1); !e.empty())
        return e;
    int b = 0;
    if (std::string e = get_int("bowden", b, 0, 1); !e.empty())
        return e;
    out.bowden = b != 0;
    int resize = 1;
    if (std::string e = get_int("resize", resize, 0, 1); !e.empty())
        return e;
    p.nozzle_based_resize = resize != 0;
    if (g_raw.has("pattern")) {
        std::string pat = boost::algorithm::to_lower_copy(g_raw.kv["pattern"]);
        if (pat == "archimedeanchords" || pat == "archimedean_chords")
            pat = "archimedean";
        if (pat != "archimedean" && pat != "monotonic")
            return "--calib-params: pattern=" + g_raw.kv["pattern"] + " must be archimedean or monotonic";
        out.flow_pattern = pat;
    }
    for (const char* k : {"speeds", "accels"})
        if (g_raw.has(k)) {
            std::vector<double> v;
            if (!parse_list(g_raw.kv[k], v) || v.empty())
                return std::string("--calib-params: ") + k + "=" + g_raw.kv[k] + " must be numbers separated by ':'";
            for (double x : v)
                if (x <= 0)
                    return std::string("--calib-params: ") + k + " must be positive";
            (std::string(k) == "speeds" ? p.speeds : p.accelerations) = v;
        }
    for (const char* k : {"freq_x", "freq_y"})
        if (g_raw.has(k)) {
            std::vector<double> v;
            if (!parse_list(g_raw.kv[k], v) || v.empty() || v.size() > 2)
                return std::string("--calib-params: ") + k + "=" + g_raw.kv[k] + " must be FREQ or START:END in Hz";
        }
    return {};
}

std::string cli_calib_setup(CliCalibration& calib, Model& model, DynamicPrintConfig& config, const std::set<std::string>& user_keys)
{
    if (!calib.active)
        return {};
    if (is_belt_printer(config))
        return "--calib is not available for belt printers on the command line (their calibration layouts need the desktop app)";

    ensure_keys(config);
    Editor        ed{calib, config, user_keys};
    Calib_Params& p    = calib.params;
    const auto&   raw  = g_raw.kv;
    auto          dbl  = [&](const char* k, double def) {
        double v = def;
        if (auto it = raw.find(k); it != raw.end())
            parse_double(it->second, v);
        return v;
    };
    const std::string mode = calib.mode_name;

    // Defaults of each dialog, then its checks.
    if (mode == "pa_line" || mode == "pa_pattern" || mode == "pa_tower") {
        // PA_Calibration_Dlg::reset_params(): direct drive by default, Bowden (bowden=1) uses wider ranges.
        double end = mode == "pa_pattern" ? 0.08 : 0.1, step = mode == "pa_pattern" ? 0.005 : 0.002;
        if (calib.bowden) {
            end  = 1.0;
            step = mode == "pa_pattern" ? 0.05 : 0.02;
        }
        p.start         = dbl("start", 0.0);
        p.end           = dbl("end", end);
        p.step          = dbl("step", step);
        p.print_numbers = raw.count("print_numbers") ? raw.at("print_numbers") == "1" : mode != "pa_tower";
        if (p.start < 0 || p.step < 10 * EPSILON || p.end < p.start + p.step)
            return "invalid PA range: need start >= 0, step > 0 and end >= start + step (got start=" + num(p.start) + " end=" + num(p.end) +
                   " step=" + num(p.step) + ")";
        if (!p.accelerations.empty() && !p.speeds.empty() &&
            *std::min_element(p.accelerations.begin(), p.accelerations.end()) <= *std::max_element(p.speeds.begin(), p.speeds.end()))
            return "acceleration values must be greater than speed values (speeds and accels swapped?)";
    } else if (mode == "temp_tower") {
        p.start = dbl("start", 230);
        p.end   = dbl("end", 190);
        // Temp_Calibration_Dlg: the fields snap to multiples of 5 within 155..500.
        for (double* t : {&p.start, &p.end}) {
            double snapped = std::floor(std::clamp(*t, 155.0, 500.0) / 5) * 5;
            if (snapped != *t)
                ed.warn("temperature " + num(*t) + " snapped to " + num(snapped) + " (the tower has 5 degree blocks from 155 to 500)");
            *t = snapped;
        }
        p.step = 5;
        if (p.start > 500 || p.end < 155 || p.end > p.start - 5)
            return "invalid temperature range: need start <= 500, end >= 155 and start >= end + 5";
    } else if (mode == "vol_speed") {
        p.start = dbl("start", 5);
        p.end   = dbl("end", 20);
        p.step  = dbl("step", 0.5);
        if (p.start <= 0 || p.step <= 0 || p.end < p.start + p.step)
            return "invalid volumetric speed range: need start > 0, step > 0 and end >= start + step";
    } else if (mode == "vfa") {
        p.start            = dbl("start", 40);
        p.end              = dbl("end", 200);
        p.step             = dbl("step", 10);
        p.vfa_layer_height = dbl("layer_height", 0.0);
        if (p.start <= 10 || p.step <= 0 || p.end < p.start + p.step)
            return "invalid VFA speed range: need start > 10, step > 0 and end >= start + step";
        if (p.vfa_layer_height < 0)
            return "layer_height must be positive";
    } else if (mode == "retraction") {
        p.start = dbl("start", 0);
        p.end   = dbl("end", 2);
        p.step  = dbl("step", 0.1);
        if (p.start < 0 || p.step <= 0 || p.end < p.start + p.step)
            return "invalid retraction range: need start >= 0, step > 0 and end >= start + step";
    } else if (mode == "input_shaping_freq" || mode == "input_shaping_damp") {
        const bool freq_test = mode == "input_shaping_freq";
        const bool reprap    = ed.flavor() == gcfRepRapFirmware;
        auto range = [&](const char* k, double s, double e, double& o_s, double& o_e) {
            o_s = s;
            o_e = e;
            if (auto it = raw.find(k); it != raw.end()) {
                std::vector<double> v;
                parse_list(it->second, v);
                o_s = v[0];
                o_e = v.size() > 1 ? v[1] : v[0];
            }
        };
        if (freq_test) {
            range("freq_x", 15, 110, p.freqStartX, p.freqEndX);
            if (raw.count("freq_y") && !reprap)
                range("freq_y", 15, 110, p.freqStartY, p.freqEndY);
            else {
                p.freqStartY = p.freqStartX;
                p.freqEndY   = p.freqEndX;
            }
            p.start = dbl("damp", 0.15);
            if (p.freqStartX < 0 || p.freqEndX > 500 || (!reprap && (p.freqStartY < 0 || p.freqEndY > 500)) || p.freqStartX >= p.freqEndX ||
                (!reprap && p.freqStartY >= p.freqEndY))
                return "invalid frequency range: need 0 <= start < end <= 500 Hz, as freq_x=START:END";
            if (p.start < 0 || p.start >= 1)
                return "damp must be from 0 to below 1";
        } else {
            double unused;
            range("freq_x", 30, 30, p.freqStartX, unused);
            if (raw.count("freq_y") && !reprap)
                range("freq_y", 30, 30, p.freqStartY, unused);
            else
                p.freqStartY = p.freqStartX;
            p.freqEndX = p.freqStartX;
            p.freqEndY = p.freqStartY;
            p.start    = dbl("start", 0.0);
            p.end      = dbl("end", 0.4);
            if (p.freqStartX < 0 || p.freqStartX > 500 || (!reprap && (p.freqStartY < 0 || p.freqStartY > 500)))
                return "invalid frequency: need 0..500 Hz";
            if (p.start < 0 || p.end > 1 || p.start >= p.end)
                return "invalid damping range: need 0 <= start < end <= 1";
        }
        const auto types = shaper_types_for_flavor(ed.flavor());
        p.shaper_type    = shaper_name(types.front());
        if (auto it = raw.find("shaper"); it != raw.end()) {
            std::string found;
            for (InputShaperType t : types)
                if (boost::iequals(shaper_name(t), it->second))
                    found = shaper_name(t);
            if (found.empty()) {
                std::vector<std::string> names;
                for (InputShaperType t : types)
                    names.push_back(shaper_name(t));
                return "shaper=" + it->second + " is not offered for this printer's G-code flavor; use one of: " + boost::algorithm::join(names, ", ");
            }
            p.shaper_type = found;
        }
    } else if (mode == "cornering") {
        const bool jd = ed.has_junction_deviation();
        p.start       = dbl("start", jd ? 0.0 : 1.0);
        p.end         = dbl("end", jd ? 0.25 : 15.0);
        const double max_end = jd ? 0.3 : 100.0, warn_at = jd ? 0.25 : 20.0;
        if (p.start < 0 || p.end > max_end || p.start >= p.end)
            return "invalid cornering range: need 0 <= start < end <= " + num(max_end) + (jd ? " mm (junction deviation)" : " mm/s (jerk)");
        if (p.end > warn_at)
            ed.warn("high cornering values may cause layer shifts (> " + num(warn_at) + ")");
    }

    std::string err;
    try {
        if (mode == "pa_line")
            err = calib_pa_line(ed, model);
        else if (mode == "pa_pattern")
            err = calib_pa_pattern(ed, model);
        else if (mode == "pa_tower")
            err = calib_pa_tower(ed, model);
        else if (mode == "flow_rate")
            err = calib_flowrate(ed, model, false);
        else if (mode == "flow_rate_yolo")
            err = calib_flowrate(ed, model, true);
        else if (mode == "temp_tower")
            err = calib_temp(ed, model);
        else if (mode == "vol_speed")
            err = calib_max_vol_speed(ed, model);
        else if (mode == "vfa")
            err = calib_vfa(ed, model);
        else if (mode == "retraction")
            err = calib_retraction(ed, model);
        else if (mode == "input_shaping_freq" || mode == "input_shaping_damp")
            err = calib_input_shaping(ed, model);
        else if (mode == "cornering")
            err = calib_cornering(ed, model);
    } catch (const std::exception& e) {
        err = std::string("building the calibration model failed: ") + e.what();
    }
    if (!err.empty())
        return err;
    if (model.objects.empty())
        return "the calibration produced no objects";
    spiral_vase_fix(ed);
    for (const ModelObject* o : model.objects)
        calib.objects.push_back(o->name);
    return {};
}

void cli_calib_before_slice(const CliCalibration& calib, Model& model, int plate_index, const Vec3d& plate_origin,
                            const DynamicPrintConfig& full_config, bool is_bbl_machine, const std::vector<ModelObject*>& plate_objects)
{
    // Plater::_calib_pa_pattern_gen_gcode()
    if (!calib.active || calib.params.mode != CalibMode::Calib_PA_Pattern || !model.calib_pa_pattern)
        return;
    // Without arrange the CLI does not fill the plate's object list: take the objects inside its bed area.
    std::vector<ModelObject*> objects = plate_objects;
    if (objects.empty()) {
        const BoundingBoxf bed = bed_extents(full_config);
        for (ModelObject* obj : model.objects) {
            if (obj->instances.empty())
                continue;
            const Vec3d c = obj->instance_bounding_box(0).center() - plate_origin;
            if (c.x() >= bed.min.x() && c.x() <= bed.max.x() && c.y() >= bed.min.y() && c.y() <= bed.max.y())
                objects.push_back(obj);
        }
    }
    if (objects.empty())
        return;
    std::vector<CustomGCode::Info> mgc;
    for (ModelObject* obj : objects)
        mgc.emplace_back(model.calib_pa_pattern->generate_custom_gcodes(full_config, is_bbl_machine, *obj, plate_origin));
    auto& pcgc = model.plates_custom_gcodes[plate_index];
    pcgc       = std::move(mgc[0]);
    mgc.erase(mgc.begin());
    for (size_t i = 0; i < pcgc.gcodes.size(); i++)
        for (auto& gc : mgc)
            if (i < gc.gcodes.size())
                pcgc.gcodes[i].extra += gc.gcodes[i].extra;
}

nlohmann::json cli_calib_to_json(const CliCalibration& calib)
{
    nlohmann::json j;
    if (!calib.active)
        return j;
    const Calib_Params& p = calib.params;
    j["mode"]       = calib.mode_name;
    j["calib_mode"] = int(p.mode);
    switch (p.mode) {
    case CalibMode::Calib_Flow_Rate:
        j["pass"]    = calib.flow_pass;
        j["pattern"] = calib.flow_pattern;
        break;
    case CalibMode::Calib_Input_shaping_freq:
        j["freq_x"]      = {p.freqStartX, p.freqEndX};
        j["freq_y"]      = {p.freqStartY, p.freqEndY};
        j["damp"]        = p.start;
        j["shaper_type"] = p.shaper_type;
        j["model"]       = p.test_model;
        break;
    case CalibMode::Calib_Input_shaping_damp:
        j["freq_x"]      = p.freqStartX;
        j["freq_y"]      = p.freqStartY;
        j["start"]       = p.start;
        j["end"]         = p.end;
        j["shaper_type"] = p.shaper_type;
        j["model"]       = p.test_model;
        break;
    default:
        j["start"] = p.start;
        j["end"]   = p.end;
        j["step"]  = p.step;
        break;
    }
    if (p.mode == CalibMode::Calib_PA_Line || p.mode == CalibMode::Calib_PA_Pattern)
        j["print_numbers"] = p.print_numbers;
    if (p.mode == CalibMode::Calib_Temp_Tower || p.mode == CalibMode::Calib_VFA_Tower)
        j["nozzle_based_resize"] = p.nozzle_based_resize;
    if (p.mode == CalibMode::Calib_VFA_Tower)
        j["layer_height"] = p.vfa_layer_height;
    if (p.mode == CalibMode::Calib_Cornering)
        j["model"] = p.test_model;
    if (p.mode == CalibMode::Calib_Vol_speed_Tower)
        j["units"] = "start/end/step are outer wall speeds in mm/s; volumetric_* are what was asked, in mm3/s";
    for (auto it = calib.extra.begin(); it != calib.extra.end(); ++it)
        j[it.key()] = it.value();
    j["objects"]        = calib.objects;
    j["user_overrides"] = calib.user_overrides;
    j["brim_skipped"]   = calib.brim_skipped;
    j["warnings"]       = calib.warnings;
    return j;
}

} // namespace Slic3r
