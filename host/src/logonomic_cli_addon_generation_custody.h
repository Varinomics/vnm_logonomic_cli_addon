#pragma once

#include "logonomic_cli_addon_registry.h"

#include "logonomic_cli_addon/package_validator.h"

#include <QString>
#include <QtTypes>

#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>

#ifdef LOGONOMIC_ENABLE_CLI_ADDON_CUSTODY_TEST_HOOKS
#include <functional>
#endif

namespace logonomic::cli_addon_host {

/**
 * @brief Layout of a custody root, shared by materialization, stale
 * recovery and the tests that seed stale state:
 *
 *   <custody_root>/<package_id>/<nonce>          published generation (read-only)
 *   <custody_root>/<package_id>/<nonce>.lock     run lock, held while the generation is live
 *   <custody_root>/.staging/<nonce>              copy in progress
 *   <custody_root>/.staging/<nonce>.lock         held while the copy is in progress
 *   <custody_root>/<package_id>/<nonce>/generation.json   host record of the generation
 *
 * A nonce is 32 lowercase hexadecimal characters. Entries that do not fit the
 * layout were not created by this module and are never removed.
 */
inline constexpr const char* k_staging_directory_name      = ".staging";
inline constexpr const char* k_run_lock_suffix             = ".lock";
inline constexpr const char* k_generation_record_file_name = "generation.json";

/**
 * @brief A non-blocking advisory lock on a lock file (flock on POSIX,
 * LockFileEx on Windows), released on destruction. Generation and staging
 * ownership is exclusive. Immutable managed-asset readers take shared leases,
 * so recover_stale() can remove only directories with no live reader.
 */
class Run_lock
{
public:
    /// nullptr with an empty *out_error when another process (or descriptor)
    /// holds the lock; nullptr with *out_error set when the file could not be
    /// opened or locked for another reason.
    static std::unique_ptr<Run_lock> try_acquire(const QString& path, QString* out_error);

    /// Shared leases keep a published immutable state directory alive across
    /// cooperating broker processes while stale recovery takes the same file
    /// exclusively.
    static std::unique_ptr<Run_lock> try_acquire_shared(
        const QString& path,
        QString*       out_error);

    ~Run_lock();

    Run_lock(const Run_lock&)            = delete;
    Run_lock& operator=(const Run_lock&) = delete;

    const QString& path() const { return m_path; }

private:
    Run_lock(QString path, std::intptr_t handle);

    QString       m_path;
    std::intptr_t m_handle;
};

struct Custody_ledger
{
    int     admitted_generations = 0;
    quint64 aggregate_bytes      = 0;
};

struct Materialization_result
{
    bool                             ok = false;
    std::optional<Generation_record> record;
    std::unique_ptr<Run_lock>        run_lock;   // held by the registry for its lifetime
    QString                          reason_code;
    QString                          detail;
};

class Generation_custody
{
public:
    enum class Published_key
    {
        GENERATION_NONCE,   // 32 lowercase hexadecimal characters
        SHA256_DIGEST,      // 64 lowercase hexadecimal characters
    };
    // Checks the aggregate bounds against the ledger, then copies every
    // indexed byte from source_directory into <custody_root>/.staging/<nonce>,
    // verifying size and digest while writing, fsyncs, reopens and re-verifies
    // every file, computes the generation digest (package contract,
    // "Generation digest") over the custodied bytes, writes generation.json,
    // makes files and directories read-only, holds
    // <custody_root>/<package_id>/<nonce>.lock, renames the staging directory
    // to <custody_root>/<package_id>/<nonce>, and updates the ledger. The
    // bootstrap and index are written from the validated bytes, so the copy
    // cannot carry a bootstrap or index the validator did not see. Any
    // mismatch or source mutation before publication removes the staging
    // directory and returns ok = false with reason source_mutated,
    // digest_mismatch, custody_bound_exceeded or custody_write_failed.
    // Shutdown cancellation follows the same discard path before publication
    // and returns shutdown without updating the ledger. After the rename,
    // the completed generation remains owned by the registry and stale recovery.
    static Materialization_result materialize(
        const QString&                                 source_directory,
        const logonomic::cli_addon::Validated_package& validated_package,
        const Generation_record&                       validated_candidate,   // digest, nonce, path, bytes filled here
        const QString&                                 custody_root,
        Custody_ledger&                                ledger,
        std::stop_token                                cancellation = {});

    // Removes every <custody_root>/<package_id>/<published-key> and
    // <custody_root>/.staging/<nonce> whose lock can be acquired
    // non-blocking, clearing read-only permissions first, together with the
    // lock file. A held lock means a live process; that directory is never
    // touched. A missing root is not an error. published_key selects a
    // generation nonce for custody/scratch and a digest for managed assets;
    // staging names are always nonces.
    static void recover_stale(
        const QString&  custody_root,
        Published_key   published_key = Published_key::GENERATION_NONCE,
        std::stop_token cancellation = {});
};

#ifdef LOGONOMIC_ENABLE_CLI_ADDON_CUSTODY_TEST_HOOKS
void set_materialization_copy_observer_for_tests(
    std::function<void(const QString& staging_file, quint64 copied_bytes)> observer);
#endif

} // namespace logonomic::cli_addon_host
