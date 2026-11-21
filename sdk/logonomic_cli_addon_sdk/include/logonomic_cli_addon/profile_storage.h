#pragma once

#include "logonomic_cli_addon/json.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace logonomic::cli_addon {

class Invocation_context;

/**
 * @brief profile-storage@1 (docs/profile_storage_1.md): one host-owned,
 * package-scoped directory whose exact version and canonical native path
 * the adapter must echo before the host accepts a result.
 */
inline constexpr const char*   k_profile_storage_capability_id = "profile-storage";
inline constexpr std::uint16_t k_profile_storage_version       = 1;
inline constexpr const char*   k_profile_storage_member         = "profile_storage";
inline constexpr const char*   k_profile_storage_acknowledge    = "acknowledge";

struct Profile_storage_context
{
    std::uint16_t version = k_profile_storage_version;
    std::string   root;   // absolute, canonical, native separators
};

/// Pure counterpart of allocation: resolves the expected absolute root without
/// creating directories. Existing links or foreign objects in the owned suffix
/// are refused, with the same package and containment rules as allocation.
std::optional<std::string> expected_profile_storage_root(
    const std::filesystem::path& profile_storage_root,
    std::string_view             package_id,
    std::string*                 out_error,
    std::optional<std::string_view> account_key = std::nullopt);

/// Host provenance identity for the package's default profile scope. SHA-256
/// over length-delimited members (package_id, "host-profile-scope", "default"),
/// prefixed with 'a'. The discriminator is a separate member, not an adapter
/// account key; account identities must use their own discriminator domain.
std::string profile_storage_provenance_account_id(std::string_view package_id);

/// Creates `<profile_storage_root>/<package_id>/<scope>` and returns its
/// canonical path in UTF-8. Scope is `default` without an account key and the
/// accounts-owned identity digest otherwise. The package ID and lexical
/// containment are
/// validated before any directory is created. New suffix directories are
/// owner-only on POSIX; existing permissions are preserved. Windows rejects
/// reparse points and inherits the profile local-data root's ACL. Existing
/// links and non-directories in the package-owned suffix are refused.
std::optional<std::string> allocate_profile_storage_root(
    const std::filesystem::path& profile_storage_root,
    std::string_view             package_id,
    std::string*                 out_error,
    std::optional<std::string_view> account_key = std::nullopt);

/// `{"version": <version>, "root": "<root>"}`.
Json_value profile_storage_member(const Profile_storage_context& context);

/// Strictly decodes that member. The failure detail is suitable for the
/// enclosing capability's contract failure.
std::optional<Profile_storage_context> decode_profile_storage_context(
    const Json_value& value,
    std::string*      out_detail);

/// Handles a dispatched `profile-storage@1` / `acknowledge` invocation.
/// Echoes the exact decoded context or fails with `profile_storage_mismatch`.
void acknowledge_profile_storage(Invocation_context& context);

/// Exact comparison of `payload.profile_storage` against the context.
/// Returns nullopt when acknowledged, else one of
/// `profile_storage_unacknowledged`, `profile_storage_version_mismatch`,
/// `profile_storage_mismatch`.
std::optional<std::string> profile_storage_acknowledgment_failure(
    const Profile_storage_context& context,
    const Json_value&              result_payload);

} // namespace logonomic::cli_addon
