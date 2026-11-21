#include "logonomic_cli_addon/package_validator.h"
#include "logonomic_cli_addon/generation_digest.h"
#include "logonomic_cli_addon/limits.h"
#include "logonomic_cli_addon/sha256.h"

#include <algorithm>
#include <cstdint>
#include <set>

namespace logonomic::cli_addon {

namespace {

bool report(Contract_failure* out_failure, const char* reason_code, std::string detail)
{
    if (out_failure != nullptr) {
        out_failure->reason_code = reason_code;
        out_failure->detail      = std::move(detail);
    }
    return false;
}

// Maps a read failure of a listed file onto the contract's reason codes.
const char* reason_for_read_error(const Source_error& error, const char* default_reason)
{
    switch (error.kind) {
        case Source_error_kind::MISSING:     return reason::k_missing_file;
        case Source_error_kind::LINK:        return reason::k_link_entry;
        case Source_error_kind::NOT_REGULAR: return reason::k_link_entry;
        case Source_error_kind::TOO_LARGE:   return reason::k_digest_mismatch;
        case Source_error_kind::BOUND_EXCEEDED:
            return reason::k_custody_bound_exceeded;
        case Source_error_kind::INVALID_UTF8: return reason::k_invalid_utf8;
        case Source_error_kind::IO_ERROR:
        default:                             return default_reason;
    }
}

bool verify_bytes(const Index_entry& entry, const std::string& bytes, std::string* out_detail)
{
    if (bytes.size() != entry.bytes) {
        *out_detail = entry.path + " has " + std::to_string(bytes.size()) + " bytes, index records " +
                      std::to_string(entry.bytes);
        return false;
    }
    const std::string actual = to_hex(Sha256::digest(bytes));
    if (actual != entry.sha256_hex) {
        *out_detail = entry.path + " has digest " + actual + ", index records " + entry.sha256_hex;
        return false;
    }
    return true;
}

// Reads one indexed file and verifies size and digest.
bool read_verified(
    const Package_source& source,
    const Index_entry&    entry,
    std::string&          out_bytes,
    Contract_failure*     out_failure,
    const char*           mismatch_reason)
{
    Source_error read_error;
    const std::optional<std::string> bytes = source.read(entry.path, limits::k_max_file_bytes, &read_error);
    if (!bytes.has_value()) {
        return report(out_failure, reason_for_read_error(read_error, mismatch_reason), read_error.detail);
    }
    std::string detail;
    if (!verify_bytes(entry, *bytes, &detail)) {
        return report(out_failure, mismatch_reason, detail);
    }
    out_bytes = *bytes;
    return true;
}

// `capabilities/<capability_id>@<version>.json` with the pair parsed out.
std::optional<Capability_pair> sidecar_pair_from_path(std::string_view path)
{
    const std::vector<std::string_view> segments = split_path_segments(path);
    if (segments.size() != 2) {
        return std::nullopt;
    }
    const std::string_view file_name = segments[1];
    const std::string_view suffix    = ".json";
    if (file_name.size() <= suffix.size() || file_name.substr(file_name.size() - suffix.size()) != suffix) {
        return std::nullopt;
    }
    const std::string_view stem = file_name.substr(0, file_name.size() - suffix.size());
    const std::size_t at = stem.rfind('@');
    if (at == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view id     = stem.substr(0, at);
    const std::string_view digits = stem.substr(at + 1);
    if (!is_valid_capability_id(id) || digits.empty() || digits.size() > 5) {
        return std::nullopt;
    }
    if (digits.size() > 1 && digits.front() == '0') {
        return std::nullopt;
    }
    std::int64_t version = 0;
    for (const char c : digits) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        version = version * 10 + (c - '0');
    }
    if (!is_valid_version(version)) {
        return std::nullopt;
    }
    return Capability_pair{std::string(id), (std::uint16_t)version};
}

} // namespace

bool has_elf_magic(std::string_view bytes)
{
    return bytes.size() >= 4 && bytes[0] == 0x7F && bytes[1] == 'E' && bytes[2] == 'L' && bytes[3] == 'F';
}

bool is_macos_arm64_executable(std::string_view bytes)
{
    const auto word = [](std::string_view value, std::size_t offset, bool little) {
        std::uint32_t result = 0;
        for (std::size_t index = 0; index < 4; ++index) {
            const auto byte = static_cast<unsigned char>(value[offset + (little ? 3 - index : index)]);
            result = (result << 8) | byte;
        }
        return result;
    };
    const auto thin = [&](std::string_view image) {
        return image.size() >= 32 && word(image, 0, true) == 0xfeedfacf &&
            word(image, 4, true) == 0x0100000c && word(image, 12, true) == 2 &&
            word(image, 20, true) <= image.size() - 32;
    };
    if (thin(bytes)) {
        return true;
    }
    if (bytes.size() < 8) {
        return false;
    }
    const auto magic = word(bytes, 0, false);
    if (magic != 0xcafebabe && magic != 0xbebafeca) {
        return false;
    }
    const bool little = magic == 0xbebafeca;
    const auto count = word(bytes, 4, little);
    if (count == 0 || count > (bytes.size() - 8) / 20) {
        return false;
    }
    for (std::size_t index = 0; index < count; ++index) {
        const auto record = 8 + index * 20;
        if (word(bytes, record, little) != 0x0100000c) {
            continue;
        }
        const auto offset = word(bytes, record + 8, little);
        const auto size = word(bytes, record + 12, little);
        if (offset < 8 + count * 20 || offset > bytes.size() || size > bytes.size() - offset) {
            return false;
        }
        return thin(bytes.substr(offset, size));
    }
    return false;
}

std::vector<Capability_pair> Validated_package::declared_capabilities() const
{
    std::vector<Capability_pair> pairs;
    pairs.reserve(sidecars.size());
    for (const Capability_sidecar& sidecar : sidecars) {
        pairs.push_back(sidecar.pair());
    }
    return pairs;
}

Package_validator::Package_validator(Validation_options options)
:
    m_options(std::move(options))
{}

std::optional<Bootstrap_manifest> Package_validator::read_bootstrap(
    const Package_source& source,
    Contract_failure*     out_failure) const
{
    Source_error read_error;
    const std::optional<std::string> bytes = source.read(k_bootstrap_file_name, limits::k_max_bootstrap_bytes, &read_error);
    if (!bytes.has_value()) {
        const char* reason_code = read_error.kind == Source_error_kind::LINK
            ? reason::k_link_entry
            : reason::k_invalid_bootstrap;
        report(out_failure, reason_code, read_error.detail);
        return std::nullopt;
    }
    return parse_bootstrap(*bytes, out_failure);
}

std::optional<Validated_package> Package_validator::validate(
    const Package_source& source,
    Contract_failure*     out_failure) const
{
    Validated_package package;

    // 1. Bootstrap.
    {
        Source_error read_error;
        std::optional<std::string> bytes = source.read(k_bootstrap_file_name, limits::k_max_bootstrap_bytes, &read_error);
        if (!bytes.has_value()) {
            const char* reason_code = read_error.kind == Source_error_kind::LINK
                ? reason::k_link_entry
                : reason::k_invalid_bootstrap;
            report(out_failure, reason_code, read_error.detail);
            return std::nullopt;
        }
        package.bootstrap_bytes = std::move(*bytes);
    }
    {
        std::optional<Bootstrap_manifest> manifest = parse_bootstrap(package.bootstrap_bytes, out_failure);
        if (!manifest.has_value()) {
            return std::nullopt;
        }
        package.bootstrap = std::move(*manifest);
    }

    // 2. Platform and core-generation membership.
    if (!package.bootstrap.supports_platform(m_options.platform_id)) {
        report(out_failure, reason::k_platform_not_supported,
            "package does not list platform " + m_options.platform_id);
        return std::nullopt;
    }
    {
        const bool intersects = std::any_of(
            m_options.host_core_protocol_generations.begin(),
            m_options.host_core_protocol_generations.end(),
            [&package](std::uint16_t generation) { return package.bootstrap.supports_core_generation(generation); });
        if (!intersects) {
            report(out_failure, reason::k_unsupported_core_generation,
                "no core protocol generation is shared with the host");
            return std::nullopt;
        }
    }
    package.adapter_relative_path = *package.bootstrap.adapter_path_for(m_options.platform_id);

    // 3. Index.
    {
        Source_error read_error;
        std::optional<std::string> bytes = source.read(k_index_file_name, limits::k_max_index_bytes, &read_error);
        if (!bytes.has_value()) {
            const char* reason_code = reason::k_invalid_index;
            if (read_error.kind == Source_error_kind::LINK)      { reason_code = reason::k_link_entry; }
            if (read_error.kind == Source_error_kind::TOO_LARGE) { reason_code = reason::k_custody_bound_exceeded; }
            report(out_failure, reason_code,
                read_error.kind == Source_error_kind::TOO_LARGE
                    ? std::string("k_max_index_bytes exceeded by the integrity index")
                    : read_error.detail);
            return std::nullopt;
        }
        package.index_bytes = std::move(*bytes);
    }
    {
        std::optional<Integrity_index> index = parse_integrity_index(package.index_bytes, out_failure);
        if (!index.has_value()) {
            return std::nullopt;
        }
        package.index = std::move(*index);
    }

    // 4. The adapter for this platform is indexed and is a direct executable.
    const Index_entry* adapter_entry = package.index.find(package.adapter_relative_path);
    if (adapter_entry == nullptr) {
        report(out_failure, reason::k_missing_file, "adapter is not indexed: " + package.adapter_relative_path);
        return std::nullopt;
    }
    {
        std::string adapter_bytes;
        if (!read_verified(source, *adapter_entry, adapter_bytes, out_failure, reason::k_digest_mismatch)) {
            return std::nullopt;
        }
        if (m_options.platform_id == k_platform_linux_x86_64 && !has_elf_magic(adapter_bytes)) {
            report(out_failure, reason::k_shell_adapter_entry_point,
                "adapter is not an ELF executable: " + package.adapter_relative_path);
            return std::nullopt;
        }
        if (m_options.platform_id == k_platform_macos_arm64 && !is_macos_arm64_executable(adapter_bytes)) {
            report(out_failure, reason::k_shell_adapter_entry_point,
                "adapter is not an ARM64 Mach-O executable: " + package.adapter_relative_path);
            return std::nullopt;
        }
    }

    // 5. Third-party notice and provenance records, digest-bound.
    for (const Index_entry& entry : package.index.files) {
        if (entry.origin != Resource_origin::THIRD_PARTY) {
            continue;
        }
        for (const std::string* reference : {&entry.notice, &entry.provenance}) {
            const Index_entry* record = package.index.find(*reference);   // present: parse checked it
            std::string record_bytes;
            Contract_failure record_failure;
            if (!read_verified(source, *record, record_bytes, &record_failure, reason::k_third_party_record_invalid)) {
                report(out_failure, reason::k_third_party_record_invalid,
                    "record of " + entry.path + ": " + record_failure.detail);
                return std::nullopt;
            }
        }
    }

    // 6. Every regular file is indexed, every indexed file exists, no links.
    {
        Source_error list_error;
        const std::optional<std::vector<Source_entry>> entries = source.list(&list_error);
        if (!entries.has_value()) {
            const char* reason_code = list_error.kind == Source_error_kind::BOUND_EXCEEDED
                ? reason::k_custody_bound_exceeded
                : reason::k_invalid_index;
            report(out_failure, reason_code,
                "cannot list the package directory: " + list_error.detail);
            return std::nullopt;
        }
        std::set<std::string> present_files;
        for (const Source_entry& entry : *entries) {
            switch (entry.kind) {
                case Source_entry_kind::DIRECTORY:
                    break;
                case Source_entry_kind::REGULAR_FILE:
                    if (entry.relative_path == k_bootstrap_file_name || entry.relative_path == k_index_file_name) {
                        break;
                    }
                    if (package.index.find(entry.relative_path) == nullptr) {
                        report(out_failure, reason::k_unindexed_file, "file is not indexed: " + entry.relative_path);
                        return std::nullopt;
                    }
                    present_files.insert(entry.relative_path);
                    break;
                case Source_entry_kind::LINK:
                case Source_entry_kind::OTHER:
                default:
                    report(out_failure, reason::k_link_entry,
                        "entry is a link, device or unreadable name: " + entry.relative_path);
                    return std::nullopt;
            }
        }
        for (const Index_entry& entry : package.index.files) {
            if (present_files.count(entry.path) == 0) {
                report(out_failure, reason::k_missing_file, "indexed file is missing: " + entry.path);
                return std::nullopt;
            }
        }
    }

    // 7. Size and digest of every indexed file; sidecar bytes are kept.
    std::vector<std::pair<const Index_entry*, std::string>> sidecar_bytes;
    for (const Index_entry& entry : package.index.files) {
        std::string bytes;
        if (!read_verified(source, entry, bytes, out_failure, reason::k_digest_mismatch)) {
            return std::nullopt;
        }
        if (entry.kind == Resource_kind::SIDECAR) {
            sidecar_bytes.emplace_back(&entry, std::move(bytes));
        }
    }

    // 8. Sidecars.
    if (sidecar_bytes.size() > limits::k_max_sidecars_per_package) {
        report(out_failure, reason::k_custody_bound_exceeded, "k_max_sidecars_per_package exceeded");
        return std::nullopt;
    }
    for (const auto& [entry, bytes] : sidecar_bytes) {
        const std::optional<Capability_pair> named = sidecar_pair_from_path(entry->path);
        if (!named.has_value()) {
            report(out_failure, reason::k_sidecar_name_mismatch,
                "sidecar file name is not <capability_id>@<version>.json: " + entry->path);
            return std::nullopt;
        }
        Contract_failure sidecar_failure;
        std::optional<Capability_sidecar> sidecar = parse_sidecar(bytes, &sidecar_failure);
        if (!sidecar.has_value()) {
            report(out_failure, sidecar_failure.reason_code.c_str(), entry->path + ": " + sidecar_failure.detail);
            return std::nullopt;
        }
        if (sidecar->pair() != *named) {
            report(out_failure, reason::k_sidecar_name_mismatch,
                "sidecar declares " + sidecar_file_name(sidecar->capability_id, sidecar->version) +
                " but is named " + entry->path);
            return std::nullopt;
        }
        package.sidecars.push_back(std::move(*sidecar));
    }

    // 9. Generation digest over the bytes as read.
    package.total_bytes           = package.index.total_bytes();
    package.generation_digest_hex = compute_generation_digest_hex(package.bootstrap_bytes, package.index_bytes, package.index);
    return package;
}

} // namespace logonomic::cli_addon
