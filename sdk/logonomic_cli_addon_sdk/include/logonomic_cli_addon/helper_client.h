#pragma once

#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/json.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace logonomic::cli_addon {

/// The native activation captured before an asynchronous hook request starts.
struct Model_context_activation
{
    std::string provider_session_id;
    std::string transition_nonce;
};

/// Presence requests model-context-delivery@1. An activation requests @2 and
/// must never be replaced with the transition current when delivery arrives.
struct Model_context_delivery
{
    std::optional<Model_context_activation> activation;
};

Json_value encode_model_context_delivery(const Model_context_delivery& delivery);
std::optional<Model_context_delivery> decode_model_context_delivery(
    const Json_value& value, Contract_failure* out_failure);

/**
 * @brief The package-helper client (docs/helper_ingress_2.md, "The helper
 * client"): the only client code a package ships against Logonomic. It
 * knows the subcommands and exit codes of the `vnmctl` executable named by
 * LOGONOMIC_VNMCTL_PATH and nothing of the control protocol, the endpoint
 * or the token. Every call spawns that executable with the current
 * environment, writes one argument object to its stdin and reads its
 * stdout up to a bound; the executable supplies the session's authority
 * from the environment itself.
 */

/// The variables a bound terminal carries.
inline constexpr const char* k_helper_env_vnmctl_path        = "LOGONOMIC_VNMCTL_PATH";
inline constexpr const char* k_helper_env_session_capability = "LOGONOMIC_SESSION_CAPABILITY";
inline constexpr const char* k_helper_env_package_id         = "LOGONOMIC_PROVIDER_PACKAGE_ID";
inline constexpr const char* k_helper_env_generation_digest  = "LOGONOMIC_PROVIDER_GENERATION_DIGEST";
inline constexpr const char* k_helper_env_generation_nonce   = "LOGONOMIC_PROVIDER_GENERATION_NONCE";
inline constexpr const char* k_helper_env_helpers_dir        = "LOGONOMIC_PROVIDER_HELPERS_DIR";

/// The wrapper's exit codes, which every client method reports.
inline constexpr int k_helper_exit_committed         = 0;   // committed; the outcome says whether it was accepted
inline constexpr int k_helper_exit_transport_failure = 1;
inline constexpr int k_helper_exit_invalid_arguments = 2;   // arguments or environment
inline constexpr int k_helper_exit_pending           = 4;   // still pending at the client deadline

/// The bound terminal's environment, read once from the process
/// environment.
struct Helper_environment
{
    std::string vnmctl_path;          // LOGONOMIC_VNMCTL_PATH
    std::string session_capability;   // LOGONOMIC_SESSION_CAPABILITY
    std::string package_id;           // LOGONOMIC_PROVIDER_PACKAGE_ID
    std::string generation_digest;    // LOGONOMIC_PROVIDER_GENERATION_DIGEST
    std::string generation_nonce;     // LOGONOMIC_PROVIDER_GENERATION_NONCE
    std::string helpers_dir;          // LOGONOMIC_PROVIDER_HELPERS_DIR
};

enum class Helper_local_failure
{
    INVALID_ARGUMENTS, SPAWN_FAILED, CANCELLED, TIMED_OUT, OUTPUT_TOO_LARGE,
    ABNORMAL_EXIT, WRAPPER_FAILED, INVALID_RESPONSE
};

/// Developer detail contains only SDK-owned text and numeric status or size.
struct Helper_local_diagnostic
{
    Helper_local_failure failure;
    std::string          detail;
};

/// The host-owned, one-shot instruction decision for a title-capable hook.
/// `completed` distinguishes a wrapper failure from a committed refusal.
struct Title_instruction_result
{
    bool        completed            = false;
    bool        accepted             = false;
    bool        instruction_required = false;
    std::string instruction;
    std::string reason;
    int         exit_code            = 0;
    std::optional<Helper_local_diagnostic> local_diagnostic;
};

/// Reads and validates the six variables. Reason
/// `invalid_helper_environment`, the detail naming the absent, empty or
/// out-of-grammar variable.
std::optional<Helper_environment> read_helper_environment(Contract_failure* out_failure);

/// The same validation over an explicit environment (name, value pairs).
std::optional<Helper_environment> parse_helper_environment(
    const std::vector<std::pair<std::string, std::string>>& environment,
    Contract_failure*                                       out_failure);

struct Provider_event_identity
{
    std::string idempotency_key;
    std::string transition_nonce;
};

/// Keep the event and both identities together until delivery and acknowledgement finish.
struct Prepared_provider_event
{
    std::string             event_name;
    Json_value              body;
    Provider_event_identity identity = {random_nonce_hex(), random_nonce_hex()};
};

/// What one delivery ended as. `committed` is false for a transport
/// failure, invalid arguments or the deadline; `exit_code` is then the
/// wrapper's, or k_helper_exit_invalid_arguments when the client refused
/// the call before spawning.
struct Provider_event_outcome
{
    bool                         committed = false;
    bool                         accepted  = false;
    std::string                  reason;            // when !accepted
    std::optional<std::uint64_t> epoch;             // the epoch the event was applied to
    std::optional<Json_value>    helper_response;   // when the adapter returned one
    int                          exit_code = k_helper_exit_transport_failure;
    Provider_event_identity      identity;
    std::optional<Helper_local_diagnostic> local_diagnostic;
};

class Helper_client
{
public:
    /// The optional predicate must be nonblocking and must not throw. When it
    /// becomes true, no new wrapper is spawned and a live wrapper is killed and
    /// reaped before the call returns a transport failure. This does not revoke
    /// an event the host already admitted.
    explicit Helper_client(Helper_environment environment, std::function<bool()> cancellation_requested = {});

    /// `provider-event deliver --args-stdin` with `{"idempotency_key",
    /// "transition_nonce", "event": {"name", "body"}}`. An empty key or
    /// nonce is minted as 32 lowercase hex from the random source; a
    /// retry after a lost response passes the same pair again.
    Provider_event_outcome deliver_provider_event(
        std::string_view  event_name,
        const Json_value& body,
        std::string_view  idempotency_key  = {},
        std::string_view  transition_nonce = {});

    Provider_event_outcome deliver_provider_event(Prepared_provider_event delivery);

    /// Releases the committed response retained for `idempotency_key`.
    /// Repeating an acknowledgement is idempotent until the journal TTL.
    int acknowledge_provider_event(std::string_view idempotency_key);

    /// `set-title <title>`.
    int set_title(std::string_view title);

    /// `request-title-instruction --args-stdin`. The helper supplies only
    /// hook facts; the authenticated host owns the one-shot decision and the
    /// rendered instruction bytes.
    Title_instruction_result request_title_instruction(
        std::string_view source,
        std::string_view hook_event_name,
        bool             changed_files_requested = false,
        std::optional<Model_context_delivery> delivery = std::nullopt);

    /// `call logonomic.diff show_changed_files -- <paths...>`.
    int show_changed_files(const std::vector<std::string>& paths);

    /// `call logonomic.diff show --manifest-stdin` with the manifest on stdin.
    int show_diff(std::string_view manifest_json);

    const Helper_environment& environment() const { return m_environment; }

private:
    Helper_environment m_environment;
    std::function<bool()> m_cancellation_requested;
};

} // namespace logonomic::cli_addon
