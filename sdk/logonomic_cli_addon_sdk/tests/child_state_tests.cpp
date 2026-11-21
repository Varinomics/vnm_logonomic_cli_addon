#include "sdk_test_support.h"

#include "logonomic_cli_addon/capability_ids.h"
#include "logonomic_cli_addon/child_state.h"
#include "logonomic_cli_addon/helper_ingress.h"
#include "logonomic_cli_addon/lifecycle_events.h"

#include <limits>

namespace sdk_tests {
namespace {

namespace addon = logonomic::cli_addon;

addon::Child_update observed_child()
{
    return {"native-a", "0123456789abcdef0123456789abcdef", 1, "child-1", {}, 1,
        addon::Child_status::RUNNING, "Inspect the native build", "read", 0.25, "native.task_progress"};
}

bool optional_contract()
{
    addon::Classify_event_request request;
    request.event.name = "native_progress";
    request.transition_nonce = observed_child().transition_nonce;
    auto payload = addon::encode_classify_event_request(request);
    SDK_CHECK(!payload.find("child_state"), "older peers receive no optional child envelope");
    request.child_state_version = addon::k_child_state_version;
    addon::Contract_failure core_failure;
    auto decoded = addon::decode_classify_event_request(addon::encode_classify_event_request(request), &core_failure);
    SDK_CHECK(decoded && decoded->child_state_version == 1 && !decoded->child_failure,
        "child state negotiates independently of helper ingress");
    payload = addon::encode_classify_event_request(request);
    payload.find("child_state")->set("version", addon::Json_value(2));
    decoded = addon::decode_classify_event_request(payload, &core_failure);
    SDK_CHECK(decoded && decoded->child_state_version == 0 && decoded->child_failure && core_failure.reason_code.empty(),
        "unsupported child data cannot reject the native event");

    addon::Classify_event_result result;
    result.classification = addon::Classification::EVENT;
    addon::Lifecycle_observation started;
    started.provider_event_name = "native_progress";
    started.provider_session_id = "native-a";
    started.subagent_transition = addon::Subagent_transition::STARTED;
    started.agent_id = "child-1";
    result.observations = {started};
    result.child_updates = {observed_child()};
    payload = addon::encode_classify_event_result(result);
    auto classified = addon::decode_classify_event_result(payload, &core_failure, 2);
    SDK_CHECK(classified && classified->child_updates == result.child_updates && !classified->child_failure,
        "helper-ingress@2 retains native admission and independent child metadata");
    payload.find("child_state")->find("updates")->as_array().front().set("progress", addon::Json_value(2));
    classified = addon::decode_classify_event_result(payload, &core_failure, 2);
    SDK_CHECK(classified && classified->child_updates.empty() && classified->child_failure &&
        classified->observations.size() == 1 && core_failure.reason_code.empty(),
        "invalid child metadata cannot discard an adjacent child start");

    addon::Observe_result observed;
    observed.child_updates = result.child_updates;
    auto window = addon::decode_observe_result(addon::encode_observe_result(observed), &core_failure);
    SDK_CHECK(window && window->child_updates == result.child_updates, "continuous windows share the optional child codec");
    addon::Poll_observations_result polled;
    polled.child_updates = result.child_updates;
    auto poll = addon::decode_poll_observations_result(addon::encode_poll_observations_result(polled), 16, &core_failure);
    SDK_CHECK(poll && poll->child_updates == result.child_updates, "child-only polling needs no invented root transition");
    return true;
}

bool attributed_loss()
{
    const auto child = observed_child();
    addon::Child_loss source{child.provider_session_id, child.transition_nonce, 8,
        "observation_gap", "native.nested_snapshot"};
    auto payload = addon::Json_value::object();
    addon::encode_child_updates(payload, {}, source);
    std::vector<addon::Child_update> updates;
    std::optional<addon::Child_loss> loss;
    std::optional<addon::Contract_failure> failure;
    addon::decode_child_updates(payload, updates, loss, failure);
    SDK_CHECK(!failure && updates.empty() && loss == source,
        "child loss preserves root activation and source sequence");
    auto damaged = payload;
    damaged.find("child_state")->set("updates", addon::Json_value::array());
    addon::decode_child_updates(damaged, updates, loss, failure);
    SDK_CHECK(failure && !loss && updates.empty(), "loss and samples cannot race within one envelope");
    damaged = payload;
    damaged.find("child_state")->find("loss")->set("transition_nonce", addon::Json_value("current"));
    addon::decode_child_updates(damaged, updates, loss, failure);
    SDK_CHECK(failure && !loss, "loss cannot attribute itself to whichever activation is current on arrival");

    addon::Classify_event_result result;
    result.lifecycle_degradation = addon::Lifecycle_degradation{};
    result.child_loss = source;
    addon::Contract_failure core_failure;
    const auto classified = addon::decode_classify_event_result(addon::encode_classify_event_result(result), &core_failure, 2);
    SDK_CHECK(classified && classified->lifecycle_degradation && classified->child_loss == source,
        "optional child loss preserves the established whole-lifecycle loss contract");
    return true;
}

bool wire_bounds()
{
    auto child = observed_child();
    child.sequence = std::numeric_limits<std::uint64_t>::max();
    auto payload = addon::Json_value::object();
    addon::encode_child_updates(payload, {child});
    std::vector<addon::Child_update> updates;
    std::optional<addon::Child_loss> loss;
    std::optional<addon::Contract_failure> failure;
    addon::decode_child_updates(payload, updates, loss, failure);
    SDK_CHECK(!failure && updates == std::vector{child}, "source ordering retains the full uint64 range");
    for (const char* invalid : {"01", "-1", "+1", "1.0", "18446744073709551616", ""}) {
        auto damaged = payload;
        damaged.find("child_state")->find("updates")->as_array().front().set("sequence", addon::Json_value(invalid));
        addon::decode_child_updates(damaged, updates, loss, failure);
        SDK_CHECK(failure && updates.empty(), "noncanonical child source order is refused");
    }
    child = observed_child();
    auto nested = child;
    nested.agent_id = "independent-native-id";
    nested.parent_agent_id = child.agent_id;
    nested.depth = 2;
    nested.sequence = 2;
    nested.status = addon::Child_status::FAILED;
    nested.description = "Read failed\nNative reason\tkept as text";
    addon::encode_child_updates(payload, {child, nested});
    addon::decode_child_updates(payload, updates, loss, failure);
    SDK_CHECK(!failure && updates == std::vector({child, nested}), "native structural parentage and multiline description round trip");
    for (const auto& invalid : {std::string(1, '\xff'), std::string(513, 'x'), std::string("text\x01")}) {
        nested.description = invalid;
        SDK_CHECK(!addon::is_valid_child_update(nested), "invalid UTF-8, control bytes and oversized descriptions are refused");
    }
    nested = child;
    nested.parent_agent_id = child.agent_id;
    SDK_CHECK(!addon::is_valid_child_update(nested), "a child cannot be its own parent");
    nested = child;
    nested.depth = 2;
    SDK_CHECK(!addon::is_valid_child_update(nested), "depth cannot contradict explicit root parentage");
    for (const double invalid : {-0.01, 1.01, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        nested = child;
        nested.progress = invalid;
        SDK_CHECK(!addon::is_valid_child_update(nested), "progress must be an observed finite fraction");
    }
    nested = child;
    nested.agent_id = "child-2";
    addon::encode_child_updates(payload, {child, nested});
    addon::decode_child_updates(payload, updates, loss, failure);
    SDK_CHECK(failure && updates.empty(), "one source sequence cannot identify two different child samples");
    nested.sequence = 2;
    nested.provider_session_id = "native-b";
    addon::encode_child_updates(payload, {child, nested});
    addon::decode_child_updates(payload, updates, loss, failure);
    SDK_CHECK(failure && updates.empty(), "one batch cannot combine different root activations");
    addon::encode_child_updates(payload, std::vector(17, child));
    addon::decode_child_updates(payload, updates, loss, failure);
    SDK_CHECK(failure && updates.empty(), "oversized optional batches fail atomically");
    return true;
}

} // namespace

bool test_child_state_contracts()
{
    return optional_contract() && attributed_loss() && wire_bounds();
}

} // namespace sdk_tests
