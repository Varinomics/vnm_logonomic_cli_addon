#pragma once

#include "logonomic_cli_addon_generation_custody.h"
#include "logonomic_cli_addon_roots.h"

#include <QTemporaryDir>

namespace logonomic::cli_addon_host::tests {

struct Test_profile
{
    QTemporaryDir directory;
    Roots         roots;

    explicit Test_profile(const QString& development_root = {})
        : roots(Roots::defaults(directory.path(), development_root))
    {}

    ~Test_profile()
    {
        if (!directory.isValid()) {
            return;
        }
        // The registry has released its locks. Reclaim its read-only publications.
        Generation_custody::recover_stale(roots.custody_root);
        Generation_custody::recover_stale(
            roots.managed_assets_root, Generation_custody::Published_key::SHA256_DIGEST);
        if (!directory.remove()) {
            qFatal("Could not remove add-on test profile: %s", qPrintable(directory.path()));
        }
    }
};

} // namespace logonomic::cli_addon_host::tests
