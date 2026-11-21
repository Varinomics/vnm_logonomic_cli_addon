#pragma once

#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/json.h"

#include <cstddef>
#include <optional>
#include <string>

namespace logonomic::cli_addon {

inline constexpr const char* k_managed_configuration_transform_operation = "transform";

// The base64 representation fits the SDK's one-megabyte JSON-string limit.
inline constexpr std::size_t k_max_managed_configuration_bytes = 768u * 1024u;

// managed-configuration@1 grants an adapter one bounded transform of the
// host-resolved target selected by this declaration. The adapter never sees a
// native path. @3 declares only relative_path below the frozen platform home
// and requires a verified retained preimage before a changed replacement.
struct Managed_configuration_declaration
{
    std::string environment_name;
    std::string relative_path;
    std::string default_home_relative_path;
    std::string selector_policy = {};       // @2: raw or raw-tilde
    std::string default_home_policy = {};   // @2: canonical-home or home-or-temp
    std::uint16_t version = 1;
};

Json_value encode_managed_configuration_declaration(
    const Managed_configuration_declaration& declaration,
    std::uint16_t version = 1);
std::optional<Managed_configuration_declaration>
decode_managed_configuration_declaration(
    const Json_value& payload,
    Contract_failure* failure,
    std::uint16_t version = 1);

// account-profile-managed-configuration@1 is static metadata. It associates
// a retained accounts@2 selection with one exact managed-configuration pair;
// it has no adapter operation or handshake entry.
struct Account_profile_managed_configuration_declaration
{
    std::uint16_t managed_configuration_version = 1;
};

Json_value encode_account_profile_managed_configuration_declaration(
    const Account_profile_managed_configuration_declaration& declaration);
std::optional<Account_profile_managed_configuration_declaration>
decode_account_profile_managed_configuration_declaration(
    const Json_value& payload,
    Contract_failure* failure);

struct Managed_configuration_transform_request
{
    std::string grant;       // host-minted opaque nonce, 32 lowercase hexadecimal bytes
    bool        present = false;
    std::string content;     // raw snapshot bytes, at most k_max_managed_configuration_bytes
};

struct Managed_configuration_transform_result
{
    std::string grant;       // exact echo of the request grant
    std::string content;     // raw transformed bytes, at most k_max_managed_configuration_bytes
};

Json_value encode_managed_configuration_transform_request(
    const Managed_configuration_transform_request& request);
std::optional<Managed_configuration_transform_request>
decode_managed_configuration_transform_request(
    const Json_value& payload,
    Contract_failure* failure);
Json_value encode_managed_configuration_transform_result(
    const Managed_configuration_transform_result& result);
std::optional<Managed_configuration_transform_result>
decode_managed_configuration_transform_result(
    const Json_value& payload,
    Contract_failure* failure);

} // namespace logonomic::cli_addon
