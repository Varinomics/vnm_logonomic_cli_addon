#include "logonomic_cli_addon/launcher_approval.h"

#include "logonomic_cli_addon/adapter_runtime.h"
#include "logonomic_cli_addon/limits.h"
#include "strict_json.h"

#include <initializer_list>
#include <string_view>
#include <utility>

namespace logonomic::cli_addon {
namespace {

bool fail(Contract_failure* failure, const char* reason_code, std::string detail)
{
    if (failure != nullptr) {
        *failure = {reason_code, std::move(detail)};
    }
    return false;
}

bool closed_object(const Json_value& value, std::initializer_list<std::string_view> members,
    Contract_failure* failure)
{
    if (!value.is_object()) {
        return fail(failure, reason::k_frame_violation, "launcher approval payload is not an object");
    }
    if (const auto unexpected = strict::unexpected_member(value, members)) {
        return fail(failure, reason::k_unexpected_member, "launcher approval." + *unexpected);
    }
    return true;
}

bool bounded_text(const std::string& text, std::size_t maximum)
{
    return !text.empty() && text.size() <= maximum &&
        text.find('\0') == std::string::npos && is_valid_utf8(text);
}

std::optional<Launcher_approval_comparison_request> request_fields(
    const Json_value& payload, Contract_failure* failure)
{
    const auto* launcher_value = payload.find("launcher");
    const auto* platform       = strict::string_member(payload, "platform");
    if (launcher_value == nullptr || platform == nullptr || !is_valid_platform_id(*platform)) {
        fail(failure, reason::k_frame_violation, "launcher approval requires a launcher and platform");
        return std::nullopt;
    }
    const auto launcher = decode_launcher_request(*launcher_value, failure);
    if (!launcher) {
        return std::nullopt;
    }
    return Launcher_approval_comparison_request{*launcher, *platform};
}

Json_value encode_request(const Launcher_request& launcher, const std::string& platform)
{
    Json_value payload = Json_value::object();
    payload.set("launcher", encode_launcher_request(launcher));
    payload.set("platform", Json_value(platform));
    return payload;
}

} // namespace

bool validate_launcher_approval_declaration(const Json_value& payload, Contract_failure* failure)
{
    return (payload.is_object() && payload.as_object().empty()) ||
        fail(failure, reason::k_invalid_sidecar, "launcher-approval payload must be an empty object");
}

Json_value encode_launcher_approval_transform_request(const Launcher_approval_transform_request& request)
{
    Json_value payload = encode_request(request.launcher, request.platform_id);
    payload.set("action", Json_value(request.action == Launcher_approval_action::YOLOFY ? "yolofy" : "unyolofy"));
    return payload;
}

std::optional<Launcher_approval_transform_request> decode_launcher_approval_transform_request(
    const Json_value& payload, Contract_failure* failure)
{
    if (!closed_object(payload, {"launcher", "platform", "action"}, failure)) {
        return std::nullopt;
    }
    const auto common  = request_fields(payload, failure);
    const auto* action = strict::string_member(payload, "action");
    if (!common) {
        return std::nullopt;
    }
    if (action == nullptr || (*action != "yolofy" && *action != "unyolofy")) {
        fail(failure, reason::k_frame_violation, "launcher approval action is not yolofy or unyolofy");
        return std::nullopt;
    }
    return Launcher_approval_transform_request{common->launcher, common->platform_id,
        *action == "yolofy" ? Launcher_approval_action::YOLOFY : Launcher_approval_action::UNYOLOFY};
}

Json_value encode_launcher_approval_transform_result(const Launcher_approval_transform_result& result)
{
    Json_value payload = Json_value::object();
    if (result.disposition == Launcher_approval_disposition::UNSUPPORTED) {
        payload.set("disposition", Json_value("unsupported"));
    }
    else {
        payload.set("disposition", Json_value(
            result.disposition == Launcher_approval_disposition::CHANGED ? "changed" : "unchanged"));
        payload.set("script_body", Json_value(result.script_body));
    }
    return payload;
}

std::optional<Launcher_approval_transform_result> decode_launcher_approval_transform_result(
    const Json_value& payload, Contract_failure* failure)
{
    if (!closed_object(payload, {"disposition", "script_body"}, failure)) {
        return std::nullopt;
    }
    const auto* disposition = strict::string_member(payload, "disposition");
    if (disposition != nullptr && *disposition == "unsupported" && payload.find("script_body") == nullptr) {
        return Launcher_approval_transform_result{};
    }
    const auto* script = strict::string_member(payload, "script_body");
    if (disposition == nullptr || (*disposition != "changed" && *disposition != "unchanged") ||
        script == nullptr || !bounded_text(*script, limits::k_max_script_body_bytes))
    {
        fail(failure, reason::k_frame_violation, "launcher approval result has an invalid disposition or script");
        return std::nullopt;
    }
    return Launcher_approval_transform_result{
        *disposition == "changed" ? Launcher_approval_disposition::CHANGED : Launcher_approval_disposition::UNCHANGED,
        *script};
}

Json_value encode_launcher_approval_comparison_request(const Launcher_approval_comparison_request& request)
{
    return encode_request(request.launcher, request.platform_id);
}

std::optional<Launcher_approval_comparison_request> decode_launcher_approval_comparison_request(
    const Json_value& payload, Contract_failure* failure)
{
    if (!closed_object(payload, {"launcher", "platform"}, failure)) {
        return std::nullopt;
    }
    return request_fields(payload, failure);
}

Json_value encode_launcher_approval_comparison_result(const Launcher_approval_comparison_result& result)
{
    Json_value payload = Json_value::object();
    payload.set("supported", Json_value(result.supported));
    if (result.supported) {
        payload.set("key", Json_value(result.key));
    }
    return payload;
}

std::optional<Launcher_approval_comparison_result> decode_launcher_approval_comparison_result(
    const Json_value& payload, Contract_failure* failure)
{
    if (!closed_object(payload, {"supported", "key"}, failure)) {
        return std::nullopt;
    }
    const auto* supported = strict::member_of_type(payload, "supported", Json_type::BOOLEAN);
    const auto* key       = strict::string_member(payload, "key");
    if (supported != nullptr && !supported->as_boolean() && payload.find("key") == nullptr) {
        return Launcher_approval_comparison_result{};
    }
    if (supported == nullptr || !supported->as_boolean() || key == nullptr ||
        !bounded_text(*key, k_max_launcher_approval_key_bytes) || !strict::has_no_control_characters(*key))
    {
        fail(failure, reason::k_frame_violation, "launcher approval comparison result is invalid");
        return std::nullopt;
    }
    return Launcher_approval_comparison_result{true, *key};
}

void handle_launcher_approval(Invocation_context& context, const Launcher_approval_rules& rules)
{
    if (context.cancel_requested()) {
        context.fail(Result_status::CANCELLED, "cancelled");
        return;
    }
    Contract_failure failure;
    if (context.request().operation == k_launcher_approval_transform_operation) {
        const auto request = decode_launcher_approval_transform_request(context.request().payload, &failure);
        if (request) {
            context.succeed(encode_launcher_approval_transform_result(transform_launcher_approval(*request, rules)));
            return;
        }
    }
    else
    if (context.request().operation == k_launcher_approval_comparison_operation) {
        const auto request = decode_launcher_approval_comparison_request(context.request().payload, &failure);
        if (request) {
            context.succeed(encode_launcher_approval_comparison_result(
                launcher_approval_comparison_key(*request, rules)));
            return;
        }
    }
    else {
        context.reject_unknown_operation();
        return;
    }
    context.fail(Result_status::FAILED, failure.reason_code, failure.detail);
}

} // namespace logonomic::cli_addon
