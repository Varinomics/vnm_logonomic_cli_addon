#pragma once

#include "logonomic_cli_addon/auth_status.h"
#include "logonomic_cli_addon/quota.h"

namespace logonomic::cli_addon {

/// account-refresh@1 owns one fresh native cycle. Observation completion is
/// independent of native completion: only final inspection completes the cycle.
struct Account_refresh_inspect_result
{
    bool                                complete = false;
    std::string                         protocol_state;
    std::optional<Quota_inspect_result> quota;
    std::optional<Auth_inspect_result>  auth;
};

Json_value encode_account_refresh_prepare_request(const Probe_prepare_request& request);
std::optional<Probe_prepare_request> decode_account_refresh_prepare_request(
    const Json_value& payload, Contract_failure* out_failure);
Json_value encode_account_refresh_inspect_request(const Probe_inspect_request& request);
std::optional<Probe_inspect_request> decode_account_refresh_inspect_request(
    const Json_value& payload, Contract_failure* out_failure);

/// Both optional reports have the same meaning as for the quota result.
Json_value encode_account_refresh_inspect_result(
    const Account_refresh_inspect_result& result,
    bool report_quota_failure = false,
    bool report_quota_cost = false);
std::optional<Account_refresh_inspect_result> decode_account_refresh_inspect_result(
    const Json_value& payload, bool final_inspection, Contract_failure* out_failure);

} // namespace logonomic::cli_addon
