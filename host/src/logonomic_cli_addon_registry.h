#pragma once

#include "logonomic_cli_addon_roots.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>

namespace logonomic::cli_addon_host {

struct Declared_capability
{
    QString  capability_id;
    uint32_t version = 0;
};

/**
 * @brief One published package generation: the immutable custodied copy of
 * a validated package, bound to its digest and a host-issued nonce. Every
 * adapter, sidecar, helper and asset byte is resolved below custody_path;
 * source_root and source_level are diagnostics only.
 */
struct Generation_record
{
    QString                    package_id;
    QString                    release_version;
    QByteArray                 generation_digest_hex;   // 64 lowercase hex
    QByteArray                 generation_nonce_hex;    // frozen snapshot directory, 32 lowercase hex
    QString                    custody_path;            // read-only generation directory
    QString                    source_root;             // diagnostics only
    Root_level                 source_level = Root_level::INSTALLATION;
    QString                    adapter_relative_path;   // for this platform
    QStringList                adapter_argv;
    QVariantMap                adapter_environment;
    QList<uint32_t>            core_protocol_generations;
    QList<Declared_capability> declared_capabilities;   // valid sidecars
    quint64                    bytes = 0;
};

struct Unavailable_package
{
    QString package_id;   // may be empty when the bootstrap could not name one
    QString source_path;  // candidate directory, diagnostics only
    QString reason_code;  // logonomic::cli_addon::reason vocabulary
    QString detail;
};

struct Registry_options
{
    QString platform_id;                           // "linux-x86_64" / "windows-x86_64"
    QList<uint32_t> host_core_protocol_generations; // {1}
};

/**
 * @brief The frozen add-on package set of one process. discover() recovers
 * stale custody, scans the roots in precedence order, validates each
 * selected candidate through the SDK validator and custodies it; the result
 * is immutable. The object holds the run lock of every published generation
 * for its lifetime, so a concurrent or later process never removes a live
 * custody directory; destroying it (process exit) releases them.
 *
 * discover() is synchronous and has no thread affinity: it uses no event
 * loop, no signals and no QObject, so a broker may run it on a worker thread.
 * Cancellation returns nullptr after the current operation releases partial
 * staging. Checks occur between work items; an in-flight filesystem call is
 * not interrupted. Completed custody retains its normal stale-recovery owner.
 *
 * Ordering is deterministic for identical inputs: roots in precedence order,
 * directory entries sorted bytewise by name, package IDs sorted bytewise for
 * selection and custody. unavailable() lists root-scan failures (link
 * entries, candidate limit, unreadable bootstraps) in scan order, followed by
 * per-package failures in package-ID order.
 */
class Registry
{
public:
    static std::unique_ptr<Registry> discover(
        const Roots&            roots,
        const Registry_options& options,
        std::stop_token         cancellation = {});

    ~Registry();

    Registry(const Registry&)            = delete;
    Registry& operator=(const Registry&) = delete;

    const QList<Generation_record>&    generations() const;
    const QList<Unavailable_package>&  unavailable() const;
    std::optional<Generation_record>   generation(const QString& package_id) const;

private:
    struct Impl;
    explicit Registry(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

} // namespace logonomic::cli_addon_host
