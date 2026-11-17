#ifndef slic3r_GUI_FilamentHub_hpp_
#define slic3r_GUI_FilamentHub_hpp_

// Fork: owned-filament inventory from the self-hosted filament hub (JonasMerrell/filament-hub).
//
// The hub joins Spoolman's inventory with the OrcaSlicer preset each filament is linked to. The
// filament combo boxes use it to list "My filaments" first and, optionally, hide everything else.

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

class AppConfig;

// App config keys (also read by OrcaCloudServiceAgent for profile sync).
static const std::string FILAMENT_HUB_URL_KEY        = "filament_hub_url";
static const std::string FILAMENT_HUB_TOKEN_KEY      = "filament_hub_token";
static const std::string FILAMENT_HUB_OWNED_ONLY_KEY = "filament_hub_owned_only";

namespace GUI {

struct OwnedFilament
{
    std::string preset_name;  // OrcaSlicer preset name the filament is linked to
    std::string name;         // Spoolman filament name
    std::string vendor;
    std::string material;
    double      remaining_g = -1.0; // < 0: unknown
    int         spool_count = 0;
};

class FilamentHubInventory
{
public:
    static FilamentHubInventory& instance();

    static bool configured(const AppConfig* config);

    // Fetch the owned filaments in the background, then refresh the filament combo boxes.
    void refresh_async();
    // Refresh only when the last refresh is older than max_age.
    void refresh_if_stale(std::chrono::seconds max_age);

    // Owned filament linked to this preset, by full preset name or by its alias.
    std::optional<OwnedFilament> find(const std::string& preset_name, const std::string& alias) const;
    bool has_data() const;

private:
    FilamentHubInventory() = default;
    void set_items(std::vector<OwnedFilament> items);

    mutable std::mutex                    m_mutex;
    std::vector<OwnedFilament>            m_items;
    bool                                  m_has_data = false;
    std::atomic_bool                      m_busy{false};
    std::chrono::steady_clock::time_point m_last_refresh{};
};

// " (640 g)" / " (1.2 kg)" suffix for a combo box entry, empty when unknown.
std::string owned_filament_suffix(const OwnedFilament& f);

} // namespace GUI
} // namespace Slic3r

#endif
