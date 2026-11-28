#include "logonomic_cli_addon/authoring_availability.h"
#include "logonomic_cli_addon/limits.h"
#include "strict_json.h"

#include <utility>

namespace logonomic::cli_addon {

Json_value encode_authoring_availability_request(const Authoring_availability_request& request)
{
    auto payload = encode_accounts_launcher_request(request.launcher);
    if (request.account_context) {
        payload.set(k_account_context_member, encode_account_context(*request.account_context));
    }
    return payload;
}

std::optional<Authoring_availability_request> decode_authoring_availability_request(
    const Json_value& payload, Contract_failure* out_failure)
{
    std::optional<Account_context> account;
    if (const auto* value = payload.find(k_account_context_member)) {
        account = decode_account_context(*value, out_failure);
        if (!account) return std::nullopt;
    }
    auto context = payload;
    if (account) context.erase(k_account_context_member);
    auto launcher = decode_accounts_launcher_request(context, out_failure);
    if (!launcher) return std::nullopt;
    return Authoring_availability_request{std::move(*launcher), std::move(account)};
}

Json_value encode_authoring_availability_result(const Authoring_availability_result& result)
{
    auto payload = Json_value::object();
    payload.set("available", Json_value(result.available));
    if (!result.reason.empty()) {
        payload.set("reason", Json_value(result.reason));
    }
    if (result.account_context) {
        payload.set(k_account_context_member, encode_account_context(*result.account_context));
    }
    return payload;
}

std::optional<Authoring_availability_result> decode_authoring_availability_result(
    const Json_value& payload, const std::optional<Account_context>& selected_account, Contract_failure* out_failure)
{
    const auto* available = strict::member_of_type(payload, "available", Json_type::BOOLEAN);
    const auto* reason = strict::string_member(payload, "reason");
    const bool valid_reason = reason && reason->size() <= limits::k_max_result_message_bytes &&
        reason->find_first_not_of(' ') != std::string::npos && is_valid_utf8(*reason) &&
        strict::has_no_control_characters(*reason);
    if (!available || strict::unexpected_member(payload, {"available", "reason", k_account_context_member}) ||
        (available->as_boolean() ? payload.find("reason") != nullptr : !valid_reason)) {
        if (out_failure) *out_failure = {"frame_violation", "invalid authoring availability result or reason"};
        return std::nullopt;
    }
    if (selected_account) {
        if (const auto failure = account_context_acknowledgment_failure(*selected_account, payload)) {
            if (out_failure) *out_failure = {*failure, "the result must acknowledge the exact selected account"};
            return std::nullopt;
        }
    }
    else if (payload.find(k_account_context_member)) {
        if (out_failure) *out_failure = {"account_context_mismatch", "the request carried no account grant"};
        return std::nullopt;
    }
    return Authoring_availability_result{available->as_boolean(), reason ? *reason : std::string{}, selected_account};
}

} // namespace logonomic::cli_addon
