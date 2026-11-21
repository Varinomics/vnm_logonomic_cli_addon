#include "logonomic_cli_addon/lifecycle_correlation_lifetime.h"

#include "logonomic_cli_addon/capability_ids.h"

#include "strict_json.h"

#include <algorithm>

namespace logonomic::cli_addon {
namespace {

bool fail(Contract_failure* out_failure, const char* reason_code, const char* detail)
{
    if (out_failure != nullptr) {
        *out_failure = {reason_code, detail};
    }
    return false;
}

bool valid_cursor(const std::string& cursor)
{
    return cursor.size() <= limits::k_max_observe_cursor_bytes && cursor.find('\0') == std::string::npos;
}

bool helper_ingress_live(const std::vector<Capability_pair>& live_pairs)
{
    for (const std::uint16_t version : {
             k_helper_ingress_degradation_version,
             k_helper_ingress_subagent_reconciliation_version,
         })
    {
        if (std::find(live_pairs.begin(), live_pairs.end(),
                Capability_pair{k_helper_ingress_capability_id, version}) != live_pairs.end())
        {
            return true;
        }
    }
    return false;
}

bool read_nonce(
    const Json_value& payload,
    Session_correlation& correlation,
    Contract_failure* out_failure,
    const char* context)
{
    const std::string* nonce = strict::string_member(payload, "transition_nonce");
    if (nonce == nullptr || !is_valid_transition_nonce_hex(*nonce)) {
        return fail(out_failure, reason::k_frame_violation, context);
    }
    correlation.transition_nonce = *nonce;
    return true;
}

const char* disposition_name(Lifecycle_correlation_retire_disposition disposition)
{
    switch (disposition) {
        case Lifecycle_correlation_retire_disposition::ABANDONED:
            return "abandoned";
        case Lifecycle_correlation_retire_disposition::TERMINAL_CONSUMED:
            return "terminal-consumed";
    }
    return "";
}

std::optional<Lifecycle_correlation_retire_disposition> parse_disposition(
    std::string_view value)
{
    if (value == "abandoned") {
        return Lifecycle_correlation_retire_disposition::ABANDONED;
    }
    if (value == "terminal-consumed") {
        return Lifecycle_correlation_retire_disposition::TERMINAL_CONSUMED;
    }
    return std::nullopt;
}

} // namespace

Json_value encode_lifecycle_correlation_reserve_request(
    const Lifecycle_correlation_reserve_request& request)
{
    Json_value payload = Json_value::object();
    payload.set("transition_nonce", Json_value(request.correlation.transition_nonce));
    return payload;
}

std::optional<Lifecycle_correlation_reserve_request>
decode_lifecycle_correlation_reserve_request(
    const Json_value& payload,
    Contract_failure* out_failure)
{
    if (!payload.is_object()) {
        fail(out_failure, reason::k_frame_violation, "reserve payload is not an object");
        return std::nullopt;
    }
    if (strict::unexpected_member(payload, {"transition_nonce"}).has_value()) {
        fail(out_failure, reason::k_unexpected_member, "reserve has an unexpected member");
        return std::nullopt;
    }
    Lifecycle_correlation_reserve_request request;
    if (!read_nonce(payload, request.correlation, out_failure, "reserve transition_nonce is invalid")) {
        return std::nullopt;
    }
    return request;
}

Json_value encode_lifecycle_correlation_retire_request(
    const Lifecycle_correlation_retire_request& request)
{
    Json_value payload = Json_value::object();
    payload.set("transition_nonce", Json_value(request.correlation.transition_nonce));
    payload.set("disposition", Json_value(disposition_name(request.disposition)));
    if (request.disposition == Lifecycle_correlation_retire_disposition::TERMINAL_CONSUMED &&
        request.consumed_cursor.has_value())
    {
        payload.set("consumed_cursor", Json_value(*request.consumed_cursor));
    }
    return payload;
}

std::optional<Lifecycle_correlation_retire_request>
decode_lifecycle_correlation_retire_request(
    const Json_value& payload,
    Contract_failure* out_failure)
{
    if (!payload.is_object()) {
        fail(out_failure, reason::k_frame_violation, "retire payload is not an object");
        return std::nullopt;
    }
    if (strict::unexpected_member(payload,
            {"transition_nonce", "disposition", "consumed_cursor"}).has_value())
    {
        fail(out_failure, reason::k_unexpected_member, "retire has an unexpected member");
        return std::nullopt;
    }
    Lifecycle_correlation_retire_request request;
    if (!read_nonce(payload, request.correlation, out_failure, "retire transition_nonce is invalid")) {
        return std::nullopt;
    }
    const std::string* disposition = strict::string_member(payload, "disposition");
    const std::optional<Lifecycle_correlation_retire_disposition> parsed =
        disposition == nullptr ? std::nullopt : parse_disposition(*disposition);
    const Json_value* cursor_value = payload.find("consumed_cursor");
    if (!parsed.has_value() ||
        (parsed == Lifecycle_correlation_retire_disposition::ABANDONED && cursor_value != nullptr) ||
        (parsed == Lifecycle_correlation_retire_disposition::TERMINAL_CONSUMED &&
            (cursor_value == nullptr || !cursor_value->is_string() ||
                !valid_cursor(cursor_value->as_string()))))
    {
        fail(out_failure, reason::k_frame_violation, "retire disposition or consumed_cursor is invalid");
        return std::nullopt;
    }
    request.disposition = *parsed;
    if (cursor_value != nullptr) {
        request.consumed_cursor = cursor_value->as_string();
    }
    return request;
}

Json_value encode_lifecycle_correlation_lifetime_ack(
    const Lifecycle_correlation_lifetime_ack&)
{
    return Json_value::object();
}

std::optional<Lifecycle_correlation_lifetime_ack>
decode_lifecycle_correlation_lifetime_ack(
    const Json_value& payload,
    Contract_failure* out_failure)
{
    if (!payload.is_object() || !payload.as_object().empty()) {
        fail(out_failure, reason::k_frame_violation,
            "lifecycle correlation lifetime acknowledgment is not empty");
        return std::nullopt;
    }
    return Lifecycle_correlation_lifetime_ack{};
}

bool validate_lifecycle_correlation_lifetime_dependency(
    const std::vector<Capability_pair>& live_pairs,
    Contract_failure* out_failure)
{
    const Capability_pair lifecycle{
        k_lifecycle_events_capability_id,
        k_lifecycle_events_version,
    };
    if (std::find(live_pairs.begin(), live_pairs.end(), lifecycle) == live_pairs.end() ||
        !helper_ingress_live(live_pairs))
    {
        return fail(out_failure, reason::k_dependency_capability_unavailable,
            "lifecycle-correlation-lifetime@1 requires helper-ingress@2/@3 and lifecycle-events@2");
    }
    return true;
}

} // namespace logonomic::cli_addon
