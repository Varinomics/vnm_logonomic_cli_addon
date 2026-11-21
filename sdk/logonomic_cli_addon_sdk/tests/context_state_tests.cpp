#include "sdk_test_support.h"

#include "logonomic_cli_addon/capability_ids.h"
#include "logonomic_cli_addon/context_state.h"
#include "logonomic_cli_addon/helper_ingress.h"
#include "logonomic_cli_addon/lifecycle_events.h"

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace sdk_tests {
namespace {

template <typename Decode, typename Encode>
bool check_request_context_isolation(
    const char* core_json, const char* core_member, const logonomic::cli_addon::Json_value& invalid_core,
    Decode decode, Encode encode)
{
    namespace addon = logonomic::cli_addon;
    const auto core = addon::parse_json(core_json, {}, nullptr);
    SDK_CHECK(core.has_value(), "the fixture uses the documented core request grammar");
    const struct
    {
        std::optional<addon::Json_value> version;
        std::uint32_t available_version;
        bool unavailable;
    }
    cases[] = {
        {std::nullopt,           0, false},
        {addon::Json_value(2),   2, false},
        {addon::Json_value(1),   0, true},
        {addon::Json_value(3),   0, true},
        {addon::Json_value("2"), 0, true},
        {addon::Json_value(),    0, true},
        {addon::Json_value(0),   0, true},
        {addon::Json_value(1.5), 0, true},
    };
    for (const auto& context : cases) {
        auto payload = *core;
        if (context.version) {
            payload.set("context_state_version", *context.version);
        }
        addon::Contract_failure failure;
        const auto decoded = decode(payload, &failure);
        SDK_CHECK(decoded.has_value(), "optional context versions must not discard a valid base request");
        SDK_CHECK(decoded->context_state_version == context.available_version,
            "only a supported context version enables telemetry");
        SDK_CHECK(decoded->context_failure.has_value() == context.unavailable,
            "unavailable context is distinct from an absent or supported opt-in");
        if (context.unavailable) {
            SDK_CHECK(decoded->context_failure->reason_code == addon::reason::k_unsupported_version,
                "an unavailable optional version retains its capability-local failure reason");
        }
        auto preserved_core = encode(*decoded);
        preserved_core.erase("context_state_version");
        SDK_CHECK(preserved_core == *core, "the exact base request facts survive optional context decoding");
        SDK_CHECK(failure.reason_code.empty(), "optional context failures do not use the core failure channel");

        payload.set(core_member, invalid_core);
        SDK_CHECK(!decode(payload, &failure) && failure.reason_code == addon::reason::k_frame_violation,
            "malformed core facts still fail with their own reason for every optional context version");
    }
    return true;
}

bool check_request_context_contracts()
{
    namespace addon = logonomic::cli_addon;
    SDK_CHECK(check_request_context_isolation(
        R"({"event":{"name":"SessionStart","body":{"session_id":"session-a"}},)"
        R"("ledger":{"current_epoch":3,"current_transition":{"kind":"resume",)"
        R"("provider_session_id":"session-a","nonce":"fedcba9876543210fedcba9876543210"}},)"
        R"("transition_nonce":"0123456789abcdef0123456789abcdef"})",
        "event", addon::Json_value("invalid-event"),
        addon::decode_classify_event_request, addon::encode_classify_event_request),
        "classify_event isolates its optional context request");
    SDK_CHECK(check_request_context_isolation(
        R"({"session":{"working_directory":"/workspace","launcher_argv":["provider","--resume","session-a"],)"
        R"("provider_session_id":"session-a"},"resume_cursor":"cursor-a","window_ms":1000,)"
        R"("correlation":{"version":1,"transition_nonce":"0123456789abcdef0123456789abcdef"}})",
        "window_ms", addon::Json_value(0), addon::decode_observe_request, addon::encode_observe_request),
        "observe isolates its optional context request");
    SDK_CHECK(check_request_context_isolation(
        R"({"resume_cursor":"cursor-a","limit":16,)"
        R"("correlation":{"version":1,"transition_nonce":"0123456789abcdef0123456789abcdef"}})",
        "limit", addon::Json_value(0),
        addon::decode_poll_observations_request, addon::encode_poll_observations_request),
        "poll_observations isolates its optional context request");
    return true;
}

bool check_count_revision_grammar()
{
    namespace addon = logonomic::cli_addon;
    constexpr std::uint64_t max_revision = 9007199254740991;
    addon::Context_update snapshot;
    snapshot.provider_session_id       = "session-a";
    snapshot.event_id                  = "history-snapshot";
    snapshot.compaction_count          = 3;
    snapshot.compaction_count_complete = true;
    for (const std::uint64_t revision : {std::uint64_t(0), max_revision}) {
        snapshot.compaction_count_revision = revision;
        auto payload = addon::Json_value::object();
        addon::encode_context_updates(payload, {snapshot});
        std::vector<addon::Context_update> updates;
        std::optional<addon::Context_failure> failure;
        addon::decode_context_updates(payload, updates, failure);
        SDK_CHECK(!failure && updates.size() == 1 &&
            updates.front().compaction_count_revision == revision &&
            updates.front().compaction_count == 3 && updates.front().compaction_count_complete,
            "count revisions retain their exact values at both integer bounds");
    }

    const std::optional<addon::Json_value> invalid_revisions[] = {
        std::nullopt,
        addon::Json_value(),
        addon::Json_value(-1),
        addon::Json_value(1.5),
        addon::Json_value("7"),
        addon::Json_value(true),
        addon::Json_value(max_revision + 1),
    };
    for (const auto& revision : invalid_revisions) {
        addon::Classify_event_result result;
        result.context_updates = {snapshot};
        auto payload = addon::encode_classify_event_result(result);
        auto& update = payload.find("context_updates")->as_array().front();
        update.erase("compaction_count_revision");
        if (revision) {
            update.set("compaction_count_revision", *revision);
        }
        addon::Contract_failure failure;
        const auto decoded = addon::decode_classify_event_result(payload, &failure);
        SDK_CHECK(decoded && decoded->context_failure && decoded->context_updates.empty() &&
            decoded->classification == addon::Classification::IGNORED && failure.reason_code.empty(),
            "a missing or invalid count revision fails only the optional capability");
        SDK_CHECK(decoded->context_failure->failure.reason_code == addon::reason::k_observation_invalid,
            "invalid count evidence retains its capability-local failure reason");
    }

    snapshot.compaction_count.reset();
    snapshot.compaction_count_complete = false;
    snapshot.used_fraction             = 0.25;
    auto payload = addon::Json_value::object();
    addon::encode_context_updates(payload, {snapshot});
    std::vector<addon::Context_update> updates;
    std::optional<addon::Context_failure> failure;
    addon::decode_context_updates(payload, updates, failure);
    SDK_CHECK(failure && updates.empty(), "a count revision cannot stand alone beside unrelated usage");
    return true;
}

bool check_count_revision_ordering()
{
    namespace addon = logonomic::cli_addon;
    addon::Context_state state;
    addon::Context_update snapshot;
    snapshot.provider_session_id       = "session-a";
    snapshot.event_id                  = "complete-history";
    snapshot.compaction_count          = 3;
    snapshot.compaction_count_revision = 5;
    snapshot.compaction_count_complete = true;
    addon::apply_context_update(state, snapshot);

    auto partial = snapshot;
    partial.event_id                  = "older-partial-history";
    partial.compaction_count          = 1;
    partial.compaction_count_revision = 4;
    partial.compaction_count_complete = false;
    addon::apply_context_update(state, partial);
    SDK_CHECK(state.compaction_count == 3 && state.compaction_count_complete &&
        state.compaction_count_revision == 5,
        "a delayed lower partial snapshot cannot withdraw newer complete history");

    partial.event_id                  = "newer-partial-history";
    partial.compaction_count_revision = 6;
    addon::apply_context_update(state, partial);
    SDK_CHECK(state.compaction_count == 3 && !state.compaction_count_complete &&
        state.compaction_count_revision == 6,
        "newer partial history withdraws completeness even when its count is lower");
    addon::apply_context_update(state, snapshot);
    addon::apply_context_update(state, partial);
    SDK_CHECK(state.compaction_count == 3 && !state.compaction_count_complete &&
        state.compaction_count_revision == 6,
        "older complete history and duplicate partial history cannot undo withdrawal");

    snapshot.event_id                  = "restored-complete-history";
    snapshot.compaction_count_revision = 7;
    addon::apply_context_update(state, snapshot);
    SDK_CHECK(state.compaction_count == 3 && state.compaction_count_complete &&
        state.compaction_count_revision == 7,
        "newer complete history can restore completeness without increasing the count");
    addon::apply_context_update(state, partial);
    addon::apply_context_update(state, snapshot);
    SDK_CHECK(state.compaction_count == 3 && state.compaction_count_complete &&
        state.compaction_count_revision == 7,
        "delayed partial history and duplicate complete history preserve the restored state");

    snapshot.event_id                  = "next-complete-history";
    snapshot.compaction_count          = 4;
    snapshot.compaction_count_revision = 8;
    addon::apply_context_update(state, snapshot);
    SDK_CHECK(state.compaction_count == 4 && state.compaction_count_complete,
        "a newer absolute snapshot still advances a monotonic count");
    snapshot.compaction_count          = 1;
    snapshot.compaction_count_revision = 9;
    addon::apply_context_update(state, snapshot);
    SDK_CHECK(state.compaction_count == 4 && !state.compaction_count_complete &&
        state.compaction_count_revision == 9,
        "a nominally complete truncated history cannot contradict the retained lower bound");
    snapshot.compaction_count          = 4;
    snapshot.compaction_count_revision = 10;
    addon::apply_context_update(state, snapshot);
    SDK_CHECK(state.compaction_count_complete,
        "a fresh complete read covering the retained count can restore completeness");
    snapshot.provider_session_id       = "session-b";
    snapshot.compaction_count          = 0;
    snapshot.compaction_count_revision = 0;
    addon::apply_context_update(state, snapshot);
    SDK_CHECK(state.compaction_count == 0 && state.compaction_count_complete &&
        state.compaction_count_revision == 0,
        "another provider session starts its own count and revision order");
    return true;
}

bool check_snapshot_request_contract()
{
    namespace addon = logonomic::cli_addon;
    addon::Classify_event_request request;
    request.event.name = "native-context";
    request.transition_nonce = "0123456789abcdef0123456789abcdef";
    request.context_state_version = 2;
    for (const std::uint64_t revision : {std::uint64_t(1), std::uint64_t(9007199254740991)}) {
        request.context_snapshot_revision = revision;
        addon::Contract_failure core_failure;
        const auto decoded = addon::decode_classify_event_request(
            addon::encode_classify_event_request(request), &core_failure);
        SDK_CHECK(decoded && !decoded->context_failure && decoded->context_snapshot_revision == revision &&
            core_failure.reason_code.empty(), "fresh-read request bounds round-trip exactly");
    }
    const addon::Json_value invalid[] = {
        addon::Json_value(0), addon::Json_value(-1), addon::Json_value(1.5), addon::Json_value("1"),
        addon::Json_value(), addon::Json_value(true), addon::Json_value(std::uint64_t(9007199254740992)),
    };
    for (const auto& revision : invalid) {
        auto payload = addon::encode_classify_event_request(request);
        payload.set("context_snapshot_revision", revision);
        addon::Contract_failure core_failure;
        const auto decoded = addon::decode_classify_event_request(payload, &core_failure);
        SDK_CHECK(decoded && decoded->context_failure && decoded->context_state_version == 0 &&
            !decoded->context_snapshot_revision && decoded->event.name == request.event.name &&
            core_failure.reason_code.empty(), "invalid sampling metadata disables only optional telemetry");
    }
    request.context_state_version = 0;
    addon::Contract_failure core_failure;
    const auto unrequested = addon::decode_classify_event_request(
        addon::encode_classify_event_request(request), &core_failure);
    SDK_CHECK(unrequested && unrequested->context_failure && !unrequested->context_snapshot_revision,
        "sampling authority requires explicit context-state opt-in");
    return true;
}

bool check_scoped_failure_contract()
{
    namespace addon = logonomic::cli_addon;
    addon::Classify_event_result classified;
    classified.context_failure = addon::Context_failure{"session-a",
        {addon::reason::k_observation_invalid, "native context is unreadable"}};
    addon::Contract_failure core_failure;
    const auto decoded = addon::decode_classify_event_result(
        addon::encode_classify_event_result(classified), &core_failure);
    SDK_CHECK(decoded && decoded->context_failure && decoded->context_updates.empty() &&
        decoded->context_failure->provider_session_id == "session-a" &&
        decoded->context_failure->failure.detail == "native context is unreadable" &&
        core_failure.reason_code.empty(), "a context-only failure preserves its source and core result");

    addon::Observe_result observed;
    observed.resume_cursor = "window-next";
    observed.context_failure = classified.context_failure;
    const auto window = addon::decode_observe_result(addon::encode_observe_result(observed), &core_failure);
    SDK_CHECK(window && window->resume_cursor == "window-next" && window->context_failure &&
        window->context_failure->provider_session_id == "session-a", "observe retains scoped optional loss");
    addon::Poll_observations_result polled;
    polled.resume_cursor = "poll-next";
    polled.context_failure = classified.context_failure;
    addon::Lifecycle_observation working;
    working.provider_event_name = "native-working";
    working.provider_session_id = "session-a";
    working.phase_transition = addon::Phase_transition::WORKING;
    working.phase_evidence = addon::Evidence::CONFIRMED;
    polled.observations.push_back(working);
    const auto batch = addon::decode_poll_observations_result(
        addon::encode_poll_observations_result(polled), 16, &core_failure);
    SDK_CHECK(batch && batch->observations.size() == 1 && batch->context_failure &&
        batch->context_failure->provider_session_id == "session-a",
        "valid continuous lifecycle remains beside scoped optional failure");

    auto wire = addon::encode_classify_event_result(classified);
    wire.find("context_failure")->erase("provider_session_id");
    const auto unscoped = addon::decode_classify_event_result(wire, &core_failure);
    SDK_CHECK(unscoped && unscoped->context_failure && unscoped->context_failure->provider_session_id.empty(),
        "an unscoped diagnostic does not acquire the current host's session identity");
    wire.find("context_failure")->set("provider_session_id", addon::Json_value(42));
    const auto invalid_scope = addon::decode_classify_event_result(wire, &core_failure);
    SDK_CHECK(invalid_scope && invalid_scope->context_failure &&
        invalid_scope->context_failure->provider_session_id.empty(), "invalid failure scope has no mutation authority");

    const auto malformed = addon::parse_json(
        R"({"classification":"ignore","observations":[],"context_updates":[)"
        R"({"provider_session_id":"session-a","event_id":"bad","used_fraction":2}]})", {}, nullptr);
    SDK_CHECK(malformed.has_value(), "malformed telemetry fixture is valid JSON");
    const auto source_loss = addon::decode_classify_event_result(*malformed, &core_failure);
    SDK_CHECK(source_loss && source_loss->context_failure &&
        source_loss->context_failure->provider_session_id == "session-a",
        "malformed optional measurement retains its independently valid source identity");
    auto mixed = *malformed;
    auto foreign = mixed.find("context_updates")->as_array().front();
    foreign.set("provider_session_id", addon::Json_value("session-b"));
    mixed.find("context_updates")->push_back(std::move(foreign));
    const auto mixed_loss = addon::decode_classify_event_result(mixed, &core_failure);
    SDK_CHECK(mixed_loss && mixed_loss->context_failure && mixed_loss->context_failure->provider_session_id.empty(),
        "mixed-source malformed telemetry cannot withdraw either session's measurements");
    wire.set("context_updates", addon::Json_value::array());
    const auto conflicting = addon::decode_classify_event_result(wire, &core_failure);
    SDK_CHECK(conflicting && conflicting->context_failure && conflicting->context_failure->provider_session_id.empty(),
        "failure and updates together are a capability-local diagnostic without source authority");
    return true;
}

} // namespace

bool test_context_state_contracts()
{
    SDK_CHECK(check_request_context_contracts(), "optional context request isolation");
    SDK_CHECK(check_count_revision_grammar(), "count evidence codec contract");
    SDK_CHECK(check_count_revision_ordering(), "count evidence ordering contract");
    SDK_CHECK(check_snapshot_request_contract(), "causal current-read request contract");
    SDK_CHECK(check_scoped_failure_contract(), "source-scoped capability failure contract");
    namespace addon = logonomic::cli_addon;
    addon::Contract_failure failure;
    addon::Classify_event_request request;
    request.event.name = "native-context";
    request.transition_nonce = "0123456789abcdef0123456789abcdef";
    const auto old_payload = addon::encode_classify_event_request(request);
    const auto old_request = addon::decode_classify_event_request(old_payload, &failure);
    SDK_CHECK(old_request && old_request->context_state_version == 0 &&
        !old_payload.find("context_state_version"),
        "an old host does not opt into the optional capability");
    request.context_state_version = addon::k_context_state_capability_version;
    const auto requested = addon::decode_classify_event_request(
        addon::encode_classify_event_request(request), &failure);
    SDK_CHECK(requested && requested->context_state_version == 2,
        "the explicit capability version survives helper classification");

    addon::Context_update usage;
    usage.provider_session_id = "session-a";
    usage.event_id = "usage-1";
    usage.used_tokens = 25000;
    usage.context_window = 100000;
    usage.compaction_count = 0;
    usage.compaction_count_revision = 0;
    usage.compaction_count_complete = true;
    addon::Classify_event_result result;
    SDK_CHECK(!addon::encode_classify_event_result(result).find("context_updates"),
        "an empty optional capability preserves the original result grammar");
    result.context_updates.push_back(usage);
    auto decoded = addon::decode_classify_event_result(addon::encode_classify_event_result(result), &failure);
    SDK_CHECK(decoded && !decoded->context_failure && decoded->observations.empty() &&
        decoded->context_updates.size() == 1,
        "context-only results do not invent lifecycle observations");

    addon::Context_state state;
    addon::apply_context_update(state, decoded->context_updates.front());
    SDK_CHECK(state.used_fraction == 0.25 && state.compaction_count == 0 && state.compaction_count_complete,
        "current occupancy is current tokens divided by this session's actual window");
    addon::Context_update compacted;
    compacted.provider_session_id = "session-a";
    compacted.event_id = "successful-summary-1";
    compacted.compacted = true;
    addon::apply_context_update(state, compacted);
    SDK_CHECK(state.compaction_count == 1 && state.compaction_count_complete && !state.used_fraction,
        "a confirmed compaction counts once and invalidates pre-compaction occupancy");
    usage.event_id = "usage-2";
    usage.used_tokens = 10000;
    usage.compaction_count.reset();
    usage.compaction_count_revision.reset();
    usage.compaction_count_complete = false;
    addon::apply_context_update(state, usage);
    addon::apply_context_update(state, compacted);
    SDK_CHECK(state.compaction_count == 1 && state.used_fraction == 0.1,
        "replayed successful compaction neither recounts nor clears a newer measurement");
    addon::Context_update uncertain;
    uncertain.provider_session_id = "session-a";
    uncertain.event_id = "history-gap";
    uncertain.compaction_count = 1;
    uncertain.compaction_count_revision = 1;
    addon::apply_context_update(state, uncertain);
    SDK_CHECK(!state.compaction_count_complete,
        "a current snapshot can withdraw completeness when native history becomes uncertain");
    uncertain.event_id = "older-snapshot";
    uncertain.compaction_count = 0;
    uncertain.compaction_count_revision = 0;
    uncertain.compaction_count_complete = true;
    addon::apply_context_update(state, uncertain);
    SDK_CHECK(state.compaction_count == 1 && !state.compaction_count_complete,
        "an older absolute snapshot cannot overwrite current count or completeness");
    addon::apply_context_update(state, compacted);
    SDK_CHECK(state.compaction_count == 1 && state.used_fraction == 0.1,
        "history updates preserve successful-event deduplication and newer usage");
    compacted.provider_session_id = "resumed-unknown-session";
    addon::apply_context_update(state, compacted);
    SDK_CHECK(state.compaction_count == 1 && !state.compaction_count_complete && !state.used_fraction &&
        !state.compaction_count_revision && state.compaction_event_ids.size() == 1,
        "a new unknown-history session resets revisions and event identities without inventing completeness");

    auto invalid = addon::encode_classify_event_result(result);
    auto bad_update = addon::Json_value::object();
    bad_update.set("provider_session_id", addon::Json_value("session-a"));
    bad_update.set("event_id", addon::Json_value("bad"));
    bad_update.set("used_fraction", addon::Json_value(1.5));
    auto bad_updates = addon::Json_value::array();
    bad_updates.push_back(bad_update);
    invalid.set("context_updates", bad_updates);
    decoded = addon::decode_classify_event_result(invalid, &failure);
    SDK_CHECK(decoded && decoded->context_failure && decoded->context_updates.empty(),
        "invalid context data is isolated from the valid core classification");

    addon::Observe_request observe;
    observe.session = {"/workspace", {"provider"}, "session-a"};
    observe.window_ms = 1000;
    observe.context_state_version = 2;
    const auto observed = addon::decode_observe_request(addon::encode_observe_request(observe), &failure);
    SDK_CHECK(observed && observed->context_state_version == 2,
        "continuous observers receive the same explicit opt-in");
    addon::Poll_observations_request poll;
    poll.limit = 16;
    poll.context_state_version = 2;
    const auto polled_request = addon::decode_poll_observations_request(
        addon::encode_poll_observations_request(poll), &failure);
    SDK_CHECK(polled_request && polled_request->context_state_version == 2,
        "polling preserves the opt-in without changing lifecycle version");
    addon::Poll_observations_result polled;
    polled.context_updates = result.context_updates;
    const auto polled_result = addon::decode_poll_observations_result(
        addon::encode_poll_observations_result(polled), 16, &failure);
    SDK_CHECK(polled_result && !polled_result->context_failure && polled_result->context_updates.size() == 1,
        "usage snapshots share the existing continuous observation transport");
    return true;
}

} // namespace sdk_tests
