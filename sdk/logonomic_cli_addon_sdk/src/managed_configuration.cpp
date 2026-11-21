#include "logonomic_cli_addon/managed_configuration.h"

#include "logonomic_cli_addon/capability_ids.h"
#include "logonomic_cli_addon/session_contracts.h"

#include "strict_json.h"

#include <algorithm>
#include <type_traits>
#include <vector>

namespace logonomic::cli_addon {
namespace {

bool fail(Contract_failure* failure, const char* reason, const char* detail)
{
    if (failure) *failure = {reason, detail};
    return false;
}

bool valid_relative_path(std::string_view path)
{
    if (path.empty() || path.size() > limits::k_max_package_relative_path_bytes ||
        !is_valid_utf8(path) || path.find('\0') != std::string_view::npos ||
        path.front() == '/' || path.find('\\') != std::string_view::npos)
    {
        return false;
    }
    const std::vector<std::string_view> segments = split_path_segments(path);
    if (segments.empty() || segments.size() > limits::k_max_package_path_segments) return false;
    for (const std::string_view segment : segments) {
        if (segment.empty() || segment == "." || segment == ".." ||
            segment.size() > 128 ||
            segment.back() == '.' || segment.back() == ' ')
        {
            return false;
        }
        if (std::ranges::any_of(segment, [](unsigned char byte) {
                return byte < 0x20 || byte == '<' || byte == '>' || byte == ':' ||
                    byte == '"' || byte == '|' || byte == '?' || byte == '*';
            }))
        {
            return false;
        }
    }
    return true;
}

bool valid_content(std::string_view content)
{
    return content.size() <= k_max_managed_configuration_bytes;
}

bool valid_grant(std::string_view grant)
{
    return is_valid_generation_nonce_hex(grant);
}

template <typename Value>
std::optional<Value> decode_transform(
    const Json_value& payload,
    Contract_failure* failure,
    bool request)
{
    const auto* grant   = strict::string_member(payload, "grant");
    const auto* content = strict::string_member(payload, "content_base64");
    const auto* present = request
        ? strict::member_of_type(payload, "present", Json_type::BOOLEAN)
        : nullptr;
    const std::optional<std::string> unexpected = strict::unexpected_member(
        payload, request
            ? std::initializer_list<std::string_view>{"grant", "present", "content_base64"}
            : std::initializer_list<std::string_view>{"grant", "content_base64"});
    if (!payload.is_object() || unexpected || grant == nullptr || content == nullptr ||
        (request && present == nullptr) || !valid_grant(*grant) ||
        content->size() > ((k_max_managed_configuration_bytes + 2) / 3) * 4)
    {
        fail(failure, reason::k_frame_violation, "managed configuration transform has invalid members");
        return std::nullopt;
    }
    std::optional<std::string> decoded = decode_base64(*content);
    if (!decoded || !valid_content(*decoded) ||
        (request && !present->as_boolean() && !decoded->empty()))
    {
        fail(failure, reason::k_payload_too_large, "managed configuration content is invalid or exceeds its bound");
        return std::nullopt;
    }
    if constexpr (std::is_same_v<Value, Managed_configuration_transform_request>) {
        return Value{*grant, present->as_boolean(), std::move(*decoded)};
    }
    else {
        return Value{*grant, std::move(*decoded)};
    }
}

} // namespace

Json_value encode_managed_configuration_declaration(
    const Managed_configuration_declaration& declaration, std::uint16_t version)
{
    Json_value payload = Json_value::object();
    if (version == k_managed_configuration_home_version) {
        payload.set("relative_path", Json_value(declaration.relative_path));
        return payload;
    }
    payload.set("environment_name", Json_value(declaration.environment_name));
    payload.set("relative_path", Json_value(declaration.relative_path));
    payload.set("default_home_relative_path", Json_value(declaration.default_home_relative_path));
    if (version == 2) {
        payload.set("selector_policy", Json_value(declaration.selector_policy));
        payload.set("default_home_policy", Json_value(declaration.default_home_policy));
    }
    return payload;
}

std::optional<Managed_configuration_declaration>
decode_managed_configuration_declaration(
    const Json_value& payload,
    Contract_failure* failure, std::uint16_t version)
{
    if (version == k_managed_configuration_home_version) {
        const auto* relative_path = strict::string_member(payload, "relative_path");
        if (!payload.is_object() || strict::unexpected_member(payload, {"relative_path"}) ||
            !relative_path || !valid_relative_path(*relative_path))
        {
            fail(failure, reason::k_invalid_sidecar, "managed configuration home declaration is invalid");
            return std::nullopt;
        }
        return Managed_configuration_declaration{{}, *relative_path, {}, {}, {}, version};
    }
    const auto* environment_name = strict::string_member(payload, "environment_name");
    const auto* relative_path = strict::string_member(payload, "relative_path");
    const auto* default_home_relative_path = strict::string_member(payload, "default_home_relative_path");
    const auto* selector = strict::string_member(payload, "selector_policy");
    const auto* fallback = strict::string_member(payload, "default_home_policy");
    const auto unexpected = strict::unexpected_member(payload, version == 2
        ? std::initializer_list<std::string_view>{"environment_name", "relative_path", "default_home_relative_path", "selector_policy", "default_home_policy"}
        : std::initializer_list<std::string_view>{"environment_name", "relative_path", "default_home_relative_path"});
    if ((version != 1 && version != 2) || !payload.is_object() || unexpected ||
        (version == 2 && (!selector || (*selector != "raw" && *selector != "raw-tilde") ||
            !fallback || (*fallback != "canonical-home" && *fallback != "home-or-temp"))) ||
        environment_name == nullptr || relative_path == nullptr ||
        default_home_relative_path == nullptr || !is_valid_environment_name(*environment_name) ||
        is_reserved_environment_name(*environment_name) || !valid_relative_path(*relative_path) ||
        !valid_relative_path(*default_home_relative_path))
    {
        fail(failure, reason::k_invalid_sidecar, "managed configuration declaration is invalid");
        return std::nullopt;
    }
    return Managed_configuration_declaration{
        *environment_name,
        *relative_path,
        *default_home_relative_path,
        selector ? *selector : std::string{},
        fallback ? *fallback : std::string{},
        version,
    };
}

Json_value encode_account_profile_managed_configuration_declaration(
    const Account_profile_managed_configuration_declaration& declaration)
{
    Json_value payload = Json_value::object();
    payload.set("managed_configuration_version",
        Json_value((std::int64_t)declaration.managed_configuration_version));
    return payload;
}

std::optional<Account_profile_managed_configuration_declaration>
decode_account_profile_managed_configuration_declaration(
    const Json_value& payload,
    Contract_failure* failure)
{
    if (!payload.is_object()) {
        fail(failure, reason::k_invalid_sidecar,
            "account profile managed configuration declaration is invalid");
        return std::nullopt;
    }
    const auto* version = strict::member_of_type(
        payload, "managed_configuration_version", Json_type::INTEGER);
    const auto unexpected = strict::unexpected_member(
        payload, {"managed_configuration_version"});
    if (unexpected || version == nullptr ||
        (version->as_integer() != k_managed_configuration_version &&
            version->as_integer() != k_managed_configuration_selector_version))
    {
        fail(failure, reason::k_invalid_sidecar,
            "account profile managed configuration declaration is invalid");
        return std::nullopt;
    }
    return Account_profile_managed_configuration_declaration{
        (std::uint16_t)version->as_integer()};
}

Json_value encode_managed_configuration_transform_request(
    const Managed_configuration_transform_request& request)
{
    Json_value payload = Json_value::object();
    payload.set("grant", Json_value(request.grant));
    payload.set("present", Json_value(request.present));
    payload.set("content_base64", Json_value(encode_base64(request.content)));
    return payload;
}

std::optional<Managed_configuration_transform_request>
decode_managed_configuration_transform_request(
    const Json_value& payload,
    Contract_failure* failure)
{
    return decode_transform<Managed_configuration_transform_request>(payload, failure, true);
}

Json_value encode_managed_configuration_transform_result(
    const Managed_configuration_transform_result& result)
{
    Json_value payload = Json_value::object();
    payload.set("grant", Json_value(result.grant));
    payload.set("content_base64", Json_value(encode_base64(result.content)));
    return payload;
}

std::optional<Managed_configuration_transform_result>
decode_managed_configuration_transform_result(
    const Json_value& payload,
    Contract_failure* failure)
{
    return decode_transform<Managed_configuration_transform_result>(payload, failure, false);
}

} // namespace logonomic::cli_addon
