#include "logonomic_cli_addon/session_wake.h"

#include "logonomic_cli_addon/capability_ids.h"
#include "logonomic_cli_addon/limits.h"

#include "strict_json.h"

#include <initializer_list>
#include <string_view>
#include <utility>

namespace logonomic::cli_addon {
namespace {

constexpr strict::Name_entry<Session_wake_disposition> k_dispositions[] = {
    {Session_wake_disposition::ACCEPTED,    "accepted"},
    {Session_wake_disposition::DEFERRED,    "deferred"},
    {Session_wake_disposition::UNAVAILABLE, "unavailable"},
    {Session_wake_disposition::UNCERTAIN,   "uncertain"},
};

bool fail(Contract_failure* failure, const char* code, const char* detail)
{
    if (failure != nullptr) {
        *failure = {code, detail};
    }
    return false;
}

bool object_members(const Json_value& value, std::initializer_list<std::string_view> members,
    Contract_failure* failure)
{
    if (!value.is_object()) {
        return fail(failure, reason::k_frame_violation, "session wake value is not an object");
    }
    if (strict::unexpected_member(value, members)) {
        return fail(failure, reason::k_unexpected_member, "session wake object has an unexpected member");
    }
    return true;
}

bool record_version(const Json_value& value, Contract_failure* failure)
{
    const auto version = strict::integer_member(value, "version", 1, 65535);
    if (!version) {
        return fail(failure, reason::k_frame_violation, "session wake record needs a positive version");
    }
    if (*version != k_session_wake_version) {
        return fail(failure, reason::k_unknown_capability, "session wake record version is unavailable");
    }
    return true;
}

bool text_member(const Json_value& value, const char* name, std::size_t maximum,
    bool allow_empty, std::string& result, Contract_failure* failure)
{
    const auto* text = strict::string_member(value, name);
    if (text == nullptr || (!allow_empty && text->empty()) || text->find('\0') != std::string::npos) {
        return fail(failure, reason::k_frame_violation, "session wake text is missing, empty or contains NUL");
    }
    if (text->size() > maximum) {
        return fail(failure, reason::k_payload_too_large, "session wake text exceeds its bound");
    }
    if (!is_valid_utf8(*text)) {
        return fail(failure, reason::k_invalid_utf8, "session wake text is not UTF-8");
    }
    result = *text;
    return true;
}

bool read_reason(const Json_value& value, std::string& result, Contract_failure* failure)
{
    const auto* code = strict::string_member(value, "reason");
    if (code == nullptr || !is_valid_reason_code(*code)) {
        return fail(failure, reason::k_frame_violation, "session wake refusal needs a reason code");
    }
    result = *code;
    return true;
}

bool read_binding(const Json_value& value, Json_value& binding, Contract_failure* failure)
{
    const auto* object = strict::member_of_type(value, "binding", Json_type::OBJECT);
    if (object == nullptr) {
        return fail(failure, reason::k_frame_violation, "session wake binding is not an object");
    }
    if (serialize_json_canonical(*object).size() > k_max_session_wake_binding_bytes) {
        return fail(failure, reason::k_payload_too_large, "session wake binding exceeds its bound");
    }
    binding = *object;
    return true;
}

bool read_availability(const Json_value& value, bool& available, std::string& refusal,
    Contract_failure* failure)
{
    const auto* flag = strict::member_of_type(value, "available", Json_type::BOOLEAN);
    if (flag == nullptr) {
        return fail(failure, reason::k_frame_violation, "session wake availability is missing");
    }
    available = flag->as_boolean();
    if (!available) {
        return read_reason(value, refusal, failure);
    }
    if (value.find("reason") != nullptr) {
        return fail(failure, reason::k_frame_violation, "available session wake cannot carry a refusal");
    }
    return true;
}

Json_value encode_target(const Session_wake_target& target)
{
    Json_value argv = Json_value::array();
    for (const auto& argument : target.launcher_argv) {
        argv.push_back(Json_value(argument));
    }
    Json_value session = Json_value::object();
    session.set("provider_session_id", Json_value(target.provider_session_id));
    session.set("working_directory",   Json_value(target.working_directory));
    session.set("launcher_argv",       std::move(argv));
    session.set("launch_environment",  encode_launch_environment(target.launch_environment));
    Json_value correlation = Json_value::object();
    correlation.set("version",          Json_value(1));
    correlation.set("transition_nonce", Json_value(target.correlation.transition_nonce));
    Json_value payload = Json_value::object();
    payload.set("session",     std::move(session));
    payload.set("correlation", std::move(correlation));
    payload.set("binding",     target.binding);
    return payload;
}

std::optional<Session_wake_target> decode_target(const Json_value& payload, Contract_failure* failure)
{
    const auto* session     = payload.find("session");
    const auto* correlation = payload.find("correlation");
    if (session == nullptr || correlation == nullptr) {
        fail(failure, reason::k_frame_violation, "session wake target or correlation is missing");
        return std::nullopt;
    }
    if (!object_members(*session,
            {"provider_session_id", "working_directory", "launcher_argv", "launch_environment"}, failure) ||
        !object_members(*correlation, {"version", "transition_nonce"}, failure))
    {
        return std::nullopt;
    }
    Session_wake_target target;
    if (!text_member(*session, "provider_session_id", k_max_session_wake_identity_bytes,
            false, target.provider_session_id, failure) ||
        !text_member(*session, "working_directory", limits::k_max_working_directory_bytes,
            false, target.working_directory, failure) ||
        !text_member(*correlation, "transition_nonce", limits::k_transition_nonce_hex_chars,
            false, target.correlation.transition_nonce, failure) ||
        !read_binding(payload, target.binding, failure))
    {
        return std::nullopt;
    }
    if (!strict::integer_member(*correlation, "version", 1, 1) ||
        !is_valid_transition_nonce_hex(target.correlation.transition_nonce))
    {
        fail(failure, reason::k_frame_violation, "session wake correlation is invalid");
        return std::nullopt;
    }
    const auto* environment = session->find("launch_environment");
    if (environment == nullptr) {
        fail(failure, reason::k_frame_violation, "session wake launch environment is missing");
        return std::nullopt;
    }
    auto decoded_environment = decode_launch_environment(*environment, failure);
    if (!decoded_environment) {
        return std::nullopt;
    }
    target.launch_environment = std::move(*decoded_environment);
    const auto* argv = strict::member_of_type(*session, "launcher_argv", Json_type::ARRAY);
    if (argv == nullptr || argv->as_array().empty()) {
        fail(failure, reason::k_frame_violation, "session wake argv is missing or empty");
        return std::nullopt;
    }
    if (argv->as_array().size() > limits::k_max_argv_entries) {
        fail(failure, reason::k_payload_too_large, "session wake argv has too many entries");
        return std::nullopt;
    }
    std::size_t bytes = 0;
    for (const auto& argument : argv->as_array()) {
        if (!argument.is_string() || argument.as_string().find('\0') != std::string::npos ||
            (target.launcher_argv.empty() && argument.as_string().empty()))
        {
            fail(failure, reason::k_frame_violation, "session wake argv entry is invalid");
            return std::nullopt;
        }
        const auto& text = argument.as_string();
        bytes += text.size();
        if (text.size() > limits::k_max_argv_entry_bytes || bytes > limits::k_max_argv_total_bytes) {
            fail(failure, reason::k_payload_too_large, "session wake argv exceeds its bound");
            return std::nullopt;
        }
        if (!is_valid_utf8(text)) {
            fail(failure, reason::k_invalid_utf8, "session wake argv is not UTF-8");
            return std::nullopt;
        }
        target.launcher_argv.push_back(text);
    }
    return target;
}

} // namespace

std::optional<Session_wake_declaration> decode_session_wake_declaration(
    const Json_value& payload, Contract_failure* out_failure)
{
    return decode_launcher_discovery_declaration(payload, out_failure);
}

bool validate_session_wake_declaration(const Json_value& payload, Contract_failure* out_failure)
{
    return decode_session_wake_declaration(payload, out_failure).has_value();
}

Json_value encode_session_wake_offer(const Session_wake_offer&)
{
    Json_value result = Json_value::object();
    result.set("version", Json_value(k_session_wake_version));
    return result;
}

std::optional<Session_wake_offer> decode_session_wake_offer(
    const Json_value& payload, Contract_failure* out_failure)
{
    if (!record_version(payload, out_failure) || !object_members(payload, {"version"}, out_failure)) {
        return std::nullopt;
    }
    return Session_wake_offer{};
}

Json_value encode_session_wake_preparation(const Session_wake_preparation& preparation)
{
    Json_value payload = encode_session_wake_offer();
    payload.set("available", Json_value(preparation.available));
    if (preparation.available) {
        payload.set("binding", preparation.binding);
    }
    else {
        payload.set("reason", Json_value(preparation.reason));
    }
    return payload;
}

std::optional<Session_wake_preparation> decode_session_wake_preparation(
    const Json_value& payload, Contract_failure* out_failure)
{
    if (!record_version(payload, out_failure) ||
        !object_members(payload, {"version", "available", "binding", "reason"}, out_failure))
    {
        return std::nullopt;
    }
    Session_wake_preparation result;
    if (!read_availability(payload, result.available, result.reason, out_failure)) {
        return std::nullopt;
    }
    if (result.available) {
        if (!read_binding(payload, result.binding, out_failure)) {
            return std::nullopt;
        }
    }
    else
    if (payload.find("binding") != nullptr) {
        fail(out_failure, reason::k_frame_violation, "unavailable session wake cannot carry a binding");
        return std::nullopt;
    }
    return result;
}

const char* session_wake_disposition_name(Session_wake_disposition disposition)
{
    return strict::name_of(k_dispositions, disposition);
}

Json_value encode_session_wake_inspect_request(const Session_wake_inspect_request& request)
{
    return encode_target(request.target);
}

std::optional<Session_wake_inspect_request> decode_session_wake_inspect_request(
    const Json_value& payload, Contract_failure* out_failure)
{
    if (!object_members(payload, {"session", "correlation", "binding"}, out_failure)) {
        return std::nullopt;
    }
    auto target = decode_target(payload, out_failure);
    if (!target) {
        return std::nullopt;
    }
    return Session_wake_inspect_request{std::move(*target)};
}

Json_value encode_session_wake_readiness(const Session_wake_readiness& readiness)
{
    Json_value payload = Json_value::object();
    payload.set("available", Json_value(readiness.available));
    if (!readiness.available) {
        payload.set("reason", Json_value(readiness.reason));
    }
    return payload;
}

std::optional<Session_wake_readiness> decode_session_wake_readiness(
    const Json_value& payload, Contract_failure* out_failure)
{
    if (!object_members(payload, {"available", "reason"}, out_failure)) {
        return std::nullopt;
    }
    Session_wake_readiness result;
    if (!read_availability(payload, result.available, result.reason, out_failure)) {
        return std::nullopt;
    }
    return result;
}

Json_value encode_session_wake_notify_request(const Session_wake_notify_request& request)
{
    Json_value payload = encode_target(request.target);
    payload.set("notification_id", Json_value(request.notification_id));
    payload.set("instruction",     Json_value(request.instruction));
    return payload;
}

std::optional<Session_wake_notify_request> decode_session_wake_notify_request(
    const Json_value& payload, Contract_failure* out_failure)
{
    if (!object_members(payload,
            {"session", "correlation", "binding", "notification_id", "instruction"}, out_failure))
    {
        return std::nullopt;
    }
    auto target = decode_target(payload, out_failure);
    if (!target) {
        return std::nullopt;
    }
    Session_wake_notify_request result;
    result.target = std::move(*target);
    if (!text_member(payload, "notification_id", k_max_session_wake_identity_bytes,
            false, result.notification_id, out_failure) ||
        !text_member(payload, "instruction", k_max_session_wake_instruction_bytes,
            false, result.instruction, out_failure))
    {
        return std::nullopt;
    }
    return result;
}

Json_value encode_session_wake_notify_result(const Session_wake_notify_result& result)
{
    Json_value payload = Json_value::object();
    payload.set("disposition", Json_value(session_wake_disposition_name(result.disposition)));
    if (result.disposition == Session_wake_disposition::ACCEPTED) {
        if (result.receipt) {
            payload.set("receipt", Json_value(*result.receipt));
        }
    }
    else {
        payload.set("reason", Json_value(result.reason));
    }
    return payload;
}

std::optional<Session_wake_notify_result> decode_session_wake_notify_result(
    const Json_value& payload, Contract_failure* out_failure)
{
    if (!object_members(payload, {"disposition", "reason", "receipt"}, out_failure)) {
        return std::nullopt;
    }
    const auto* name = strict::string_member(payload, "disposition");
    const auto disposition = name == nullptr ? std::nullopt : strict::value_of(k_dispositions, *name);
    if (!disposition) {
        fail(out_failure, reason::k_frame_violation, "session wake disposition is invalid");
        return std::nullopt;
    }
    Session_wake_notify_result result;
    result.disposition = *disposition;
    if (result.disposition == Session_wake_disposition::ACCEPTED) {
        if (payload.find("reason") != nullptr) {
            fail(out_failure, reason::k_frame_violation, "accepted notification cannot carry a refusal");
            return std::nullopt;
        }
        if (payload.find("receipt") != nullptr) {
            std::string receipt;
            if (!text_member(payload, "receipt", k_max_session_wake_identity_bytes, false, receipt, out_failure)) {
                return std::nullopt;
            }
            result.receipt = std::move(receipt);
        }
    }
    else {
        if (!read_reason(payload, result.reason, out_failure)) {
            return std::nullopt;
        }
        if (payload.find("receipt") != nullptr) {
            fail(out_failure, reason::k_frame_violation, "unaccepted notification cannot carry a receipt");
            return std::nullopt;
        }
    }
    return result;
}

} // namespace logonomic::cli_addon
