// Fork (headless CLI): guards that every setting stays reachable without the GUI, and unit tests of
// the --load-job loader. See docs/headless-cli.md.
//
// The audit table docs/headless-cli-audit.json lists every option of print_config_def with where it
// can be set. A new option, or an option that moves between presets, fails here until the table is
// regenerated (tools/orca-slice/gen_headless_cli_audit.py) and its CLI path reviewed.

#include <catch2/catch_all.hpp>

#include "libslic3r/CliOptionCatalog.hpp"
#include "libslic3r/Format/CliJob.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <set>

using namespace Slic3r;
using json = nlohmann::json;

namespace {

json load_audit()
{
    const boost::filesystem::path p = boost::filesystem::path(TEST_DATA_DIR) / ".." / ".." / "docs" / "headless-cli-audit.json";
    boost::nowide::ifstream in(p.string());
    REQUIRE(in.good());
    return json::parse(in);
}

std::vector<std::string> scopes_of(const CliOptionInfo &info)
{
    std::vector<std::string> s;
    if (!info.preset.empty()) s.push_back("global");
    if (info.per_object) s.push_back("object");
    if (info.per_part) s.push_back("part");
    if (info.per_layer_range) s.push_back("layer_range");
    if (info.per_plate) s.push_back("plate");
    return s;
}

bool load_job(const json &job, Model &model, CliJobResult &res)
{
    return load_cli_job(job, boost::filesystem::temp_directory_path().string(), model, res);
}

} // namespace

TEST_CASE("Every option is in the headless CLI audit table", "[HeadlessCLI]")
{
    const json audit = load_audit();
    REQUIRE(audit.contains("options"));
    const json &table = audit["options"];

    std::vector<std::string> missing, stale, changed, unexplained;
    std::set<std::string>    keys;
    for (const CliOptionInfo &info : cli_option_catalog()) {
        keys.insert(info.key);
        if (!table.contains(info.key)) {
            missing.push_back(info.key);
            continue;
        }
        const json &row = table[info.key];
        const std::string preset = row.contains("preset") && row["preset"].is_string() ? row["preset"].get<std::string>() : "";
        const std::vector<std::string> scopes = row.value("scopes", std::vector<std::string>());
        if (preset != info.preset || scopes != scopes_of(info))
            changed.push_back(info.key);
        // An option no preset stores and no object/plate can carry is only reachable as a raw
        // --key on the command line: that has to be a deliberate, documented choice.
        if (info.preset.empty() && scopes_of(info).empty() && row.value("note", std::string()).empty())
            unexplained.push_back(info.key);
    }
    for (auto it = table.begin(); it != table.end(); ++it)
        if (!keys.count(it.key()))
            stale.push_back(it.key());

    INFO("Options without an audit row (regenerate docs/headless-cli-audit.json and check their CLI path): "
         << missing.size());
    for (const auto &k : missing) INFO(k);
    CHECK(missing.empty());
    INFO("Audit rows for options that no longer exist: " << stale.size());
    CHECK(stale.empty());
    INFO("Options whose preset or scopes changed since the audit: " << changed.size());
    CHECK(changed.empty());
    INFO("Options reachable only as raw --key without a note in the audit: " << unexplained.size());
    CHECK(unexplained.empty());
    if (!missing.empty() || !changed.empty() || !unexplained.empty() || !stale.empty()) {
        std::string all;
        for (const auto *v : {&missing, &changed, &unexplained, &stale})
            for (const auto &k : *v)
                all += k + " ";
        FAIL_CHECK("offending keys: " << all);
    }
}

TEST_CASE("Every CLI option is in the audit table", "[HeadlessCLI]")
{
    const json audit = load_audit();
    REQUIRE(audit.contains("cli_options"));
    std::set<std::string> listed;
    for (const json &k : audit["cli_options"])
        listed.insert(k.get<std::string>());
    std::string missing;
    for (const ConfigDef *def : {static_cast<const ConfigDef *>(&cli_actions_config_def), static_cast<const ConfigDef *>(&cli_transform_config_def),
                                 static_cast<const ConfigDef *>(&cli_misc_config_def)})
        for (const auto &[key, d] : def->options)
            if (d.cli != ConfigOptionDef::nocli && !listed.count(key))
                missing += key + " ";
    INFO("CLI options missing from docs/headless-cli-audit.json cli_options: " << missing);
    CHECK(missing.empty());
}

TEST_CASE("Option catalog JSON describes every option", "[HeadlessCLI]")
{
    const json cat = cli_option_catalog_json();
    REQUIRE(cat["options"].size() == print_config_def.options.size());
    bool found = false;
    for (const json &o : cat["options"])
        if (o["key"] == "nozzle_temperature") {
            found = true;
            CHECK(o["preset"] == "filament");
            CHECK(o["cli"] == "--nozzle-temperature");
            CHECK(o["per_extruder"] == true);
        }
    CHECK(found);
    CHECK(cli_option_info("wall_loops").per_part);
    CHECK(cli_option_info("wall_loops").preset == "process");
    CHECK(cli_option_info("curr_bed_type").per_plate);
    CHECK(cli_option_info("no_such_option").key.empty());
}

TEST_CASE("Job file builds objects, parts, ranges and plates", "[HeadlessCLI]")
{
    const json job = json::parse(R"({
      "version": 1,
      "plates": [
        {"name": "A", "settings": {"curr_bed_type": "Textured PEI Plate"},
         "objects": [{
           "primitive": {"shape": "box", "size": [20, 20, 10]}, "name": "cube", "filament": 2, "count": 2,
           "settings": {"wall_loops": 4, "sparse_infill_density": "30%"},
           "height_ranges": [{"min_z": 0, "max_z": 3, "settings": {"sparse_infill_density": "80%"}}],
           "parts": [
             {"type": "modifier", "primitive": {"shape": "box", "size": [5, 5, 5]}, "position": [0, 0, 5], "settings": {"wall_loops": 1}},
             {"type": "negative", "primitive": {"shape": "cylinder", "radius": 2, "height": 20}},
             {"type": "support_blocker", "primitive": {"shape": "sphere", "radius": 3}}
           ],
           "paint": [
             {"layer": "support", "value": "enforce", "normal": [0, 0, -1], "max_angle": 10},
             {"layer": "fuzzy_skin", "z": [5, 10]},
             {"layer": "color", "value": 3, "box": {"min": [0, 0, 9], "max": [20, 20, 10]}}
           ]
         }]},
        {"name": "B", "arrange": false,
         "objects": [{"primitive": {"shape": "cylinder", "radius": 5, "height": 8}, "position": [100, 100], "rotate": [90, 0, 0]}]}
      ]})");
    Model        model;
    CliJobResult res;
    REQUIRE(load_job(job, model, res));
    INFO(res.error);
    REQUIRE(res.plates.size() == 2);
    CHECK(res.plates[0].plate_name == "A");
    CHECK(res.plates[0].need_arrange);
    CHECK(res.plates[0].plate_params.at("curr_bed_type") == "Textured PEI Plate");
    CHECK_FALSE(res.plates[1].need_arrange);
    REQUIRE(model.objects.size() == 2);

    const ModelObject *cube = model.objects[0];
    CHECK(cube->name == "cube");
    CHECK(cube->instances.size() == 2);
    CHECK(cube->config.opt_int("extruder") == 2);
    CHECK(cube->config.opt_int("wall_loops") == 4);
    REQUIRE(cube->layer_config_ranges.size() == 1);
    CHECK(cube->layer_config_ranges.begin()->second.has("layer_height"));
    int modifiers = 0, negatives = 0, blockers = 0;
    for (const ModelVolume *v : cube->volumes) {
        modifiers += v->is_modifier();
        negatives += v->type() == ModelVolumeType::NEGATIVE_VOLUME;
        blockers += v->is_support_blocker();
    }
    CHECK(modifiers == 1);
    CHECK(negatives == 1);
    CHECK(blockers == 1);
    // Every instance rests on the bed.
    CHECK(cube->instance_bounding_box(0).min.z() == Catch::Approx(0.).margin(1e-6));

    const ModelVolume *part = cube->volumes.front();
    REQUIRE(part->is_model_part());
    CHECK_FALSE(part->supported_facets.empty());
    CHECK_FALSE(part->fuzzy_skin_facets.empty());
    CHECK_FALSE(part->mmu_segmentation_facets.empty());

    // The rotated cylinder lies on its side, 10 mm tall, centred on its position.
    const BoundingBoxf3 bb = model.objects[1]->instance_bounding_box(0);
    CHECK(bb.size().z() == Catch::Approx(10.).margin(0.05));
    CHECK(bb.center().x() == Catch::Approx(100.).margin(0.05));
}

TEST_CASE("Job file cuts an object", "[HeadlessCLI]")
{
    const json job = json::parse(R"({"objects": [{"primitive": {"shape": "box", "size": [10, 10, 20]}, "cut": {"z": 5}}]})");
    Model        model;
    CliJobResult res;
    REQUIRE(load_job(job, model, res));
    REQUIRE(model.objects.size() == 2);
    REQUIRE(res.plates.size() == 1);
    CHECK(res.plates[0].loaded_obj_list.size() == 2);
    std::multiset<int> heights;
    for (const ModelObject *o : model.objects)
        heights.insert(int(std::lround(o->instance_bounding_box(0).size().z())));
    CHECK(heights == std::multiset<int>{5, 15});
}

TEST_CASE("Job file errors name the offending field", "[HeadlessCLI]")
{
    auto error_of = [](const char *text) {
        Model        model;
        CliJobResult res;
        CHECK_FALSE(load_job(json::parse(text), model, res));
        return res.error;
    };
    CHECK_THAT(error_of(R"({"objects": [{"primitive": {"shape": "box", "size": [1,1,1]}, "settings": {"nozzle_temperature": 200}}]})"),
               Catch::Matchers::ContainsSubstring("plates[0].objects[0].settings.nozzle_temperature") &&
                   Catch::Matchers::ContainsSubstring("filament setting"));
    CHECK_THAT(error_of(R"({"objects": [{"primitive": {"shape": "box", "size": [1,1,1]}, "settings": {"no_such_key": 1}}]})"),
               Catch::Matchers::ContainsSubstring("unknown setting"));
    CHECK_THAT(error_of(R"({"objects": [{"primitive": {"shape": "box", "size": [1,1,1]}, "settings": {"seam_position": "bogus"}}]})"),
               Catch::Matchers::ContainsSubstring("bad value"));
    CHECK_THAT(error_of(R"({"objects": [{"file": "/no/such/file.stl"}]})"), Catch::Matchers::ContainsSubstring("no such file"));
    CHECK_THAT(error_of(R"({"objects": [{"primitive": {"shape": "box", "size": [1,1,1]}, "colour": 1}]})"),
               Catch::Matchers::ContainsSubstring("unknown field"));
    CHECK_THAT(error_of(R"({"plates": [{"arrange": false, "objects": [{"primitive": {"shape": "box", "size": [1,1,1]}}]}]})"),
               Catch::Matchers::ContainsSubstring("needs a position"));
    CHECK_THAT(error_of(R"({"objects": [{"primitive": {"shape": "box", "size": [1,1,1]}, "parts": [{"type": "negative", "primitive": {"shape": "box", "size": [1,1,1]}, "settings": {"wall_loops": 2}}]}]})"),
               Catch::Matchers::ContainsSubstring("only parts and modifiers take settings"));
}

TEST_CASE("Job file simplifies and subtracts meshes", "[HeadlessCLI]")
{
    const json job = json::parse(R"({"objects": [{"primitive": {"shape": "sphere", "radius": 10}, "simplify": {"ratio": 0.25},
                                     "boolean": [{"op": "minus", "primitive": {"shape": "box", "size": [30, 30, 10]}, "position": [0, 0, 5]}]}]})");
    Model        model;
    CliJobResult res;
    REQUIRE(load_job(job, model, res));
    INFO(res.error);
    REQUIRE(model.objects.size() == 1);
    // the upper half of the sphere is gone
    CHECK(model.objects[0]->instance_bounding_box(0).size().z() == Catch::Approx(10.).margin(0.3));
}
