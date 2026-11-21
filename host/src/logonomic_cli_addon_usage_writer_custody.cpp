#include "logonomic_cli_addon_usage_writer_custody.h"
#include "logonomic_cli_addon_generation_custody_platform.h"

#include "logonomic_cli_addon/json.h"
#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/package_source.h"
#include "logonomic_cli_addon/sha256.h"
#include "logonomic_cli_addon/usage_scan.h"

#include <filesystem>
#include <string>
#include <system_error>

namespace logonomic::cli_addon_host::usage_writer_custody {

namespace {

namespace sdk = logonomic::cli_addon;
namespace platform = custody_platform;
namespace fs = std::filesystem;

constexpr std::string_view k_open = "usage-cache-writers:1\n";
constexpr std::string_view k_settled = "usage-cache-writers-settled:1\n";
constexpr std::uint64_t k_max_reference_bytes = 256 * 1024;

fs::path io_path(std::string_view path)
{
    return sdk::usage_cache_io_path(sdk::path_from_utf8(path));
}

std::string normalized_path(const fs::path& path)
{
    const auto bytes = path.lexically_normal().generic_u8string();
    return std::string(bytes.begin(), bytes.end());
}

fs::file_type kind(const fs::path& path)
{
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory) return fs::file_type::not_found;
    return error ? fs::file_type::unknown : status.type();
}

bool fail(std::string* out_error, std::string message)
{
    *out_error = std::move(message);
    return false;
}

std::optional<std::string> read_file(const fs::path& path, std::string* out_error)
{
    platform::Native_handle parent;
    platform::Native_handle file;
    std::uint64_t size = 0;
    if (!platform::open_directory(path.parent_path(), true, parent, out_error) ||
        !platform::open_child_file_for_read(parent, sdk::path_to_utf8(path.filename()), file, size, out_error))
    {
        return std::nullopt;
    }
    if (size > k_max_reference_bytes) {
        fail(out_error, "usage writer reference exceeds its size bound");
        return std::nullopt;
    }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        std::size_t count = 0;
        if (!platform::read_some(file, bytes.data() + offset, bytes.size() - offset, count, out_error) || count == 0) {
            return std::nullopt;
        }
        offset += count;
    }
    char extra = 0;
    std::size_t count = 0;
    if (!platform::read_some(file, &extra, 1, count, out_error) || count != 0) return std::nullopt;
    return bytes;
}

bool matches(const fs::path& path, std::string_view expected)
{
    std::string error;
    const auto bytes = read_file(path, &error);
    return bytes && *bytes == expected;
}

bool write_new(const fs::path& path, std::string_view bytes, std::string* out_error)
{
    platform::Native_handle parent;
    platform::Native_handle file;
    if (!platform::open_directory(path.parent_path(), true, parent, out_error) ||
        !platform::create_child_file(parent, sdk::path_to_utf8(path.filename()), file, out_error))
    {
        return false;
    }
    if (!platform::write_all(file, bytes.data(), bytes.size(), out_error) ||
        !platform::flush_file(file, out_error))
    {
        // Only this invocation created the file. A failed publication must
        // not leave a partial record that prevents an authoritative retry.
        file.close();
        std::error_code ignored;
        (void)fs::remove(path, ignored);
        return false;
    }
    platform::flush_directory(path.parent_path());
    return true;
}

bool remove_file(const fs::path& path, std::string* out_error)
{
    std::error_code error;
    const bool removed = fs::remove(path, error);
    if (error) return fail(out_error, "could not remove usage writer reference: " + error.message());
    if (removed) platform::flush_directory(path.parent_path());
    return true;
}

bool publish_settlement(const fs::path& registry, std::string* out_error)
{
    const auto pending = registry / ".settled.pending";
    const auto pending_kind = kind(pending);
    if (pending_kind != fs::file_type::not_found && pending_kind != fs::file_type::regular) {
        return fail(out_error, "usage writer settlement staging is unknown");
    }
    // This fixed staging name belongs to the native closer under the registry
    // lock. Replay never calls this producer or treats pending bytes as proof.
    if (!remove_file(pending, out_error) || !write_new(pending, k_settled, out_error)) return false;
    if (!platform::publish_file(pending, registry / ".settled", out_error)) return false;
    platform::flush_directory(registry);
    return true;
}

std::string entry_name(const Reference& reference)
{
    return sdk::to_hex(sdk::Sha256::digest(reference.root)) + ".json";
}

class Registry_lock
{
public:
    Registry_lock() = default;
    ~Registry_lock() { release(); }
    Registry_lock(const Registry_lock&) = delete;
    Registry_lock& operator=(const Registry_lock&) = delete;

    void release()
    {
        if (m_handle != platform::k_invalid_native_handle) {
            platform::release_lock(m_handle);
            m_handle = platform::k_invalid_native_handle;
        }
    }

    bool acquire(
        const fs::path& root, std::string* out_error,
        platform::Lock_wait wait = platform::Lock_wait::BLOCKING)
    {
        if (kind(root) != fs::file_type::directory || kind(root / ".lock") != fs::file_type::regular) {
            return fail(out_error, "usage writer registry or its lock is missing or unknown");
        }
        // Never create a missing admission lock. A closer can remove an empty
        // registry while another caller is between discovery and this lock.
        return platform::acquire_lock(root / ".lock", platform::Lock_mode::EXCLUSIVE,
            m_handle, out_error, false, wait) == platform::Lock_outcome::ACQUIRED;
    }

private:
    platform::Native_handle_value m_handle = platform::k_invalid_native_handle;
};

bool contains_only_generation_markers(const fs::path& root)
{
    std::error_code error;
    fs::directory_iterator entries(root, error);
    if (error) return false;
    for (const auto end = fs::directory_iterator{}; entries != end; entries.increment(error)) {
        if (error) return false;
        const auto name = entries->path().filename();
        if (name != ".lock" && name != ".open" && name != ".settled") return false;
    }
    return !error;
}

// Remove admission while still locked. A waiter on the retired lock inode
// must then fail its state check. Release our handle before unlink/rmdir so
// Windows does not leave the lock delete-pending beneath the directory.
void remove_empty_registry(const fs::path& root, Registry_lock& lock)
{
    if (!contains_only_generation_markers(root) || !matches(root / ".settled", k_settled) ||
        (kind(root / ".open") != fs::file_type::not_found && !matches(root / ".open", k_open))) return;
    std::string ignored;
    if (!remove_file(root / ".open", &ignored)) return;
    lock.release();
    if (!remove_file(root / ".lock", &ignored)) return;
    if (!remove_file(root / ".settled", &ignored)) return;
    std::error_code error;
    (void)fs::remove(root, error);
    platform::flush_directory(root.parent_path());
}

bool release_locked(const Reference& reference, const fs::path& registry, Registry_lock& lock, std::string* out_error)
{
    const fs::path marker = io_path(reference.root + ".writer");
    const fs::path entry = registry / entry_name(reference);
    const std::string expected = encode_reference(reference);
    const bool marker_missing = kind(marker) == fs::file_type::not_found;
    const bool entry_missing = kind(entry) == fs::file_type::not_found;
    if ((!marker_missing && !matches(marker, expected)) ||
        (!entry_missing && !matches(entry, expected)) || (!marker_missing && entry_missing))
    {
        return fail(out_error, "usage writer allocation and generation registration do not match");
    }
    // An interruption after removing the marker leaves a retryable orphan
    // registration. The opposite order would discard the only restart proof.
    if (!remove_file(marker, out_error) || !remove_file(entry, out_error)) return false;
    if (matches(registry / ".settled", k_settled)) remove_empty_registry(registry, lock);
    return true;
}

} // namespace

std::string registry_for_scratch(std::string_view scratch)
{
    return normalized_path(sdk::path_from_utf8(scratch)) + ".usage-writers";
}

std::string encode_reference(const Reference& reference)
{
    auto value = sdk::Json_value::object();
    value.set("version", sdk::Json_value(2));
    value.set("root", sdk::Json_value(reference.root));
    value.set("accepted_root", sdk::Json_value(reference.accepted_root));
    value.set("generation", sdk::Json_value(reference.generation));
    return sdk::serialize_json_canonical(value);
}

std::optional<Reference> decode_reference(std::string_view bytes)
{
    sdk::Json_parse_limits limits;
    limits.max_bytes = k_max_reference_bytes;
    sdk::Json_parse_error error;
    const auto value = sdk::parse_json(bytes, limits, &error);
    if (!value || !value->is_object() || value->as_object().size() != 4) return std::nullopt;
    const auto* version = value->find("version");
    const auto* root = value->find("root");
    const auto* accepted = value->find("accepted_root");
    const auto* generation = value->find("generation");
    if (!version || !version->is_integer() || version->as_integer() != 2 ||
        !root || !root->is_string() || !accepted || !accepted->is_string() ||
        !generation || !generation->is_string()) return std::nullopt;
    Reference reference{root->as_string(), accepted->as_string(), generation->as_string()};
    for (const auto* path : {&reference.root, &reference.accepted_root, &reference.generation}) {
        if (path->empty() && path == &reference.accepted_root) continue;
        const auto native = sdk::path_from_utf8(*path);
        if (!native.is_absolute() || normalized_path(native) != *path) return std::nullopt;
    }
    return reference;
}

bool initialize_generation(const std::string& generation, std::string* out_error)
{
    const fs::path root = io_path(generation);
    std::error_code error;
    if (!fs::create_directory(root, error)) {
        return fail(out_error, "could not create fresh usage writer registry: " + error.message());
    }
    // Only this newly created generation can publish its admission marker.
    return write_new(root / ".lock", {}, out_error) && write_new(root / ".open", k_open, out_error);
}

bool begin_write(const Reference& reference, const std::function<bool()>& publish_marker, std::string* out_error)
{
    const fs::path registry = io_path(reference.generation);
    Registry_lock lock;
    if (!lock.acquire(registry, out_error)) return false;
    if (!matches(registry / ".open", k_open) || kind(registry / ".settled") != fs::file_type::not_found) {
        return fail(out_error, "usage writer generation no longer admits allocations");
    }
    const std::string bytes = encode_reference(reference);
    const fs::path entry = registry / entry_name(reference);
    if (kind(entry) == fs::file_type::not_found) {
        if (!write_new(entry, bytes, out_error)) return false;
    }
    else if (!matches(entry, bytes)) {
        return fail(out_error, "usage writer allocation already has a different generation registration");
    }
    const fs::path marker = io_path(reference.root + ".writer");
    if (kind(marker) != fs::file_type::not_found) return matches(marker, bytes);
    // The native closer cannot settle admission between this marker's durable
    // publication and its registration above: both occur under the same lock.
    return publish_marker() && matches(marker, bytes);
}

bool settle_write(const Reference& reference, std::string* out_error)
{
    const fs::path registry = io_path(reference.generation);
    if (kind(registry) == fs::file_type::not_found) {
        return kind(io_path(reference.root + ".writer")) == fs::file_type::not_found;
    }
    Registry_lock lock;
    return lock.acquire(registry, out_error) && release_locked(reference, registry, lock, out_error);
}

namespace {

bool close_generation_impl(const std::string& generation, bool require_closed, std::string* out_error)
{
    const fs::path registry = io_path(generation);
    if (kind(registry) == fs::file_type::not_found) return true;
    std::error_code error;
    if (!require_closed && kind(registry) == fs::file_type::directory && fs::is_empty(registry, error) && !error) {
        // Explicit native authority can retry retirement after all metadata
        // disappeared but rmdir did not. Replay never takes this branch.
        (void)fs::remove(registry, error);
        return true;
    }
    if (kind(registry / ".open") == fs::file_type::not_found &&
        kind(registry / ".lock") == fs::file_type::not_found &&
        matches(registry / ".settled", k_settled) && contains_only_generation_markers(registry))
    {
        // Admission was removed under the old lock and no registration
        // remains. This is an existing proof, never an inferred one.
        if (remove_file(registry / ".settled", out_error)) (void)fs::remove(registry, error);
        return true;
    }
    Registry_lock lock;
    if (!lock.acquire(registry, out_error,
            require_closed ? platform::Lock_wait::NON_BLOCKING : platform::Lock_wait::BLOCKING)) return false;
    if (require_closed && !matches(registry / ".settled", k_settled)) return true;
    if (!matches(registry / ".open", k_open) && !matches(registry / ".settled", k_settled)) {
        if (!require_closed && kind(registry / ".open") == fs::file_type::not_found &&
            kind(registry / ".settled") == fs::file_type::not_found && contains_only_generation_markers(registry))
        {
            // A lock-only fragment can also precede admission publication.
            // Only explicit native settlement may retire it; no writer was
            // admitted here and no cache marker is changed by this cleanup.
            lock.release();
            if (remove_file(registry / ".lock", out_error)) (void)fs::remove(registry, error);
            return true;
        }
        return fail(out_error, "usage writer registry admission is unknown");
    }
    if (kind(registry / ".settled") == fs::file_type::not_found) {
        if (!publish_settlement(registry, out_error)) return false;
    }
    else if (!matches(registry / ".settled", k_settled)) {
        return fail(out_error, "usage writer settlement proof is unknown");
    }
    // A host can die after registration but before marker publication, or after
    // removing a settled marker. Such entries no longer protect any dispatch.
    fs::directory_iterator entries(registry, error);
    if (error) return true;
    for (const auto end = fs::directory_iterator{}; entries != end; entries.increment(error)) {
        if (error) return true;
        const auto& entry = *entries;
        if (entry.path().extension() != ".json") continue;
        const auto bytes = read_file(entry.path(), out_error);
        const auto reference = bytes ? decode_reference(*bytes) : std::nullopt;
        if (reference && reference->generation == generation &&
            entry.path().filename() == entry_name(*reference) &&
            kind(io_path(reference->root + ".writer")) == fs::file_type::not_found)
        {
            (void)remove_file(entry.path(), out_error);
        }
    }
    remove_empty_registry(registry, lock);
    return true;
}

} // namespace

bool close_generation(const std::string& generation, std::string* out_error)
{
    return close_generation_impl(generation, false, out_error);
}

bool settle_if_closed(const Reference& reference)
{
    const fs::path registry = io_path(reference.generation);
    Registry_lock lock;
    std::string error;
    return lock.acquire(registry, &error, platform::Lock_wait::NON_BLOCKING) &&
        matches(registry / ".settled", k_settled) &&
        matches(registry / entry_name(reference), encode_reference(reference)) &&
        release_locked(reference, registry, lock, &error);
}

void recover_closed_generations(std::string_view scratch_root)
{
    std::error_code error;
    fs::directory_iterator families(io_path(scratch_root), error);
    if (error) return;
    const fs::directory_iterator end;
    for (; families != end; families.increment(error)) {
        if (error) return;
        const auto family = families->path();
        if (kind(family) != fs::file_type::directory ||
            !sdk::is_valid_package_id(sdk::path_to_utf8(family.filename()))) continue;
        fs::directory_iterator generations(family, error);
        if (error) return;
        for (; generations != end; generations.increment(error)) {
            if (error) return;
            const auto root = generations->path();
            const std::string name = sdk::path_to_utf8(root.filename());
            constexpr std::string_view suffix = ".usage-writers";
            if (!name.ends_with(suffix) ||
                !sdk::is_valid_generation_nonce_hex(name.substr(0, name.size() - suffix.size())) ||
                kind(root) != fs::file_type::directory) continue;
            std::string ignored;
            // Use the logical path, without Windows' extended I/O prefix, so
            // registration identity remains identical to host publication.
            const auto logical = sdk::path_from_utf8(scratch_root) / family.filename() / root.filename();
            (void)close_generation_impl(normalized_path(logical), true, &ignored);
        }
    }
}

} // namespace logonomic::cli_addon_host::usage_writer_custody
