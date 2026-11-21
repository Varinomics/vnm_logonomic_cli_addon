#include "sdk_test_support.h"

#include "logonomic_cli_addon/capability_ids.h"
#include "logonomic_cli_addon/helper_ingress.h"
#include "logonomic_cli_addon/lifecycle_events.h"
#include "logonomic_cli_addon/limits.h"
#include "logonomic_cli_addon/model_catalog.h"

#include <limits>

namespace sdk_tests {
namespace {

namespace addon = logonomic::cli_addon;

addon::Model_catalog_update native_catalog()
{
    return {"native-root", "0123456789abcdef0123456789abcdef", 0,
        addon::Model_catalog_status::AVAILABLE,
        {{"native-provider", "model-a", "Model A", true}, {"native-provider", "model-b", std::nullopt, false}},
        2, false, "native.models.list"};
}

bool source_authority()
{
    auto update = native_catalog();
    addon::Model_catalog_state state;
    state.provider_session_id = update.provider_session_id;
    state.transition_nonce = update.transition_nonce;
    SDK_CHECK(addon::apply_model_catalog_update(state, update) && state.models == update.models,
        "the native snapshot establishes configured availability");
    SDK_CHECK(!addon::apply_model_catalog_update(state, update), "replayed snapshots are inert");
    auto foreign = update;
    foreign.sequence = 1;
    foreign.transition_nonce = "11111111111111111111111111111111";
    SDK_CHECK(!addon::apply_model_catalog_update(state, foreign), "an old same-ID activation cannot replace the catalog");
    foreign.transition_nonce = update.transition_nonce;
    foreign.provider_session_id = "other-root";
    SDK_CHECK(!addon::apply_model_catalog_update(state, foreign), "another native root cannot replace the catalog");
    update.sequence = 2;
    update.models.clear();
    update.total_count = 0;
    SDK_CHECK(addon::apply_model_catalog_update(state, update) && state.status == addon::Model_catalog_status::AVAILABLE &&
        state.models.empty(), "an observed empty registry replaces the previous catalog without implying unavailable");
    update = native_catalog();
    update.sequence = 3;
    update.total_count = 200;
    update.truncated = true;
    SDK_CHECK(addon::apply_model_catalog_update(state, update) && state.total_count == 200 && state.truncated,
        "bounded native snapshots keep the exact count and explicit incompleteness");
    addon::invalidate_model_catalog(state);
    SDK_CHECK(state.status == addon::Model_catalog_status::UNKNOWN && state.models.empty() && state.sequence == 3 &&
        !addon::apply_model_catalog_update(state, update), "host loss withdraws facts while retaining the source barrier");
    update.sequence = 4;
    update.status = addon::Model_catalog_status::UNAVAILABLE;
    update.models.clear();
    update.total_count = 0;
    update.truncated = false;
    SDK_CHECK(addon::apply_model_catalog_update(state, update) && state.status == addon::Model_catalog_status::UNAVAILABLE,
        "a newer native inability to sample is distinct from an observed empty registry");
    return true;
}

bool optional_contract()
{
    addon::Classify_event_request request;
    request.event.name = "native_start";
    request.transition_nonce = native_catalog().transition_nonce;
    auto payload = addon::encode_classify_event_request(request);
    SDK_CHECK(!payload.find("model_catalog"), "older peers receive no catalog envelope");
    request.model_catalog_version = addon::k_model_catalog_version;
    payload = addon::encode_classify_event_request(request);
    addon::Contract_failure core_failure;
    auto decoded = addon::decode_classify_event_request(payload, &core_failure);
    SDK_CHECK(decoded && decoded->model_catalog_version == 1 && !decoded->catalog_failure,
        "model catalog negotiates independently of model-state and helper versions");
    payload.find("model_catalog")->set("version", addon::Json_value(2));
    decoded = addon::decode_classify_event_request(payload, &core_failure);
    SDK_CHECK(decoded && decoded->model_catalog_version == 0 && decoded->catalog_failure && core_failure.reason_code.empty(),
        "an unsupported catalog request does not reject its lifecycle event");

    addon::Classify_event_result result;
    result.classification = addon::Classification::TRANSITION;
    result.transition = addon::Transition_kind::START;
    addon::Lifecycle_observation started;
    started.provider_event_name = "native_start";
    started.session_transition = addon::Session_transition::STARTED;
    started.provider_session_id = "native-root";
    result.observations = {started};
    result.catalog_update = native_catalog();
    payload = addon::encode_classify_event_result(result);
    auto classified = addon::decode_classify_event_result(payload, &core_failure);
    SDK_CHECK(classified && classified->catalog_update == result.catalog_update && !classified->catalog_failure,
        "a full snapshot and accepted native start round trip together");
    payload.find("model_catalog")->find("snapshot")->set("truncated", addon::Json_value(true));
    classified = addon::decode_classify_event_result(payload, &core_failure);
    SDK_CHECK(classified && classified->catalog_failure && !classified->catalog_update &&
        classified->observations.size() == 1 && core_failure.reason_code.empty(),
        "invalid partiality fails catalog alone and retains adjacent lifecycle facts");

    addon::Observe_request observing;
    observing.session = {"/native/workspace", {"native-cli"}, "native-root"};
    observing.window_ms = addon::limits::k_observe_window_ms;
    observing.model_catalog_version = 1;
    auto observed_request = addon::decode_observe_request(addon::encode_observe_request(observing), &core_failure);
    SDK_CHECK(observed_request && observed_request->model_catalog_version == 1, "continuous observation opts in independently");
    addon::Poll_observations_request polling;
    polling.limit = 16;
    polling.model_catalog_version = 1;
    auto poll_request = addon::decode_poll_observations_request(addon::encode_poll_observations_request(polling), &core_failure);
    SDK_CHECK(poll_request && poll_request->model_catalog_version == 1, "poll requests preserve optional negotiation");
    addon::Observe_result observed;
    observed.catalog_update = result.catalog_update;
    auto window = addon::decode_observe_result(addon::encode_observe_result(observed), &core_failure);
    SDK_CHECK(window && window->catalog_update == result.catalog_update, "observation uses the same atomic snapshot");
    addon::Poll_observations_result polled;
    polled.catalog_update = result.catalog_update;
    auto poll = addon::decode_poll_observations_result(addon::encode_poll_observations_result(polled), 16, &core_failure);
    SDK_CHECK(poll && poll->catalog_update == result.catalog_update && poll->observations.empty(),
        "a catalog-only poll invents no lifecycle transition");
    return true;
}

bool bounds_and_privacy()
{
    auto update = native_catalog();
    update.sequence = std::numeric_limits<std::uint64_t>::max();
    auto payload = addon::Json_value::object();
    addon::encode_model_catalog_update(payload, update);
    std::optional<addon::Model_catalog_update> decoded;
    std::optional<addon::Contract_failure> failure;
    addon::decode_model_catalog_update(payload, decoded, failure);
    SDK_CHECK(!failure && decoded == update && decoded->models[1].reasoning == false,
        "full source order and explicit non-reasoning support survive encoding");
    update.models[1].reasoning.reset();
    addon::encode_model_catalog_update(payload, update);
    addon::decode_model_catalog_update(payload, decoded, failure);
    SDK_CHECK(!failure && !decoded->models[1].reasoning, "unknown reasoning support is omitted and never inferred false");
    for (const char* invalid : {"01", "-1", "+1", "1.0", "18446744073709551616", ""}) {
        auto damaged = payload;
        damaged.find("model_catalog")->find("snapshot")->set("sequence", addon::Json_value(invalid));
        addon::decode_model_catalog_update(damaged, decoded, failure);
        SDK_CHECK(failure && !decoded, "invalid sequence cannot establish a catalog");
    }
    auto damaged = payload;
    damaged.find("model_catalog")->find("snapshot")->find("models")->as_array().front().set("api_key", addon::Json_value("secret"));
    addon::decode_model_catalog_update(damaged, decoded, failure);
    SDK_CHECK(failure && !decoded, "entry shape excludes credential and pricing fields");
    for (const auto& invalid : {std::string{}, std::string("native\nprovider"), std::string(1, '\xff'), std::string(513, 'x')}) {
        update = native_catalog();
        update.models.front().provider = invalid;
        SDK_CHECK(!addon::is_valid_model_catalog_update(update), "empty, malformed and oversized identity is refused");
    }
    update = native_catalog();
    update.models.push_back(update.models.front());
    update.total_count = 3;
    SDK_CHECK(!addon::is_valid_model_catalog_update(update), "duplicate identity makes the snapshot ambiguous");
    update = native_catalog();
    update.total_count = 1;
    SDK_CHECK(!addon::is_valid_model_catalog_update(update), "shown entries cannot exceed the native count");
    update.total_count = 3;
    SDK_CHECK(!addon::is_valid_model_catalog_update(update), "omitted native entries require the partial flag");
    update = native_catalog();
    update.status = addon::Model_catalog_status::UNAVAILABLE;
    SDK_CHECK(!addon::is_valid_model_catalog_update(update), "unavailable cannot retain a previous inventory");
    update = native_catalog();
    update.models.clear();
    for (std::size_t i = 0; i < addon::limits::k_max_model_catalog_entries + 1; ++i) {
        update.models.push_back({"provider", "model-" + std::to_string(i), std::nullopt, std::nullopt});
    }
    update.total_count = static_cast<std::uint32_t>(update.models.size());
    SDK_CHECK(!addon::is_valid_model_catalog_update(update), "the snapshot has a bounded entry count");
    update.models.resize(addon::limits::k_max_model_catalog_entries);
    update.total_count = static_cast<std::uint32_t>(update.models.size());
    SDK_CHECK(addon::is_valid_model_catalog_update(update), "a compact full bounded snapshot is admitted");
    for (auto& model : update.models) model.display_name = std::string(256, 'x');
    SDK_CHECK(!addon::is_valid_model_catalog_update(update), "the canonical byte bound applies in addition to row count");
    return true;
}

} // namespace

bool test_model_catalog_contracts()
{
    return source_authority() && optional_contract() && bounds_and_privacy();
}

} // namespace sdk_tests
