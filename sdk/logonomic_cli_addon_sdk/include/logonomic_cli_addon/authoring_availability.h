#pragma once

#include "logonomic_cli_addon/accounts.h"

namespace logonomic::cli_addon {

inline constexpr const char* k_authoring_availability_check_operation = "check";

// The launcher supplies normalization and capability-scoped environment input.
// Only account_context denotes a host-selected account grant.
struct Authoring_availability_request
{
    Accounts_launcher_request      launcher;
    std::optional<Account_context> account_context;
};

struct Authoring_availability_result
{
    bool                           available = false;
    std::string                    reason;
    std::optional<Account_context> account_context;
};

Json_value encode_authoring_availability_request(const Authoring_availability_request& request);
std::optional<Authoring_availability_request> decode_authoring_availability_request(
    const Json_value& payload, Contract_failure* out_failure);

Json_value encode_authoring_availability_result(const Authoring_availability_result& result);
std::optional<Authoring_availability_result> decode_authoring_availability_result(
    const Json_value& payload, const std::optional<Account_context>& selected_account, Contract_failure* out_failure);

} // namespace logonomic::cli_addon
