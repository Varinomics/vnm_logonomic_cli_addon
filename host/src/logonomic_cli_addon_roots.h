#pragma once

#include <QString>

class QSettings;

namespace logonomic::cli_addon_host {

enum class Root_level
{
    DEVELOPMENT,    // operator development root, highest precedence
    PER_USER,
    INSTALLATION,   // lowest precedence
};

/// Diagnostics name of a level: "development", "per-user" or "installation".
const char* root_level_name(Root_level level);

/// The profile settings that name package roots, `[cli_addons]` in the
/// profile's settings file.
inline constexpr char k_development_root_setting[]  = "cli_addons/development_root";
inline constexpr char k_installation_root_setting[] = "cli_addons/installation_root";

/**
 * @brief Every directory the add-on host reads packages from or owns state
 * in. Package roots are read-only to Logonomic; the custody, scratch,
 * managed-assets and profile-storage roots are host-owned and live below the
 * profile's local-data root.
 */
struct Roots
{
    QString development_root;    // empty = none configured
    QString per_user_root;       // <local_data_root>/cli-addons/packages
    QString installation_root;   // empty = none; for_profile() derives it
    QString custody_root;        // <local_data_root>/cli-addons/generations
    QString scratch_root;        // <local_data_root>/cli-addons/scratch
    QString managed_assets_root; // <local_data_root>/cli-addons/managed-assets
    QString profile_storage_root;// <local_data_root>/cli-addons/profile-storage

    // profile_local_data_root may be empty (profile not overridden); the
    // fallback is QStandardPaths::AppLocalDataLocation, the same choice
    // logonomic_manager.cpp already makes for its profile-owned directories.
    // development_root is the caller's; empty means none. The result has no
    // installation root: only a running application reads the packages beside
    // its executable (for_profile()), so roots built here never pick up what a
    // build staged there. This reads no settings and no environment variable.
    static Roots defaults(
        const QString& profile_local_data_root,
        const QString& development_root);

    // The roots of a running application: defaults() with both package roots
    // taken from the profile's settings. An empty or absent development_root
    // is none. An absent installation_root means <applicationDirPath>/cli_addons,
    // the packages that ship beside the executable; present and empty means
    // none; any other value names the directory.
    static Roots for_profile(
        const QString&   profile_local_data_root,
        const QSettings& profile_settings);
};

} // namespace logonomic::cli_addon_host
