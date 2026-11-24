#ifndef slic3r_CLICalibration_hpp_
#define slic3r_CLICalibration_hpp_

// ORCA headless-cli: the calibration generators of the Calibration menu, without the GUI.
//
// `--calib MODE --calib-params "k=v,..."` builds the same test model and applies the same
// settings as Plater::calib_*() in the desktop app, then the CLI slices it as usual. The
// G-code side (per-layer PA / temperature / speed / retraction / input shaping steps) is
// libslic3r's own, driven by Print::set_calib_params().

#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "libslic3r/calib.hpp"
#include "libslic3r/Point.hpp"

namespace Slic3r {

class DynamicPrintConfig;
class Model;
class ModelObject;

struct CliCalibration
{
    bool          active{false};
    std::string   mode_name;          // as given on the command line, normalized (pa_tower, ...)
    Calib_Params  params;             // what Print::set_calib_params() gets
    // Flow-rate tests do not set calib params; they are described by these.
    int           flow_pass{1};
    std::string   flow_pattern{"archimedean"};
    bool          keep_layout{false}; // the PA pattern places its objects itself: no arrange
    bool          bowden{false};
    // Reported in result.json under "calibration".
    std::vector<std::string> objects;
    std::vector<std::string> user_overrides;  // keys the calibration left alone because the command line set them
    std::vector<std::string> brim_skipped;    // object brim settings not applied because brim_type is no_brim
    std::vector<std::string> warnings;
    nlohmann::json           extra = nlohmann::json::object();
};

// The calibration modes, in the order --help lists them.
const std::vector<std::string>& cli_calib_modes();

// Parse --calib / --calib-params into `out`. Defaults that depend on the printer are resolved later
// in cli_calib_setup(). Returns an error message, empty on success.
std::string cli_calib_parse(const std::string& mode, const std::string& params, CliCalibration& out);

// Build the calibration model into `model` (which must be empty) and change `config` (the CLI's merged
// print + filament + printer config) the way the desktop app does. `user_keys` are config keys typed on
// the command line: the calibration does not override them. Returns an error message, empty on success.
std::string cli_calib_setup(CliCalibration& calib, Model& model, DynamicPrintConfig& config, const std::set<std::string>& user_keys);

// Called for each plate right before Print::apply(): the PA pattern writes its test lines as custom
// G-code per layer, generated from the handle objects on the plate.
void cli_calib_before_slice(const CliCalibration& calib, Model& model, int plate_index, const Vec3d& plate_origin,
                            const DynamicPrintConfig& full_config, bool is_bbl_machine, const std::vector<ModelObject*>& plate_objects);

nlohmann::json cli_calib_to_json(const CliCalibration& calib);

} // namespace Slic3r

#endif
