#include "sdk_test_support.h"

#include "logonomic_cli_addon/capability_ids.h"
#include "logonomic_cli_addon/launch_preparation.h"
#include "logonomic_cli_addon/session_contracts.h"
#include "logonomic_cli_addon/session_wake.h"
#include "logonomic_cli_addon/transcript.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sdk_tests {
namespace {

namespace addon = logonomic::cli_addon;

addon::Session_wake_target target()
{
    addon::Session_wake_target result;
    result.provider_session_id = "native-thread-51";
    result.working_directory   = "/srv/project";
    result.launcher_argv       = {"/usr/bin/provider", "--interactive"};
    result.launch_environment.platform_id = "linux-x86_64";
    result.launch_environment.effective_environment.set("HOME", addon::Json_value("/home/zoe"));
    result.correlation.transition_nonce = "7f3c9a2e5b1d4f607f3c9a2e5b1d4f60";
    result.binding.set("registration_id", addon::Json_value("private-launch-context"));
    return result;
}

} // namespace

bool test_session_wake_contracts()
{
    // Oracle: session-wake@1's native admission and optional capability boundary.
    addon::Contract_failure failure;
    const auto bound = target();
    const addon::Session_wake_preparation prepared{true, bound.binding, {}};
    addon::Prepare_request launch;
    launch.launcher          = {"provider", addon::k_runner_direct, ""};
    launch.launcher_argv     = bound.launcher_argv;
    launch.working_directory = bound.working_directory;
    launch.launch_environment = bound.launch_environment;
    launch.session_wake      = addon::Session_wake_offer{};
    addon::Launch_patch patch;
    patch.argv         = bound.launcher_argv;
    patch.disposition  = addon::Launch_disposition{};
    patch.session_wake = prepared;
    const auto encoded_launch = addon::encode_prepare_request(launch, 3);
    const auto encoded_patch  = addon::encode_launch_patch(patch, 3);
    const auto decoded_launch = addon::decode_prepare_request(encoded_launch, &failure, 3);
    const auto decoded_patch  = addon::decode_launch_patch(encoded_patch, &failure, 3);
    SDK_CHECK(decoded_launch && decoded_launch->session_wake && decoded_patch && decoded_patch->session_wake &&
        decoded_patch->session_wake->available && decoded_patch->argv == patch.argv,
        "interactive preparation carries the optional wake offer and live binding without changing launch behavior");

    auto unknown = addon::encode_session_wake_offer();
    unknown.set("version", addon::Json_value(7));
    for (const auto& unusable : {unknown, addon::Json_value("malformed"), addon::Json_value::object()}) {
        auto request = encoded_launch;
        auto result  = encoded_patch;
        request.set(addon::k_session_wake_member, unusable);
        result.set(addon::k_session_wake_member, unusable);
        const auto common_request = addon::decode_prepare_request(request, &failure, 3);
        const auto common_result  = addon::decode_launch_patch(result, &failure, 3);
        SDK_CHECK(common_request && !common_request->session_wake && common_result && !common_result->session_wake &&
            common_result->argv == patch.argv,
            "unknown or malformed wake records disable only wake, preserving ordinary launch interpretation");
    }

    addon::Resume_prepare_request resume;
    resume.context.launcher          = launch.launcher;
    resume.context.launcher_argv     = bound.launcher_argv;
    resume.context.working_directory = bound.working_directory;
    resume.context.launch_environment = bound.launch_environment;
    resume.context.session_wake      = addon::Session_wake_offer{};
    resume.resume_record.set("provider_session_id", addon::Json_value(bound.provider_session_id));
    addon::Resume_prepare_result resumed;
    resumed.process.executable     = "helpers/provider_bridge";
    resumed.process.argv           = bound.launcher_argv;
    resumed.process.stdin_behavior = addon::Stdin_behavior::INHERIT;
    resumed.patch.session_wake     = prepared;
    resumed.acknowledgment.record_digest_hex = addon::resume_record_digest_hex(resume.resume_record);
    {
        constexpr std::uint16_t version = addon::k_interactive_resume_environment_version;
        const auto request = addon::decode_resume_prepare_request(
            addon::encode_resume_prepare_request(resume, version), &failure, version);
        auto encoded = addon::encode_resume_prepare_result(resumed, version);
        const auto result = addon::decode_resume_prepare_result(encoded, &failure, version);
        SDK_CHECK(request && request->context.session_wake && result && result->patch.session_wake &&
            result->process.argv == resumed.process.argv,
            "resume obtains a new wake binding without changing the common resume schema");
        encoded.set(addon::k_session_wake_member, unknown);
        const auto core = addon::decode_resume_prepare_result(encoded, &failure, version);
        SDK_CHECK(core && !core->patch.session_wake && core->process.argv == resumed.process.argv,
            "unknown wake preparation does not prevent ordinary resume");
    }

    const addon::Session_wake_notify_request notify{bound, "notification-51", "Logonomic: read your mailbox."};
    const auto encoded_notify = addon::encode_session_wake_notify_request(notify);
    const auto decoded_notify = addon::decode_session_wake_notify_request(encoded_notify, &failure);
    SDK_CHECK(decoded_notify && decoded_notify->target.provider_session_id == bound.provider_session_id &&
        decoded_notify->target.correlation.transition_nonce == bound.correlation.transition_nonce &&
        decoded_notify->notification_id == notify.notification_id && decoded_notify->instruction == notify.instruction,
        "a notification preserves native identity, authenticated correlation and exact host instruction");
    const auto inspect = addon::decode_session_wake_inspect_request(
        addon::encode_session_wake_inspect_request({bound}), &failure);
    SDK_CHECK(inspect && inspect->target.launcher_argv == bound.launcher_argv,
        "readiness sees the same frozen launch target as notification");
    for (const char* member : {"session", "correlation", "binding", "notification_id", "instruction"}) {
        auto malformed = encoded_notify;
        malformed.erase(member);
        SDK_CHECK(!addon::decode_session_wake_notify_request(malformed, &failure),
            "notification requires every target, correlation and content member");
    }
    auto malformed = encoded_notify;
    malformed.find("correlation")->set("transition_nonce", addon::Json_value("unbound"));
    SDK_CHECK(!addon::decode_session_wake_notify_request(malformed, &failure),
        "native thread identity cannot substitute for authenticated transition correlation");
    malformed = encoded_notify;
    malformed.set("instruction", addon::Json_value(std::string(addon::k_max_session_wake_instruction_bytes + 1, 'x')));
    SDK_CHECK(!addon::decode_session_wake_notify_request(malformed, &failure) &&
        failure.reason_code == addon::reason::k_payload_too_large,
        "notification text is bounded independently of the common protocol frame");
    malformed = encoded_notify;
    malformed.find("binding")->set("unbounded", addon::Json_value(std::string(addon::k_max_session_wake_binding_bytes, 'x')));
    SDK_CHECK(!addon::decode_session_wake_notify_request(malformed, &failure) &&
        failure.reason_code == addon::reason::k_payload_too_large,
        "opaque launch binding is bounded independently of its provider structure");

    for (const auto disposition : {addon::Session_wake_disposition::ACCEPTED,
            addon::Session_wake_disposition::DEFERRED, addon::Session_wake_disposition::UNAVAILABLE,
            addon::Session_wake_disposition::UNCERTAIN})
    {
        addon::Session_wake_notify_result result;
        result.disposition = disposition;
        if (disposition == addon::Session_wake_disposition::ACCEPTED) {
            result.receipt = "native-queue-51";
        }
        else {
            result.reason = "native_state_changed";
        }
        const auto decoded = addon::decode_session_wake_notify_result(addon::encode_session_wake_notify_result(result), &failure);
        SDK_CHECK(decoded && decoded->disposition == disposition && decoded->receipt == result.receipt,
            "native acceptance, deferral, unavailability and uncertain acceptance remain distinct");
    }
    auto rejected = addon::encode_session_wake_notify_result(
        {addon::Session_wake_disposition::DEFERRED, "paused", std::nullopt});
    rejected.set("receipt", addon::Json_value("would-imply-admission"));
    SDK_CHECK(!addon::decode_session_wake_notify_result(rejected, &failure),
        "a deferred notification cannot claim a native admission receipt");
    const auto unavailable = addon::decode_session_wake_readiness(
        addon::encode_session_wake_readiness({false, "unbound_session"}), &failure);
    SDK_CHECK(unavailable && !unavailable->available && unavailable->reason == "unbound_session",
        "unbound session readiness remains an explicit capability-local reason");
    auto contradictory = addon::encode_session_wake_readiness({true, {}});
    contradictory.set("reason", addon::Json_value("paused"));
    SDK_CHECK(!addon::decode_session_wake_readiness(contradictory, &failure),
        "readiness cannot simultaneously report available and refused");

    addon::Json_value declaration = addon::Json_value::object();
    declaration.set("environment_names", addon::Json_value::array());
    SDK_CHECK(addon::validate_session_wake_declaration(declaration, &failure),
        "native bridges may need no additional environment inputs");
    declaration.find("environment_names")->push_back(addon::Json_value("LOGONOMIC_SESSION_CAPABILITY"));
    SDK_CHECK(!addon::validate_session_wake_declaration(declaration, &failure),
        "the wake declaration cannot request reserved host authority");

    addon::Transcript_recorder transcript;
    addon::Json_value invocation = addon::Json_value::object();
    invocation.set("capability_id", addon::Json_value(addon::k_session_wake_capability_id));
    invocation.set("payload", encoded_notify);
    transcript.record_host_frame(invocation);
    transcript.record_adapter_frame(encoded_patch);
    SDK_CHECK(transcript.text().find("private-launch-context") == std::string::npos &&
        transcript.text().find(notify.instruction) == std::string::npos &&
        transcript.text().find("<session-wake-private>") != std::string::npos,
        "diagnostic transcripts retain protocol structure without private wake binding or notification content");

    addon::Session_wake_preparation private_preparation = prepared;
    const std::string launch_token = "birth-token-7f3c9a2e5b1d4f60";
    private_preparation.binding.set("launch_token", addon::Json_value(launch_token));
    private_preparation.binding.set("short_value",  addon::Json_value("/"));
    private_preparation.binding.set("json_value",   addon::Json_value("true"));
    private_preparation.binding.set("common_word",  addon::Json_value("success"));
    addon::Json_value nested = addon::Json_value::object();
    nested.set("private", private_preparation.binding);
    nested.set("enabled", addon::Json_value(true));
    nested.set("other_path", addon::Json_value("/srv/unrelated"));
    addon::Launch_patch private_patch = patch;
    private_patch.session_wake = private_preparation;
    private_patch.set.emplace_back("PROVIDER_WAKE_BINDING", addon::serialize_json(nested));
    private_patch.set.emplace_back("PROVIDER_WAKE_TOKEN", launch_token);
    addon::Json_value response = addon::Json_value::object();
    response.set("type",       addon::Json_value("result"));
    response.set("request_id", addon::Json_value("r51"));
    response.set("status",     addon::Json_value("success"));
    response.set("payload",    addon::encode_launch_patch(private_patch, 3));
    transcript.record_adapter_frame(response);
    const std::string normalized = transcript.lines().back().substr(2);
    const auto frame = addon::parse_json(normalized, {}, nullptr);
    SDK_CHECK(frame && frame->find("type")->as_string() == "result" &&
        frame->find("status")->as_string() == "success" && normalized.find(launch_token) == std::string::npos,
        "duplicated wake values are removed without changing protocol envelope fields");
    const auto* environment = frame->find("payload")->find("environment")->find("set");
    const auto nested_result = addon::parse_json(environment->find("PROVIDER_WAKE_BINDING")->as_string(), {}, nullptr);
    SDK_CHECK(nested_result && nested_result->find("enabled")->as_boolean() &&
        nested_result->find("other_path")->as_string() == "/srv/unrelated" &&
        nested_result->find("private")->find("launch_token")->as_string() == "<session-wake-private>" &&
        environment->find("PROVIDER_WAKE_TOKEN")->as_string() == "<session-wake-private>",
        "JSON-valued environment redaction preserves structure, typed booleans and unrelated paths");
    return true;
}

} // namespace sdk_tests
