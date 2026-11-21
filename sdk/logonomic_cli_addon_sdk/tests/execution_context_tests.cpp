#include "sdk_test_support.h"

#include "logonomic_cli_addon/capability_ids.h"
#include "logonomic_cli_addon/execution_context.h"
#include "logonomic_cli_addon/helper_ingress.h"
#include "logonomic_cli_addon/lifecycle_events.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace sdk_tests {
namespace {

namespace addon = logonomic::cli_addon;

addon::Execution_context_update native_cwd()
{
    return {
        "conversation-a", "0123456789abcdef0123456789abcdef", 0,
        addon::Execution_context_status::CONFIRMED, "local",
#ifdef _WIN32
        "C:\\repo\\nested folder",
#else
        "/repo/nested folder",
#endif
        "native.SessionStart",
    };
}

bool check_activation_order()
{
    auto update = native_cwd();
    addon::Execution_context_state state;
    state.provider_session_id = update.provider_session_id;
    state.transition_nonce = update.transition_nonce;
    SDK_CHECK(addon::apply_execution_context_update(state, update) && state.cwd == update.cwd,
        "joint SessionStart establishes the exact nested default cwd");
    SDK_CHECK(!addon::apply_execution_context_update(state, update), "duplicate sequence zero is inert");

    const auto first_a = update;
    state = {};
    state.provider_session_id = "conversation-b";
    state.transition_nonce = "11111111111111111111111111111111";
    SDK_CHECK(!addon::apply_execution_context_update(state, first_a) && state.cwd.empty(),
        "a delayed A directory never initializes B");
    state = {};
    state.provider_session_id = first_a.provider_session_id;
    state.transition_nonce = "22222222222222222222222222222222";
    SDK_CHECK(!addon::apply_execution_context_update(state, first_a) && state.cwd.empty(),
        "returning to native ID A does not make an earlier A activation authoritative");

    update.transition_nonce = state.transition_nonce;
    update.sequence = 3;
    SDK_CHECK(addon::apply_execution_context_update(state, update), "the current A source can confirm cwd");
    auto old = update;
    old.sequence = 2;
    old.cwd += "/old";
    SDK_CHECK(!addon::apply_execution_context_update(state, old) && state.cwd == update.cwd,
        "source order rejects completion order inversion");

    auto loss = update;
    loss.sequence = 4;
    loss.status = addon::Execution_context_status::UNKNOWN;
    loss.environment.clear();
    loss.cwd.clear();
    SDK_CHECK(addon::apply_execution_context_update(state, loss) && state.cwd.empty() &&
        state.status == addon::Execution_context_status::UNKNOWN,
        "attributed explicit loss withdraws current cwd");
    SDK_CHECK(!addon::apply_execution_context_update(state, update), "loss retains the sequence replay barrier");
    update.sequence = 5;
    SDK_CHECK(addon::apply_execution_context_update(state, update), "new native evidence recovers after loss");
    addon::invalidate_execution_context(state);
    SDK_CHECK(state.cwd.empty() && state.sequence == 5 && !addon::apply_execution_context_update(state, update),
        "host observation loss preserves its replay barrier without inventing another source revision");
    return true;
}

bool check_optional_isolation()
{
    addon::Classify_event_request request;
    request.event.name = "SessionStart";
    request.event.body = addon::Json_value::object();
    request.transition_nonce = native_cwd().transition_nonce;
    auto legacy = addon::encode_classify_event_request(request);
    SDK_CHECK(!legacy.find("execution_context"), "old peers receive the exact legacy request shape");
    request.execution_context_version = addon::k_execution_context_version;
    auto encoded = addon::encode_classify_event_request(request);
    addon::Contract_failure core_failure;
    auto decoded = addon::decode_classify_event_request(encoded, &core_failure);
    SDK_CHECK(decoded && decoded->execution_context_version == 1 && !decoded->execution_failure,
        "supported optional request opts in independently");
    encoded.find("execution_context")->set("version", addon::Json_value(9));
    decoded = addon::decode_classify_event_request(encoded, &core_failure);
    SDK_CHECK(decoded && decoded->execution_context_version == 0 && decoded->execution_failure &&
        core_failure.reason_code.empty() && decoded->event.name == "SessionStart",
        "unsupported optional request preserves the adjacent native event");

    addon::Classify_event_result result;
    result.classification = addon::Classification::TRANSITION;
    result.transition = addon::Transition_kind::RESUME;
    addon::Lifecycle_observation started;
    started.provider_event_name = "SessionStart";
    started.session_transition = addon::Session_transition::STARTED;
    started.provider_session_id = "conversation-a";
    result.observations = {started};
    result.execution_updates = {native_cwd()};
    auto payload = addon::encode_classify_event_result(result);
    auto classified = addon::decode_classify_event_result(payload, &core_failure);
    SDK_CHECK(classified && !classified->execution_failure && classified->execution_updates == result.execution_updates,
        "joint identity and directory facts round trip together");
    payload.find("execution_context")->find("updates")->as_array().front().set("sequence", addon::Json_value(0));
    classified = addon::decode_classify_event_result(payload, &core_failure);
    SDK_CHECK(classified && classified->execution_failure && classified->execution_updates.empty() &&
        classified->classification == addon::Classification::TRANSITION && classified->observations.size() == 1 &&
        classified->observations.front().provider_session_id == "conversation-a" && core_failure.reason_code.empty(),
        "invalid optional ordering cannot discard valid native identity");

    addon::Observe_result observed;
    observed.resume_cursor = "cursor";
    observed.execution_updates = result.execution_updates;
    const auto observe = addon::decode_observe_result(addon::encode_observe_result(observed), &core_failure);
    SDK_CHECK(observe && observe->execution_updates == result.execution_updates && observe->resume_cursor == "cursor",
        "continuous observe shares the same optional contract");
    addon::Poll_observations_result polled;
    polled.execution_updates = result.execution_updates;
    const auto poll = addon::decode_poll_observations_result(
        addon::encode_poll_observations_result(polled), 16, &core_failure);
    SDK_CHECK(poll && poll->execution_updates == result.execution_updates,
        "directory-only observations do not require identity transitions");
    return true;
}

bool check_wire_bounds()
{
    auto update = native_cwd();
    update.sequence = std::numeric_limits<std::uint64_t>::max();
    auto payload = addon::Json_value::object();
    addon::encode_execution_updates(payload, {update});
    std::vector<addon::Execution_context_update> updates;
    std::optional<addon::Contract_failure> failure;
    addon::decode_execution_updates(payload, updates, failure);
    SDK_CHECK(!failure && updates == std::vector{update}, "full uint64 source order is exact decimal text");
    for (const char* invalid : {"01", "-1", "+1", "1.0", "18446744073709551616", ""}) {
        auto damaged = payload;
        damaged.find("execution_context")->find("updates")->as_array().front().set("sequence", addon::Json_value(invalid));
        addon::decode_execution_updates(damaged, updates, failure);
        SDK_CHECK(failure && updates.empty(), "noncanonical or overflowing source order is unavailable");
    }
    SDK_CHECK(!addon::is_execution_local_path("relative/subdirectory") &&
        !addon::is_execution_local_path("ssh://host/repo") && !addon::is_execution_local_path("/repo\nother"),
        "relative, remote namespace and control-bearing paths are not local evidence");
    update.environment = "remote";
    SDK_CHECK(!addon::is_valid_execution_context_update(update), "remote paths cannot masquerade as local cwd");
    update.status = addon::Execution_context_status::UNAVAILABLE;
    update.environment = "unsupported";
    update.cwd.clear();
    SDK_CHECK(addon::is_valid_execution_context_update(update), "unsupported namespaces have an explicit empty path");
    update.status = addon::Execution_context_status::UNKNOWN;
    update.environment.clear();
    SDK_CHECK(addon::is_valid_execution_context_update(update), "explicit unknown is distinct from omission");

    update = native_cwd();
    update.cwd.append(4096 - update.cwd.size(), 'x');
    update.source = std::string(128, 's');
    SDK_CHECK(addon::is_valid_execution_context_update(update), "exact path and source byte limits are accepted");
    update.cwd.push_back('x');
    SDK_CHECK(!addon::is_valid_execution_context_update(update), "oversized path is rejected");
    update.cwd = native_cwd().cwd;
    update.source.push_back('s');
    SDK_CHECK(!addon::is_valid_execution_context_update(update), "oversized source is rejected");
    for (const auto& source : {std::string{}, std::string("native\nSessionStart"), std::string(1, '\xff')}) {
        update.source = source;
        SDK_CHECK(!addon::is_valid_execution_context_update(update), "empty, control-bearing and invalid UTF-8 sources are rejected");
    }
    update = native_cwd();
    update.cwd.push_back('\xff');
    SDK_CHECK(!addon::is_valid_execution_context_update(update), "invalid UTF-8 cannot reach local filesystem probing");

    payload = addon::Json_value::object();
    addon::encode_execution_updates(payload, {});
    SDK_CHECK(!payload.find("execution_context"), "an empty result omits the optional envelope");
    addon::decode_execution_updates(payload, updates, failure);
    SDK_CHECK(!failure && updates.empty(), "omission has no source authority");
    addon::encode_execution_updates(payload, std::vector(16, native_cwd()));
    addon::decode_execution_updates(payload, updates, failure);
    SDK_CHECK(!failure && updates.size() == 16, "the declared update bound is accepted");
    addon::encode_execution_updates(payload, std::vector(17, native_cwd()));
    addon::decode_execution_updates(payload, updates, failure);
    SDK_CHECK(failure && updates.empty(), "an oversized batch is rejected atomically");
    addon::encode_execution_updates(payload, {native_cwd()});
    payload.find("execution_context")->find("updates")->as_array().front().set("future", addon::Json_value(true));
    addon::decode_execution_updates(payload, updates, failure);
    SDK_CHECK(failure && updates.empty(), "update fields belong to a closed versioned contract");
    addon::encode_execution_updates(payload, {native_cwd()});
    payload.find("execution_context")->set("version", addon::Json_value(2));
    addon::decode_execution_updates(payload, updates, failure);
    SDK_CHECK(failure && updates.empty(), "an unsupported result version cannot supply a current fact");
    return true;
}

} // namespace

bool test_execution_context_contracts()
{
    return check_activation_order() && check_optional_isolation() && check_wire_bounds();
}

} // namespace sdk_tests
