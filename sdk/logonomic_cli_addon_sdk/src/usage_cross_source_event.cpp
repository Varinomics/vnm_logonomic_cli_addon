#include "logonomic_cli_addon/usage_cross_source_event.h"

#include "strict_json.h"

#include <algorithm>
#include <cstddef>
#include <string_view>

namespace logonomic::cli_addon {
namespace {

constexpr std::size_t k_max_namespace_bytes = 128;
constexpr std::size_t k_max_event_id_bytes = 256;

bool fail(Contract_failure* out_failure, const char* detail)
{
    if (out_failure != nullptr) {
        *out_failure = {"usage_cross_source_event_invalid", detail};
    }
    return false;
}

bool valid_namespace(std::string_view value)
{
    return !value.empty() && value.size() <= k_max_namespace_bytes &&
        std::all_of(value.begin(), value.end(), [](char ch) {
            return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
                ch == '.' || ch == '-';
        });
}

bool valid_event_id(std::string_view value)
{
    return !value.empty() && value.size() <= k_max_event_id_bytes &&
        strict::has_no_control_characters(value) && is_valid_utf8(value);
}

} // namespace

bool decode_usage_cross_source_event_declaration(
    const Json_value& payload, Contract_failure* out_failure)
{
    if (!payload.is_object() || !payload.as_object().empty()) {
        return fail(out_failure, "usage cross-source event declaration must be an empty object");
    }
    return true;
}

Json_value encode_usage_cross_source_event(const Usage_cross_source_event& event)
{
    Json_value payload = Json_value::object();
    payload.set("version", Json_value(static_cast<int>(k_usage_cross_source_event_version)));
    payload.set("namespace", Json_value(event.authority_namespace));
    payload.set("event_id", Json_value(event.event_id));
    return payload;
}

std::optional<Usage_cross_source_event> decode_usage_cross_source_event(
    const Json_value& payload, Contract_failure* out_failure)
{
    if (!payload.is_object() || strict::unexpected_member(payload,
            {"version", "namespace", "event_id"}))
    {
        fail(out_failure, "usage cross-source event has unexpected members");
        return std::nullopt;
    }
    const auto version = strict::integer_member(payload, "version", 1, 1);
    const std::string* authority_namespace = strict::string_member(payload, "namespace");
    const std::string* event_id = strict::string_member(payload, "event_id");
    if (!version || !authority_namespace || !valid_namespace(*authority_namespace) ||
        !event_id || !valid_event_id(*event_id))
    {
        fail(out_failure, "usage cross-source event version or identity is invalid");
        return std::nullopt;
    }
    return Usage_cross_source_event{*authority_namespace, *event_id};
}

} // namespace logonomic::cli_addon
