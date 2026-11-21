#pragma once

#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/json.h"
#include "logonomic_cli_addon/limits.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace logonomic::cli_addon {

/**
 * @brief Core protocol generation 1 messages (docs/core_protocol.md). The
 * envelope member set of every message is closed: a member the generation
 * does not define is the violation `unexpected_member`. Capability payloads
 * are opaque JSON objects.
 */

/// Limits the host announces in its hello; all four are required.
struct Protocol_limits
{
    std::uint64_t max_frame_bytes                 = limits::k_max_frame_bytes;
    std::uint64_t max_json_depth                  = limits::k_max_json_depth;
    std::uint64_t max_in_flight_requests          = limits::k_max_in_flight_requests;
    std::uint64_t max_progress_frames_per_request = limits::k_unlimited_progress_frames;
};

/// Limits an adapter may lower in its hello; each is optional and must not
/// exceed the host's value. The effective limit is the smaller.
struct Adapter_limits
{
    std::optional<std::uint64_t> max_frame_bytes;
    std::optional<std::uint64_t> max_json_depth;
    std::optional<std::uint64_t> max_in_flight_requests;
    std::optional<std::uint64_t> max_progress_frames_per_request;
};

struct Host_hello
{
    std::vector<std::uint16_t> core_protocol_generations;
    std::string                package_id;
    std::string                generation_digest;   // 64 lowercase hex
    std::string                generation_nonce;    // 32 lowercase hex
    Protocol_limits            limits;
    std::string                host_product;
    std::string                host_version;
};

struct Invoke_request
{
    std::string     request_id;
    Capability_pair capability;
    std::string     operation;
    Json_value      payload = Json_value::object();
    // k_no_invoke_deadline_ms leaves completion to the result or cancellation.
    std::int64_t    deadline_ms = limits::k_default_invoke_deadline_ms;
};

struct Cancel_request
{
    std::string request_id;
};

struct Shutdown_request
{
    std::int64_t grace_ms = limits::k_shutdown_grace_ms;
};

struct Adapter_hello
{
    std::uint16_t  core_protocol_generation = 0;
    std::string    package_id;
    std::string    generation_digest;
    std::string    generation_nonce;
    Adapter_limits limits;
    std::string    adapter_name;
    std::string    adapter_version;
};

struct Capabilities_message
{
    std::vector<Capability_pair> capabilities;
};

struct Progress_message
{
    std::string                  request_id;
    std::uint64_t                completed = 0;
    std::optional<std::uint64_t> total;
    std::string                  note;
};

enum class Result_status
{
    SUCCESS,
    UNAVAILABLE,
    CANCELLED,
    FAILED,   // the wire status `error`; named to stay clear of the ERROR macro of <windows.h>
};

const char* result_status_name(Result_status status);
std::optional<Result_status> parse_result_status(std::string_view name);

struct Result_message
{
    std::string   request_id;
    Result_status status = Result_status::FAILED;
    Json_value    payload = Json_value::object();   // SUCCESS only
    std::string   reason;                           // UNAVAILABLE, CANCELLED, ERROR
    std::string   message;                          // optional, with reason
};

using Host_message    = std::variant<Host_hello, Invoke_request, Cancel_request, Shutdown_request>;
using Adapter_message = std::variant<Adapter_hello, Capabilities_message, Progress_message, Result_message>;

const char* host_message_type(const Host_message& message);
const char* adapter_message_type(const Adapter_message& message);

/// The JSON object of a message; encode_frame() turns it into transport bytes.
Json_value encode_host_message(const Host_message& message);
Json_value encode_adapter_message(const Adapter_message& message);

/// Strict decode of one frame (without its newline). Reason codes:
/// `frame_violation` (not a JSON object, missing or mistyped member, grammar),
/// `unknown_message_type`, `unexpected_member`.
std::optional<Host_message> decode_host_message(
    std::string_view         frame,
    const Json_parse_limits& parse_limits,
    Contract_failure*        out_failure);

std::optional<Adapter_message> decode_adapter_message(
    std::string_view         frame,
    const Json_parse_limits& parse_limits,
    Contract_failure*        out_failure);

/**
 * @brief Host-side sequencing and correlation rules of one adapter session:
 * exactly one hello then exactly one capabilities before anything else,
 * identity equal to the host's hello, adapter limits within the host's,
 * progress and results only for live requests, one terminal result per
 * request, and any finite progress limit negotiated with a peer. The host simulator and Logonomic's broker
 * apply the same rules through this one class.
 */
class Session_tracker
{
public:
    explicit Session_tracker(Host_hello sent_hello);

    /// Applies one decoded adapter message. On a violation, returns false
    /// with the reason code (`identity_mismatch`, `out_of_order_message`,
    /// `frame_violation`, `impossible_correlation`,
    /// `duplicate_terminal_result`, `progress_limit_exceeded`).
    bool accept(const Adapter_message& message, Contract_failure* out_violation);

    /// Registers the next request in the host's consecutive, monotonically
    /// increasing request-ID sequence so its frames correlate.
    void note_invoke_sent(const std::string& request_id);

    bool ready() const { return m_phase == Phase::READY; }
    const Host_hello& host_hello() const { return m_host_hello; }
    const std::optional<Adapter_hello>& adapter_hello() const { return m_adapter_hello; }
    const std::vector<Capability_pair>& handshake_capabilities() const { return m_capabilities; }
    bool handshake_declares(const Capability_pair& pair) const;

    /// Host limits lowered by whatever the adapter hello lowered.
    Protocol_limits effective_limits() const;

    bool is_live(std::string_view request_id) const;
    bool is_finished(std::string_view request_id) const;
    std::size_t live_request_count() const { return m_live.size(); }

private:
    enum class Phase
    {
        AWAITING_HELLO,
        AWAITING_CAPABILITIES,
        READY,
    };

    bool accept_hello(const Adapter_hello& hello, Contract_failure* out_violation);
    bool accept_capabilities(const Capabilities_message& capabilities, Contract_failure* out_violation);
    bool accept_progress(const Progress_message& progress, Contract_failure* out_violation);
    bool accept_result(const Result_message& result, Contract_failure* out_violation);

    Host_hello                         m_host_hello;
    std::optional<Adapter_hello>       m_adapter_hello;
    std::vector<Capability_pair>       m_capabilities;
    Phase                              m_phase = Phase::AWAITING_HELLO;
    std::map<std::string, std::size_t> m_live;       // request id -> progress frames seen
    std::string                        m_highest_issued_request_id;
};

} // namespace logonomic::cli_addon
