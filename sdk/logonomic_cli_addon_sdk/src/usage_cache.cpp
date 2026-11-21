#include "logonomic_cli_addon/usage_scan.h"
#include "logonomic_cli_addon/package_source.h"
#include "logonomic_cli_addon/usage_incremental.h"
#include "logonomic_cli_addon/sha256.h"
#include "usage_cache_p.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>

namespace logonomic::cli_addon {
namespace {

namespace fs = std::filesystem;
constexpr const char* k_manifest = "cache-manifest.json";

void require(bool condition)
{
    if (!condition) {
        throw std::runtime_error("usage_cache_invalid");
    }
}

class Cache_content_error : public std::runtime_error
{
public:
    Cache_content_error() : std::runtime_error("usage_cache_content_mismatch") {}
};

void require_cache_content(bool condition)
{
    if (!condition) {
        throw Cache_content_error{};
    }
}

void check_cancel(const std::function<bool()>& cancelled)
{
    if (cancelled()) {
        throw std::runtime_error("cancelled");
    }
}

#ifdef _WIN32
std::wstring windows_io_path(const fs::path& path)
{
    auto spelling = fs::path(path).make_preferred().native();
    if (spelling.starts_with(L"\\\\?\\")) {
        return spelling;
    }
    return spelling.starts_with(L"\\\\")
        ? L"\\\\?\\UNC\\" + spelling.substr(2)
        : L"\\\\?\\" + spelling;
}
#endif

void reject_link(const fs::path& path)
{
    require(!fs::is_symlink(fs::symlink_status(usage_cache_io_path(path))));
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(windows_io_path(path).c_str());
    require(attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT));
#endif
}

fs::path checked_root(const std::string& root, const std::string& profile)
{
    const auto path = path_from_utf8(root);
    const auto scope = path_from_utf8(profile);
    require(path.is_absolute() && scope.is_absolute() && path == path.lexically_normal());
    const auto relative = path.lexically_relative(scope);
    require(!relative.empty() && relative != "." && !relative.is_absolute());
    for (const auto& component : relative) {
        require(component != "..");
    }
    auto prefix = path.root_path();
    for (const auto& component : path.relative_path()) {
        prefix /= component;
        reject_link(prefix);
        require(fs::is_directory(usage_cache_io_path(prefix)));
    }
    return usage_cache_io_path(path);
}

void sync_path(const fs::path& path, bool directory)
{
#ifdef _WIN32
    if (directory) {
        return; // The seal is installed with a write-through move.
    }
    const HANDLE handle = CreateFileW(windows_io_path(path).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    require(handle != INVALID_HANDLE_VALUE);
    const bool flushed = FlushFileBuffers(handle) != 0;
    const bool closed = CloseHandle(handle) != 0;
#else
    const int descriptor = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | (directory ? O_DIRECTORY : 0));
    require(descriptor >= 0);
    const bool flushed = fsync(descriptor) == 0;
    const bool closed = close(descriptor) == 0;
#endif
    require(flushed && closed);
}

Usage_cache_file inspect_file(
    const fs::path& path, const std::function<bool()>& cancelled, bool durable)
{
    reject_link(path);
    require(fs::is_regular_file(path));
    const auto expected = fs::file_size(path);
    require(expected <= (std::uint64_t)std::numeric_limits<std::int64_t>::max());
    std::ifstream input(path, std::ios::binary);
    require(input.good());
    Sha256 hash;
    std::uint64_t bytes = 0;
    std::array<char, 65536> buffer{};
    while (input.read(buffer.data(), buffer.size()) || input.gcount() > 0) {
        check_cancel(cancelled);
        const auto count = (std::size_t)input.gcount();
        require(count <= expected - bytes);
        bytes += count;
        hash.update(buffer.data(), count);
    }
    require(!input.bad() && bytes == expected);
    if (durable) {
        sync_path(path, false);
    }
    return {path_to_utf8(path.filename()), bytes, to_hex(hash.finish())};
}

std::vector<Usage_cache_file> inventory(
    const fs::path& root, const std::function<bool()>& cancelled, bool durable,
    const fs::path& baseline_root = {}, const Usage_cache_manifest* baseline = nullptr)
{
    std::vector<Usage_cache_file> files;
    std::uint64_t bytes = 0;
    for (const auto& entry : fs::directory_iterator(root)) {
        check_cancel(cancelled);
        if (entry.path().filename() == k_manifest) {
            continue;
        }
        std::optional<Usage_cache_file> inherited;
        if (baseline) {
            const auto name = path_to_utf8(entry.path().filename());
            const auto found = std::lower_bound(baseline->files.begin(), baseline->files.end(), name,
                [](const Usage_cache_file& file, const std::string& path) { return file.path < path; });
            if (found != baseline->files.end() && found->path == name) {
                reject_link(entry.path());
                const auto previous = baseline_root / entry.path().filename();
                reject_link(previous);
                require(fs::is_regular_file(entry.path()) && fs::is_regular_file(previous));
                if (fs::equivalent(entry.path(), previous)) {
                    require_cache_content(fs::file_size(entry.path()) == found->bytes);
                    inherited = *found;
                }
            }
        }
        auto file = inherited ? std::move(*inherited) : inspect_file(entry.path(), cancelled, durable);
        require(file.bytes <= std::numeric_limits<std::uint64_t>::max() - bytes);
        bytes += file.bytes;
        files.push_back(std::move(file));
    }
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.path < b.path; });
    return files;
}

class Cache_manifest_reader
{
public:
    Cache_manifest_reader(const fs::path& path, const std::function<bool()>& cancelled)
    :
        m_cancelled(cancelled)
    {
        reject_link(path);
        require(fs::is_regular_file(path));
        m_expected_size = fs::file_size(path);
        m_input.open(path, std::ios::binary);
        require(m_input.good());
    }

    Usage_cache_manifest read(std::string* out_digest)
    {
        Json_value header = Json_value::object();
        std::vector<Usage_cache_file> files;
        std::set<std::string> names;
        std::uint64_t total = 0;
        consume('{');
        for (;;) {
            const auto name = value();
            require_cache_content(name.is_string() && names.insert(name.as_string()).second);
            const auto& key = name.as_string();
            require_cache_content(key == "version" || key == "account_context" || key == "snapshot_id" ||
                key == "format" || key == "files");
            consume(':');
            if (key == "files") {
                consume('[');
                whitespace();
                if (peek() != ']') {
                    for (;;) {
                        auto file = decode_usage_cache_file(value(), nullptr);
                        require_cache_content(file.has_value());
                        require_cache_content(files.empty() || files.back().path < file->path);
                        require_cache_content(file->bytes <= std::numeric_limits<std::uint64_t>::max() - total);
                        total += file->bytes;
                        files.push_back(std::move(*file));
                        whitespace();
                        if (peek() != ',') {
                            break;
                        }
                        consume(',');
                    }
                }
                consume(']');
                header.set(key, Json_value::array());
            }
            else {
                header.set(key, value());
            }
            whitespace();
            if (peek() != ',') {
                break;
            }
            consume(',');
        }
        consume('}');
        whitespace();
        require_cache_content(peek() == -1);
        auto manifest = decode_usage_cache_manifest(header, nullptr);
        require_cache_content(manifest.has_value());
        manifest->files = std::move(files);
        *out_digest = to_hex(m_hash.finish());
        return std::move(*manifest);
    }

private:
    int peek()
    {
        if (m_offset == m_size) {
            check_cancel(m_cancelled);
            m_input.read(m_buffer.data(), m_buffer.size());
            require(!m_input.bad());
            m_size = (std::size_t)m_input.gcount();
            m_offset = 0;
            require(m_size <= m_expected_size - m_received_size);
            m_received_size += m_size;
            require(!m_input.fail() || (m_input.eof() && m_received_size == m_expected_size));
            if (m_size == 0) {
                require(m_input.eof() && m_received_size == m_expected_size);
                return -1;
            }
            m_hash.update(m_buffer.data(), m_size);
        }
        return (unsigned char)m_buffer[m_offset];
    }

    int take()
    {
        const int next = peek();
        require_cache_content(next != -1);
        ++m_offset;
        return next;
    }

    static bool is_whitespace(int next) { return next == ' ' || next == '\t' || next == '\n' || next == '\r'; }

    void whitespace()
    {
        while (is_whitespace(peek())) {
            take();
        }
    }

    void consume(char expected)
    {
        whitespace();
        require_cache_content(take() == expected);
    }

    Json_value value()
    {
        whitespace();
        std::string bytes;
        std::size_t depth = 0;
        bool quoted = false;
        bool escaped = false;
        // Only frame one bounded value here. The shared strict parser below owns
        // JSON syntax, duplicate keys, Unicode, numeric ranges and nesting rules.
        for (;;) {
            const int next = peek();
            if (!quoted && depth == 0 && (next == -1 || is_whitespace(next) ||
                    next == ',' || next == ':' || next == ']' || next == '}'))
            {
                break;
            }
            require_cache_content(bytes.size() < limits::k_max_frame_bytes);
            const char character = (char)take();
            bytes.push_back(character);
            if (quoted) {
                if (escaped) {
                    escaped = false;
                }
                else
                if (character == '\\') {
                    escaped = true;
                }
                else
                if (character == '"') {
                    quoted = false;
                    if (depth == 0) {
                        break;
                    }
                }
            }
            else
            if (character == '"') {
                quoted = true;
            }
            else
            if (character == '{' || character == '[') {
                require_cache_content(++depth <= limits::k_max_json_depth);
            }
            else
            if (character == '}' || character == ']') {
                if (--depth == 0) {
                    break;
                }
            }
        }
        auto parsed = parse_json(bytes, Json_parse_limits{}, nullptr);
        require_cache_content(parsed.has_value());
        return std::move(*parsed);
    }

    const std::function<bool()>& m_cancelled;
    std::ifstream m_input;
    std::array<char, 65536> m_buffer{};
    std::size_t m_offset = 0;
    std::size_t m_size = 0;
    std::uint64_t m_expected_size = 0;
    std::uint64_t m_received_size = 0;
    Sha256 m_hash;
};

std::string write_manifest(
    const fs::path& path, const Usage_cache_manifest& manifest, const std::function<bool()>& cancelled)
{
    const auto header = encode_usage_cache_manifest(
        {manifest.account_context, manifest.snapshot_id, manifest.format, {}});
    require(decode_usage_cache_manifest(header, nullptr).has_value());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(output.good());
    Sha256 hash;
    const auto write = [&](std::string_view bytes) {
        check_cancel(cancelled);
        output.write(bytes.data(), (std::streamsize)bytes.size());
        require(output.good());
        hash.update(bytes.data(), bytes.size());
    };
    write("{");
    bool first_member = true;
    for (const auto& member : header.as_object()) {
        if (!first_member) {
            write(",");
        }
        first_member = false;
        write(serialize_json(Json_value(member.name)));
        write(":");
        if (member.name == "files") {
            write("[");
            bool first_file = true;
            for (const auto& file : manifest.files) {
                const auto value = encode_usage_cache_file(file);
                require(decode_usage_cache_file(value, nullptr).has_value());
                if (!first_file) {
                    write(",");
                }
                first_file = false;
                write(serialize_json(value));
            }
            write("]");
        }
        else {
            write(serialize_json(member.value));
        }
    }
    write("}");
    output.close();
    require(output.good());
    return to_hex(hash.finish());
}

Json_value account_value(const std::optional<Account_context>& account)
{
    return account ? encode_account_context(*account) : Json_value();
}

void validate_inventory(
    const fs::path& root, const Usage_cache_manifest& manifest,
    const std::function<bool()>& cancelled,
    const fs::path& baseline_root = {}, const Usage_cache_manifest* baseline = nullptr)
{
    const auto files = inventory(root, cancelled, false, baseline_root, baseline);
    check_cancel(cancelled);
    require_cache_content(files.size() == manifest.files.size());
    for (std::size_t i = 0; i < files.size(); ++i) {
        require_cache_content(files[i].path == manifest.files[i].path && files[i].bytes == manifest.files[i].bytes &&
            files[i].sha256_hex == manifest.files[i].sha256_hex);
    }
    check_cancel(cancelled);
}

std::string seal_manifest(
    const fs::path& root, const Usage_cache_manifest& manifest, const std::function<bool()>& cancelled)
{
    const auto pending = root / "cache-manifest.pending";
    require(!fs::exists(fs::symlink_status(pending)));
    const auto digest = write_manifest(pending, manifest, cancelled);
    sync_path(pending, false);
    check_cancel(cancelled);
#ifdef _WIN32
    require(MoveFileExW(windows_io_path(pending).c_str(), windows_io_path(root / k_manifest).c_str(),
        MOVEFILE_WRITE_THROUGH) != 0);
#else
    fs::rename(pending, root / k_manifest);
#endif
    sync_path(root, true);
    sync_path(root.parent_path(), true);
    return digest;
}

void failure(Contract_failure* output, const std::exception& error)
{
    if (output) {
        output->reason_code = std::string_view(error.what()) == "cancelled" ? "cancelled" :
            dynamic_cast<const Cache_content_error*>(&error)
                ? "usage_cache_content_mismatch" : "usage_cache_unavailable";
        output->detail = "The selected usage cache or candidate could not be validated or durably sealed.";
    }
}

} // namespace

fs::path usage_cache_io_path(const fs::path& path)
{
#ifdef _WIN32
    return windows_io_path(path);
#else
    return path;
#endif
}

bool validate_usage_cache_root(
    const std::string& root, const std::string& profile_root, Contract_failure* out_failure)
{
    try {
        (void)checked_root(root, profile_root);
        return true;
    }
    catch (const std::exception& error) {
        failure(out_failure, error);
        return false;
    }
}

bool validate_usage_cache_allocation(
    const std::string& candidate_root, const std::string& profile_root, Contract_failure* out_failure)
{
    try {
        require(fs::is_empty(checked_root(candidate_root, profile_root)));
        return true;
    }
    catch (const std::exception& error) {
        failure(out_failure, error);
        return false;
    }
}

std::optional<Usage_cache_manifest> validate_usage_cache_manifest(
    const Usage_cache_reference& reference,
    const std::string& profile_root,
    const std::optional<Account_context>& account,
    const std::function<bool()>& cancelled,
    Contract_failure* out_failure)
{
    try {
        check_cancel(cancelled);
        require(decode_usage_cache_reference(encode_usage_cache_reference(reference), out_failure).has_value());
        const auto root = checked_root(reference.root, profile_root);
        std::string digest;
        auto manifest = Cache_manifest_reader(root / k_manifest, cancelled).read(&digest);
        const auto content_mismatch = [&]() -> std::optional<Usage_cache_manifest> {
            if (out_failure) {
                out_failure->reason_code = "usage_cache_content_mismatch";
                out_failure->detail = "The selected usage cache contents no longer match its sealed inventory.";
            }
            return std::nullopt;
        };
        if (digest != reference.manifest_sha256_hex) {
            return content_mismatch();
        }
        if (manifest.snapshot_id != reference.snapshot_id ||
            account_value(manifest.account_context) != account_value(account))
        {
            return content_mismatch();
        }
        check_cancel(cancelled);
        return manifest;
    }
    catch (const std::exception& error) {
        failure(out_failure, error);
        return std::nullopt;
    }
}

bool validate_usage_cache_file(
    const Usage_cache_reference& reference,
    const Usage_cache_manifest& manifest,
    const std::string& relative_path,
    const std::function<bool()>& cancelled,
    Contract_failure* out_failure)
{
    try {
        check_cancel(cancelled);
        const auto name = path_from_utf8(relative_path);
        require(name.has_filename() && name == name.filename() && name != "." && name != "..");
        require(manifest.snapshot_id == reference.snapshot_id);
        const auto expected = std::find_if(manifest.files.begin(), manifest.files.end(),
            [&](const Usage_cache_file& file) { return file.path == relative_path; });
        require(expected != manifest.files.end());
        const auto actual = inspect_file(usage_cache_io_path(path_from_utf8(reference.root)) / name, cancelled, false);
        require_cache_content(actual.bytes == expected->bytes && actual.sha256_hex == expected->sha256_hex);
        return true;
    }
    catch (const std::exception& error) {
        failure(out_failure, error);
        return false;
    }
}

std::optional<Usage_cache_manifest> validate_usage_cache_candidate(
    const Usage_cache_reference& reference,
    const std::string& profile_root,
    const std::optional<Account_context>& account,
    const std::function<bool()>& cancelled,
    Contract_failure* out_failure)
{
    auto manifest = validate_usage_cache_manifest(reference, profile_root, account, cancelled, out_failure);
    if (!manifest) return std::nullopt;
    try {
        validate_inventory(usage_cache_io_path(path_from_utf8(reference.root)), *manifest, cancelled);
        return manifest;
    }
    catch (const std::exception& error) {
        failure(out_failure, error);
        return std::nullopt;
    }
}

std::optional<Usage_cache_reference> seal_usage_cache_candidate(
    const std::string& candidate_root,
    const std::string& profile_root,
    const std::optional<Account_context>& account,
    const std::string& snapshot_id,
    const std::string& format,
    const std::function<bool()>& cancelled,
    Contract_failure* out_failure)
{
    try {
        check_cancel(cancelled);
        const auto root = checked_root(candidate_root, profile_root);
        require(!fs::exists(fs::symlink_status(root / k_manifest)));
        Usage_cache_manifest manifest{account, snapshot_id, format, inventory(root, cancelled, true)};
        const auto digest = seal_manifest(root, manifest, cancelled);
        return Usage_cache_reference{candidate_root, snapshot_id, digest};
    }
    catch (const std::exception& error) {
        failure(out_failure, error);
        return std::nullopt;
    }
}

bool inherit_usage_cache_file(
    const Usage_cache_reference& baseline,
    const Usage_cache_manifest& baseline_manifest,
    const std::string& relative_path,
    const std::string& candidate_root,
    const std::string& profile_root,
    const std::function<bool()>& cancelled,
    Contract_failure* out_failure)
{
    try {
        check_cancel(cancelled);
        const auto source_root = checked_root(baseline.root, profile_root);
        const auto destination_root = checked_root(candidate_root, profile_root);
        require(source_root != destination_root && baseline_manifest.snapshot_id == baseline.snapshot_id);
        const auto name = path_from_utf8(relative_path);
        require(name.has_filename() && name == name.filename() && name != "." && name != "..");
        const auto found = std::lower_bound(baseline_manifest.files.begin(), baseline_manifest.files.end(), relative_path,
            [](const Usage_cache_file& file, const std::string& path) { return file.path < path; });
        require(found != baseline_manifest.files.end() && found->path == relative_path);
        const auto source = source_root / name;
        const auto destination = destination_root / name;
        reject_link(source);
        require(fs::is_regular_file(source) && !fs::exists(fs::symlink_status(destination)));
        require_cache_content(fs::file_size(source) == found->bytes);
        std::error_code link_error;
        fs::create_hard_link(source, destination, link_error);
        if (link_error) {
            require(fs::copy_file(source, destination, fs::copy_options::none));
            const auto copied = inspect_file(destination, cancelled, false);
            require_cache_content(copied.bytes == found->bytes && copied.sha256_hex == found->sha256_hex);
        }
        check_cancel(cancelled);
        return true;
    }
    catch (const std::exception& error) {
        failure(out_failure, error);
        return false;
    }
}

std::optional<Usage_cache_reference> seal_usage_incremental_cache_candidate(
    const std::string& candidate_root,
    const std::string& profile_root,
    const std::optional<Account_context>& account,
    const std::string& snapshot_id,
    const std::string& format,
    const Usage_cache_reference& baseline,
    const std::function<bool()>& cancelled,
    Contract_failure* out_failure)
{
    const auto accepted = validate_usage_cache_manifest(baseline, profile_root, account, cancelled, out_failure);
    if (!accepted) return std::nullopt;
    try {
        check_cancel(cancelled);
        const auto root = checked_root(candidate_root, profile_root);
        const auto baseline_root = checked_root(baseline.root, profile_root);
        require(root != baseline_root && !fs::exists(fs::symlink_status(root / k_manifest)));
        Usage_cache_manifest manifest{account, snapshot_id, format,
            inventory(root, cancelled, true, baseline_root, &*accepted)};
        const auto digest = seal_manifest(root, manifest, cancelled);
        return Usage_cache_reference{candidate_root, snapshot_id, digest};
    }
    catch (const std::exception& error) {
        failure(out_failure, error);
        return std::nullopt;
    }
}

std::optional<Usage_cache_manifest> validate_usage_incremental_cache_candidate(
    const Usage_cache_reference& candidate,
    const Usage_cache_reference& baseline,
    const std::string& profile_root,
    const std::optional<Account_context>& account,
    const std::function<bool()>& cancelled,
    Contract_failure* out_failure)
{
    const auto accepted = validate_usage_cache_manifest(baseline, profile_root, account, cancelled, out_failure);
    if (!accepted) return std::nullopt;
    auto manifest = validate_usage_cache_manifest(candidate, profile_root, account, cancelled, out_failure);
    if (!manifest) return std::nullopt;
    try {
        const auto root = checked_root(candidate.root, profile_root);
        const auto baseline_root = checked_root(baseline.root, profile_root);
        require(root != baseline_root);
        validate_inventory(root, *manifest, cancelled, baseline_root, &*accepted);
        return manifest;
    }
    catch (const std::exception& error) {
        failure(out_failure, error);
        return std::nullopt;
    }
}

} // namespace logonomic::cli_addon
