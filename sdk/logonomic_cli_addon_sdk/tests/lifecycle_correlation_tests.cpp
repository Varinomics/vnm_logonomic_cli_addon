#include "sdk_test_support.h"
#include "logonomic_cli_addon/capability_ids.h"
#include "logonomic_cli_addon/helper_ingress.h"
#include "logonomic_cli_addon/lifecycle_events.h"
#include "logonomic_cli_addon/limits.h"

namespace sdk_tests {

bool test_lifecycle_correlation_contracts()
{
    namespace addon = logonomic::cli_addon;
    addon::Contract_failure failure;
    addon::Observe_request observe;
    observe.session = {"/same/workspace", {"unknown-cli"}, "same-session-id"};
    observe.window_ms = addon::limits::k_observe_window_ms;
    observe.correlation = addon::Session_correlation{"0123456789abcdef0123456789abcdef"};
    const auto encoded = addon::encode_observe_request(observe);
    const auto decoded = addon::decode_observe_request(encoded, &failure);
    SDK_CHECK(decoded && decoded->resume_cursor.empty() && decoded->correlation &&
        decoded->correlation->transition_nonce == observe.correlation->transition_nonce,
        "first correlated watch preserves empty cursor and exact authenticated transition");
    auto unbound_observe = observe;
    unbound_observe.correlation.reset();
    const auto unbound_payload = addon::encode_observe_request(unbound_observe);
    const auto unbound = addon::decode_observe_request(unbound_payload, &failure);
    SDK_CHECK(!unbound_payload.find("correlation") && unbound && !unbound->correlation &&
        unbound->resume_cursor.empty(), "an unbound watch retains its empty initial cursor");

    addon::Poll_observations_request poll;
    poll.limit = 16;
    poll.correlation = observe.correlation;
    auto poll_payload = addon::encode_poll_observations_request(poll);
    auto decoded_poll = addon::decode_poll_observations_request(poll_payload, &failure);
    SDK_CHECK(decoded_poll && decoded_poll->resume_cursor.empty() && decoded_poll->correlation,
        "first poll routes by correlation before any package cursor exists");
    auto unbound_poll = poll;
    unbound_poll.correlation.reset();
    const auto unbound_poll_payload = addon::encode_poll_observations_request(unbound_poll);
    const auto decoded_unbound_poll = addon::decode_poll_observations_request(unbound_poll_payload, &failure);
    SDK_CHECK(!unbound_poll_payload.find("correlation") && decoded_unbound_poll &&
        !decoded_unbound_poll->correlation && decoded_unbound_poll->resume_cursor.empty(),
        "an unbound poll retains its empty initial cursor");
    for (const auto& nonce : {std::string{}, std::string(31, 'a'), std::string(33, 'a'),
                             std::string(32, 'z'), std::string(32, 'A')})
    {
        poll.correlation->transition_nonce = nonce;
        SDK_CHECK(!addon::decode_poll_observations_request(
            addon::encode_poll_observations_request(poll), &failure),
            "invalid transition identities cannot select another session");
    }
    poll.correlation = addon::Session_correlation{"fedcba9876543210fedcba9876543210"};
    const auto other = addon::decode_poll_observations_request(
        addon::encode_poll_observations_request(poll), &failure);
    SDK_CHECK(other && other->correlation->transition_nonce != decoded->correlation->transition_nonce,
        "same provider ID and workspace can retain distinct authenticated home bindings");
    auto correlation = addon::Json_value::object();
    correlation.set("version", addon::Json_value(std::int64_t{2}));
    correlation.set("transition_nonce", addon::Json_value(poll.correlation->transition_nonce));
    poll_payload.set("correlation", correlation);
    SDK_CHECK(!addon::decode_poll_observations_request(poll_payload, &failure),
        "unknown correlation schema is rejected");
    correlation.set("version", addon::Json_value(std::int64_t{1}));
    correlation.set("home", addon::Json_value("/untrusted/home"));
    poll_payload.set("correlation", correlation);
    SDK_CHECK(!addon::decode_poll_observations_request(poll_payload, &failure),
        "correlation carries no caller supplied replacement home");
    addon::Lifecycle_observation child;
    child.provider_event_name = "AgentStart";
    child.provider_session_id = "same-session-id";
    child.subagent_transition = addon::Subagent_transition::STARTED;
    child.agent_id = "child";
    child.agent_label = std::string(256, 'a');
    SDK_CHECK(addon::decode_lifecycle_observation(
        addon::encode_lifecycle_observation(child), &failure, addon::Lifecycle_observation_policy::CONTINUOUS).has_value(),
        "continuous observations preserve a native 256-unit child label");
    SDK_CHECK(!addon::decode_lifecycle_observation(
        addon::encode_lifecycle_observation(child), &failure, addon::Lifecycle_observation_policy::HELPER_INGRESS),
        "helper observations retain the 160-byte label limit");
    child.agent_label.push_back('a');
    SDK_CHECK(!addon::decode_lifecycle_observation(
        addon::encode_lifecycle_observation(child), &failure, addon::Lifecycle_observation_policy::CONTINUOUS),
        "continuous observations do not admit 257 ASCII units");
    child.agent_label.clear();
    for (int index = 0; index < 256; ++index) {
        child.agent_label += "\xe4\xb8\xad";
    }
    addon::Poll_observations_result page{{child}, "next", false};
    SDK_CHECK(addon::decode_poll_observations_result(
        addon::encode_poll_observations_result(page), 16, &failure).has_value(),
        "768 UTF-8 bytes representing 256 BMP units survive continuous polling");
    child.agent_label.clear();
    for (int index = 0; index < 128; ++index) {
        child.agent_label += "\xf0\x9f\x98\x80";
    }
    SDK_CHECK(addon::decode_lifecycle_observation(
        addon::encode_lifecycle_observation(child), &failure, addon::Lifecycle_observation_policy::CONTINUOUS).has_value(),
        "128 astral characters count as exactly 256 UTF-16 units");
    child.agent_label += "a";
    SDK_CHECK(!addon::decode_lifecycle_observation(
        addon::encode_lifecycle_observation(child), &failure, addon::Lifecycle_observation_policy::CONTINUOUS),
        "a BMP character after 128 astral characters exceeds the native bound");
    child.agent_label = "\xed\xa0\x80";
    SDK_CHECK(!addon::decode_lifecycle_observation(
        addon::encode_lifecycle_observation(child), &failure, addon::Lifecycle_observation_policy::CONTINUOUS),
        "unpaired surrogate UTF-8 cannot bypass the unit counter");
    for (const auto helper_version : {addon::k_helper_ingress_degradation_version,
             addon::k_helper_ingress_subagent_reconciliation_version})
    {
        addon::Classify_event_result classified;
        classified.classification = addon::Classification::EVENT;
        child.agent_label = std::string(addon::limits::k_max_lifecycle_label_bytes, 'a');
        classified.observations = {child};
        SDK_CHECK(addon::decode_classify_event_result(
            addon::encode_classify_event_result(classified), &failure, helper_version).has_value(),
            "helper classification admits a 160-byte agent label");
        classified.observations.front().agent_label.push_back('a');
        SDK_CHECK(!addon::decode_classify_event_result(
            addon::encode_classify_event_result(classified), &failure, helper_version) &&
            failure.reason_code == addon::reason::k_payload_too_large,
            "helper classification rejects a 161-byte agent label");
    }
    return true;
}

} // namespace sdk_tests
