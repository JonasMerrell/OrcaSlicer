#ifndef slic3r_Format_CliJob_hpp_
#define slic3r_Format_CliJob_hpp_

// Fork (headless CLI): a JSON "job file" that builds a project without the GUI: objects from mesh
// files or primitives, their transforms, per-object / per-part / per-height-range settings,
// modifiers, negative parts and support enforcer/blocker volumes, painting by face-selection rules
// (supports, seams, fuzzy skin, colour), planar cuts, filament assignment and plates with their own
// settings. `orca-slicer --load-job job.json` loads it; the result slices and exports like a 3MF.
// The format is documented in docs/headless-cli.md.

#include <map>
#include <string>
#include <vector>

#include "AssembleList.hpp"

#include <nlohmann/json_fwd.hpp>

namespace Slic3r {

class Model;

struct CliJobResult
{
    // One entry per plate, in the shape the CLI's --load-assemble-list path consumes
    // (plate_name, need_arrange, plate_params, loaded_obj_list, filaments_count).
    std::vector<assemble_plate_info_t> plates;
    // Object id -> run auto-orient on it (the CLI's orients_requirement).
    std::map<size_t, bool>             orient;
    // Human-readable error ("plates[0].objects[1].parts[0].file: no such file"); empty on success.
    std::string                        error;
    // Per object: name, plate, volumes and painted facet counts per rule, for the caller to check.
    std::vector<std::map<std::string, std::string>> report;
};

// Read the job file at `path` and add its objects to `model`. Relative paths in the job are taken
// relative to the job file's directory. Returns false and fills result.error on any problem;
// nothing is guessed: unknown fields, unknown or misplaced setting keys and bad values are errors.
bool load_cli_job(const std::string &path, Model &model, CliJobResult &result);

// Same, from an already parsed document; `base_dir` resolves relative paths.
bool load_cli_job(const nlohmann::json &job, const std::string &base_dir, Model &model, CliJobResult &result);

} // namespace Slic3r

#endif // slic3r_Format_CliJob_hpp_
