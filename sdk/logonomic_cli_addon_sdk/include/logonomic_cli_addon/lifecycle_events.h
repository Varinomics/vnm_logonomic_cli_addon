#pragma once

#include "logonomic_cli_addon/context_state.h"
#include "logonomic_cli_addon/execution_context.h"
#include "logonomic_cli_addon/model_state.h"
#include "logonomic_cli_addon/model_catalog.h"
#include "logonomic_cli_addon/child_state.h"
#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/json.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace logonomic::cli_addon {

/// Best-effort local hint from complete, attributed assistant text; never confirmed intent.
bool looks_like_reply_request(std::string_view assistant_text);

/// Provider-local evidence from a complete reply. Scans the full Markdown context,
/// retaining only the final prose paragraph's UTF-8 prefix/suffix, never a truncated
/// message labelled complete. Null means empty, invalid, quoted or code evidence.
inline constexpr std::size_t k_assistant_reply_edge_bytes = 128;
Json_value assistant_reply_evidence(std::string_view complete_text);

/// Validates final_paragraph_edges evidence and applies the same local hint grammar.
bool looks_like_reply_request_evidence(const Json_value& evidence);

/**
 * @brief lifecycle-events@2 (docs/lifecycle_events_2.md): the package's
 * declaration of the lifecycle facts it produces, the schema of one
 * observation, and the bounded observation protocol of continuous mode.
 * coordination-delivery@1 (docs/coordination_delivery_1.md), the static
 * declaration consumed beside it at session creation, is decoded here too.
 */
inline constexpr const char* k_observe_operation           = "observe";
inline constexpr const char* k_poll_observations_operation = "poll_observations";

/// `supported` or `unsupported`, the value of every feature member.
enum class Feature_support
{
    SUPPORTED,
    UNSUPPORTED,
};

const char* feature_support_name(Feature_support support);
std::optional<Feature_support> parse_feature_support(std::string_view name);

/// `events`: facts arrive through helper-ingress@2/@3 classification.
/// `continuous`: the adapter watches vendor state through `observe`.
enum class Observation_mode
{
    EVENTS,
    CONTINUOUS,
};

const char* observation_mode_name(Observation_mode mode);
std::optional<Observation_mode> parse_observation_mode(std::string_view name);

/// The eight feature fields of a lifecycle profile, every one required.
struct Lifecycle_features
{
    Feature_support root_turn_start         = Feature_support::UNSUPPORTED;
    Feature_support root_turn_completion    = Feature_support::UNSUPPORTED;
    Feature_support confirmed_phase_status  = Feature_support::UNSUPPORTED;
    Feature_support attention               = Feature_support::UNSUPPORTED;
    Feature_support paired_attention        = Feature_support::UNSUPPORTED;
    Feature_support subagents               = Feature_support::UNSUPPORTED;
    Feature_support cooperative_description = Feature_support::UNSUPPORTED;
    Feature_support blockable_completion    = Feature_support::UNSUPPORTED;
};

struct Lifecycle_declaration
{
    std::string        profile_id;   // is_valid_lifecycle_profile_id
    Lifecycle_features features;
    Feature_support    turn_origin_correlation = Feature_support::UNSUPPORTED;
    Observation_mode   observation             = Observation_mode::EVENTS;
};

/// 1-k_max_lifecycle_profile_id_bytes of lowercase letters, digits and '-'.
bool is_valid_lifecycle_profile_id(std::string_view profile_id);

/// Strict decode of the sidecar payload. Reason: `invalid_sidecar`.
std::optional<Lifecycle_declaration> decode_lifecycle_declaration(
    const Json_value& payload, Contract_failure* out_failure);

Json_value encode_lifecycle_declaration(const Lifecycle_declaration& declaration);

// ---------------------------------------------------------------------------
// coordination-delivery@1 (static)

struct Coordination_delivery_declaration
{
    Feature_support startup_instruction_delivery = Feature_support::UNSUPPORTED;
    Feature_support turn_context_delivery        = Feature_support::UNSUPPORTED;
};

/// Strict decode of the sidecar payload. Reason: `invalid_sidecar`.
std::optional<Coordination_delivery_declaration> decode_coordination_delivery_declaration(
    const Json_value& payload, Contract_failure* out_failure);

Json_value encode_coordination_delivery_declaration(const Coordination_delivery_declaration& declaration);

// ---------------------------------------------------------------------------
// Observations

enum class Session_transition   { NONE, STARTED, ENDED };
enum class Phase_transition     { NONE, WORKING, AWAITING_INPUT };
enum class Evidence             { NONE, INFERRED, CONFIRMED };
enum class Outcome_transition   { NONE, SUCCEEDED, FAILED, INTERRUPTED };
enum class Attention_transition { NONE, REQUESTED, RESOLVED };
enum class Subagent_transition  { NONE, STARTED, STOPPED };

const char* session_transition_name(Session_transition transition);
const char* phase_transition_name(Phase_transition transition);
const char* evidence_name(Evidence evidence);
const char* outcome_transition_name(Outcome_transition transition);
const char* attention_transition_name(Attention_transition transition);
const char* subagent_transition_name(Subagent_transition transition);

/**
 * @brief One lifecycle observation: the member set the host's state machine
 * admits, with the transitions as intent, the evidence members as quality,
 * `provider_event_id` as the identity the host deduplicates on and
 * `provider_sequence` as sequence evidence. Every string member is on the
 * wire; identities are at most k_max_lifecycle_identity_bytes and labels
 * at most k_max_lifecycle_label_bytes, except continuous agent_label
 * (768 UTF-8 bytes and 256 UTF-16 units). `resume_record` travels only with
 * `session_transition: started`.
 */
struct Lifecycle_observation
{
    std::string               provider_event_name;   // 1-k_max_lifecycle_identity_bytes
    Session_transition        session_transition   = Session_transition::NONE;
    Phase_transition          phase_transition     = Phase_transition::NONE;
    Evidence                  phase_evidence       = Evidence::NONE;
    Outcome_transition        outcome_transition   = Outcome_transition::NONE;
    Evidence                  outcome_evidence     = Evidence::NONE;
    Attention_transition      attention_transition = Attention_transition::NONE;
    Evidence                  attention_evidence   = Evidence::NONE;
    Subagent_transition       subagent_transition  = Subagent_transition::NONE;
    std::string               provider_session_id;
    std::string               provider_turn_id;
    std::string               provider_event_id;
    std::string               provider_sequence;      // empty or a canonical decimal that fits 64 bits
    std::string               attention_request_id;
    std::string               agent_id;
    std::string               session_source;
    std::string               session_end_reason;
    std::string               agent_label;
    std::optional<Json_value> resume_record;          // object, at most k_max_resume_record_bytes canonical
};

Json_value encode_lifecycle_observation(const Lifecycle_observation& observation);

/// Helper ingress retains the 160-byte agent label bound. Continuous
/// observations admit native labels of at most 768 UTF-8 bytes and 256
/// UTF-16 code units. Other observation fields have the same bounds.
enum class Lifecycle_observation_policy
{
    HELPER_INGRESS,
    CONTINUOUS,
};

/// Strict decode with the validity rules of the contract: each
/// transition/evidence pair is `none` together or neither, an outcome names
/// a turn, a session transition names a session, a subagent transition
/// names an agent, the sequence grammar, anonymous attention is only
/// `requested`+`inferred`, confirmed attention names a request, and
/// `resume_record` accompanies `started` only. Reasons: `frame_violation`,
/// `unexpected_member`, `payload_too_large`, `observation_invalid`.
std::optional<Lifecycle_observation> decode_lifecycle_observation(
    const Json_value& value, Contract_failure* out_failure, Lifecycle_observation_policy policy);

/// Decodes an `observations` array of at most `limit` observations, each
/// valid. `limit` is at most k_max_observations_per_result.
std::optional<std::vector<Lifecycle_observation>> decode_observations(
    const Json_value&             array,
    std::size_t                   limit,
    Contract_failure*             out_failure,
    Lifecycle_observation_policy policy);

Json_value encode_observations(const std::vector<Lifecycle_observation>& observations);

/**
 * @brief The host's lifecycle feature gate: whether a profile with `features`
 * admits `observation`. Confirmed phase or outcome evidence needs
 * `confirmed_phase_status`, `working` needs `root_turn_start`,
 * `awaiting_input` needs `root_turn_completion`, any attention needs
 * `attention` and an identified one `paired_attention` too, and a subagent
 * transition needs `subagents`. The host rejects an observation this refuses
 * as `unsupported_profile_event`, facts and all, so a package's own tests
 * apply it to what it emits against its declaration.
 */
bool lifecycle_profile_admits(const Lifecycle_features& features, const Lifecycle_observation& observation);

// ---------------------------------------------------------------------------
// Continuous mode: observe and poll_observations

/// The session an `observe` window watches.
struct Observe_session
{
    std::string              working_directory;
    std::vector<std::string> launcher_argv;
    std::string              provider_session_id;   // may be empty
};

/// Correlation to an authenticated, admitted helper transition.
struct Session_correlation
{
    std::string transition_nonce;   // 32 lowercase hexadecimal characters
};

struct Observe_request
{
    Observe_session session;
    std::string     resume_cursor;   // opaque, at most k_max_observe_cursor_bytes; empty for the first window
    std::int32_t    window_ms = 0;   // k_observe_window_ms
    std::optional<Session_correlation> correlation = std::nullopt;
    std::uint32_t context_state_version = 0;
    std::optional<Contract_failure> context_failure = std::nullopt;
    std::uint32_t execution_context_version = 0;
    std::optional<Contract_failure> execution_failure = std::nullopt;
    std::uint32_t child_state_version = 0;
    std::optional<Contract_failure> child_failure = std::nullopt;
    std::uint32_t model_catalog_version = 0;
    std::optional<Contract_failure> catalog_failure = std::nullopt;
    std::uint32_t model_state_version = 0;
    std::optional<Contract_failure> model_failure = std::nullopt;
};

Json_value encode_observe_request(const Observe_request& request);

/// Adapter side: strict decode. Reasons: `frame_violation`,
/// `unexpected_member`, `payload_too_large`.
std::optional<Observe_request> decode_observe_request(
    const Json_value& payload, Contract_failure* out_failure);

struct Observe_result
{
    std::string resume_cursor;
    std::vector<Context_update> context_updates = {};
    std::optional<Context_failure> context_failure = std::nullopt;
    std::vector<Execution_context_update> execution_updates = {};
    std::optional<Contract_failure> execution_failure = std::nullopt;
    std::vector<Child_update> child_updates = {};
    std::optional<Child_loss> child_loss = std::nullopt;
    std::optional<Contract_failure> child_failure = std::nullopt;
    std::optional<Model_catalog_update> catalog_update = std::nullopt;
    std::optional<Contract_failure> catalog_failure = std::nullopt;
    std::vector<Model_update> model_updates = {};
    std::optional<Contract_failure> model_failure = std::nullopt;
};

Json_value encode_observe_result(const Observe_result& result);
std::optional<Observe_result> decode_observe_result(const Json_value& payload, Contract_failure* out_failure);

/// The progress note of an `observe` window: `{"available": n}`, the count
/// of observations available since the window began. Progress frames wake
/// the host; they never carry observations.
std::string observe_progress_note(std::uint64_t available);

struct Poll_observations_request
{
    std::string resume_cursor;
    std::size_t limit = 0;   // 1-k_max_observations_per_result
    std::optional<Session_correlation> correlation = std::nullopt;
    std::uint32_t context_state_version = 0;
    std::optional<Contract_failure> context_failure = std::nullopt;
    std::uint32_t execution_context_version = 0;
    std::optional<Contract_failure> execution_failure = std::nullopt;
    std::uint32_t child_state_version = 0;
    std::optional<Contract_failure> child_failure = std::nullopt;
    std::uint32_t model_catalog_version = 0;
    std::optional<Contract_failure> catalog_failure = std::nullopt;
    std::uint32_t model_state_version = 0;
    std::optional<Contract_failure> model_failure = std::nullopt;
};

Json_value encode_poll_observations_request(const Poll_observations_request& request);
std::optional<Poll_observations_request> decode_poll_observations_request(
    const Json_value& payload, Contract_failure* out_failure);

struct Poll_observations_result
{
    std::vector<Lifecycle_observation> observations;   // sequence order, at most the request's limit
    std::string                        resume_cursor;  // follows the last returned observation
    bool                               more = false;
    std::vector<Context_update> context_updates = {};
    std::optional<Context_failure> context_failure = std::nullopt;
    std::vector<Execution_context_update> execution_updates = {};
    std::optional<Contract_failure> execution_failure = std::nullopt;
    std::vector<Child_update> child_updates = {};
    std::optional<Child_loss> child_loss = std::nullopt;
    std::optional<Contract_failure> child_failure = std::nullopt;
    std::optional<Model_catalog_update> catalog_update = std::nullopt;
    std::optional<Contract_failure> catalog_failure = std::nullopt;
    std::vector<Model_update> model_updates = {};
    std::optional<Contract_failure> model_failure = std::nullopt;
};

Json_value encode_poll_observations_result(const Poll_observations_result& result);

/// Host side: strict decode against the request's `limit`. Reasons:
/// `frame_violation`, `unexpected_member`, `payload_too_large`,
/// `observation_invalid`.
std::optional<Poll_observations_result> decode_poll_observations_result(
    const Json_value& payload,
    std::size_t       limit,
    Contract_failure* out_failure);

} // namespace logonomic::cli_addon
