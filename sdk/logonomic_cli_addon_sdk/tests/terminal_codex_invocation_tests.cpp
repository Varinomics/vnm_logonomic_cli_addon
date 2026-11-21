#include "sdk_test_support.h"

#include "logonomic_cli_addon/capability_ids.h"
#include "logonomic_cli_addon/launch_preparation.h"
#include "logonomic_cli_addon/limits.h"
#include "logonomic_cli_addon/protocol.h"
#include "logonomic_cli_addon/session_contracts.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace sdk_tests {

namespace {

namespace addon = logonomic::cli_addon;

addon::Prepare_request interactive_request()
{
    addon::Prepare_request request;
    request.launcher          = {"codex", addon::k_runner_direct, ""};
    request.launcher_argv     = {"codex", "--", "Describe the \"quoted\" path Ã©."};
    request.working_directory = "/srv/repositories/example";
    request.launch_environment =
        addon::Launch_environment{"windows-x86_64", addon::Json_value::object(), std::nullopt};
    request.codex_invocation = addon::Codex_invocation_request{};
    return request;
}

addon::Launch_patch interactive_patch(std::size_t prefix_size)
{
    addon::Launch_patch patch;
    patch.argv = std::vector<std::string>{
        "C:\\Program Files\\nodejs\\node.exe",
        "C:\\Users\\Zoe\\AppData\\Roaming\\npm\\node_modules\\@openai\\codex\\bin\\codex.js",
        "--", "Describe the \"quoted\" path Ã©.",
    };
    patch.disposition      = addon::Launch_disposition{};
    patch.codex_invocation = addon::Codex_invocation{prefix_size};
    return patch;
}

} // namespace

bool test_terminal_codex_invocation_contracts()
{
    // Oracle: the negotiated terminal-codex-invocation@1 contract.
    addon::Contract_failure failure;
    const addon::Codex_invocation_request offer;
    const addon::Prepare_request request = interactive_request();
    const addon::Launch_patch patch      = interactive_patch(2);

    const auto request_payload = addon::encode_prepare_request(request, 3);
    const auto decoded_request = addon::decode_prepare_request(request_payload, &failure, 3);
    SDK_CHECK(decoded_request && decoded_request->codex_invocation &&
        decoded_request->launcher_argv == request.launcher_argv,
        "the offer survives launch-preparation@3 without altering any launcher argument");
    const auto patch_payload = addon::encode_launch_patch(patch, 3);
    const auto decoded_patch = addon::decode_launch_patch(patch_payload, &failure, 3);
    SDK_CHECK(decoded_patch && decoded_patch->codex_invocation &&
        decoded_patch->codex_invocation->argument_prefix_size == 2 && decoded_patch->argv == patch.argv &&
        addon::validate_codex_invocation_acknowledgment(offer, *decoded_patch, *decoded_patch->argv, &failure),
        "the verified Node and JS prefix survives without changing prompt bytes or the argument boundary");

    addon::Prepare_request ordinary = request;
    ordinary.codex_invocation.reset();
    {
        constexpr std::uint16_t version = addon::k_launch_preparation_execution_version;
        const auto payload = addon::encode_prepare_request(ordinary, version);
        const auto decoded = addon::decode_prepare_request(payload, &failure, version);
        SDK_CHECK(!payload.find(addon::k_codex_invocation_member) && decoded && !decoded->codex_invocation,
            "an ordinary request carries no Codex adaptation offer in the current launch contract");
    }
    addon::Launch_patch ordinary_patch;
    SDK_CHECK(addon::validate_codex_invocation_acknowledgment(std::nullopt, ordinary_patch, {}, &failure),
        "an ordinary launch does not need capability acknowledgment");
    SDK_CHECK(!addon::validate_codex_invocation_acknowledgment(std::nullopt, patch, *patch.argv, &failure) &&
        failure.reason_code == addon::reason::k_launch_patch_invalid,
        "an unsolicited semantic invocation marker is refused");
    SDK_CHECK(!addon::validate_codex_invocation_acknowledgment(offer, ordinary_patch, *patch.argv, &failure) &&
        failure.reason_code == addon::reason::k_unknown_capability,
        "an offered capability cannot silently succeed without acknowledgment");
    addon::Launch_patch unprepared = patch;
    unprepared.disposition->prepared = false;
    SDK_CHECK(!addon::validate_codex_invocation_acknowledgment(offer, unprepared, *patch.argv, &failure),
        "an unprepared launch cannot acknowledge a verified invocation");
    addon::Launch_patch unreplaced = patch;
    unreplaced.argv.reset();
    SDK_CHECK(!addon::validate_codex_invocation_acknowledgment(offer, unreplaced, *patch.argv, &failure),
        "interactive acknowledgment needs its replacement argv");
    for (const std::size_t size : {std::size_t{0}, patch.argv->size() + 1}) {
        addon::Launch_patch invalid = patch;
        invalid.codex_invocation = addon::Codex_invocation{size};
        SDK_CHECK(!addon::validate_codex_invocation_acknowledgment(offer, invalid, *patch.argv, &failure) &&
            failure.reason_code == addon::reason::k_launch_patch_invalid,
            "the argument prefix must be nonempty and within the actual command");
    }
    const std::vector<std::string> native_argv{"C:\\Tools\\codex.exe"};
    addon::Launch_patch native_patch = patch;
    native_patch.argv             = native_argv;
    native_patch.codex_invocation = addon::Codex_invocation{1};
    SDK_CHECK(addon::validate_codex_invocation_acknowledgment(offer, native_patch, native_argv, &failure),
        "a native executable may occupy the entire argument vector");

    for (const addon::Json_value& malformed : {
            addon::Json_value(), addon::Json_value("codex"), addon::Json_value::object(),
        })
    {
        SDK_CHECK(!addon::decode_codex_invocation_request(malformed, &failure),
            "a request marker needs an object and its version");
        SDK_CHECK(!addon::decode_codex_invocation(malformed, &failure),
            "a result marker needs an object and its version");
    }
    auto malformed_request = addon::encode_codex_invocation_request(offer);
    malformed_request.set("entrypoint", addon::Json_value("codex.js"));
    SDK_CHECK(!addon::decode_codex_invocation_request(malformed_request, &failure) &&
        failure.reason_code == addon::reason::k_unexpected_member,
        "the known request record has a closed member set");
    for (const addon::Json_value& size : {
            addon::Json_value(0), addon::Json_value(-1), addon::Json_value("2"), addon::Json_value(1.5),
            addon::Json_value((std::int64_t)addon::limits::k_max_argv_entries + 1),
        })
    {
        auto malformed_result = addon::encode_codex_invocation(addon::Codex_invocation{2});
        malformed_result.set("argument_prefix_size", size);
        SDK_CHECK(!addon::decode_codex_invocation(malformed_result, &failure) &&
            failure.reason_code == addon::reason::k_launch_patch_invalid,
            "the prefix count is a bounded positive integer");
    }
    auto malformed_result = addon::encode_codex_invocation(addon::Codex_invocation{2});
    malformed_result.set("provider", addon::Json_value("codex"));
    SDK_CHECK(!addon::decode_codex_invocation(malformed_result, &failure),
        "the known result record has a closed member set");

    auto unknown_request = request_payload;
    unknown_request.find(addon::k_codex_invocation_member)->set("version", addon::Json_value(2));
    const auto request_without_capability = addon::decode_prepare_request(unknown_request, &failure, 3);
    SDK_CHECK(request_without_capability && !request_without_capability->codex_invocation &&
        request_without_capability->launcher_argv == request.launcher_argv,
        "an unknown request record version leaves the common launch core available");
    auto unknown_patch = patch_payload;
    unknown_patch.find(addon::k_codex_invocation_member)->set("version", addon::Json_value(2));
    const auto patch_without_capability = addon::decode_launch_patch(unknown_patch, &failure, 3);
    SDK_CHECK(patch_without_capability && !patch_without_capability->codex_invocation &&
        patch_without_capability->argv == patch.argv &&
        addon::validate_codex_invocation_acknowledgment(
            std::nullopt, *patch_without_capability, *patch_without_capability->argv, &failure),
        "an unknown result record version preserves unrelated launch behavior");
    SDK_CHECK(!addon::validate_codex_invocation_acknowledgment(
            offer, *patch_without_capability, *patch_without_capability->argv, &failure) &&
        failure.reason_code == addon::reason::k_unknown_capability,
        "a requested unknown result fails at the capability boundary");

    addon::Resume_prepare_request resume;
    resume.context.launcher          = request.launcher;
    resume.context.launcher_argv     = request.launcher_argv;
    resume.context.working_directory = request.working_directory;
    resume.context.launch_environment = request.launch_environment;
    resume.context.codex_invocation  = offer;
    resume.resume_record.set("provider_session_id", addon::Json_value("session-7f3c9a2e"));
    addon::Resume_prepare_result resumed;
    resumed.process.executable      = "helpers/codex_launch_bridge.exe";
    resumed.process.argv            = *patch.argv;
    resumed.process.stdin_behavior  = addon::Stdin_behavior::INHERIT;
    resumed.patch.codex_invocation  = addon::Codex_invocation{3};
    resumed.acknowledgment.record_digest_hex = addon::resume_record_digest_hex(resume.resume_record);
    std::vector<std::string> actual_resume_argv{"C:\\Logonomic\\helpers\\codex_launch_bridge.exe"};
    actual_resume_argv.insert(actual_resume_argv.end(), resumed.process.argv.begin(), resumed.process.argv.end());
    {
        constexpr std::uint16_t version = addon::k_interactive_resume_environment_version;
        const auto encoded_request = addon::encode_resume_prepare_request(resume, version);
        const auto decoded_resume = addon::decode_resume_prepare_request(encoded_request, &failure, version);
        SDK_CHECK(decoded_resume && decoded_resume->context.codex_invocation &&
            decoded_resume->context.launcher_argv == request.launcher_argv,
            "resume offers the independently versioned capability without a common schema bump");
        const auto encoded_result = addon::encode_resume_prepare_result(resumed, version);
        const auto decoded_result = addon::decode_resume_prepare_result(encoded_result, &failure, version);
        SDK_CHECK(decoded_result && decoded_result->patch.codex_invocation &&
            decoded_result->process.argv == resumed.process.argv &&
            !decoded_result->patch.argv && !decoded_result->patch.disposition &&
            addon::validate_codex_invocation_acknowledgment(
                offer, decoded_result->patch, actual_resume_argv, &failure),
            "resume validates the bridge plus verified executable prefix against the actual command");
        auto unknown_resume = encoded_result;
        unknown_resume.find(addon::k_codex_invocation_member)->set("version", addon::Json_value(2));
        const auto resume_core = addon::decode_resume_prepare_result(unknown_resume, &failure, version);
        SDK_CHECK(resume_core && !resume_core->patch.codex_invocation &&
            resume_core->process.argv == resumed.process.argv,
            "an unknown resume marker leaves the common process proposal available");
    }

    const addon::Capability_pair pair{
        addon::k_terminal_codex_invocation_capability_id, addon::k_terminal_codex_invocation_version};
    const addon::Capabilities_message handshake{{pair}};
    const auto decoded_handshake = addon::decode_adapter_message(
        addon::serialize_json_canonical(addon::encode_adapter_message(handshake)), {}, &failure);
    SDK_CHECK(decoded_handshake && std::holds_alternative<addon::Capabilities_message>(*decoded_handshake) &&
        std::get<addon::Capabilities_message>(*decoded_handshake).capabilities == handshake.capabilities,
        "the capability pair survives the existing handshake without changing its common schema");
    SDK_CHECK(addon::validate_terminal_codex_invocation_declaration(addon::Json_value::object(), &failure),
        "the capability sidecar is an empty independently negotiated declaration");
    SDK_CHECK(!addon::validate_terminal_codex_invocation_declaration(malformed_request, &failure) &&
        failure.reason_code == addon::reason::k_invalid_sidecar,
        "the sidecar has no payload fields");
    for (const addon::Capability_pair& producer : {
            addon::Capability_pair{addon::k_launch_preparation_capability_id, 3},
            addon::Capability_pair{addon::k_interactive_resume_capability_id, 2},
        })
    {
        SDK_CHECK(addon::validate_terminal_codex_invocation_dependency({pair, producer}, &failure),
            "a live invocation-producing capability satisfies the dependency");
    }
    SDK_CHECK(!addon::validate_terminal_codex_invocation_dependency({pair}, &failure) &&
        failure.reason_code == addon::reason::k_dependency_capability_unavailable,
        "a marker capability without an invocation producer cannot become live");
    return true;
}

} // namespace sdk_tests
