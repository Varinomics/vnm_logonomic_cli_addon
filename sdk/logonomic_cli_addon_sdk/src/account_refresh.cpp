#include "logonomic_cli_addon/account_refresh.h"

#include "logonomic_cli_addon/limits.h"
#include "strict_json.h"

namespace logonomic::cli_addon {

Json_value encode_account_refresh_prepare_request(const Probe_prepare_request& request)
{
    return encode_probe_prepare_request(request, 2);
}

std::optional<Probe_prepare_request> decode_account_refresh_prepare_request(
    const Json_value& payload, Contract_failure* out_failure)
{
    return decode_probe_prepare_request(payload, out_failure, 2);
}

Json_value encode_account_refresh_inspect_request(const Probe_inspect_request& request)
{
    return encode_probe_inspect_request(request, 2);
}

std::optional<Probe_inspect_request> decode_account_refresh_inspect_request(
    const Json_value& payload, Contract_failure* out_failure)
{
    return decode_probe_inspect_request(payload, out_failure, 2);
}

Json_value encode_account_refresh_inspect_result(
    const Account_refresh_inspect_result& result, bool report_quota_failure, bool report_quota_cost)
{
    Json_value payload = Json_value::object();
    payload.set("complete", Json_value(result.complete));
    payload.set("protocol_state", Json_value(result.protocol_state));
    if (result.quota) {
        payload.set("quota", encode_quota_inspect_result(*result.quota, 2, report_quota_failure, report_quota_cost));
    }
    if (result.auth) {
        payload.set("auth", encode_auth_inspect_result(*result.auth));
    }
    return payload;
}

std::optional<Account_refresh_inspect_result> decode_account_refresh_inspect_result(
    const Json_value& payload, bool final_inspection, Contract_failure* out_failure)
{
    const auto fail = [out_failure](const char* detail)
        -> std::optional<Account_refresh_inspect_result>
    {
        if (out_failure) {
            out_failure->reason_code = "observation_invalid";
            out_failure->detail = detail;
        }
        return std::nullopt;
    };
    if (!payload.is_object() ||
        strict::unexpected_member(payload, {"complete", "protocol_state", "quota", "auth"}))
    {
        return fail("account refresh result has invalid members");
    }
    const auto* complete = strict::member_of_type(payload, "complete", Json_type::BOOLEAN);
    const auto* state = strict::string_member(payload, "protocol_state");
    if (!complete || complete->as_boolean() != final_inspection || !state ||
        state->size() > limits::k_max_protocol_state_bytes ||
        state->find('\0') != std::string::npos || !is_valid_utf8(*state))
    {
        return fail("account refresh completion must match native final inspection");
    }
    Account_refresh_inspect_result result;
    result.complete = final_inspection;
    result.protocol_state = *state;
    if (const auto* quota = payload.find("quota")) {
        result.quota = decode_quota_inspect_result(*quota, true, out_failure, 2);
        if (!result.quota || result.quota->stdin_bytes) {
            return fail("account refresh quota must be one completed observation");
        }
    }
    if (const auto* auth = payload.find("auth")) {
        if (!final_inspection) {
            return fail("account refresh authentication requires final native evidence");
        }
        result.auth = decode_auth_inspect_result(*auth, true, out_failure);
        if (!result.auth) {
            return std::nullopt;
        }
    }
    if (final_inspection && !result.auth) {
        return fail("final account refresh must report authentication disposition");
    }
    return result;
}

} // namespace logonomic::cli_addon
