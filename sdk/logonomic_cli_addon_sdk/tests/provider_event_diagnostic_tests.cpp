#include "sdk_test_support.h"

#include "logonomic_cli_addon/helper_ingress.h"
#include "logonomic_cli_addon/provider_event_diagnostic.h"
#include "logonomic_cli_addon/terminal_identity.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>

namespace sdk_tests {
namespace {

bool check_value_free_messages()
{
    namespace addon = logonomic::cli_addon;
    const addon::Json_value private_value("private-token-and-transcript-value\nsecond-line");
    const auto diagnostic = addon::make_provider_event_diagnostic(
        "StopFailure", "error", addon::Provider_event_problem::CONTROL_CHARACTER,
        &private_value, 256, "single_line_identity");
    const auto message = addon::encode_provider_event_diagnostic(diagnostic);
    const auto decoded = addon::decode_provider_event_diagnostic(message);
    SDK_CHECK(decoded && decoded->event_name == "StopFailure" && decoded->field == "error",
        "a retained diagnostic identifies the native event and rejected field");
    SDK_CHECK(decoded->problem == addon::Provider_event_problem::CONTROL_CHARACTER &&
        decoded->actual_type == addon::Json_type::STRING && decoded->actual_size == private_value.as_string().size() &&
        decoded->limit == 256 && decoded->rule == "single_line_identity",
        "the diagnostic preserves the reason, observed type and relevant bounds");
    SDK_CHECK(message.find("private-token") == std::string::npos && message.find("second-line") == std::string::npos,
        "private field values never enter the diagnostic envelope");
    SDK_CHECK(message.size() <= addon::limits::k_max_result_message_bytes &&
        addon::encode_provider_event_diagnostic(*decoded) == message,
        "a canonical diagnostic survives the existing bounded failure-message transport");

    const auto missing = addon::decode_provider_event_diagnostic(addon::encode_provider_event_diagnostic(
        addon::make_provider_event_diagnostic("Stop", "prompt_id", addon::Provider_event_problem::MISSING, nullptr)));
    const addon::Json_value null;
    const auto explicit_null = addon::decode_provider_event_diagnostic(addon::encode_provider_event_diagnostic(
        addon::make_provider_event_diagnostic("Stop", "prompt_id", addon::Provider_event_problem::WRONG_TYPE, &null)));
    SDK_CHECK(missing && !missing->actual_type && explicit_null &&
        explicit_null->actual_type == addon::Json_type::NULL_VALUE,
        "an absent field and an explicitly null field remain distinguishable");

    const auto object = addon::parse_json(R"({"private-property":"private-value"})", {}, nullptr);
    const auto object_message = addon::encode_provider_event_diagnostic(addon::make_provider_event_diagnostic(
        "NativeSignal", "event.body", addon::Provider_event_problem::INVALID_SHAPE, &*object));
    SDK_CHECK(object_message.find("private-property") == std::string::npos &&
        object_message.find("private-value") == std::string::npos,
        "container diagnostics preserve only type and entry count");
    const auto object_decoded = addon::decode_provider_event_diagnostic(object_message);
    SDK_CHECK(object_decoded && object_decoded->actual_size == 1 &&
        object_decoded->actual_type == addon::Json_type::OBJECT,
        "container size is available without archiving object keys or values");

    addon::Contract_failure failure;
    const std::optional<int> rejected = addon::reject_provider_event(&failure, diagnostic);
    const auto rejected_diagnostic = addon::decode_provider_event_diagnostic(failure.detail);
    SDK_CHECK(!rejected && failure.reason_code == "invalid_provider_event" && rejected_diagnostic,
        "optional-result classifiers carry the same diagnostic in their explicit rejection");
    const addon::Provider_event_error error(diagnostic);
    SDK_CHECK(std::string(error.what()) == "invalid_provider_event" && error.diagnostic().field == "error",
        "exception classifiers carry field metadata without exposing arbitrary exception text");
    return true;
}

bool check_closed_messages()
{
    namespace addon = logonomic::cli_addon;
    const auto valid = addon::parse_json(addon::encode_provider_event_diagnostic(addon::make_provider_event_diagnostic(
        "Stop", "prompt_id", addon::Provider_event_problem::MISSING, nullptr)), {}, nullptr);
    SDK_CHECK(valid.has_value(), "the diagnostic producer emits valid JSON");
    for (const auto* name : {"value", "path", "exception", "unexpected"}) {
        auto unsafe = *valid;
        unsafe.find(addon::k_provider_event_diagnostic_member)->set(name, addon::Json_value("private-value"));
        SDK_CHECK(!addon::decode_provider_event_diagnostic(addon::serialize_json(unsafe)),
            "unknown members cannot smuggle private values into trusted diagnostics");
    }
    const struct
    {
        const char* member;
        addon::Json_value value;
    }
    malformed[] = {
        {"version",     addon::Json_value(2)},
        {"problem",     addon::Json_value("private-freeform-error")},
        {"actual_type", addon::Json_value("private-freeform-type")},
        {"actual_size", addon::Json_value(-1)},
        {"actual_size", addon::Json_value(1)},
        {"limit",       addon::Json_value(3.5)},
        {"field",       addon::Json_value("prompt_id\nprivate-value")},
        {"event_name",  addon::Json_value(std::string("Stop") + '\0' + "private-value")},
        {"rule",        addon::Json_value("private text")},
    };
    for (const auto& entry : malformed) {
        auto unsafe = *valid;
        unsafe.find(addon::k_provider_event_diagnostic_member)->set(entry.member, entry.value);
        SDK_CHECK(!addon::decode_provider_event_diagnostic(addon::serialize_json(unsafe)),
            "unknown versions, vocabularies, invalid measurements and non-label text remain untrusted");
    }
    SDK_CHECK(!addon::decode_provider_event_diagnostic("ordinary provider error") &&
        !addon::decode_provider_event_diagnostic(std::string(addon::limits::k_max_result_message_bytes + 1, 'x')),
        "ordinary text and oversized messages are not promoted to structured diagnostics");
    auto invalid = addon::make_provider_event_diagnostic(
        "Stop", "prompt_id", addon::Provider_event_problem::MISSING, nullptr);
    invalid.rule = "private error text";
    bool refused = false;
    try {
        (void)addon::encode_provider_event_diagnostic(invalid);
    }
    catch (const std::invalid_argument&) {
        refused = true;
    }
    SDK_CHECK(refused, "invalid source labels fail at the producer rather than becoming trusted log text");
    return true;
}

bool check_native_event_name_grammar()
{
    namespace addon = logonomic::cli_addon;
    const std::string names[] = {
        "Stop event",
        "会話.完了",
        "Stop\r\ncontinuation",
        std::string(addon::limits::k_max_provider_event_name_bytes, '\x01'),
    };
    for (const auto& name : names) {
        const auto message = addon::encode_provider_event_diagnostic(addon::make_provider_event_diagnostic(
            name, "hook_event_name", addon::Provider_event_problem::MISMATCH, nullptr));
        const auto decoded = addon::decode_provider_event_diagnostic(message);
        SDK_CHECK(decoded && decoded->event_name == name,
            "diagnostics preserve every provider-accepted event name, including spaces, Unicode and controls");
        SDK_CHECK(message.size() <= addon::limits::k_max_result_message_bytes &&
            message.find('\r') == std::string::npos && message.find('\n') == std::string::npos,
            "native event names stay bounded and escaped on the diagnostic transport");
        SDK_CHECK(addon::encode_provider_event_diagnostic(*decoded) == message,
            "the bounded native-name encoding is deterministic after decoding");
    }

    auto maximum = addon::make_provider_event_diagnostic(
        std::string(addon::limits::k_max_provider_event_name_bytes, '\x01'),
        std::string(160, 'f'), addon::Provider_event_problem::TOO_LARGE, nullptr,
        (std::uint64_t)std::numeric_limits<std::int64_t>::max(), std::string(64, 'r'));
    maximum.actual_type = addon::Json_type::STRING;
    maximum.actual_size = (std::uint64_t)std::numeric_limits<std::int64_t>::max();
    const auto message = addon::encode_provider_event_diagnostic(maximum);
    const auto decoded = addon::decode_provider_event_diagnostic(message);
    SDK_CHECK(message.size() <= addon::limits::k_max_result_message_bytes && decoded &&
        decoded->event_name == maximum.event_name && decoded->field == maximum.field &&
        decoded->rule == maximum.rule && decoded->actual_size == maximum.actual_size && decoded->limit == maximum.limit,
        "the full maximum event, field, rule and measurement combination fits the existing message bound");
    SDK_CHECK(message.find("event_name_base64") != std::string::npos,
        "escape-heavy names use bounded exact UTF-8 byte encoding rather than being rejected or truncated");

    auto envelope = addon::parse_json(message, {}, nullptr);
    SDK_CHECK(envelope.has_value(), "the maximum diagnostic remains valid JSON");
    envelope->find(addon::k_provider_event_diagnostic_member)->set("event_name", addon::Json_value("Stop"));
    SDK_CHECK(!addon::decode_provider_event_diagnostic(addon::serialize_json(*envelope)),
        "ambiguous plain and encoded event names are rejected");
    envelope->find(addon::k_provider_event_diagnostic_member)->erase("event_name");
    envelope->find(addon::k_provider_event_diagnostic_member)->set("event_name_base64", addon::Json_value("not-base64"));
    SDK_CHECK(!addon::decode_provider_event_diagnostic(addon::serialize_json(*envelope)),
        "malformed byte encoding cannot become trusted diagnostic metadata");
    return true;
}

bool check_classification_schema_failures()
{
    namespace addon = logonomic::cli_addon;
    auto request = addon::parse_json(
        R"({"event":{"name":"Stop","body":{}},"ledger":{"current_epoch":0,"current_transition":null},)"
        R"("transition_nonce":"0123456789abcdef0123456789abcdef"})", {}, nullptr);
    SDK_CHECK(request.has_value(), "the fixture uses the classification request contract");
    request->find("ledger")->set("current_epoch", addon::Json_value("private-invalid-value"));
    addon::Contract_failure failure;
    SDK_CHECK(!addon::decode_classify_event_request(*request, &failure) &&
        failure.reason_code == addon::reason::k_frame_violation,
        "request schema rejection preserves its existing disposition");
    auto diagnostic = addon::decode_provider_event_diagnostic(failure.detail);
    SDK_CHECK(diagnostic && diagnostic->field == "ledger.current_epoch" &&
        failure.detail.find("private-invalid-value") == std::string::npos,
        "schema failures identify their source field without private contents");

    auto result = addon::parse_json(R"({"classification":"private-invalid-value","observations":[]})", {}, nullptr);
    SDK_CHECK(result && !addon::decode_classify_event_result(*result, &failure) &&
        failure.reason_code == addon::reason::k_classification_invalid,
        "result schema rejection preserves its existing disposition");
    diagnostic = addon::decode_provider_event_diagnostic(failure.detail);
    SDK_CHECK(diagnostic && diagnostic->field == "classification" &&
        diagnostic->actual_type == addon::Json_type::STRING &&
        failure.detail.find("private-invalid-value") == std::string::npos,
        "result schema failures identify the rejected enum field without its value");

    request->find("ledger")->set("current_epoch", addon::Json_value(0));
    request->set(addon::k_terminal_identity_member, *addon::parse_json(
        R"({"version":1,"context":{},"terminal_event":"private-invalid-event"})", {}, nullptr));
    SDK_CHECK(!addon::decode_classify_event_request(*request, &failure) &&
        failure.reason_code == addon::reason::k_frame_violation,
        "nested terminal identity request defects preserve their existing failure reason");
    diagnostic = addon::decode_provider_event_diagnostic(failure.detail);
    SDK_CHECK(diagnostic && diagnostic->field == "terminal_identity.terminal_event" &&
        diagnostic->actual_type == addon::Json_type::STRING &&
        failure.detail.find("private-invalid-event") == std::string::npos,
        "the exact nested terminal event field survives its classification wrapper without payload contents");

    result->set("classification", addon::Json_value("ignore"));
    result->set(addon::k_terminal_identity_member, *addon::parse_json(
        R"({"provider_session_id":"private-session-value","title":null})", {}, nullptr));
    SDK_CHECK(!addon::decode_terminal_identity_classification_result(*result, &failure) &&
        failure.reason_code == addon::reason::k_frame_violation,
        "nested terminal identity result defects preserve their existing failure reason");
    diagnostic = addon::decode_provider_event_diagnostic(failure.detail);
    SDK_CHECK(diagnostic && diagnostic->field == "terminal_identity.title" &&
        diagnostic->actual_type == addon::Json_type::NULL_VALUE &&
        failure.detail.find("private-session-value") == std::string::npos,
        "the exact rejected proof field survives the result decoder without unrelated identity values");
    return true;
}

} // namespace

bool test_provider_event_diagnostic_contracts()
{
    return check_value_free_messages() && check_closed_messages() &&
        check_native_event_name_grammar() && check_classification_schema_failures();
}

} // namespace sdk_tests
