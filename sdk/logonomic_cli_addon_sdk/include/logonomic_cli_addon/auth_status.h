#pragma once

#include "logonomic_cli_addon/probe_contracts.h"
#include "logonomic_cli_addon/current_account.h"

namespace logonomic::cli_addon {

enum class Auth_status
{
    AUTHENTICATED,
    NOT_LOGGED_IN,
    UNKNOWN,
    UNAVAILABLE,
    FAILED,
};

struct Auth_observation
{
    Observation_scope scope;
    Auth_status       state = Auth_status::UNKNOWN;
};

struct Auth_inspect_result
{
    bool                            complete = false;
    std::string                     protocol_state;
    std::optional<Auth_observation> observation;
    std::optional<Current_account_observation> current_account = std::nullopt;
};

const char* auth_status_name(Auth_status state);

Json_value encode_auth_inspect_result(const Auth_inspect_result& result);
std::optional<Auth_inspect_result> decode_auth_inspect_result(
    const Json_value& payload, bool final_inspection, Contract_failure* out_failure);

} // namespace logonomic::cli_addon
