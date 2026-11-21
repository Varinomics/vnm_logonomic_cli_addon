#pragma once

#include "logonomic_cli_addon/adapter_runtime.h"
#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/json.h"
#include "logonomic_cli_addon/protocol.h"
#include "logonomic_cli_addon/usage_owned_event_correction.h"
#include "logonomic_cli_addon/usage_scan.h"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace logonomic::cli_addon::test {

// The first request selects an adapter operation; the handler's next read
// raises private text to exercise the real outer exception boundary.
class Sentinel_exception_context final : public Invocation_context
{
public:
    const Invoke_request& request() const override
    {
        if (++m_reads > 1) throw std::runtime_error("private-exception-sentinel");
        return m_request;
    }
    bool cancel_requested() const override { return false; }
    bool wait_for_cancel(int) override { return false; }
    void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override {}
    void succeed(Json_value) override { throw std::runtime_error("unexpected adapter success"); }
    void fail(Result_status status, std::string reason, std::string message) override
    {
        m_status = status;
        m_reason = std::move(reason);
        m_message = std::move(message);
    }
    bool finished() const override { return m_status.has_value(); }

    Invoke_request m_request;
    std::optional<Result_status> m_status;
    std::string m_reason;
    std::string m_message;

private:
    mutable unsigned m_reads = 0;
};

// Direct handler tests inspect successful payloads and catch expected failures.
// Waiting is deliberately nonblocking; timed cancellation needs its own fixture.
class Throwing_invocation_context final : public Invocation_context
{
public:
    const Invoke_request& request() const override { return m_request; }
    bool cancel_requested() const override { return m_cancelled; }
    bool wait_for_cancel(int) override { return false; }
    void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override {}
    void succeed(Json_value result) override { m_result = std::move(result); }
    void fail(Result_status status, std::string reason, std::string) override
    {
        m_failure_status = status;
        throw std::runtime_error(reason);
    }
    bool finished() const override { return m_result.has_value(); }

    Invoke_request m_request;
    std::optional<Json_value> m_result;
    std::optional<Result_status> m_failure_status;
    bool m_cancelled = false;
};

// A cancelled scan must stop before completing or failing its invocation.
class Cancelled_scan_context final : public Invocation_context
{
public:
    explicit Cancelled_scan_context(const Usage_scan_request& input)
    {
        m_request.capability = {"usage-scan", 3};
        m_request.operation  = "scan";
        m_request.payload    = encode_usage_scan_request(input, 3);
    }

    explicit Cancelled_scan_context(const Usage_owned_event_correction_request& input)
    {
        m_request.capability = {k_usage_owned_event_correction_capability_id, 1};
        m_request.operation  = "scan";
        m_request.payload    = encode_usage_owned_event_correction_request(input);
    }

    const Invoke_request& request() const override { return m_request; }
    bool cancel_requested() const override { return true; }
    bool wait_for_cancel(int) override { return true; }
    void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override {}
    void succeed(Json_value) override { throw std::runtime_error("cancelled scan succeeded"); }
    void fail(Result_status, std::string reason, std::string) override
    {
        throw std::runtime_error(reason);
    }
    bool finished() const override { return false; }

private:
    Invoke_request m_request;
};

} // namespace logonomic::cli_addon::test
