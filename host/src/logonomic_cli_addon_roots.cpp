#include "logonomic_cli_addon_roots.h"

#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>

namespace logonomic::cli_addon_host {

namespace {

// The profile's own local-data root when profiles are overridden, otherwise
// the standard per-user application data location; the temporary directory
// only when Qt cannot name one. This is the chain logonomic_manager.cpp
// applies to every other profile-owned directory, so add-on state lands
// beside them.
QString local_data_root(const QString& profile_local_data_root)
{
    if (!profile_local_data_root.isEmpty()) {
        return QDir::cleanPath(profile_local_data_root);
    }
    QString standard = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (standard.isEmpty()) {
        standard = QDir::tempPath();
    }
    return QDir::cleanPath(standard);
}

} // namespace

const char* root_level_name(Root_level level)
{
    switch (level) {
        case Root_level::DEVELOPMENT:  return "development";
        case Root_level::PER_USER:     return "per-user";
        case Root_level::INSTALLATION: return "installation";
        default:                       return "installation";
    }
}

Roots Roots::defaults(
    const QString& profile_local_data_root,
    const QString& development_root)
{
    const QString addon_root = QDir(local_data_root(profile_local_data_root))
        .filePath(QStringLiteral("cli-addons"));

    Roots roots;
    roots.development_root     = development_root.isEmpty() ? QString() : QDir::cleanPath(development_root);
    roots.per_user_root        = addon_root + QStringLiteral("/packages");
    roots.custody_root         = addon_root + QStringLiteral("/generations");
    roots.scratch_root         = addon_root + QStringLiteral("/scratch");
    roots.managed_assets_root  = addon_root + QStringLiteral("/managed-assets");
    roots.profile_storage_root = addon_root + QStringLiteral("/profile-storage");
    return roots;
}

Roots Roots::for_profile(
    const QString&   profile_local_data_root,
    const QSettings& profile_settings)
{
    Roots roots = defaults(
        profile_local_data_root,
        profile_settings.value(k_development_root_setting).toString());
    // Present: that directory, or none when empty, which is how a profile opts
    // out of the packages beside the executable. Absent: those packages.
    roots.installation_root = profile_settings.contains(k_installation_root_setting)
        ? QDir::cleanPath(profile_settings.value(k_installation_root_setting).toString())
        : QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("cli_addons"));
    return roots;
}

} // namespace logonomic::cli_addon_host
