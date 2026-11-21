#include "logonomic_cli_addon_generation_custody.h"

#include "logonomic_cli_addon_generation_custody_platform.h"

#include "logonomic_cli_addon/generation_digest.h"
#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/json.h"
#include "logonomic_cli_addon/limits.h"
#include "logonomic_cli_addon/package_source.h"
#include "logonomic_cli_addon/sha256.h"
#include "logonomic_cli_addon/version.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef LOGONOMIC_ENABLE_CLI_ADDON_CUSTODY_TEST_HOOKS
#include <mutex>
#endif

namespace logonomic::cli_addon_host {

namespace sdk      = logonomic::cli_addon;
namespace limits   = logonomic::cli_addon::limits;
namespace reason   = logonomic::cli_addon::reason;
namespace platform = logonomic::cli_addon_host::custody_platform;

namespace {

constexpr std::size_t k_copy_chunk_bytes = 256 * 1024;

#ifdef LOGONOMIC_ENABLE_CLI_ADDON_CUSTODY_TEST_HOOKS
std::mutex g_copy_observer_mutex;
std::function<void(const QString&, quint64)> g_copy_observer;
#endif

using platform::Entry_mode;
using platform::Native_handle;

std::filesystem::path native_path(const QString& path)
{
    const QByteArray utf8 = path.toUtf8();
    return sdk::path_from_utf8(std::string_view(utf8.constData(), (std::size_t)utf8.size()));
}

QString from_platform(const std::string& text)
{
    return QString::fromUtf8(text.data(), (qsizetype)text.size());
}

// Sixteen bytes from the operating system's random source, rendered as the
// 32 lowercase hexadecimal characters the layout requires.
QString fresh_nonce_hex()
{
    std::array<quint64, 2> words{};
    QRandomGenerator::system()->fillRange(words.data(), (qsizetype)words.size());
    return QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(words.data()), sizeof(words)).toHex());
}

bool ensure_owner_directory(const QString& path, QString* out_error)
{
    if (!QDir().mkpath(path)) {
        *out_error = QStringLiteral("cannot create directory %1").arg(path);
        return false;
    }
    if (!QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)) {
        *out_error = QStringLiteral("cannot restrict directory %1 to its owner").arg(path);
        return false;
    }
    return true;
}

// Published generations are read-only, so removal first hands the owner
// write permission back (POSIX) and clears the read-only attribute (Windows).
bool remove_tree_clearing_read_only(const QString& path)
{
    const QFileInfo info(path);
    if (info.isSymLink() || (info.exists() && !info.isDir())) {
        return QFile::remove(path);
    }
    if (!info.exists()) {
        return true;
    }

    const QFile::Permissions writable_directory = QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner;
    const QFile::Permissions writable_file      = QFile::ReadOwner | QFile::WriteOwner;
    QFile::setPermissions(path, writable_directory);
    QDirIterator iterator(
        path,
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
        QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QFileInfo entry = iterator.nextFileInfo();
        if (entry.isSymLink()) {
            continue;
        }
        QFile::setPermissions(entry.filePath(), entry.isDir() ? writable_directory : writable_file);
    }
    return QDir(path).removeRecursively();
}

QString lock_path_for(const QString& directory)
{
    return directory + QLatin1StringView(k_run_lock_suffix);
}

/**
 * @brief One materialization attempt. The phases run in a fixed order and
 * the first failure discards the staging directory; nothing below the
 * package directory exists until the final rename.
 */
class Materialization
{
public:
    Materialization(
        const QString&                source_directory,
        const sdk::Validated_package& validated,
        const Generation_record&      candidate,
        const QString&                custody_root,
        Custody_ledger&               ledger,
        std::stop_token               cancellation)
    :
        m_source_directory(source_directory),
        m_validated(validated),
        m_candidate(candidate),
        m_custody_root(custody_root),
        m_ledger(ledger),
        m_cancellation(cancellation)
    {
#ifdef LOGONOMIC_ENABLE_CLI_ADDON_CUSTODY_TEST_HOOKS
        const std::lock_guard lock(g_copy_observer_mutex);
        m_copy_observer = g_copy_observer;
#endif
    }

    Materialization_result run();

private:
    bool check_cancellation();
    bool check_ledger();
    bool prepare();
    bool copy_indexed_files();
    bool copy_entry(const Native_handle& source_root, const sdk::Index_entry& entry);
    bool write_validated_bytes();
    bool write_root_file(const std::string& name, const std::string& bytes, std::string* out_error);
    bool reverify();
    bool reverify_entry(const sdk::Index_entry& entry, sdk::Sha256_digest& out_digest);
    bool reverify_root_file(const std::string& name, const std::string& expected);
    bool write_generation_record();
    bool make_read_only();
    bool publish();
    void discard();
    void release_lock_and_remove_file(std::unique_ptr<Run_lock>& lock);
    bool fail(const char* reason_code, QString detail);
    bool fail(const char* reason_code, const std::string& detail) { return fail(reason_code, from_platform(detail)); }

    // Opens the directory holding the last segment of a canonical path,
    // below `root`; `create` makes missing directories on the staging side.
    bool open_parent_directory(
        const Native_handle&                 root,
        const std::vector<std::string_view>& segments,
        bool                                 create,
        Native_handle&                       out_parent,
        std::string*                         out_error);

    const QString&                  m_source_directory;
    const sdk::Validated_package&   m_validated;
    const Generation_record&        m_candidate;
    const QString&                  m_custody_root;
    Custody_ledger&                 m_ledger;
    const std::stop_token           m_cancellation;

#ifdef LOGONOMIC_ENABLE_CLI_ADDON_CUSTODY_TEST_HOOKS
    std::function<void(const QString&, quint64)> m_copy_observer;
#endif

    QString                         m_nonce;
    QString                         m_staging_root;
    QString                         m_staging_path;
    QString                         m_package_path;
    QString                         m_custody_path;
    Native_handle                   m_staging_dir;
    std::unique_ptr<Run_lock>       m_staging_lock;
    std::unique_ptr<Run_lock>       m_run_lock;
    std::vector<std::string>        m_directories;      // relative directories created below staging
    std::vector<sdk::Sha256_digest> m_file_digests;     // index order, from the re-verified copy
    std::string                     m_generation_digest_hex;
    Materialization_result          m_result;
};

Materialization_result Materialization::run()
{
    const bool published =
        check_cancellation()       &&
        check_ledger()             &&
        prepare()                  &&
        copy_indexed_files()       &&
        check_cancellation()       &&
        write_validated_bytes()    &&
        reverify()                 &&
        check_cancellation()       &&
        write_generation_record()  &&
        make_read_only()           &&
        check_cancellation()       &&
        publish();
    if (!published) {
        discard();
        return std::move(m_result);
    }

    m_ledger.admitted_generations += 1;
    m_ledger.aggregate_bytes      += m_validated.total_bytes;

    Generation_record record     = m_candidate;
    record.generation_digest_hex = QByteArray::fromStdString(m_generation_digest_hex);
    record.generation_nonce_hex  = m_nonce.toLatin1();
    record.custody_path          = m_custody_path;
    record.bytes                 = m_validated.total_bytes;

    m_result.ok       = true;
    m_result.record   = std::move(record);
    m_result.run_lock = std::move(m_run_lock);
    return std::move(m_result);
}

bool Materialization::fail(const char* reason_code, QString detail)
{
    m_result.ok          = false;
    m_result.reason_code = QLatin1StringView(reason_code);
    m_result.detail      = std::move(detail);
    return false;
}

bool Materialization::check_cancellation()
{
    if (m_cancellation.stop_requested()) {
        return fail(reason::k_shutdown,
            QStringLiteral("package materialization was cancelled during shutdown"));
    }
    return true;
}

bool Materialization::check_ledger()
{
    if ((std::size_t)m_ledger.admitted_generations + 1 > limits::k_max_admitted_generations) {
        return fail(reason::k_custody_bound_exceeded,
            QStringLiteral("k_max_admitted_generations (%1) exceeded: %2 generations are already admitted")
                .arg(limits::k_max_admitted_generations)
                .arg(m_ledger.admitted_generations));
    }
    if (m_validated.total_bytes > limits::k_max_aggregate_custody_bytes ||
        m_ledger.aggregate_bytes > limits::k_max_aggregate_custody_bytes - m_validated.total_bytes)
    {
        return fail(reason::k_custody_bound_exceeded,
            QStringLiteral("k_max_aggregate_custody_bytes (%1) exceeded: %2 bytes are already admitted "
                           "and the package needs %3")
                .arg(limits::k_max_aggregate_custody_bytes)
                .arg(m_ledger.aggregate_bytes)
                .arg(m_validated.total_bytes));
    }
    return true;
}

bool Materialization::prepare()
{
    QString error;
    m_staging_root = m_custody_root + QLatin1Char('/') + QLatin1StringView(k_staging_directory_name);
    m_package_path = m_custody_root + QLatin1Char('/') + m_candidate.package_id;
    for (const QString& directory : {m_custody_root, m_staging_root, m_package_path}) {
        if (!ensure_owner_directory(directory, &error)) {
            return fail(reason::k_custody_write_failed, error);
        }
    }

    // Both locks are taken before the first byte is copied, so a concurrent
    // process recovering stale custody never sees an unlocked directory of
    // ours. A busy lock means another process is taking the lock of this very
    // nonce at this instant; one fresh nonce settles it.
    for (int attempt = 0; attempt < 2 && !m_run_lock; ++attempt) {
        m_nonce        = fresh_nonce_hex();
        m_staging_lock = Run_lock::try_acquire(lock_path_for(m_staging_root + QLatin1Char('/') + m_nonce), &error);
        if (!m_staging_lock) {
            if (!error.isEmpty()) {
                return fail(reason::k_custody_write_failed, error);
            }
            continue;
        }
        m_run_lock = Run_lock::try_acquire(lock_path_for(m_package_path + QLatin1Char('/') + m_nonce), &error);
        if (!m_run_lock) {
            release_lock_and_remove_file(m_staging_lock);
            if (!error.isEmpty()) {
                return fail(reason::k_custody_write_failed, error);
            }
        }
    }
    if (!m_run_lock) {
        return fail(reason::k_custody_write_failed,
            QStringLiteral("cannot take the run lock below %1 for a fresh nonce").arg(m_package_path));
    }

    m_staging_path = m_staging_root + QLatin1Char('/') + m_nonce;
    m_custody_path = m_package_path + QLatin1Char('/') + m_nonce;

    std::string       platform_error;
    Native_handle     staging_root;
    const std::string nonce = m_nonce.toStdString();
    if (!platform::open_directory(native_path(m_staging_root), false, staging_root, &platform_error) ||
        !platform::create_child_directory(staging_root, nonce, &platform_error) ||
        !platform::open_child_directory(staging_root, nonce, m_staging_dir, &platform_error))
    {
        return fail(reason::k_custody_write_failed, platform_error);
    }
    // Every terminal binding publishes this namespace, including packages
    // whose programs all live below adapter/.
    if (!platform::create_child_directory(m_staging_dir, sdk::k_segment_helpers, &platform_error)) {
        return fail(reason::k_custody_write_failed, platform_error);
    }
    m_directories.emplace_back(sdk::k_segment_helpers);
    return true;
}

bool Materialization::open_parent_directory(
    const Native_handle&                 root,
    const std::vector<std::string_view>& segments,
    bool                                 create,
    Native_handle&                       out_parent,
    std::string*                         out_error)
{
    Native_handle current;
    if (!platform::reopen_directory(root, current, out_error)) {
        return false;
    }
    std::string relative;
    for (std::size_t i = 0; i + 1 < segments.size(); ++i) {
        const std::string segment(segments[i]);
        if (create && !platform::create_child_directory(current, segment, out_error)) {
            return false;
        }
        Native_handle next;
        if (!platform::open_child_directory(current, segment, next, out_error)) {
            return false;
        }
        current = std::move(next);
        if (create) {
            relative += relative.empty() ? segment : "/" + segment;
            if (std::find(m_directories.begin(), m_directories.end(), relative) == m_directories.end()) {
                m_directories.push_back(relative);
            }
        }
    }
    out_parent = std::move(current);
    return true;
}

bool Materialization::copy_indexed_files()
{
    if (!check_cancellation()) {
        return false;
    }
    // The candidate directory was listed as a directory, not a link, at
    // discovery; a link in its place now is a swap since then.
    std::string   error;
    Native_handle source_root;
    if (!platform::open_directory(native_path(m_source_directory), true, source_root, &error)) {
        return fail(reason::k_source_mutated, error + " (source changed after validation)");
    }
    for (const sdk::Index_entry& entry : m_validated.index.files) {
        if (!copy_entry(source_root, entry)) {
            return false;
        }
    }
    return true;
}

bool Materialization::copy_entry(const Native_handle& source_root, const sdk::Index_entry& entry)
{
    if (!check_cancellation()) {
        return false;
    }
    const std::vector<std::string_view> segments = sdk::split_path_segments(entry.path);
    const std::string file_name(segments.back());
    const QString     path = QString::fromStdString(entry.path);
    std::string       error;

    Native_handle source_parent;
    if (!open_parent_directory(source_root, segments, false, source_parent, &error)) {
        return fail(reason::k_source_mutated, error + " (source changed after validation)");
    }
    Native_handle staging_parent;
    if (!open_parent_directory(m_staging_dir, segments, true, staging_parent, &error)) {
        return fail(reason::k_custody_write_failed, error);
    }

    Native_handle source_file;
    std::uint64_t source_size = 0;
    if (!platform::open_child_file_for_read(source_parent, file_name, source_file, source_size, &error)) {
        return fail(reason::k_source_mutated, error + " (source changed after validation)");
    }
    if (source_size != entry.bytes) {
        return fail(reason::k_source_mutated,
            QStringLiteral("%1 has %2 bytes, index records %3 (source changed after validation)")
                .arg(path).arg(source_size).arg(entry.bytes));
    }

    Native_handle staging_file;
    if (!platform::create_child_file(staging_parent, file_name, staging_file, &error)) {
        return fail(reason::k_custody_write_failed, error);
    }

    sdk::Sha256       hash;
    std::uint64_t     copied = 0;
    std::vector<char> buffer(k_copy_chunk_bytes);
    while (true) {
        if (!check_cancellation()) {
            return false;
        }
        std::size_t count = 0;
        if (!platform::read_some(source_file, buffer.data(), buffer.size(), count, &error)) {
            return fail(reason::k_source_mutated, error + " (source changed after validation)");
        }
        if (count == 0) {
            break;
        }
        copied += count;
        if (copied > entry.bytes) {
            return fail(reason::k_source_mutated,
                QStringLiteral("%1 grew beyond the %2 bytes the index records while copying")
                    .arg(path).arg(entry.bytes));
        }
        hash.update(buffer.data(), count);
        if (!platform::write_all(staging_file, buffer.data(), count, &error)) {
            return fail(reason::k_custody_write_failed, error);
        }
#ifdef LOGONOMIC_ENABLE_CLI_ADDON_CUSTODY_TEST_HOOKS
        if (m_copy_observer) {
            m_copy_observer(m_staging_path + QLatin1Char('/') + path, copied);
        }
#endif
    }
    if (copied != entry.bytes) {
        return fail(reason::k_source_mutated,
            QStringLiteral("%1 delivered %2 bytes, index records %3 (source changed while copying)")
                .arg(path).arg(copied).arg(entry.bytes));
    }
    const std::string digest_hex = sdk::to_hex(hash.finish());
    if (digest_hex != entry.sha256_hex) {
        return fail(reason::k_source_mutated,
            QStringLiteral("%1 has digest %2, index records %3 (source changed after validation)")
                .arg(path, QString::fromStdString(digest_hex), QString::fromStdString(entry.sha256_hex)));
    }
    if (!platform::flush_file(staging_file, &error)) {
        return fail(reason::k_custody_write_failed, error);
    }
    return true;
}

bool Materialization::write_root_file(const std::string& name, const std::string& bytes, std::string* out_error)
{
    Native_handle file;
    return platform::create_child_file(m_staging_dir, name, file, out_error) &&
           platform::write_all(file, bytes.data(), bytes.size(), out_error) &&
           platform::flush_file(file, out_error);
}

// The bootstrap and index are written from the bytes the validator parsed,
// not copied again, so the custodied copy cannot carry a manifest or index
// the validator did not see.
bool Materialization::write_validated_bytes()
{
    std::string error;
    if (!write_root_file(sdk::k_bootstrap_file_name, m_validated.bootstrap_bytes, &error) ||
        !write_root_file(sdk::k_index_file_name, m_validated.index_bytes, &error))
    {
        return fail(reason::k_custody_write_failed, error);
    }
    return true;
}

bool Materialization::reverify()
{
    if (!reverify_root_file(sdk::k_bootstrap_file_name, m_validated.bootstrap_bytes) ||
        !reverify_root_file(sdk::k_index_file_name, m_validated.index_bytes))
    {
        return false;
    }
    m_file_digests.clear();
    m_file_digests.reserve(m_validated.index.files.size());
    for (const sdk::Index_entry& entry : m_validated.index.files) {
        sdk::Sha256_digest digest{};
        if (!reverify_entry(entry, digest)) {
            return false;
        }
        m_file_digests.push_back(digest);
    }
    m_generation_digest_hex = sdk::compute_generation_digest_hex(
        m_validated.bootstrap_bytes, m_validated.index_bytes, m_file_digests);
    return true;
}

bool Materialization::reverify_entry(const sdk::Index_entry& entry, sdk::Sha256_digest& out_digest)
{
    if (!check_cancellation()) {
        return false;
    }
    const std::vector<std::string_view> segments = sdk::split_path_segments(entry.path);
    const std::string file_name(segments.back());
    const QString     path = QString::fromStdString(entry.path);
    std::string       error;

    Native_handle parent;
    Native_handle file;
    std::uint64_t size = 0;
    if (!open_parent_directory(m_staging_dir, segments, false, parent, &error) ||
        !platform::open_child_file_for_read(parent, file_name, file, size, &error))
    {
        return fail(reason::k_custody_write_failed, error);
    }
    if (size != entry.bytes) {
        return fail(reason::k_digest_mismatch,
            QStringLiteral("custodied %1 has %2 bytes, index records %3").arg(path).arg(size).arg(entry.bytes));
    }

    sdk::Sha256       hash;
    std::uint64_t     total = 0;
    std::vector<char> buffer(k_copy_chunk_bytes);
    while (true) {
        if (!check_cancellation()) {
            return false;
        }
        std::size_t count = 0;
        if (!platform::read_some(file, buffer.data(), buffer.size(), count, &error)) {
            return fail(reason::k_custody_write_failed, error);
        }
        if (count == 0) {
            break;
        }
        total += count;
        hash.update(buffer.data(), count);
    }
    out_digest = hash.finish();
    const std::string digest_hex = sdk::to_hex(out_digest);
    if (total != entry.bytes || digest_hex != entry.sha256_hex) {
        return fail(reason::k_digest_mismatch,
            QStringLiteral("custodied %1 has digest %2 over %3 bytes, index records %4 over %5 bytes")
                .arg(path, QString::fromStdString(digest_hex)).arg(total)
                .arg(QString::fromStdString(entry.sha256_hex)).arg(entry.bytes));
    }
    return true;
}

bool Materialization::reverify_root_file(const std::string& name, const std::string& expected)
{
    if (!check_cancellation()) {
        return false;
    }
    std::string   error;
    Native_handle file;
    std::uint64_t size = 0;
    if (!platform::open_child_file_for_read(m_staging_dir, name, file, size, &error)) {
        return fail(reason::k_custody_write_failed, error);
    }
    std::string       bytes;
    std::vector<char> buffer(k_copy_chunk_bytes);
    while (bytes.size() <= expected.size()) {
        if (!check_cancellation()) {
            return false;
        }
        std::size_t count = 0;
        if (!platform::read_some(file, buffer.data(), buffer.size(), count, &error)) {
            return fail(reason::k_custody_write_failed, error);
        }
        if (count == 0) {
            break;
        }
        bytes.append(buffer.data(), count);
    }
    if (bytes != expected) {
        return fail(reason::k_digest_mismatch,
            QStringLiteral("custodied %1 differs from the validated bytes").arg(QString::fromStdString(name)));
    }
    return true;
}

bool Materialization::write_generation_record()
{
    sdk::Json_value record = sdk::Json_value::object();
    record.set("package_id",            sdk::Json_value(m_validated.bootstrap.package_id));
    record.set("release_version",       sdk::Json_value(m_validated.bootstrap.release_version));
    record.set("generation_digest",     sdk::Json_value(m_generation_digest_hex));
    record.set("generation_nonce",      sdk::Json_value(m_nonce.toStdString()));
    record.set("source_root",           sdk::Json_value(m_candidate.source_root.toStdString()));
    record.set("source_level",          sdk::Json_value(root_level_name(m_candidate.source_level)));
    record.set("adapter_relative_path", sdk::Json_value(m_validated.adapter_relative_path));
    record.set("custodied_at",
        sdk::Json_value(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toStdString()));

    std::string error;
    if (!write_root_file(k_generation_record_file_name, sdk::serialize_json(record) + "\n", &error)) {
        return fail(reason::k_custody_write_failed, error);
    }
    return true;
}

bool Materialization::make_read_only()
{
    std::string error;
    for (const sdk::Index_entry& entry : m_validated.index.files) {
        if (!check_cancellation()) {
            return false;
        }
        const std::vector<std::string_view> segments = sdk::split_path_segments(entry.path);
        const bool executable =
            entry.kind == sdk::Resource_kind::EXECUTABLE ||
            entry.kind == sdk::Resource_kind::HELPER;
        Native_handle parent;
        if (!open_parent_directory(m_staging_dir, segments, false, parent, &error) ||
            !platform::set_child_mode(parent, std::string(segments.back()),
                executable ? Entry_mode::READ_ONLY_EXECUTABLE : Entry_mode::READ_ONLY_FILE, &error))
        {
            return fail(reason::k_custody_write_failed, error);
        }
    }
    for (const char* name : {sdk::k_bootstrap_file_name, sdk::k_index_file_name, k_generation_record_file_name}) {
        if (!platform::set_child_mode(m_staging_dir, name, Entry_mode::READ_ONLY_FILE, &error)) {
            return fail(reason::k_custody_write_failed, error);
        }
    }

    // Deepest directories first, so every chmod still runs through a parent
    // that is itself still open for search. The top-level directory follows
    // in publish(), once it has been renamed into place.
    std::vector<std::string> directories = m_directories;
    std::sort(directories.begin(), directories.end(), [](const std::string& a, const std::string& b) {
        const auto depth_a = std::count(a.begin(), a.end(), '/');
        const auto depth_b = std::count(b.begin(), b.end(), '/');
        return depth_a != depth_b ? depth_a > depth_b : a < b;
    });
    for (const std::string& directory : directories) {
        if (!check_cancellation()) {
            return false;
        }
        const std::vector<std::string_view> segments = sdk::split_path_segments(directory);
        Native_handle parent;
        if (!open_parent_directory(m_staging_dir, segments, false, parent, &error) ||
            !platform::set_child_mode(parent, std::string(segments.back()), Entry_mode::READ_ONLY_DIRECTORY, &error))
        {
            return fail(reason::k_custody_write_failed, error);
        }
    }
    return true;
}

bool Materialization::publish()
{
    std::string error;
    if (!platform::publish_directory(m_staging_dir, native_path(m_custody_path), &error)) {
        return fail(reason::k_custody_write_failed, error);
    }
    // The generation is complete and verified once the rename returns. The
    // directory flushes only decide what a power loss leaves for the next
    // start's stale recovery, which removes an unlocked directory either way,
    // so they carry no failure outcome.
    platform::flush_directory(native_path(m_package_path));
    platform::flush_directory(native_path(m_staging_root));
    release_lock_and_remove_file(m_staging_lock);
    return true;
}

void Materialization::release_lock_and_remove_file(std::unique_ptr<Run_lock>& lock)
{
    if (lock) {
        // Removed while still held, so a concurrent recovery cannot take the
        // lock of a file that is about to disappear.
        QFile::remove(lock->path());
        lock.reset();
    }
}

// A failed attempt leaves its directory at the staging path, or at the
// custody path when only the step after the rename failed; both are ours to
// remove, and a path that does not exist is nothing to remove.
void Materialization::discard()
{
    m_staging_dir.close();
    for (const QString& path : {m_staging_path, m_custody_path}) {
        if (!path.isEmpty()) {
            remove_tree_clearing_read_only(path);
        }
    }
    release_lock_and_remove_file(m_staging_lock);
    release_lock_and_remove_file(m_run_lock);
}

// Every <family>/<nonce> directory and <family>/<nonce>.lock file of one
// package (or of .staging) whose lock can be taken is stale.
bool is_valid_published_key(
    const std::string&                     text,
    Generation_custody::Published_key     key)
{
    if (key == Generation_custody::Published_key::GENERATION_NONCE) {
        return sdk::is_valid_generation_nonce_hex(text);
    }
    return sdk::is_valid_sha256_hex(text);
}

void recover_family(
    const QString&                         family_path,
    Generation_custody::Published_key     published_key,
    std::stop_token                        cancellation)
{
    const QFileInfoList entries = QDir(family_path).entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name);
    std::vector<std::string> nonces;
    for (const QFileInfo& entry : entries) {
        if (cancellation.stop_requested()) {
            return;
        }
        QString name = entry.fileName();
        if (name.endsWith(QLatin1StringView(k_run_lock_suffix))) {
            name.chop((qsizetype)std::strlen(k_run_lock_suffix));
        }
        const std::string key = name.toStdString();
        if (is_valid_published_key(key, published_key)) {
            nonces.push_back(key);
        }
    }
    std::sort(nonces.begin(), nonces.end());
    nonces.erase(std::unique(nonces.begin(), nonces.end()), nonces.end());

    for (const std::string& nonce : nonces) {
        if (cancellation.stop_requested()) {
            return;
        }
        const QString directory = family_path + QLatin1Char('/') + QString::fromStdString(nonce);
        const QString lock_path = lock_path_for(directory);
        QString       error;
        const std::unique_ptr<Run_lock> lock = Run_lock::try_acquire(lock_path, &error);
        if (!lock) {
            continue;   // held by a live process, or not lockable: never touched
        }
        remove_tree_clearing_read_only(directory);
        QFile::remove(lock_path);
    }
}

} // namespace

std::unique_ptr<Run_lock> Run_lock::try_acquire(const QString& path, QString* out_error)
{
    out_error->clear();
    std::string                    error;
    platform::Native_handle_value  handle = platform::k_invalid_native_handle;
    switch (platform::acquire_lock(
        native_path(path), platform::Lock_mode::EXCLUSIVE, handle, &error))
    {
        case platform::Lock_outcome::ACQUIRED: return std::unique_ptr<Run_lock>(new Run_lock(path, handle));
        case platform::Lock_outcome::BUSY:     return nullptr;
        case platform::Lock_outcome::FAILED:
        default:
            *out_error = from_platform(error);
            return nullptr;
    }
}

std::unique_ptr<Run_lock> Run_lock::try_acquire_shared(
    const QString& path,
    QString*       out_error)
{
    out_error->clear();
    std::string                   error;
    platform::Native_handle_value handle = platform::k_invalid_native_handle;
    switch (platform::acquire_lock(
        native_path(path), platform::Lock_mode::SHARED, handle, &error))
    {
        case platform::Lock_outcome::ACQUIRED:
            return std::unique_ptr<Run_lock>(new Run_lock(path, handle));
        case platform::Lock_outcome::BUSY:
            return nullptr;
        case platform::Lock_outcome::FAILED:
        default:
            *out_error = from_platform(error);
            return nullptr;
    }
}

Run_lock::~Run_lock()
{
    platform::release_lock(m_handle);
}

Run_lock::Run_lock(QString path, std::intptr_t handle)
:
    m_path(std::move(path)),
    m_handle(handle)
{}

Materialization_result Generation_custody::materialize(
    const QString&                source_directory,
    const sdk::Validated_package& validated_package,
    const Generation_record&      validated_candidate,
    const QString&                custody_root,
    Custody_ledger&               ledger,
    std::stop_token               cancellation)
{
    Materialization materialization(
        source_directory, validated_package, validated_candidate, custody_root, ledger, cancellation);
    return materialization.run();
}

void Generation_custody::recover_stale(
    const QString&  custody_root,
    Published_key   published_key,
    std::stop_token cancellation)
{
    if (cancellation.stop_requested() || custody_root.isEmpty() || !QFileInfo(custody_root).isDir()) {
        return;
    }
    const QFileInfoList families = QDir(custody_root).entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name);
    for (const QFileInfo& family : families) {
        if (cancellation.stop_requested()) {
            return;
        }
        if (family.isSymLink()) {
            continue;
        }
        const QString name = family.fileName();
        if (name != QLatin1StringView(k_staging_directory_name) && !sdk::is_valid_package_id(name.toStdString())) {
            continue;   // not created by this module
        }
        recover_family(
            custody_root + QLatin1Char('/') + name,
            name == QLatin1StringView(k_staging_directory_name)
                ? Published_key::GENERATION_NONCE
                : published_key,
            cancellation);
    }
}

#ifdef LOGONOMIC_ENABLE_CLI_ADDON_CUSTODY_TEST_HOOKS
void set_materialization_copy_observer_for_tests(
    std::function<void(const QString&, quint64)> observer)
{
    const std::lock_guard lock(g_copy_observer_mutex);
    g_copy_observer = std::move(observer);
}
#endif

} // namespace logonomic::cli_addon_host
