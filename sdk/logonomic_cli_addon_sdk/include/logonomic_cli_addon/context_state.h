#pragma once

#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/json.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace logonomic::cli_addon {

/**
 * Optional context-state@2 observations, independent of lifecycle facts.
 * Missing measurements mean unknown; cumulative billing tokens are not context
 * occupancy. A successful compaction clears previous usage unless this same
 * observation supplies a new measurement.
 */
struct Context_update
{
    std::string provider_session_id;
    std::string event_id;
    std::optional<double> used_fraction;
    std::optional<std::uint64_t> used_tokens;
    std::optional<std::uint64_t> context_window;
    std::optional<std::uint64_t> compaction_count;
    // Echoes the host's serialized fresh-read request. It orders snapshots,
    // including read loss and recovery, independently of their count.
    std::optional<std::uint64_t> compaction_count_revision;
    bool usage_unavailable = false;
    bool compaction_count_complete = false;
    bool compacted = false;
};

// The host owns this state for one exact provider session. Successful-event
// identities survive ordinary usage updates so redelivery never adds a count.
struct Context_state
{
    std::string provider_session_id;
    std::optional<double> used_fraction;
    std::optional<std::uint64_t> used_tokens;
    std::optional<std::uint64_t> context_window;
    std::optional<std::uint64_t> compaction_count;
    std::optional<std::uint64_t> compaction_count_revision;
    bool compaction_count_complete = false;
    std::vector<std::string> compaction_event_ids;

    bool operator==(const Context_state&) const = default;
};

// A diagnostic without a valid source session has no authority to withdraw
// another session's measurements. Request metadata failures remain unscoped.
struct Context_failure
{
    std::string provider_session_id;
    Contract_failure failure;
};

// Applies one observation satisfying the context-state@2 contract.
void apply_context_update(Context_state& state, const Context_update& update);

// These helpers add only the negotiated optional members to an existing
// operation. No lifecycle/helper version changes when this capability evolves.
void encode_context_state_request(Json_value& payload, std::uint32_t version);

// An unsupported optional request leaves version zero and reports its failure
// separately, so the base request remains usable.
void decode_context_state_request(
    const Json_value& payload,
    std::uint32_t& version,
    std::optional<Contract_failure>& failure);

void encode_context_updates(
    Json_value& payload,
    const std::vector<Context_update>& updates,
    const std::optional<Context_failure>& failure = std::nullopt);

// A malformed optional observation is reported separately from the operation's
// core result, so it cannot discard a valid lifecycle transition beside it.
void decode_context_updates(
    const Json_value& payload,
    std::vector<Context_update>& updates,
    std::optional<Context_failure>& failure);

} // namespace logonomic::cli_addon
