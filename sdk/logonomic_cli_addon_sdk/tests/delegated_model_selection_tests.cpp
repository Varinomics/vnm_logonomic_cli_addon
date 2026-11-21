#include "sdk_test_support.h"

#include "logonomic_cli_addon/capability_ids.h"
#include "logonomic_cli_addon/limits.h"
#include "logonomic_cli_addon/session_contracts.h"

#include <optional>
#include <string>

namespace sdk_tests {
namespace {

namespace addon = logonomic::cli_addon;
namespace limits = addon::limits;

addon::Delegated_prepare_request delegated_request()
{
    addon::Delegated_prepare_request request;
    request.purpose = addon::Structured_purpose::READ_ONLY_REVIEW;
    request.context.launcher = {"example-agent --model saved-model", addon::k_runner_direct, ""};
    request.context.launcher_argv = {"example-agent", "--model", "saved-model"};
    request.context.working_directory = "/srv/repositories/example";
    request.context.launch_environment =
        addon::Launch_environment{addon::k_platform_linux_x86_64, addon::Json_value::object(), std::nullopt};
    request.prompt = "Review the command parser.";
    request.control.task_id = "task-model-review";
    request.control.private_artifact_root = "/tmp/logonomic/task-model-review";
    request.control.servers = {{"coordination", "/opt/logonomic/coordination-mcp", {"status"}, {}}};
    return request;
}

addon::Delegated_prepare_result delegated_result()
{
    addon::Delegated_prepare_result result;
    result.process.executable = addon::Process_executable::launcher_program();
    result.process.argv = {"--review"};
    result.acknowledgment.server_ids = {"coordination"};
    return result;
}

} // namespace

bool test_delegated_model_selection_contracts()
{
    // Oracle: the requested per-task override and exact acknowledgment contract
    // in delegated_model_selection_1.md, with provider-native opaque values.
    addon::Contract_failure failure;
    const addon::Delegated_model_selection_declaration both{true, true};
    const addon::Delegated_model_selection_declaration model_only{true, false};
    const addon::Delegated_model_selection_declaration effort_only{false, true};
    for (const auto& declaration : {both, model_only, effort_only}) {
        const auto decoded = addon::decode_delegated_model_selection_declaration(
            addon::encode_delegated_model_selection_declaration(declaration), &failure);
        SDK_CHECK(decoded && decoded->model == declaration.model &&
            decoded->reasoning_effort == declaration.reasoning_effort,
            "each independently supported setting survives its declaration");
    }
    auto declaration = addon::encode_delegated_model_selection_declaration(both);
    declaration.erase("reasoning_effort");
    SDK_CHECK(!addon::decode_delegated_model_selection_declaration(declaration, &failure) &&
        failure.reason_code == addon::reason::k_invalid_sidecar,
        "the capability declaration requires an explicit support flag for each setting");
    declaration = addon::encode_delegated_model_selection_declaration({false, false});
    SDK_CHECK(!addon::decode_delegated_model_selection_declaration(declaration, &failure),
        "a declaration cannot advertise no model selection capabilities");
    declaration = addon::encode_delegated_model_selection_declaration(both);
    declaration.set("model", addon::Json_value("true"));
    SDK_CHECK(!addon::decode_delegated_model_selection_declaration(declaration, &failure),
        "support flags are booleans rather than truthy strings");
    declaration = addon::encode_delegated_model_selection_declaration(both);
    declaration.set("models", addon::Json_value::array());
    SDK_CHECK(!addon::decode_delegated_model_selection_declaration(declaration, &failure),
        "the declaration is a closed support contract without a model inventory");

    const addon::Delegated_model_selection selection{"provider/future-model", "provider-effort"};
    const addon::Delegated_model_selection model{"provider/future-model", std::nullopt};
    const addon::Delegated_model_selection effort{std::nullopt, "provider-effort"};
    for (const auto& requested : {selection, model, effort}) {
        const auto decoded = addon::decode_delegated_model_selection(
            addon::encode_delegated_model_selection(requested), &failure);
        SDK_CHECK(decoded == requested && addon::validate_delegated_model_selection(requested, &both, &failure),
            "the SDK transports provider-native settings without a model or effort allowlist");
    }
    SDK_CHECK(addon::validate_delegated_model_selection(model, &model_only, &failure) &&
        addon::validate_delegated_model_selection(effort, &effort_only, &failure),
        "requesting one supported setting does not require support for the other");
    SDK_CHECK(!addon::validate_delegated_model_selection(selection, nullptr, &failure) &&
        failure.reason_code == addon::reason::k_model_selection_unavailable,
        "a missing live capability cannot silently drop model selection");
    SDK_CHECK(!addon::validate_delegated_model_selection(model, &effort_only, &failure) &&
        failure.reason_code == addon::reason::k_model_selection_unavailable,
        "a provider without model selection refuses the requested model");
    SDK_CHECK(!addon::validate_delegated_model_selection(selection, &model_only, &failure) &&
        failure.reason_code == addon::reason::k_reasoning_effort_unavailable,
        "model support cannot silently drop the separately requested effort");

    const auto encoded_selection = addon::encode_delegated_model_selection(selection);
    auto malformed = encoded_selection;
    malformed.set("version", addon::Json_value(2));
    SDK_CHECK(!addon::decode_delegated_model_selection(malformed, &failure) &&
        failure.reason_code == addon::reason::k_unsupported_version,
        "an unknown optional record version is explicitly unavailable");
    malformed = encoded_selection;
    malformed.erase("version");
    SDK_CHECK(!addon::decode_delegated_model_selection(malformed, &failure),
        "model selection requires its own version");
    malformed = encoded_selection;
    malformed.set("provider", addon::Json_value("another-account"));
    SDK_CHECK(!addon::decode_delegated_model_selection(malformed, &failure) &&
        failure.reason_code == addon::reason::k_unexpected_member,
        "model selection does not carry account or provider authority");
    for (const auto& invalid : {
            addon::Json_value(), addon::Json_value(false), addon::Json_value(1),
            addon::Json_value(""), addon::Json_value(std::string("model\0suffix", 12)),
            addon::Json_value(std::string("\xc0\x80", 2))})
    {
        for (const char* field : {"model", "reasoning_effort"}) {
            malformed = encoded_selection;
            malformed.set(field, invalid);
            SDK_CHECK(!addon::decode_delegated_model_selection(malformed, &failure),
                "settings must be nonempty UTF-8 strings without NUL");
        }
    }
    SDK_CHECK(!addon::decode_delegated_model_selection(
            addon::encode_delegated_model_selection({}), &failure),
        "an empty selection is not a request to change defaults");
    addon::Delegated_model_selection bounded{
        std::string(limits::k_max_delegated_model_bytes, 'm'),
        std::string(limits::k_max_delegated_reasoning_effort_bytes, 'e')};
    SDK_CHECK(addon::decode_delegated_model_selection(
            addon::encode_delegated_model_selection(bounded), &failure) == bounded,
        "model and effort byte limits are inclusive");
    bounded.model->push_back('m');
    SDK_CHECK(!addon::validate_delegated_model_selection(bounded, &both, &failure) &&
        failure.reason_code == addon::reason::k_payload_too_large,
        "typed requests also enforce the model byte bound");
    bounded.model->pop_back();
    bounded.reasoning_effort->push_back('e');
    SDK_CHECK(!addon::decode_delegated_model_selection(
            addon::encode_delegated_model_selection(bounded), &failure) &&
        failure.reason_code == addon::reason::k_payload_too_large,
        "effort is bounded separately from the model");

    auto request = delegated_request();
    auto result  = delegated_result();
    const auto ordinary_request = addon::encode_delegated_prepare_request(request);
    const auto ordinary_result  = addon::encode_delegated_prepare_result(result);
    SDK_CHECK(!ordinary_request.find("model_selection") && !ordinary_result.find("model_selection") &&
        addon::decode_delegated_prepare_request(ordinary_request, &failure) &&
        addon::decode_delegated_prepare_result(ordinary_result, &failure) &&
        addon::validate_delegated_model_selection_acknowledgment(std::nullopt, std::nullopt, &failure),
        "ordinary delegation retains its existing wire shape and needs no model acknowledgment");
    request.model_selection = selection;
    result.model_selection  = selection;
    auto request_payload = addon::encode_delegated_prepare_request(request);
    auto result_payload  = addon::encode_delegated_prepare_result(result);
    const auto decoded_request = addon::decode_delegated_prepare_request(request_payload, &failure);
    const auto decoded_result  = addon::decode_delegated_prepare_result(result_payload, &failure);
    SDK_CHECK(decoded_request && decoded_result && decoded_request->model_selection == selection &&
        decoded_result->model_selection == selection && addon::validate_delegated_model_selection_acknowledgment(
            decoded_request->model_selection, decoded_result->model_selection, &failure),
        "the request and prepared result bind the same exact per-task settings");
    request_payload.erase("model_selection");
    result_payload.erase("model_selection");
    SDK_CHECK(request_payload == ordinary_request && result_payload == ordinary_result,
        "selection does not rewrite the saved launcher, purpose, account or preparation context");
    for (const auto& acknowledgment : {
            model, effort, addon::Delegated_model_selection{"other-model", "provider-effort"}})
    {
        SDK_CHECK(!addon::validate_delegated_model_selection_acknowledgment(selection, acknowledgment, &failure) &&
            failure.reason_code == addon::reason::k_acknowledgment_mismatch,
            "partial or substituted settings cannot satisfy the caller's request");
    }
    SDK_CHECK(!addon::validate_delegated_model_selection_acknowledgment(selection, std::nullopt, &failure) &&
        !addon::validate_delegated_model_selection_acknowledgment(std::nullopt, selection, &failure),
        "missing and unsolicited acknowledgments are both refused");
    malformed = encoded_selection;
    malformed.set("version", addon::Json_value(2));
    request_payload.set("model_selection", malformed);
    result_payload.set("model_selection", malformed);
    SDK_CHECK(!addon::decode_delegated_prepare_request(request_payload, &failure) &&
        !addon::decode_delegated_prepare_result(result_payload, &failure),
        "an unsupported selection record cannot become an ordinary successful delegation");
    return true;
}

} // namespace sdk_tests
