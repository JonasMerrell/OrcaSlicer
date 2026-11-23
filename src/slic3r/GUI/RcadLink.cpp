#include "RcadLink.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <vector>

#include <wx/string.h>
#include <wx/utils.h>

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Plater.hpp"
#include "Selection.hpp"
#include "libslic3r/Model.hpp"
#include "slic3r/Utils/Http.hpp"

namespace Slic3r {
namespace GUI {

std::optional<RcadSource> rcad_source(const std::string &input_file)
{
    if (input_file.empty())
        return std::nullopt;
    const boost::filesystem::path sidecar(input_file + ".rcad.json");
    boost::system::error_code     ec;
    if (!boost::filesystem::exists(sidecar, ec))
        return std::nullopt;
    try {
        boost::nowide::ifstream in(sidecar.string());
        nlohmann::json          j = nlohmann::json::parse(in);
        RcadSource              s;
        s.app      = j.value("app", "");
        s.document = j.value("document", "");
        s.studio   = j.value("studio", "");
        s.part     = j.value("part", "");
        s.link     = j.value("link", "");
        s.file     = input_file;
        if (s.document.empty() && s.link.empty())
            return std::nullopt;
        return s;
    } catch (const std::exception &) {
        return std::nullopt;
    }
}

std::optional<RcadSource> selected_rcad_source()
{
    Plater *plater = wxGetApp().plater();
    if (plater == nullptr)
        return std::nullopt;
    const Selection &selection = plater->get_selection();
    if (selection.is_empty() || selection.get_object_idx() < 0)
        return std::nullopt;
    const ModelObject *object = selection.get_model()->objects[selection.get_object_idx()];
    // a single selected part names its own file; a whole object uses its first part's
    const int volume_idx = selection.is_single_volume() ? selection.get_first_volume()->volume_idx() : 0;
    if (volume_idx < 0 || volume_idx >= int(object->volumes.size()))
        return std::nullopt;
    return rcad_source(object->volumes[volume_idx]->source.input_file);
}

static bool ask_running_rcad(const RcadSource &s)
{
    if (s.link.empty())
        return false;
    const std::string url = "http://" + s.link + "/edit?document=" + Http::url_encode(s.document) + "&studio=" + Http::url_encode(s.studio) +
                            "&part=" + Http::url_encode(s.part) + "&file=" + Http::url_encode(s.file);
    bool shown = false;
    Http::get(url)
        .timeout_connect(2)
        .timeout_max(5)
        .on_complete([&shown](std::string, unsigned status) { shown = status == 200; })
        .on_error([](std::string, std::string, unsigned) {})
        .perform_sync();
    return shown;
}

std::string edit_in_rcad(const RcadSource &s)
{
    if (ask_running_rcad(s))
        return {};
    boost::system::error_code ec;
    if (s.app.empty() || !boost::filesystem::exists(s.app, ec))
        return _u8L("rCAD isn't running, and the rCAD program that sent this part wasn't found.");
    if (s.document.empty() || !boost::filesystem::exists(s.document, ec))
        return _u8L("The rCAD document this part came from wasn't found.");

    const std::vector<std::wstring> args = {boost::nowide::widen(s.app), boost::nowide::widen(s.document), L"--studio",
                                            boost::nowide::widen(s.studio), L"--part", boost::nowide::widen(s.part)};
    std::vector<const wchar_t *>    argv;
    for (const std::wstring &a : args)
        argv.push_back(a.c_str());
    argv.push_back(nullptr);
    wxExecuteEnv   env;
    wxGetEnvMap(&env.env);
    // an AppImage's library paths are for the slicer, not for rCAD
    for (const char *var : {"APPIMAGE", "APPDIR", "LD_LIBRARY_PATH", "LD_PRELOAD", "UNION_PRELOAD"})
        env.env.erase(var);
    if (::wxExecute(argv.data(), wxEXEC_ASYNC, nullptr, &env) <= 0)
        return _u8L("rCAD could not be started.");
    return {};
}

} // namespace GUI
} // namespace Slic3r
