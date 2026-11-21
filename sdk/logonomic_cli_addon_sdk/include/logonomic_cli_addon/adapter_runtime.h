#pragma once

#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/json.h"
#include "logonomic_cli_addon/protocol.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace logonomic::cli_addon {

/**
 * @brief One live invocation as seen by an adapter's handler. The handler
 * runs on a thread of its own, reports progress through the context and
 * ends the request with exactly one terminal result; a second terminal call
 * is ignored, and a handler that returns without one is answered with
 * `error`/`handler_incomplete` by the runtime.
 */
class Invocation_context
{
public:
    virtual ~Invocation_context() = default;

    /// Borrowed through the handler call, including cleanup after a terminal
    /// result. Copy any input needed after the handler returns.
    virtual const Invoke_request& request() const = 0;

    /// True once the host cancelled this request or asked for shutdown.
    virtual bool cancel_requested() const = 0;

    /// Sleeps until cancellation, shutdown or the timeout; true when the
    /// wait ended because cancellation was requested.
    virtual bool wait_for_cancel(int timeout_ms) = 0;

    /// Reports progress throughout the request. After an initial burst, frames
    /// are spaced at least 10 ms apart; a newer pending report replaces the
    /// previous one. A held report is flushed even when its handler falls
    /// quiet, unless a result ends the request first. A peer's explicit finite
    /// progress limit retains budget-aware pacing and an exhaustion diagnostic.
    virtual void report_progress(
        std::uint64_t                completed,
        std::optional<std::uint64_t> total,
        std::string                  note = {}) = 0;

    virtual void succeed(Json_value payload) = 0;
    virtual void fail(Result_status status, std::string reason, std::string message = {}) = 0;

    /// `unavailable`/`unknown_operation`, the answer for an operation the
    /// capability does not define.
    void reject_unknown_operation() { fail(Result_status::UNAVAILABLE, reason::k_unknown_operation); }

    virtual bool finished() const = 0;
};

struct Adapter_runtime_options
{
    std::string                  adapter_name;
    std::string                  adapter_version;
    std::vector<Capability_pair> capabilities;   // declared in the handshake
    Adapter_limits               limits;         // announced when set; the runtime never raises a host limit
};

/**
 * @brief The adapter side of the core protocol for adapter authors: reads
 * frames from stdin, answers the host hello, dispatches `invoke` to the
 * handler on a thread per request, routes `cancel` to the live context,
 * honours `shutdown` and stdin end-of-file, and serializes every frame it
 * writes to stdout. An invoke for a capability pair the adapter did not
 * declare is answered `unavailable`/`unknown_operation` without reaching the
 * handler.
 */
class Adapter_runtime
{
public:
    using Invoke_handler = std::function<void(Invocation_context&)>;
    using Hello_hook     = std::function<void(const Host_hello&, Adapter_hello&)>;
    using Shutdown_hook  = std::function<void(const Shutdown_request&)>;

    explicit Adapter_runtime(Adapter_runtime_options options);
    ~Adapter_runtime();

    Adapter_runtime(const Adapter_runtime&)            = delete;
    Adapter_runtime& operator=(const Adapter_runtime&) = delete;

    void set_invoke_handler(Invoke_handler handler);

    /// Called with the decoded host hello and the hello the runtime is
    /// about to send, which the adapter may adjust (for example to lower a
    /// limit) before it is written.
    void set_hello_hook(Hello_hook hook);

    /// Called when the host asks for shutdown, before live requests are
    /// cancelled.
    void set_shutdown_hook(Shutdown_hook hook);

    /// Runs until stdin end-of-file or shutdown. Process exit codes are:
    /// 0 after a clean stop, 1 after a fatal runtime failure, 2 when the host
    /// violated the protocol, and 3 when no core protocol generation was
    /// shared with the host. Fatal runtime failure or shutdown grace expiry
    /// may terminate the process directly; otherwise returns the exit code.
    int run();

    /// Writes bytes to stdout verbatim under the frame lock. Conformance
    /// fixtures use it to emit deliberately nonconforming frames; a
    /// conforming adapter has no use for it.
    void write_raw(std::string_view bytes);

    /// Available after the handshake.
    const Host_hello& host_hello() const;
    Protocol_limits effective_limits() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace logonomic::cli_addon
