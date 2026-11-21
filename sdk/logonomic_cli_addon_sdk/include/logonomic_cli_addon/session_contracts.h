#pragma once

#include "logonomic_cli_addon/accounts.h"
#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/json.h"
#include "logonomic_cli_addon/launch_preparation.h"
#include "logonomic_cli_addon/launcher_discovery.h"
#include "logonomic_cli_addon/model_state.h"
#include "logonomic_cli_addon/process_proposal.h"
#include "logonomic_cli_addon/profile_storage.h"
#include "logonomic_cli_addon/startup_observer.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace logonomic::cli_addon {

/**
 * @brief structured-session@3 and delegated-session@3,
 * and interactive-resume@2
 * (docs/structured_session_3.md, docs/delegated_session_3.md,
 * docs/interactive_resume_2.md): the adapter capabilities whose success
 * payloads are process proposals for a process the host owns. The adapter
 * prepares and interprets; the host validates every proposal against the
 * frozen request context and performs the launch. structured-session@3 and
 * delegated-session@3 share the `prepare` operation name of
 * launch-preparation@3 (k_prepare_operation).
 */
inline constexpr const char* k_inspect_operation        = "inspect";
inline constexpr const char* k_prepare_resume_operation = "prepare_resume";
inline constexpr const char* k_inspect_resume_operation = "inspect_resume";

/// `*_base64` members carry raw bytes in the standard alphabet with padding
/// and no whitespace; decode_base64 rejects anything else, non-canonical
/// padding bits included.
std::string encode_base64(std::string_view bytes);
std::optional<std::string> decode_base64(std::string_view text);

/// Continuation state is valid UTF-8 without NUL, bounded in UTF-16 code units
/// to retain the desktop runtime's native continuation limit.
bool exchange_protocol_state_valid(std::string_view state);
/// Parse JSON continuation state after the same UTF-8, NUL and UTF-16 checks.
/// Byte and string allowances follow the exchange bound; ordinary JSON depth
/// and member limits remain. Invalid state or JSON returns nullopt. The caller
/// owns the root type and provider-specific state schema.
std::optional<Json_value> parse_exchange_protocol_state(std::string_view state);
/// The aggregate encoded payload bound reserves room for the RPC envelope.
bool validate_session_exchange_payload(const Json_value& payload, Contract_failure* out_failure);

// ---------------------------------------------------------------------------
// Purposes: the sidecar payload of structured-session@3 and delegated-session@3

enum class Structured_purpose
{
    READ_ONLY_REVIEW,
    IMPLEMENTATION,
};

const char* structured_purpose_name(Structured_purpose purpose);
std::optional<Structured_purpose> parse_structured_purpose(std::string_view name);

struct Purposes_declaration
{
    std::vector<Structured_purpose> purposes;   // 1-k_max_structured_purposes distinct entries
    std::vector<std::string> environment_names;   // frozen preparation input
};

/// Strict decode of the sidecar payload's purposes and environment names.
/// Reason: `invalid_sidecar`.
std::optional<Purposes_declaration> decode_purposes_declaration(
    const Json_value& payload, Contract_failure* out_failure);

Json_value encode_purposes_declaration(const Purposes_declaration& declaration);

/// delegated-session@3 requires structured-session@3 live in the same
/// generation and declaring every delegated purpose. `live_structured` is
/// the live structured declaration or nullptr when the pair is not live;
/// the failure is `dependency_capability_unavailable`.
bool validate_delegated_session_dependency(
    const Purposes_declaration& delegated,
    const Purposes_declaration* live_structured,
    Contract_failure*           out_failure);

// ---------------------------------------------------------------------------
// The request context every prepare operation carries

/// The frozen launch context: the typed launcher record of
/// launcher-discovery@1, its host-resolved argv, the working directory, the
/// admitted managed-assets@1 declaration and the profile-storage context.
struct Session_launch_context
{
    Launcher_request                       launcher;
    std::vector<std::string>               launcher_argv;       // 1-k_max_argv_entries entries under the argv bounds
    std::string                            working_directory;   // at most k_max_working_directory_bytes, no NUL
    std::vector<std::string>               declared_assets;     // the valid managed-assets@1 declaration, or empty
    std::optional<Profile_storage_context> profile_storage;     // present when profile-storage@1 is live
    std::optional<Account_context>         account_context = std::nullopt;
    std::optional<Launch_environment>      launch_environment;
    std::optional<Codex_invocation_request> codex_invocation = std::nullopt;
    std::optional<Session_wake_offer> session_wake = std::nullopt;
    std::optional<Terminal_identity_offer> terminal_identity = std::nullopt;
};

// ---------------------------------------------------------------------------
// structured-session@3

struct Structured_prepare_request
{
    Structured_purpose     purpose = Structured_purpose::READ_ONLY_REVIEW;
    Session_launch_context context;
    std::string            prompt;   // raw bytes; `prompt_base64` on the wire, at most k_max_structured_prompt_bytes
};

Json_value encode_structured_prepare_request(const Structured_prepare_request& request);

/// Adapter side: strict decode. Reasons: `frame_violation`,
/// `unexpected_member`, `payload_too_large`.
std::optional<Structured_prepare_request> decode_structured_prepare_request(
    const Json_value& payload, Contract_failure* out_failure);

/// How the host inspects the process: incrementally or at exit only, from
/// the protocol state the adapter starts with.
struct Inspection_setup
{
    bool        incremental = false;
    std::string protocol_state;   // exchange_protocol_state_valid bounds the current chunked state
    bool        keep_stdin_open = false;   // incremental inspection only
};

struct Structured_prepare_result
{
    Process_proposal                       process;
    Inspection_setup                       inspection;
    std::optional<Profile_storage_context> profile_storage;   // the echo, when the request carried one
    std::optional<Account_context>         account_context = std::nullopt;
};

/// Adapter side.
Json_value encode_structured_prepare_result(const Structured_prepare_result& result);

/// Host side: strict structural decode. Policy over the proposal is
/// validate_structured_process_proposal's job. Reasons: `frame_violation`,
/// `unexpected_member`, `payload_too_large`.
std::optional<Structured_prepare_result> decode_structured_prepare_result(
    const Json_value& payload, Contract_failure* out_failure);

/// The process-proposal policy of structured and delegated processes: the
/// launcher-program reference is allowed, `stdin` may not be `inherit`
/// (`launch_patch_invalid`; the runtime has no interactive stdin), and the
/// general rules of validate_process_proposal apply.
bool validate_structured_process_proposal(const Process_proposal& proposal, Contract_failure* out_failure);

/// One inspection of the process the host runs: the bytes accumulated since
/// the previous inspection, and the facts of the final one.
struct Structured_inspection
{
    std::string  protocol_state;   // the value returned by prepare or the previous inspect
    std::string  stdout_bytes;     // raw; chunked to k_max_structured_stream_bytes
    std::string  stderr_bytes;     // raw; `stderr_base64` on the wire, at most k_max_structured_stderr_bytes
    bool         final     = false;
    bool         timed_out = false;
    std::int32_t exit_code = 0;
};

struct Structured_inspect_request
{
    Structured_purpose    purpose = Structured_purpose::READ_ONLY_REVIEW;
    Structured_inspection inspection;
    bool                  report_failure_diagnostics = false;
    std::uint32_t         model_state_version = 0;
    std::optional<Contract_failure> model_failure;
};

Json_value encode_structured_inspect_request(const Structured_inspect_request& request);

/// Adapter side: strict decode. Reasons: `frame_violation`,
/// `unexpected_member`, `payload_too_large`.
std::optional<Structured_inspect_request> decode_structured_inspect_request(
    const Json_value& payload, Contract_failure* out_failure);

/// The presentation record kinds an inspection may report.
enum class Presentation_event_kind
{
    REVIEW_INITIALIZED,
    REASONING,
    ANALYSIS_UPDATE,
    TOOL_RESPONSE,
    RATE_LIMIT_STATUS,
    RESULT_AUTHENTICATED,
};

const char* presentation_event_kind_name(Presentation_event_kind kind);
std::optional<Presentation_event_kind> parse_presentation_event_kind(std::string_view name);

struct Presentation_event
{
    Presentation_event_kind kind  = Presentation_event_kind::REVIEW_INITIALIZED;
    std::uint64_t           count = 1;   // at least 1
};

/// The disposition of the optional `presentation` member: absent, decoded,
/// or present and rejected by the strict decode. A rejected member does
/// not fail the inspection; the host records the error and continues.
enum class Presentation_state
{
    UNAVAILABLE,
    AVAILABLE,
    INVALID,
};

/// Package-defined failure reasons follow the reason-code grammar; the
/// four host-known reasons are `hook_approval_required`, `update_required`,
/// `authentication_required` and `startup_failed`.
bool is_valid_failure_reason(std::string_view reason);

/// Optional structured-failure-diagnostics@1 evidence beside a failed
/// inspection. The message is bounded UTF-8 without NUL; source is one of
/// native_error, protocol_error and process_exit.
struct Structured_failure_diagnostics
{
    std::string source;
    std::string message;
    bool        truncated = false;
};

bool decode_structured_failure_diagnostics_declaration(
    const Json_value& payload, Contract_failure* out_failure);
Json_value encode_structured_failure_diagnostics(const Structured_failure_diagnostics& diagnostics);
std::optional<Structured_failure_diagnostics> decode_structured_failure_diagnostics(const Json_value& value);

struct Structured_inspect_result
{
    bool                            complete = false;
    bool                            close_stdin = false;  // incomplete reply; EOF after accepted input
    std::string                     protocol_state;       // exchange state bound; empty on completion
    std::optional<std::string>      stdin_bytes;          // <= k_max_structured_stream_bytes; complete == false only
    std::optional<std::string>      result_bytes;         // <= k_max_structured_stream_bytes; complete == true only, exactly one with failure_reason
    std::optional<std::string>      failure_reason;
    Presentation_state              presentation_state = Presentation_state::UNAVAILABLE;
    std::vector<Presentation_event> presentation;         // AVAILABLE only
    std::string                     presentation_error;   // INVALID only

    std::optional<Structured_failure_diagnostics> failure_diagnostics;
    std::optional<Structured_model_observation> model_observation;
    std::optional<Contract_failure> model_failure;
};

/// Adapter side. A result whose presentation_state is INVALID is encoded
/// without a presentation member. Failure diagnostics are emitted only when
/// the independently negotiated request opted in; invalid evidence is omitted.
/// Throws std::invalid_argument for an invalid stdin-closure request.
Json_value encode_structured_inspect_result(
    const Structured_inspect_result& result, bool report_failure_diagnostics = false);

/// Host side: strict decode against the inspection it answers.
/// `final_inspection` is the request's `final`, which must be answered
/// `complete`. Reasons: `frame_violation`, `unexpected_member`,
/// `payload_too_large`, `inspection_invalid` (the completion rules).
/// Unsupported or malformed optional failure diagnostics leave the core reply intact.
std::optional<Structured_inspect_result> decode_structured_inspect_result(
    const Json_value& payload,
    bool              final_inspection,
    Contract_failure* out_failure);

// ---------------------------------------------------------------------------
// delegated-session@3

inline constexpr std::uint16_t k_delegated_control_version        = 1;
inline constexpr std::uint16_t k_delegated_acknowledgment_version = 1;
inline constexpr std::uint16_t k_delegated_connection_version = 1;

/// Per-request provider-native settings. Absence preserves the saved launcher
/// and account defaults; neither value changes the request's purpose or permissions.
struct Delegated_model_selection
{
    std::optional<std::string> model;
    std::optional<std::string> reasoning_effort;

    bool operator==(const Delegated_model_selection&) const = default;
};

struct Delegated_model_selection_declaration
{
    bool model            = false;
    bool reasoning_effort = false;
};

std::optional<Delegated_model_selection_declaration> decode_delegated_model_selection_declaration(
    const Json_value& payload, Contract_failure* out_failure);
Json_value encode_delegated_model_selection_declaration(const Delegated_model_selection_declaration& declaration);

/// The independently versioned model_selection record, with at least one setting.
std::optional<Delegated_model_selection> decode_delegated_model_selection(
    const Json_value& value, Contract_failure* out_failure);
Json_value encode_delegated_model_selection(const Delegated_model_selection& selection);

/// A missing live declaration or unsupported requested setting refuses only
/// model selection. Provider-specific values remain the provider's responsibility.
bool validate_delegated_model_selection(
    const Delegated_model_selection&             selection,
    const Delegated_model_selection_declaration* live_declaration,
    Contract_failure*                            out_failure);

/// Every requested value must be acknowledged exactly; unsolicited selection
/// also fails so an ordinary request cannot change its saved defaults.
bool validate_delegated_model_selection_acknowledgment(
    const std::optional<Delegated_model_selection>& requested,
    const std::optional<Delegated_model_selection>& acknowledged,
    Contract_failure*                              out_failure);

/// One task-control MCP server the host starts for the process.
struct Delegated_task_server
{
    std::string                                      server_id;     // 1-k_max_delegated_server_id_bytes, distinct
    std::string                                      program;       // absolute native path
    std::vector<std::string>                         actions;       // at most k_max_delegated_server_actions
    std::vector<std::pair<std::string, std::string>> environment;   // at most k_max_capability_environment_entries
};

struct Delegated_connection
{
    std::string relay_program;     // host-owned absolute executable path
    std::string descriptor_path;   // host-owned absolute path; need not exist at prepare
};

struct Delegated_control
{
    std::string                        task_id;                 // 1-k_max_delegated_task_id_bytes
    std::vector<Delegated_task_server> servers;                 // 1-k_max_delegated_task_servers
    std::string                        private_artifact_root;   // host-owned absolute directory
    std::optional<Delegated_connection> connection = std::nullopt;
};

struct Delegated_prepare_request
{
    Structured_purpose     purpose = Structured_purpose::IMPLEMENTATION;
    Session_launch_context context;
    std::string            prompt;
    Delegated_control      control;

    std::optional<Delegated_model_selection> model_selection;
};

Json_value encode_delegated_prepare_request(const Delegated_prepare_request& request);

/// Adapter side: strict decode. Reasons: `frame_violation`,
/// `unexpected_member`, `payload_too_large`.
std::optional<Delegated_prepare_request> decode_delegated_prepare_request(
    const Json_value& payload, Contract_failure* out_failure);

struct Delegated_acknowledgment
{
    std::vector<std::string> server_ids;   // set-equal to control.servers[].server_id
    std::optional<std::uint16_t> connection_version = std::nullopt;
};

/// A file the host writes below `private_artifact_root` before launch.
struct Private_artifact
{
    std::string relative_path;   // is_valid_private_artifact_relative_path
    std::string content;         // raw; `content_base64` on the wire, at most k_max_delegated_private_artifact_bytes
};

/// A '/'-separated relative path without empty, `.` or `..` segments,
/// without `\` and without NUL.
bool is_valid_private_artifact_relative_path(std::string_view path);

struct Delegated_prepare_result
{
    Process_proposal                       process;
    Delegated_acknowledgment               acknowledgment;
    std::vector<Private_artifact>          private_artifacts;   // at most k_max_delegated_private_artifacts
    Inspection_setup                       inspection;
    std::optional<Profile_storage_context> profile_storage;
    std::optional<Account_context>         account_context = std::nullopt;

    std::optional<Delegated_model_selection> model_selection;
};

/// Adapter side.
Json_value encode_delegated_prepare_result(const Delegated_prepare_result& result);

/// Host side: strict structural decode with the private-artifact rules
/// (`private_artifact_invalid`: count, decoded size, relative path). The
/// acknowledgment is checked against the control it answers by
/// validate_delegated_acknowledgment. Reasons: `frame_violation`,
/// `unexpected_member`, `payload_too_large`, `private_artifact_invalid`.
std::optional<Delegated_prepare_result> decode_delegated_prepare_result(
    const Json_value& payload, Contract_failure* out_failure);

/// `acknowledgment_mismatch` unless `server_ids` is set-equal to the
/// control's server ids.
bool validate_delegated_acknowledgment(
    const Delegated_control&        control,
    const Delegated_acknowledgment& acknowledgment,
    Contract_failure*               out_failure);

// ---------------------------------------------------------------------------
// interactive-resume@2

inline constexpr std::uint16_t k_resume_record_version         = 1;
inline constexpr std::uint16_t k_resume_acknowledgment_version = 1;

/// The `resume` member: `{"version": 1, "record": {...}}`. The record is
/// package-defined, opaque to the host beyond its default
/// `{"provider_session_id": "..."}`, and at most k_max_resume_record_bytes
/// canonical.
Json_value encode_resume_record(const Json_value& record);

/// Reasons: `frame_violation`, `unexpected_member`, `payload_too_large`.
std::optional<Json_value> decode_resume_record(const Json_value& value, Contract_failure* out_failure);

struct Resume_prepare_request
{
    Session_launch_context context;
    std::string            title_instruction;   // at most k_max_title_instruction_bytes
    Json_value             resume_record = Json_value::object();
    std::optional<Startup_observer_offer> startup_observer = std::nullopt;
};

/// interactive-resume@3 and @4 read the exact conversation without preparing a
/// launch. The context has no assets, wake offer or invocation replacement.
struct Resume_inspect_request
{
    Session_launch_context context;
    Json_value             resume_record = Json_value::object();
};

enum class Resume_availability
{
    AVAILABLE,
    ABSENT,
    UNKNOWN,
};

/// Content is independent of native resumability: a CLI may resume an empty
/// conversation. Only a complete, understood inspection can establish EMPTY.
enum class Resume_content
{
    UNKNOWN,
    EMPTY,
    NONEMPTY,
};

struct Resume_inspect_result
{
    Resume_availability availability = Resume_availability::UNKNOWN;
    std::string         request_digest_hex;
    Resume_content      content = Resume_content::UNKNOWN;
};

Json_value encode_resume_inspect_request(const Resume_inspect_request& request);
std::optional<Resume_inspect_request> decode_resume_inspect_request(
    const Json_value& payload, Contract_failure* out_failure);
Json_value encode_resume_inspect_result(const Resume_inspect_result& result, std::uint16_t version = 3);
std::optional<Resume_inspect_result> decode_resume_inspect_result(
    const Json_value& payload, Contract_failure* out_failure, std::uint16_t version = 3);

/// Binds both the opaque record and its complete resolved read-only context.
std::string resume_inspect_request_digest_hex(const Resume_inspect_request& request);
bool validate_resume_inspection(
    const Resume_inspect_request& request,
    const Resume_inspect_result&  result,
    Contract_failure*             out_failure);

Json_value encode_resume_prepare_request(const Resume_prepare_request& request, std::uint16_t version = 2);

/// Adapter side: strict decode. Reasons: `frame_violation`,
/// `unexpected_member`, `payload_too_large`.
std::optional<Resume_prepare_request> decode_resume_prepare_request(
    const Json_value& payload, Contract_failure* out_failure, std::uint16_t version = 2);

struct Resume_acknowledgment
{
    std::string record_digest_hex;   // SHA-256 over the canonical bytes of the record the host sent
};

/// `patch` carries the result's `environment.set`/`unset` and its
/// profile-storage echo; its working_directory stays INHERIT because the
/// process proposal, not the patch, places a resume.
struct Resume_prepare_result
{
    Process_proposal      process;
    Launch_patch          patch;
    Resume_acknowledgment acknowledgment;
    std::optional<Startup_observer_offer> startup_observer = std::nullopt;
};

/// Adapter side.
Json_value encode_resume_prepare_result(const Resume_prepare_result& result, std::uint16_t version = 2);

/// Host side: strict structural decode. Reasons: `frame_violation`,
/// `unexpected_member`, `launch_patch_invalid` (the environment member).
std::optional<Resume_prepare_result> decode_resume_prepare_result(
    const Json_value& payload, Contract_failure* out_failure, std::uint16_t version = 2);

/// The digest an acknowledgment must carry for `record`.
std::string resume_record_digest_hex(const Json_value& record);

/// `acknowledgment_mismatch` unless the digest names the record.
bool validate_resume_acknowledgment(
    const Json_value&            record,
    const Resume_acknowledgment& acknowledgment,
    Contract_failure*            out_failure);

/// The process-proposal policy of a resume: `working_directory` is
/// `session_workspace`, `stdin` is `inherit` (`launch_patch_invalid`
/// otherwise). Process environment names, assignments and assets must be empty;
/// the terminal applies environment and asset references through the launch patch.
/// The launcher-program reference is allowed, and the general rules of
/// validate_process_proposal apply.
bool validate_resume_process_proposal(const Process_proposal& proposal, Contract_failure* out_failure);

} // namespace logonomic::cli_addon
