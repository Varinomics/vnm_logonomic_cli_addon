#include "sdk_test_support.h"
#include "logonomic_cli_addon/permission_snapshot.h"
#include "logonomic_cli_addon/limits.h"

namespace sdk_tests {
namespace {

namespace sdk = logonomic::cli_addon;

sdk::Permission_snapshot_classification_result snapshot(bool pending)
{
    sdk::Permission_snapshot_classification_result result;
    result.base.classification = sdk::Classification::EVENT;
    sdk::Lifecycle_observation observation;
    observation.provider_event_name = "StatusLine";
    observation.provider_session_id = "session-a";
    result.base.observations.push_back(std::move(observation));
    result.permission_snapshot = sdk::Permission_snapshot_update{1, 0, pending};
    return result;
}

} // namespace

bool test_permission_snapshot_contracts()
{
    sdk::Contract_failure failure;
    const sdk::Permission_snapshot_declaration declaration{".native/settings.json", "StatusLine"};
    const auto encoded = sdk::encode_permission_snapshot_declaration(declaration);
    const auto decoded = sdk::decode_permission_snapshot_declaration(encoded, &failure);
    SDK_CHECK(decoded && decoded->relative_path == declaration.relative_path && decoded->event_name == declaration.event_name,
        "fixed-home declaration round trip");
    for (const char* path : {"", "../settings.json", "/settings.json", "C:/settings.json", ".native/../settings.json"}) {
        auto invalid = encoded;
        invalid.set("relative_path", sdk::Json_value(path));
        SDK_CHECK(!sdk::decode_permission_snapshot_declaration(invalid, &failure), "invalid fixed-home path rejected");
    }
    for (const std::string& event : {std::string{}, std::string("NativeSignal"),
             std::string(sdk::limits::k_max_provider_event_name_bytes + 1, 'e')})
    {
        auto invalid = encoded;
        invalid.set("event_name", sdk::Json_value(event));
        SDK_CHECK(!sdk::decode_permission_snapshot_declaration(invalid, &failure), "invalid or host-owned event rejected");
    }
    auto extra = encoded;
    extra.set("environment_name", sdk::Json_value("HOME"));
    SDK_CHECK(!sdk::decode_permission_snapshot_declaration(extra, &failure), "observer cannot add a selector override");

    for (const bool pending : {true, false, true}) {
        const auto payload = sdk::encode_permission_snapshot_classification_result(snapshot(pending));
        for (const std::uint16_t version : {2, 3}) {
            const auto result = sdk::decode_permission_snapshot_classification_result(payload, &failure, version);
            SDK_CHECK(result && result->permission_snapshot && result->permission_snapshot->pending == pending &&
                result->base.observations.size() == 1, "native level preserved with either supported helper ingress");
            SDK_CHECK(!sdk::decode_classify_event_result(payload, &failure, version),
                "ordinary helper ingress keeps its strict envelope");
        }
    }
    const auto ignore = sdk::encode_permission_snapshot_classification_result({});
    SDK_CHECK(sdk::decode_permission_snapshot_classification_result(ignore, &failure).has_value(),
        "an invalid native callback can be ignored without clearing attention");

    auto valid = sdk::encode_permission_snapshot_classification_result(snapshot(true));
    for (const auto& [member, value] : std::vector<std::pair<std::string, sdk::Json_value>>{
             {"version", sdk::Json_value(2)}, {"observation_index", sdk::Json_value(1)},
             {"observation_index", sdk::Json_value(-1)}, {"pending", sdk::Json_value("false")},
             {"request_id", sdk::Json_value("invented")}})
    {
        auto invalid = valid;
        auto update = *invalid.find("permission_snapshot");
        update.set(member, value);
        invalid.set("permission_snapshot", std::move(update));
        SDK_CHECK(!sdk::decode_permission_snapshot_classification_result(invalid, &failure),
            "malformed snapshot never becomes a false level");
    }
    auto absent = valid;
    absent.erase("permission_snapshot");
    SDK_CHECK(!sdk::decode_permission_snapshot_classification_result(absent, &failure),
        "a neutral event requires an explicit snapshot level");
    auto mixed = snapshot(true);
    mixed.base.observations.front().phase_transition = sdk::Phase_transition::WORKING;
    mixed.base.observations.front().phase_evidence = sdk::Evidence::CONFIRMED;
    SDK_CHECK(!sdk::decode_permission_snapshot_classification_result(
        sdk::encode_permission_snapshot_classification_result(mixed), &failure), "phase claims rejected");
    for (const char* member : {"provider_turn_id", "provider_event_id", "provider_sequence", "attention_request_id", "agent_id"}) {
        auto invalid = valid;
        auto observations = *invalid.find("observations");
        auto observation = observations.as_array().front();
        observation.set(member, sdk::Json_value("1"));
        auto replacement = sdk::Json_value::array();
        replacement.push_back(std::move(observation));
        invalid.set("observations", std::move(replacement));
        SDK_CHECK(!sdk::decode_permission_snapshot_classification_result(invalid, &failure),
            "snapshot does not fabricate native correlation");
    }
    auto unbound = snapshot(true);
    unbound.base.observations.front().provider_session_id.clear();
    SDK_CHECK(!sdk::decode_permission_snapshot_classification_result(
        sdk::encode_permission_snapshot_classification_result(unbound), &failure), "session binding required");
    auto multiple = snapshot(true);
    multiple.base.observations.push_back(multiple.base.observations.front());
    SDK_CHECK(!sdk::decode_permission_snapshot_classification_result(
        sdk::encode_permission_snapshot_classification_result(multiple), &failure), "one neutral carrier required");
    return true;
}

} // namespace sdk_tests
