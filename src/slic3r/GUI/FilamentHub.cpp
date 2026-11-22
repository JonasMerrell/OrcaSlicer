#include "FilamentHub.hpp"

#include "GUI_App.hpp"
#include "Plater.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "slic3r/Utils/Http.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

#include <cstdio>

namespace Slic3r {
namespace GUI {

FilamentHubInventory& FilamentHubInventory::instance()
{
    static FilamentHubInventory inst;
    return inst;
}

bool FilamentHubInventory::configured(const AppConfig* config)
{
    return config != nullptr && !config->get(FILAMENT_HUB_URL_KEY).empty() && !config->get(FILAMENT_HUB_TOKEN_KEY).empty();
}

bool FilamentHubInventory::has_data() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_has_data;
}

void FilamentHubInventory::set_items(std::vector<OwnedFilament> items)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_items    = std::move(items);
    m_has_data = true;
}

std::optional<OwnedFilament> FilamentHubInventory::find(const std::string& preset_name, const std::string& alias) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& f : m_items) {
        if (f.preset_name.empty())
            continue;
        if (f.preset_name == preset_name || (!alias.empty() && f.preset_name == alias))
            return f;
    }
    return std::nullopt;
}

void FilamentHubInventory::refresh_if_stale(std::chrono::seconds max_age)
{
    std::chrono::steady_clock::time_point last;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        last = m_last_refresh;
    }
    if (last.time_since_epoch().count() == 0 || std::chrono::steady_clock::now() - last > max_age)
        refresh_async();
}

void FilamentHubInventory::refresh_async()
{
    AppConfig* config = wxGetApp().app_config;
    if (!configured(config))
        return;
    if (m_busy.exchange(true))
        return; // a refresh is already running

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_last_refresh = std::chrono::steady_clock::now();
    }

    std::string base = config->get(FILAMENT_HUB_URL_KEY);
    while (!base.empty() && base.back() == '/')
        base.pop_back();
    const std::string token = config->get(FILAMENT_HUB_TOKEN_KEY);

    auto http = Http::get(base + "/api/v1/inventory?owned=true");
    http.header("Authorization", "Bearer " + token)
        .timeout_connect(5)
        .timeout_max(15)
        .on_complete([this](std::string body, unsigned status) {
            std::vector<OwnedFilament> items;
            auto json = nlohmann::json::parse(body, nullptr, false);
            if (json.is_array()) {
                auto str = [](const nlohmann::json& j, const char* key) {
                    return j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : std::string();
                };
                for (const auto& j : json) {
                    if (!j.is_object())
                        continue;
                    OwnedFilament f;
                    f.preset_name = str(j, "orca_preset");
                    f.name        = str(j, "name");
                    f.vendor      = str(j, "vendor");
                    f.material    = str(j, "material");
                    if (j.contains("remaining_g") && j["remaining_g"].is_number())
                        f.remaining_g = j["remaining_g"].get<double>();
                    if (j.contains("spool_count") && j["spool_count"].is_number_integer())
                        f.spool_count = j["spool_count"].get<int>();
                    items.push_back(std::move(f));
                }
                BOOST_LOG_TRIVIAL(info) << "FilamentHub: " << items.size() << " owned filaments";
                set_items(std::move(items));
            } else {
                BOOST_LOG_TRIVIAL(warning) << "FilamentHub: unexpected inventory response (HTTP " << status << ")";
            }
            m_busy = false;
            wxGetApp().CallAfter([] {
                if (Plater* plater = wxGetApp().plater())
                    plater->sidebar().update_presets(Preset::TYPE_FILAMENT);
            });
        })
        .on_error([this](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(warning) << "FilamentHub: inventory request failed: " << error << " (HTTP " << status << ")";
            m_busy = false;
        })
        .perform();
}

std::string owned_filament_suffix(const OwnedFilament& f)
{
    if (f.remaining_g < 0)
        return {};
    char buf[32];
    if (f.remaining_g >= 1000.0)
        std::snprintf(buf, sizeof(buf), "  (%.1f kg)", f.remaining_g / 1000.0);
    else
        std::snprintf(buf, sizeof(buf), "  (%d g)", int(f.remaining_g + 0.5));
    return buf;
}

} // namespace GUI
} // namespace Slic3r
