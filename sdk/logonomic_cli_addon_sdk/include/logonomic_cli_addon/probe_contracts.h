#pragma once

#include "logonomic_cli_addon/session_contracts.h"

namespace logonomic::cli_addon {

/// auth-status@2, quota@2/@3 and the current account-refresh wrapper reuse this
/// frozen launcher context and chunked inspection grammar.
/// Neither capability depends on structured-session or on the other probe.
struct Probe_prepare_request
{
    Session_launch_context context;
    bool                   observe_current_account = false;
};

struct Probe_inspect_request
{
    Structured_inspection          inspection;
    std::optional<Account_context> account_context;
    bool                           observe_current_account = false;
    bool                           report_quota_failure    = false;
    bool                           report_quota_cost       = false;
};

struct Observation_scope
{
    std::optional<std::string> source_account_key;
    std::uint64_t              observed_at_ms = 0;
    std::uint64_t              fresh_for_ms   = 0;
};

Json_value encode_probe_prepare_request(const Probe_prepare_request& request, std::uint16_t version = 2);
std::optional<Probe_prepare_request> decode_probe_prepare_request(
    const Json_value& payload, Contract_failure* out_failure, std::uint16_t version = 2);

Json_value encode_probe_inspect_request(const Probe_inspect_request& request, std::uint16_t version = 2);
std::optional<Probe_inspect_request> decode_probe_inspect_request(
    const Json_value& payload, Contract_failure* out_failure, std::uint16_t version = 2);

/// Scope is embedded as the required `scope` member of each observation.
Json_value encode_observation_scope(const Observation_scope& scope);
std::optional<Observation_scope> decode_observation_scope(
    const Json_value& value, Contract_failure* out_failure);

} // namespace logonomic::cli_addon
