#include "sdk_test_support.h"

#include "logonomic_cli_addon/capability_ids.h"
#include "logonomic_cli_addon/helper_ingress.h"
#include "logonomic_cli_addon/lifecycle_events.h"
#include "logonomic_cli_addon/model_state.h"
#include "logonomic_cli_addon/session_contracts.h"

#include <limits>

namespace sdk_tests {
namespace {

namespace addon = logonomic::cli_addon;

addon::Model_update observed_model()
{
    return {"native-a", "0123456789abcdef0123456789abcdef", 0, {},
        addon::Model_status::CONFIRMED, "native-provider", "resolved-model", "high", "native.agent_start"};
}

bool source_authority()
{
    auto update = observed_model();
    addon::Model_state state;
    state.provider_session_id = update.provider_session_id;
    state.transition_nonce = update.transition_nonce;
    SDK_CHECK(addon::apply_model_update(state, update) && state.model == "resolved-model" &&
        state.reasoning_effort == "high", "native model and effort establish the root fact");
    SDK_CHECK(!addon::apply_model_update(state, update), "duplicate source sequence is inert");
    auto child = update;
    child.agent_id = "child-1";
    child.sequence = 2;
    child.model = "child-model";
    SDK_CHECK(!addon::apply_model_update(state, child) && state.model == "resolved-model",
        "child evidence cannot replace its root's model");
    auto foreign = update;
    foreign.sequence = 3;
    foreign.transition_nonce = "11111111111111111111111111111111";
    SDK_CHECK(!addon::apply_model_update(state, foreign), "return to the same native ID cannot revive old activation facts");
    foreign.transition_nonce = update.transition_nonce;
    foreign.provider_session_id = "native-b";
    SDK_CHECK(!addon::apply_model_update(state, foreign), "a different native session cannot change the model");
    update.sequence = 4;
    update.model = "fallback-model";
    update.reasoning_effort.reset();
    SDK_CHECK(addon::apply_model_update(state, update) && state.model == "fallback-model" &&
        !state.reasoning_effort, "a native replacement clears an unobserved previous effort");
    auto loss = update;
    loss.sequence = 5;
    loss.status = addon::Model_status::UNKNOWN;
    loss.provider.clear();
    loss.model.clear();
    SDK_CHECK(addon::apply_model_update(state, loss) && state.model.empty() &&
        !addon::apply_model_update(state, update), "attributed loss clears facts and preserves the replay barrier");
    update.sequence = 6;
    SDK_CHECK(addon::apply_model_update(state, update), "new evidence can recover observation");
    addon::invalidate_model_state(state);
    SDK_CHECK(state.model.empty() && state.sequence == 6 && !addon::apply_model_update(state, update),
        "host observation loss cannot resurrect the previous sample");
    return true;
}

bool optional_contract()
{
    addon::Classify_event_request request;
    request.event.name = "native_start";
    request.transition_nonce = observed_model().transition_nonce;
    auto payload = addon::encode_classify_event_request(request);
    SDK_CHECK(!payload.find("model_state"), "older peers receive no unsolicited optional envelope");
    request.model_state_version = addon::k_model_state_version;
    payload = addon::encode_classify_event_request(request);
    addon::Contract_failure core_failure;
    auto decoded = addon::decode_classify_event_request(payload, &core_failure);
    SDK_CHECK(decoded && decoded->model_state_version == 1 && !decoded->model_failure,
        "the model capability negotiates independently");
    payload.find("model_state")->set("version", addon::Json_value(9));
    decoded = addon::decode_classify_event_request(payload, &core_failure);
    SDK_CHECK(decoded && decoded->model_state_version == 0 && decoded->model_failure && core_failure.reason_code.empty(),
        "unsupported model telemetry cannot reject a valid native event");

    addon::Classify_event_result result;
    result.classification = addon::Classification::TRANSITION;
    result.transition = addon::Transition_kind::START;
    addon::Lifecycle_observation started;
    started.provider_event_name = "native_start";
    started.session_transition = addon::Session_transition::STARTED;
    started.provider_session_id = "native-a";
    result.observations = {started};
    result.model_updates = {observed_model()};
    payload = addon::encode_classify_event_result(result);
    auto classified = addon::decode_classify_event_result(payload, &core_failure);
    SDK_CHECK(classified && classified->model_updates == result.model_updates && !classified->model_failure,
        "joint identity and model facts round trip together");
    payload.find("model_state")->find("updates")->as_array().front().set("sequence", addon::Json_value(0));
    classified = addon::decode_classify_event_result(payload, &core_failure);
    SDK_CHECK(classified && classified->model_failure && classified->model_updates.empty() &&
        classified->observations.size() == 1 && core_failure.reason_code.empty(),
        "malformed model telemetry cannot discard the neighboring lifecycle fact");

    addon::Observe_result observed;
    observed.model_updates = result.model_updates;
    auto window = addon::decode_observe_result(addon::encode_observe_result(observed), &core_failure);
    SDK_CHECK(window && window->model_updates == result.model_updates, "continuous observation retains the same model contract");
    addon::Poll_observations_result polled;
    polled.model_updates = result.model_updates;
    auto poll = addon::decode_poll_observations_result(addon::encode_poll_observations_result(polled), 16, &core_failure);
    SDK_CHECK(poll && poll->model_updates == result.model_updates, "model-only poll needs no invented lifecycle transition");
    return true;
}

bool wire_bounds()
{
    auto update = observed_model();
    update.sequence = std::numeric_limits<std::uint64_t>::max();
    auto payload = addon::Json_value::object();
    addon::encode_model_updates(payload, {update});
    std::vector<addon::Model_update> updates;
    std::optional<addon::Contract_failure> failure;
    addon::decode_model_updates(payload, updates, failure);
    SDK_CHECK(!failure && updates == std::vector{update}, "full uint64 source order survives the wire");
    for (const char* invalid : {"01", "-1", "+1", "1.0", "18446744073709551616", ""}) {
        auto damaged = payload;
        damaged.find("model_state")->find("updates")->as_array().front().set("sequence", addon::Json_value(invalid));
        addon::decode_model_updates(damaged, updates, failure);
        SDK_CHECK(failure && updates.empty(), "invalid source order cannot establish a model");
    }
    for (const auto& invalid : {std::string("native\nprovider"), std::string(1, '\xff'), std::string(513, 'x')}) {
        update = observed_model();
        update.provider = invalid;
        SDK_CHECK(!addon::is_valid_model_update(update), "control-bearing, invalid UTF-8 and oversized providers are refused");
    }
    update = observed_model();
    update.provider.clear();
    addon::encode_model_updates(payload, {update});
    addon::decode_model_updates(payload, updates, failure);
    SDK_CHECK(!failure && updates == std::vector{update}, "an independently observed model needs no invented provider");
    auto& model_only = payload.find("model_state")->find("updates")->as_array().front();
    SDK_CHECK(!model_only.find("provider"), "model-only evidence omits an unobserved provider");
    model_only.set("provider", addon::Json_value(""));
    addon::decode_model_updates(payload, updates, failure);
    SDK_CHECK(!failure && updates == std::vector{update}, "an explicitly empty provider is also absence");
    model_only.set("provider", addon::Json_value(7));
    addon::decode_model_updates(payload, updates, failure);
    SDK_CHECK(failure && updates.empty(), "an optional provider still requires a string when present");
    update = observed_model();
    update.status = addon::Model_status::UNAVAILABLE;
    SDK_CHECK(!addon::is_valid_model_update(update), "unavailable cannot retain a confirmed model");
    update.provider.clear();
    update.model.clear();
    update.reasoning_effort.reset();
    SDK_CHECK(addon::is_valid_model_update(update), "unsupported sampling has explicit empty evidence");
    for (const auto status : {addon::Model_status::UNKNOWN, addon::Model_status::UNAVAILABLE}) {
        update.status = status;
        addon::encode_model_updates(payload, {update});
        const auto* provider = payload.find("model_state")->find("updates")->as_array().front().find("provider");
        SDK_CHECK(provider && provider->is_string() && provider->as_string().empty(),
            "withdrawals retain the empty provider member required by older model-state readers");
        addon::decode_model_updates(payload, updates, failure);
        SDK_CHECK(!failure && updates == std::vector{update}, "withdrawals retain their attributed source order");
    }
    addon::encode_model_updates(payload, std::vector(17, observed_model()));
    addon::decode_model_updates(payload, updates, failure);
    SDK_CHECK(failure && updates.empty(), "oversized updates fail atomically");
    addon::encode_model_updates(payload, {observed_model()});
    payload.find("model_state")->find("updates")->as_array().front().set("reasoning_effort", addon::Json_value());
    addon::decode_model_updates(payload, updates, failure);
    SDK_CHECK(failure && updates.empty(), "an unobserved reasoning level is omitted, not an arbitrary JSON value");
    return true;
}

bool structured_contract()
{
    addon::Structured_inspect_request request;
    addon::Contract_failure core_failure;
    auto payload = addon::encode_structured_inspect_request(request);
    SDK_CHECK(!payload.find("model_state"), "older structured adapters receive no model request");
    request.model_state_version = addon::k_model_state_version;
    payload = addon::encode_structured_inspect_request(request);
    const auto decoded_request = addon::decode_structured_inspect_request(payload, &core_failure);
    SDK_CHECK(decoded_request && decoded_request->model_state_version == 1,
        "structured inspection independently negotiates native model observations");
    payload.find("model_state")->set("version", addon::Json_value(99));
    const auto unsupported = addon::decode_structured_inspect_request(payload, &core_failure);
    SDK_CHECK(unsupported && unsupported->model_state_version == 0 && unsupported->model_failure,
        "unsupported model sampling leaves the structured inspection usable");
    payload.set("model_state", addon::Json_value(std::string(131072, 'x')));
    const auto malformed_request = addon::decode_structured_inspect_request(payload, &core_failure);
    SDK_CHECK(malformed_request && malformed_request->model_state_version == 0 && malformed_request->model_failure,
        "oversized optional model sampling leaves a valid inspection request usable");

    addon::Structured_inspect_result result;
    result.complete = true;
    result.result_bytes = "accepted result";
    result.model_observation = addon::Structured_model_observation{"native-provider", "native-model", "native.response"};
    payload = addon::encode_structured_inspect_result(result);
    auto decoded = addon::decode_structured_inspect_result(payload, true, &core_failure);
    SDK_CHECK(decoded && decoded->model_observation == result.model_observation && !decoded->model_failure,
        "final inspection retains native model evidence alongside completion");
    result.model_observation->provider.clear();
    decoded = addon::decode_structured_inspect_result(addon::encode_structured_inspect_result(result), true, &core_failure);
    SDK_CHECK(decoded && decoded->model_observation == result.model_observation,
        "structured model identity remains valid when its native source omits backend provider");
    for (const auto& model : {std::string{}, std::string("bad\nmodel"), std::string(513, 'x'), std::string(1, '\xff')}) {
        auto malformed = payload;
        malformed.find("model_state")->find("observation")->set("model", addon::Json_value(model));
        decoded = addon::decode_structured_inspect_result(malformed, true, &core_failure);
        SDK_CHECK(decoded && decoded->result_bytes == result.result_bytes && !decoded->model_observation && decoded->model_failure,
            "malformed optional model cannot poison a valid completion");
    }
    auto unsupported_result = payload;
    unsupported_result.find("model_state")->set("version", addon::Json_value(99));
    decoded = addon::decode_structured_inspect_result(unsupported_result, true, &core_failure);
    SDK_CHECK(decoded && decoded->result_bytes == result.result_bytes && !decoded->model_observation && decoded->model_failure,
        "unsupported model evidence leaves the completed result usable");
    result.model_failure = addon::Contract_failure{addon::reason::k_unsupported_version, "model sampling unavailable"};
    decoded = addon::decode_structured_inspect_result(addon::encode_structured_inspect_result(result), true, &core_failure);
    SDK_CHECK(decoded && decoded->result_bytes == result.result_bytes && !decoded->model_observation &&
        decoded->model_failure && decoded->model_failure->reason_code == result.model_failure->reason_code,
        "an explicit model capability failure leaves the completed result usable");
    auto conflicted = payload;
    auto failure_payload = addon::encode_structured_inspect_result(result);
    conflicted.find("model_state")->set("failure", *failure_payload.find("model_state")->find("failure"));
    decoded = addon::decode_structured_inspect_result(conflicted, true, &core_failure);
    SDK_CHECK(decoded && decoded->result_bytes == result.result_bytes && !decoded->model_observation && decoded->model_failure,
        "a conflicting observation and failure have no model authority");
    result.model_failure.reset();
    result.result_bytes.reset();
    result.failure_reason = "native_failure";
    decoded = addon::decode_structured_inspect_result(addon::encode_structured_inspect_result(result), true, &core_failure);
    SDK_CHECK(decoded && decoded->failure_reason == result.failure_reason && decoded->model_observation,
        "native failure does not erase the model observed before completion");
    payload.erase("model_state");
    decoded = addon::decode_structured_inspect_result(payload, true, &core_failure);
    SDK_CHECK(decoded && !decoded->model_observation && !decoded->model_failure,
        "no observation is absence rather than an invented model");
    return true;
}

bool invalid_structured_observation_round_trip()
{
    addon::Structured_inspect_result result;
    result.complete = true;
    result.result_bytes = "accepted result";
    addon::Contract_failure core_failure;
    for (const auto& observation : {
            addon::Structured_model_observation{{}, std::string(513, 'x'), "native.response"},
            addon::Structured_model_observation{std::string(513, 'x'), "native-model", "native.response"},
            addon::Structured_model_observation{{}, "native-model", std::string(129, 'x')}})
    {
        result.model_observation = observation;
        const auto decoded = addon::decode_structured_inspect_result(
            addon::encode_structured_inspect_result(result), true, &core_failure);
        SDK_CHECK(decoded && decoded->complete && decoded->result_bytes == result.result_bytes && core_failure.reason_code.empty(),
            "invalid producer model evidence must preserve the completed inspection result");
        SDK_CHECK(!decoded->model_observation && decoded->model_failure &&
            decoded->model_failure->reason_code == addon::reason::k_observation_invalid,
            "invalid producer model evidence must report a capability-local failure");
    }
    result.result_bytes.reset();
    result.failure_reason = "native_failure";
    auto decoded = addon::decode_structured_inspect_result(
        addon::encode_structured_inspect_result(result), true, &core_failure);
    SDK_CHECK(decoded && decoded->complete && decoded->failure_reason == result.failure_reason &&
        !decoded->model_observation && decoded->model_failure && core_failure.reason_code.empty(),
        "invalid producer model evidence must not replace a native execution failure");
    result.model_observation.reset();
    const auto absent = addon::encode_structured_inspect_result(result);
    decoded = addon::decode_structured_inspect_result(absent, true, &core_failure);
    SDK_CHECK(!absent.find("model_state") && decoded && !decoded->model_observation && !decoded->model_failure,
        "an absent producer observation must remain distinct from invalid evidence");
    return true;
}

bool full_continuation_survives_structured_sampling()
{
    addon::Structured_inspect_result result;
    for (std::size_t i = 0; i < 1'048'366; ++i) {
        result.protocol_state += "\xe7\x95\x8c";
    }
    const auto core = addon::encode_structured_inspect_result(result);
    SDK_CHECK(addon::validate_session_exchange_payload(core, nullptr), "the core continuation fits its frame");
    result.model_observation = addon::Structured_model_observation{{}, std::string(512, 'm'), "native.response"};
    const auto sampled = addon::encode_structured_inspect_result(result);
    SDK_CHECK(addon::validate_session_exchange_payload(sampled, nullptr), "sampling preserves the response frame");
    addon::Contract_failure failure;
    const auto decoded = addon::decode_structured_inspect_result(sampled, false, &failure);
    SDK_CHECK(decoded && decoded->protocol_state == result.protocol_state && !decoded->model_observation,
        "a full continuation round-trips without its optional model");
    return true;
}

} // namespace

bool test_model_state_contracts()
{
    return source_authority() && optional_contract() && wire_bounds() && structured_contract() &&
        invalid_structured_observation_round_trip() && full_continuation_survives_structured_sampling();
}

} // namespace sdk_tests
