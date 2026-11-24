#ifndef slic3r_CliOptionCatalog_hpp_
#define slic3r_CliOptionCatalog_hpp_

// Fork (headless CLI): a machine-readable catalog of every configuration option and of the CLI's
// own actions/transforms, so agents and scripts can find out, without a GUI, which settings exist,
// what values they take, and where each one can be set (process / filament / printer preset,
// project, per object, per part or modifier, per height range, per plate).
//
// `orca-slicer --list-options [file|-]` prints it; tests/libslic3r/test_cli_option_catalog.cpp
// checks it against docs/headless-cli-options.json so a new option cannot ship without a CLI path.

#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace Slic3r {

struct CliOptionInfo
{
    std::string key;
    // "process", "filament", "printer", "project" or "" (none: not stored in any preset).
    std::string preset;
    bool        per_object      = false; // PrintObjectConfig / PrintRegionConfig: object settings
    bool        per_part        = false; // PrintRegionConfig: part and modifier settings
    bool        per_layer_range = false; // height range modifiers
    bool        per_plate       = false; // plate settings (PartPlate config)
    bool        per_extruder    = false; // vector indexed by extruder (printer) or filament
};

// One entry per option of print_config_def, in key order.
std::vector<CliOptionInfo> cli_option_catalog();

// Where a key can be set, or an entry with an empty key when the key is unknown.
CliOptionInfo cli_option_info(const std::string &key);

// Keys a plate's own config may carry (job files "plates[].settings", 3MF plate metadata).
const std::vector<std::string> &cli_plate_option_keys();

// Full JSON catalog: {"version", "options": [...], "cli": {"actions": [...], "transforms": [...], "misc": [...]}}.
// Each option has key, cli flag, type, vector, nullable, default, label, category, tooltip,
// sidetext, min/max, mode, enum values/labels and the scopes above.
nlohmann::json cli_option_catalog_json();

} // namespace Slic3r

#endif // slic3r_CliOptionCatalog_hpp_
