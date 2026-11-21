#pragma once

#include "logonomic_cli_addon/capability_ids.h"
#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/json.h"
#include "logonomic_cli_addon/launcher_discovery.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace logonomic::cli_addon {

inline constexpr const char* k_accounts_discover_operation = "discover";
inline constexpr const char* k_accounts_resolve_operation  = "resolve";
inline constexpr const char* k_account_context_member      = "account_context";
inline constexpr std::uint16_t k_account_context_version = 1;

/// accounts@2 discovers and resolves against the same frozen launcher input.
struct Accounts_launcher_request
{
    Launcher_request   launcher;
    Launch_environment launch_environment;
    std::string        default_home_fallback;
    std::string        normalization_directory;
};

Json_value encode_accounts_launcher_request(const Accounts_launcher_request& request);
std::optional<Accounts_launcher_request> decode_accounts_launcher_request(
    const Json_value& payload, Contract_failure* out_failure);

enum class Account_status
{
    UNKNOWN,
    AVAILABLE,
    UNAVAILABLE,
};

struct Account_record
{
    std::string                adapter_account_key;
    std::string                display_name;
    Account_status             status = Account_status::UNKNOWN;
    std::optional<std::string> profile_path;
};

struct Account_resolution
{
    std::optional<std::string> adapter_account_key;
};

/// Accounts owns this optional member of preparation requests and results.
/// An exact echo is required so an adapter cannot silently ignore selection.
struct Account_context
{
    std::uint16_t version = k_account_context_version;
    std::string   adapter_account_key;
    std::optional<std::string> profile_path = std::nullopt;
};

bool is_valid_adapter_account_key(std::string_view key);
const char* account_status_name(Account_status status);

/// Host identity: SHA-256 of length-delimited package, "adapter-account",
/// and exact key bytes, prefixed with 'a'. Inputs must be validated first.
std::string account_identity(std::string_view package_id, std::string_view adapter_account_key);

enum class Account_profile_case_fold
{
    WINDOWS_INVARIANT,
    ASCII,
};

/// Opaque hash input for a canonical native profile. The folding convention
/// preserves provider-issued keys on ordinary Windows paths. A case-sensitive
/// Windows parent gives its children a separate identity domain.
std::optional<std::string> native_account_profile_identity(
    const std::filesystem::path& path,
    Account_profile_case_fold    fold = Account_profile_case_fold::WINDOWS_INVARIANT);

/// Rediscovery may retain a granted spelling only where the native parent
/// establishes that the differently cased component names the same location.
bool same_native_account_profile(const std::filesystem::path& granted, const std::filesystem::path& selected);

/// For providers that issued historically folded keys: a matching key alone
/// cannot prove a case-ambiguous location below a vanished directory.
bool account_profile_allows_key_only(
    const std::filesystem::path& selected,
    Account_profile_case_fold    fold = Account_profile_case_fold::WINDOWS_INVARIANT);

/// Reproduce a previously issued key's hash input only for a path-bearing
/// grant whose native location still agrees with the selection and whose
/// containing directories can be observed. The caller verifies the old key.
std::optional<std::string> historical_account_profile_identity(
    const std::filesystem::path& granted,
    const std::filesystem::path& selected,
    Account_profile_case_fold    fold = Account_profile_case_fold::WINDOWS_INVARIANT);

/// The display name of an account with a filesystem profile:
/// the name of the profile's folder, the last component of `profile_path`
/// with trailing separators and dot components ignored, or the whole path for
/// a root, fitted with fitted_display_name. Throws std::invalid_argument for
/// an empty path, which names no folder.
std::string account_display_name(std::string_view profile_path);

Json_value encode_accounts(const std::vector<Account_record>& accounts);
std::optional<std::vector<Account_record>> decode_accounts_result(
    const Json_value& payload, Contract_failure* out_failure);

Json_value encode_account_resolution(const Account_resolution& resolution);
std::optional<Account_resolution> decode_account_resolution_result(
    const Json_value& payload, Contract_failure* out_failure);

Json_value encode_account_context(const Account_context& context);
std::optional<Account_context> decode_account_context(const Json_value& value, Contract_failure* out_failure);

std::optional<std::string> account_context_acknowledgment_failure(
    const Account_context& context, const Json_value& result_payload);

} // namespace logonomic::cli_addon
