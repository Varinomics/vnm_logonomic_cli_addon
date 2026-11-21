#pragma once

#include "logonomic_cli_addon/probe_contracts.h"

namespace logonomic::cli_addon {

/// Current login identity is presentation evidence, independent of the durable
/// profile key that owns sessions and recorded usage.
struct Current_account_observation
{
    Observation_scope scope;
    std::string       identity_key;
    bool              is_default_profile = false;
};

bool decode_current_account_declaration(const Json_value& payload, Contract_failure* out_failure);
Json_value encode_current_account_request();
bool decode_current_account_request(const Json_value& value);

Json_value encode_current_account_observation(const Current_account_observation& observation);
std::optional<Current_account_observation> decode_current_account_observation(const Json_value& value);

} // namespace logonomic::cli_addon
