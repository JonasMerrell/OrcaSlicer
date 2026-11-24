#include "CliOptionCatalog.hpp"

#include "PrintConfig.hpp"
#include "Preset.hpp"
#include "PresetBundle.hpp"

#include <algorithm>
#include <cfloat>
#include <set>

#include <nlohmann/json.hpp>

namespace Slic3r {

namespace {

const std::set<std::string> &key_set(const std::vector<std::string> &keys)
{
    // Callers pass static vectors, so caching by address is safe.
    static std::map<const std::vector<std::string> *, std::set<std::string>> cache;
    auto it = cache.find(&keys);
    if (it == cache.end())
        it = cache.emplace(&keys, std::set<std::string>(keys.begin(), keys.end())).first;
    return it->second;
}

const char *type_name(ConfigOptionType t)
{
    switch (t) {
    case coFloat: return "float";
    case coFloats: return "floats";
    case coInt: return "int";
    case coInts: return "ints";
    case coString: return "string";
    case coStrings: return "strings";
    case coPercent: return "percent";
    case coPercents: return "percents";
    case coFloatOrPercent: return "float_or_percent";
    case coFloatsOrPercents: return "floats_or_percents";
    case coPoint: return "point";
    case coPoints: return "points";
    case coPoint3: return "point3";
    case coBool: return "bool";
    case coBools: return "bools";
    case coEnum: return "enum";
    case coEnums: return "enums";
    case coPointsGroups: return "points_groups";
    case coIntsGroups: return "ints_groups";
    default: return "none";
    }
}

const char *mode_name(ConfigOptionMode m)
{
    switch (m) {
    case comSimple: return "simple";
    case comAdvanced: return "advanced";
    case comExpert: return "expert";
    default: return "develop";
    }
}

std::string cli_flag(const std::string &key)
{
    std::string flag = key;
    std::replace(flag.begin(), flag.end(), '_', '-');
    return "--" + flag;
}

nlohmann::json def_json(const ConfigOptionDef &def)
{
    nlohmann::json j;
    j["key"]      = def.opt_key;
    j["cli"]      = cli_flag(def.opt_key);
    j["type"]     = type_name(def.type);
    j["vector"]   = (def.type & coVectorType) != 0;
    j["nullable"] = def.nullable;
    if (def.default_value) {
        try {
            j["default"] = def.default_value->serialize();
        } catch (...) {
        }
    }
    if (!def.label.empty())
        j["label"] = def.label;
    if (!def.full_label.empty() && def.full_label != def.label)
        j["full_label"] = def.full_label;
    if (!def.category.empty())
        j["category"] = def.category;
    if (!def.tooltip.empty())
        j["tooltip"] = def.tooltip;
    if (!def.sidetext.empty())
        j["sidetext"] = def.sidetext;
    if (def.min > -FLT_MAX)
        j["min"] = def.min;
    if (def.max < FLT_MAX)
        j["max"] = def.max;
    if (!def.ratio_over.empty())
        j["ratio_over"] = def.ratio_over;
    j["mode"] = mode_name(def.mode);
    if (def.readonly)
        j["readonly"] = true;
    if (!def.enum_values.empty()) {
        j["enum_values"] = def.enum_values;
        if (def.enum_labels.size() == def.enum_values.size())
            j["enum_labels"] = def.enum_labels;
    }
    if (!def.aliases.empty())
        j["aliases"] = def.aliases;
    if (!def.cli_params.empty())
        j["cli_params"] = def.cli_params;
    return j;
}

nlohmann::json cli_defs_json(const ConfigDef &defs)
{
    nlohmann::json out = nlohmann::json::array();
    for (const auto &[key, def] : defs.options) {
        if (def.cli == ConfigOptionDef::nocli)
            continue;
        nlohmann::json j = def_json(def);
        j.erase("mode");
        out.push_back(std::move(j));
    }
    return out;
}

} // namespace

const std::vector<std::string> &cli_plate_option_keys()
{
    // What PartPlate stores per plate (see PartPlate::get_config / bbs_3mf plate metadata).
    static const std::vector<std::string> keys = {
        "curr_bed_type",          "print_sequence",          "first_layer_print_sequence",
        "other_layers_print_sequence", "other_layers_print_sequence_nums", "spiral_mode",
        "filament_map_mode",      "filament_map",            "filament_volume_map",
        "filament_nozzle_map",
    };
    return keys;
}

CliOptionInfo cli_option_info(const std::string &key)
{
    CliOptionInfo info;
    if (print_config_def.options.find(key) == print_config_def.options.end())
        return info;
    info.key = key;
    // A few keys are in more than one list (e.g. compatible_printers): the first match wins,
    // in the order the GUI's tabs own them.
    if (key_set(Preset::print_options()).count(key))
        info.preset = "process";
    else if (key_set(Preset::filament_options()).count(key))
        info.preset = "filament";
    else if (key_set(Preset::printer_options()).count(key))
        info.preset = "printer";
    else if (key_set(PresetBundle::project_options()).count(key))
        info.preset = "project";

    static const PrintObjectConfig object_defaults;
    static const PrintRegionConfig region_defaults;
    const bool object_key = object_defaults.has(key);
    const bool region_key = region_defaults.has(key);
    info.per_object      = object_key || region_key || key == "extruder";
    info.per_part        = region_key || key == "extruder";
    info.per_layer_range = region_key || key == "extruder" || key == "layer_height";
    info.per_plate       = key_set(cli_plate_option_keys()).count(key) > 0;

    const ConfigOptionDef &def = print_config_def.options.at(key);
    if ((def.type & coVectorType) != 0 && (info.preset == "filament" || info.preset == "printer"))
        info.per_extruder = true;
    return info;
}

std::vector<CliOptionInfo> cli_option_catalog()
{
    std::vector<CliOptionInfo> out;
    out.reserve(print_config_def.options.size());
    for (const auto &[key, def] : print_config_def.options)
        out.push_back(cli_option_info(key));
    return out;
}

nlohmann::json cli_option_catalog_json()
{
    nlohmann::json options = nlohmann::json::array();
    for (const CliOptionInfo &info : cli_option_catalog()) {
        nlohmann::json j = def_json(print_config_def.options.at(info.key));
        j["preset"] = info.preset.empty() ? nlohmann::json(nullptr) : nlohmann::json(info.preset);
        nlohmann::json scopes = nlohmann::json::array();
        if (!info.preset.empty())
            scopes.push_back("global");
        if (info.per_object)
            scopes.push_back("object");
        if (info.per_part)
            scopes.push_back("part");
        if (info.per_layer_range)
            scopes.push_back("layer_range");
        if (info.per_plate)
            scopes.push_back("plate");
        j["scopes"] = scopes;
        if (info.per_extruder)
            j["per_extruder"] = true;
        options.push_back(std::move(j));
    }
    nlohmann::json out;
    out["version"] = 1;
    out["options"] = std::move(options);
    out["cli"]["actions"]    = cli_defs_json(cli_actions_config_def);
    out["cli"]["transforms"] = cli_defs_json(cli_transform_config_def);
    out["cli"]["misc"]       = cli_defs_json(cli_misc_config_def);
    return out;
}

} // namespace Slic3r
