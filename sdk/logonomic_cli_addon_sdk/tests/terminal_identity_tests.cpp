#include "sdk_test_support.h"

#include "logonomic_cli_addon/helper_ingress.h"
#include "logonomic_cli_addon/launch_preparation.h"
#include "logonomic_cli_addon/session_contracts.h"
#include "logonomic_cli_addon/terminal_identity.h"

namespace sdk_tests {
namespace {

namespace addon = logonomic::cli_addon;

} // namespace

bool test_terminal_identity_contracts()
{
    // Oracle: terminal-identity@1's opt-in, frozen context and exact proof boundary.
    addon::Contract_failure failure;
    addon::Terminal_identity_preparation preparation;
    preparation.context.set("native_home", addon::Json_value("/profiles/one/native"));
    addon::Prepare_request launch;
    launch.launcher = {"provider", addon::k_runner_direct, ""};
    launch.launcher_argv = {"/usr/bin/provider"};
    launch.working_directory = "/project";
    launch.launch_environment = addon::Launch_environment{};
    launch.launch_environment->platform_id = "linux-x86_64";
    launch.terminal_identity = addon::Terminal_identity_offer{};
    addon::Launch_patch patch;
    patch.argv = launch.launcher_argv;
    patch.disposition = addon::Launch_disposition{};
    patch.terminal_identity = preparation;
    const auto encoded_launch = addon::encode_prepare_request(launch, 3);
    const auto encoded_patch = addon::encode_launch_patch(patch, 3);
    const auto decoded_launch = addon::decode_prepare_request(encoded_launch, &failure, 3);
    const auto decoded_patch = addon::decode_launch_patch(encoded_patch, &failure, 3);
    SDK_CHECK(decoded_launch && decoded_launch->terminal_identity && decoded_patch && decoded_patch->terminal_identity &&
        decoded_patch->terminal_identity->context == preparation.context && decoded_patch->argv == patch.argv,
        "launch carries an offered companion and frozen provider context without changing its argv contract");
    launch.terminal_identity.reset();
    patch.terminal_identity.reset();
    SDK_CHECK(!addon::encode_prepare_request(launch, 3).find(addon::k_terminal_identity_member) &&
        !addon::encode_launch_patch(patch, 3).find(addon::k_terminal_identity_member),
        "ordinary peers receive no identity companion member");

    addon::Resume_prepare_request resume;
    resume.context.launcher = launch.launcher;
    resume.context.launcher_argv = launch.launcher_argv;
    resume.context.working_directory = launch.working_directory;
    resume.context.launch_environment = launch.launch_environment;
    resume.context.terminal_identity = addon::Terminal_identity_offer{};
    resume.resume_record.set("provider_session_id", addon::Json_value("thread-a"));
    addon::Resume_prepare_result resumed;
    resumed.process.executable = "helpers/provider_bridge";
    resumed.process.argv = launch.launcher_argv;
    resumed.process.stdin_behavior = addon::Stdin_behavior::INHERIT;
    resumed.patch.terminal_identity = preparation;
    resumed.acknowledgment.record_digest_hex = addon::resume_record_digest_hex(resume.resume_record);
    const auto decoded_resume = addon::decode_resume_prepare_request(
        addon::encode_resume_prepare_request(resume, 2), &failure, 2);
    const auto decoded_resumed = addon::decode_resume_prepare_result(
        addon::encode_resume_prepare_result(resumed, 2), &failure, 2);
    SDK_CHECK(decoded_resume && decoded_resume->context.terminal_identity && decoded_resumed &&
        decoded_resumed->patch.terminal_identity &&
        decoded_resumed->patch.terminal_identity->context == preparation.context,
        "exact resume prepares its own frozen title context");

    addon::Classify_event_request request;
    request.event = {"SessionStart", addon::Json_value::object()};
    request.transition_nonce = "0123456789abcdef0123456789abcdef";
    request.terminal_identity = addon::Terminal_identity_context{preparation, std::nullopt};
    auto decoded = addon::decode_classify_event_request(addon::encode_classify_event_request(request), &failure);
    SDK_CHECK(decoded && decoded->terminal_identity && !decoded->terminal_identity->terminal_event,
        "a prepared launch before its first title has explicit missing evidence");
    auto terminal_event = addon::Json_value::object();
    terminal_event.set("generation", addon::Json_value("8"));
    terminal_event.set("payload", addon::Json_value::object());
    terminal_event.find("payload")->set("title", addon::Json_value("Inspect beta | native identity"));
    request.terminal_identity->terminal_event = terminal_event;
    addon::Attention_classification_request attention{request, "pending-question"};
    const auto decoded_attention = addon::decode_attention_classification_request(
        addon::encode_attention_classification_request(attention), &failure);
    SDK_CHECK(decoded_attention && decoded_attention->pending_conversational_request_id == "pending-question" &&
        decoded_attention->base.terminal_identity &&
        decoded_attention->base.terminal_identity->terminal_event == request.terminal_identity->terminal_event,
        "ordinary attention classification retains its existing envelope beside title context");

    addon::Terminal_identity_classification_result result;
    result.base.classification = addon::Classification::TRANSITION;
    result.base.transition = addon::Transition_kind::RESUME;
    addon::Lifecycle_observation started;
    started.provider_event_name = "NativeSignal";
    started.provider_session_id = "thread-b";
    started.session_transition = addon::Session_transition::STARTED;
    started.session_source = "terminal_title";
    started.resume_record = addon::Json_value::object();
    started.resume_record->set("provider_session_id", addon::Json_value("thread-b"));
    result.base.observations = {started};
    result.terminal_identity = addon::Terminal_identity_observation{"thread-b", "Inspect beta"};
    const auto encoded_result = addon::encode_terminal_identity_classification_result(result);
    const auto decoded_result = addon::decode_terminal_identity_classification_result(encoded_result, &failure);
    SDK_CHECK(decoded_result && decoded_result->terminal_identity &&
        decoded_result->terminal_identity->title == "Inspect beta" &&
        decoded_result->base.transition == addon::Transition_kind::RESUME,
        "title selection carries exact identity through an ordinary typed lifecycle transition");
    SDK_CHECK(!addon::decode_classify_event_result(encoded_result, &failure),
        "only the explicitly selected owner companion consumes identity result metadata");
    auto mismatch = encoded_result;
    mismatch.find(addon::k_terminal_identity_member)->set("provider_session_id", addon::Json_value("thread-a"));
    SDK_CHECK(!addon::decode_terminal_identity_classification_result(mismatch, &failure),
        "identity proof cannot authorize a different transition target");
    auto missing_record = encoded_result;
    missing_record.find("observations")->as_array().front().erase("resume_record");
    SDK_CHECK(!addon::decode_terminal_identity_classification_result(missing_record, &failure),
        "a title-selected conversation must remain exactly resumable before ownership changes");
    result.base = {};
    SDK_CHECK(addon::decode_terminal_identity_classification_result(
        addon::encode_terminal_identity_classification_result(result), &failure).has_value(),
        "same-identity caption evidence is valid without opening an epoch");
    result.terminal_identity.reset();
    SDK_CHECK(addon::decode_terminal_identity_classification_result(
        addon::encode_terminal_identity_classification_result(result), &failure).has_value(),
        "unrecognized titles explicitly produce no identity proof");

    auto unknown = addon::encode_terminal_identity_preparation(preparation);
    unknown.set("version", addon::Json_value(2));
    auto oversized = addon::encode_terminal_identity_preparation(preparation);
    oversized.find("context")->set("large", addon::Json_value(
        std::string(addon::k_max_terminal_identity_context_bytes, 'x')));
    for (const auto& invalid : {unknown, oversized, addon::Json_value("bad")}) {
        auto malformed = encoded_patch;
        malformed.set(addon::k_terminal_identity_member, invalid);
        SDK_CHECK(!addon::decode_launch_patch(malformed, &failure, 3),
            "an invalid claimed title preparation cannot leave an unowned identity overlay active");
    }
    auto malformed_request = addon::encode_classify_event_request(request);
    malformed_request.find(addon::k_terminal_identity_member)->set("terminal_event", addon::Json_value("unscoped"));
    SDK_CHECK(!addon::decode_classify_event_request(malformed_request, &failure),
        "title evidence is a bounded scoped event or null");
    return true;
}

} // namespace sdk_tests
