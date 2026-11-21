#pragma once

#include "logonomic_cli_addon/execution_context.h"
#include "logonomic_cli_addon/model_state.h"
#include "logonomic_cli_addon/model_catalog.h"
#include "logonomic_cli_addon/child_state.h"
#include "logonomic_cli_addon/cooperative_description.h"

#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/json.h"
#include "logonomic_cli_addon/lifecycle_events.h"
#include "logonomic_cli_addon/terminal_identity.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace logonomic::cli_addon {

/**
 * @brief helper-ingress@2/@3 (docs/helper_ingress_2.md, docs/helper_ingress_3.md): classification of
 * vendor hook events that the package's own helper delivers through the
 * authenticated provider-event lane. The adapter is stateless across
 * invocations: everything about the session arrives in the ledger view,
 * and the host is the only writer of epochs. The pair is live only beside
 * lifecycle-events@2.
 */
inline constexpr const char* k_classify_event_operation = "classify_event";

/// `dependency_capability_unavailable` unless lifecycle-events@2 is among
/// the live pairs.
bool validate_helper_ingress_dependency(const std::vector<Capability_pair>& live_pairs, Contract_failure* out_failure);

/// One vendor hook event as the helper received it.
struct Provider_event
{
    std::string name;                         // 1-k_max_provider_event_name_bytes
    // The host applies k_max_provider_event_bytes to its transport encoding.
    Json_value  body = Json_value::object();
};

/// SHA-256 over the canonical bytes of the `event` object: the evidence
/// digest the host records beside the transition nonce as replay identity.
std::string provider_event_evidence_digest_hex(const Provider_event& event);

enum class Transition_kind
{
    START,
    RESUME,
};

const char* transition_kind_name(Transition_kind kind);
std::optional<Transition_kind> parse_transition_kind(std::string_view name);

/// The transition instance the ledger currently holds.
struct Ledger_transition
{
    Transition_kind kind = Transition_kind::START;
    std::string     provider_session_id;
    std::string     nonce;   // is_valid_transition_nonce_hex
};

/// The host's view of the session's epoch ledger: `current_epoch` is 0
/// before any transition, and `current_transition` is null at epoch 0.
struct Ledger_view
{
    std::uint64_t                    current_epoch = 0;
    std::optional<Ledger_transition> current_transition;
};

struct Classify_event_request
{
    Provider_event event;
    Ledger_view    ledger;
    std::string    transition_nonce;   // the nonce this delivery would open a transition under
    std::uint32_t context_state_version = 0;
    std::optional<std::uint64_t> context_snapshot_revision;
    std::optional<Contract_failure> context_failure = std::nullopt;
    std::uint32_t execution_context_version = 0;
    std::optional<Contract_failure> execution_failure = std::nullopt;
    std::uint32_t child_state_version = 0;
    std::optional<Contract_failure> child_failure = std::nullopt;
    std::uint32_t model_catalog_version = 0;
    std::optional<Contract_failure> catalog_failure = std::nullopt;
    std::uint32_t model_state_version = 0;
    std::optional<Contract_failure> model_failure = std::nullopt;
    std::optional<Terminal_identity_context> terminal_identity;
    std::uint32_t cooperative_description_version = 0;
    std::optional<Contract_failure> description_failure = std::nullopt;
};

Json_value encode_classify_event_request(const Classify_event_request& request);

/// Adapter side: strict decode. Reasons: `frame_violation`,
/// `unexpected_member`, `payload_too_large`.
std::optional<Classify_event_request> decode_classify_event_request(
    const Json_value& payload, Contract_failure* out_failure);

/// `transition`: exactly one observation carries `session_transition:
/// started` and `transition.kind` names the instance kind. `event`: 1-16
/// observations, none `started`. `ignore`: no observations.
enum class Classification
{
    TRANSITION,
    EVENT,
    IGNORED,   // the wire classification `ignore`; named to stay clear of the IGNORE macro of <windows.h>
};

const char* classification_name(Classification classification);
std::optional<Classification> parse_classification(std::string_view name);

/// Whole lifecycle loss, optionally quarantining one provider session.
struct Lifecycle_degradation
{
    std::uint16_t              version = 1;
    std::optional<std::string> provider_session_id;
};

/// The helper-ingress@3 subagent record. A batch binds the complete child
/// set for one provider-local group; a loss degrades and reconciles only the
/// subagent feature; a reconcile binds an accepted root awaiting-input fact
/// to the host-owned stop of every active child. No kind ends the root Session.
enum class Subagent_evidence_kind
{
    BATCH,
    LOSS,
    RECONCILE,
};

struct Subagent_evidence
{
    std::uint16_t         version = 1;
    Subagent_evidence_kind kind = Subagent_evidence_kind::BATCH;
    std::string           provider_session_id;
    std::string           batch_id;          // BATCH only
    std::vector<std::string> child_ids;      // BATCH only, complete unordered set
    std::string           provider_event_id; // RECONCILE only
    std::string           reason_code;       // LOSS only
};

inline constexpr std::size_t k_max_subagent_evidence_observations = 32;

/// `helper_response` is any JSON value whose host compact transport encoding
/// is at most k_max_helper_response_bytes and is returned to the helper verbatim.
struct Classify_event_result
{
    Classification                      classification = Classification::IGNORED;
    std::optional<Transition_kind>       transition;        // TRANSITION only
    // @1/@2: 16; @3: a BATCH may carry at most 32, otherwise 16.
    std::vector<Lifecycle_observation>    observations;
    std::optional<Json_value>            helper_response;
    // helper-ingress@2 only. No session means delivery loss; a session also
    // quarantines its correlation. Both degrade the entire lifecycle.
    std::optional<Lifecycle_degradation> lifecycle_degradation;
    // helper-ingress@3 only. The record is capability-owned and its exact
    // required members depend on its kind.
    std::optional<Subagent_evidence> subagent_evidence;
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
    std::optional<Description_submission> description_submission = std::nullopt;
    std::optional<Contract_failure> description_failure = std::nullopt;
};

/// Adapter side.
Json_value encode_classify_event_result(const Classify_event_result& result);

/// Host side: strict decode. Reasons: `classification_invalid` (shape and
/// the classification rules), `observation_invalid`, `payload_too_large`.
std::optional<Classify_event_result> decode_classify_event_result(
    const Json_value& payload, Contract_failure* out_failure, std::uint16_t version = 2);

/// Only the owner-created NativeSignal uses this result envelope. Ordinary
/// hooks retain their negotiated helper/attention result and share the input.
struct Terminal_identity_classification_result
{
    Classify_event_result base;
    std::optional<Terminal_identity_observation> terminal_identity;
};

Json_value encode_terminal_identity_classification_result(const Terminal_identity_classification_result& result);
std::optional<Terminal_identity_classification_result> decode_terminal_identity_classification_result(
    const Json_value& payload, Contract_failure* out_failure, std::uint16_t helper_version = 2);

// Optional helper-ingress companion. Only an explicit attention-kind@1 invocation
// uses these envelopes; ordinary helper-ingress retains its strict wire grammar.
inline constexpr const char* k_attention_kind_capability_id = "attention-kind";
inline constexpr std::uint16_t k_attention_kind_version = 1;

struct Attention_kind
{
    std::size_t observation_index = 0;
    std::string kind;
};

struct Attention_classification_request
{
    Classify_event_request base;
    std::optional<std::string> pending_conversational_request_id;
};

struct Attention_classification_result
{
    Classify_event_result base;
    std::optional<Attention_kind> attention_kind;
};

Json_value encode_attention_classification_request(const Attention_classification_request& request);
std::optional<Attention_classification_request> decode_attention_classification_request(
    const Json_value& payload, Contract_failure* out_failure);
Json_value encode_attention_classification_result(const Attention_classification_result& result);
std::optional<Attention_classification_result> decode_attention_classification_result(
    const Json_value& payload, Contract_failure* out_failure, std::uint16_t helper_version = 2);

// Independent optional companion; attention-kind@1 keeps its existing envelopes.
inline constexpr const char* k_async_question_attention_capability_id = "async-question-attention";
inline constexpr std::uint16_t k_async_question_attention_version = 1;
inline constexpr std::size_t k_max_async_question_ids = 128;
inline constexpr std::size_t k_max_async_question_id_bytes = 512;

enum class Async_question_update_kind
{
    STARTED,
    ACCEPTED,
    ANSWERED,
    RESET,
};

struct Async_question_update
{
    std::size_t observation_index = 0;
    Async_question_update_kind kind = Async_question_update_kind::ACCEPTED;
    std::vector<std::string> question_ids;
};

struct Async_question_classification_result
{
    Attention_classification_result base;
    std::optional<Async_question_update> async_questions;
};

Json_value encode_async_question_classification_result(const Async_question_classification_result& result);
std::optional<Async_question_classification_result> decode_async_question_classification_result(
    const Json_value& payload, Contract_failure* out_failure, std::uint16_t helper_version = 2);

} // namespace logonomic::cli_addon
