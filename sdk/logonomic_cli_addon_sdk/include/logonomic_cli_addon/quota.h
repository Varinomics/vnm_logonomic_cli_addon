#pragma once

#include "logonomic_cli_addon/probe_contracts.h"

#include <string_view>
#include <vector>

namespace logonomic::cli_addon {

enum class Quota_status
{
    READY,
    NOT_LOGGED_IN,
    UNAVAILABLE,
    FAILED,
};

enum class Quota_failure
{
    AUTHENTICATION_REQUIRED,
    CREDENTIALS_EXPIRED,
    REQUEST_REJECTED,
    REQUEST_FAILED,
    INVALID_RESPONSE,
    TIMED_OUT,
    PROCESS_FAILED,
    REGISTRY_FAILED,
};

enum class Quota_failure_stage
{
    HTTP_RESPONSE,
    PROCESS_START,
    PROCESS_EXIT,
    REGISTRY_READ,
};

struct Quota_diagnostic
{
    Quota_failure_stage stage;
    std::int64_t        code;
};

struct Quota_window
{
    double                       used_percent = 0;
    std::optional<std::uint64_t> window_minutes;
    std::optional<std::uint64_t> resets_at_ms;
    std::optional<std::string>   label;
    std::optional<std::string>   reset_text = std::nullopt;
    std::optional<bool>          headline = std::nullopt;
};

struct Quota_observation
{
    Observation_scope           scope;
    Quota_status                state = Quota_status::UNAVAILABLE;
    std::optional<Quota_window> primary;
    std::optional<Quota_window> secondary;
    std::optional<std::string>  plan;
    std::optional<std::int64_t> reset_credits;
    std::optional<std::string>  credits_balance;
    // quota@3 owns this collection; quota@2 owns primary and secondary.
    std::optional<std::vector<Quota_window>> windows = std::nullopt;
};

/// A source-scoped monetary observation, independent of percentage windows.
/// `label` names the actual coverage; local estimates are never billed spend.
struct Quota_cost
{
    double                amount = 0;
    std::string           currency;
    bool                  estimated = true;
    std::string           label;
    std::optional<double> available;
};

struct Quota_inspect_result
{
    bool                             complete = false;
    std::string                      protocol_state;
    std::optional<Quota_observation> observation;
    std::optional<std::string>       stdin_bytes = std::nullopt;
    std::optional<std::string>       failure_reason = std::nullopt;
    std::optional<Quota_diagnostic>  diagnostic = std::nullopt;

    /// Set by the decoder when a quota-failure@1 explanation arrived but could
    /// not be used. The failure then carries no cause.
    bool                             explanation_ignored = false;

    std::optional<Quota_cost>        cost = std::nullopt;
    /// A source read failed; absence of a source leaves this false.
    bool                             cost_failed = false;
    /// Unusable quota-cost evidence leaves the ordinary quota result intact.
    bool                             cost_ignored = false;
};

const char* quota_status_name(Quota_status state);
const char* quota_failure_reason(Quota_failure failure);
const char* quota_failure_description(Quota_failure failure);
std::optional<Quota_failure> parse_quota_failure(std::string_view reason);
const char* quota_failure_stage_name(Quota_failure_stage stage);

/// Produces a completed failed observation without exposing native output.
Quota_inspect_result make_quota_failure(
    Quota_failure failure,
    Observation_scope scope = {},
    std::optional<Quota_diagnostic> diagnostic = std::nullopt);

/// quota-failure@1 is optional presentation evidence beside a failed quota
/// observation. A host that adopted it opts in through the inspect request.
bool decode_quota_failure_declaration(const Json_value& payload, Contract_failure* out_failure);
Json_value encode_quota_failure_request();
bool decode_quota_failure_request(const Json_value& value);

/// quota-cost@1 is negotiated independently from the selected quota version.
bool decode_quota_cost_declaration(const Json_value& payload, Contract_failure* out_failure);
Json_value encode_quota_cost_request();
bool decode_quota_cost_request(const Json_value& value);
Json_value encode_quota_cost(const Quota_cost& cost);
std::optional<Quota_cost> decode_quota_cost(const Json_value& value);

Json_value encode_quota_diagnostic(const Quota_diagnostic& diagnostic);
std::optional<Quota_diagnostic> decode_quota_diagnostic(const Json_value& value);

/// A failure's cause leaves the package only when `report_failure` is set,
/// which a producer takes from the request's `report_quota_failure`. Without
/// it the result carries a neutral failed observation, plus
/// `failure_reason` when authentication was lost.
/// Monetary evidence leaves the package only when `report_cost` is set.
Json_value encode_quota_inspect_result(
    const Quota_inspect_result& result,
    std::uint16_t version = 2,
    bool report_failure = false,
    bool report_cost = false);
std::optional<Quota_inspect_result> decode_quota_inspect_result(
    const Json_value& payload, bool final_inspection, Contract_failure* out_failure, std::uint16_t version = 2);

} // namespace logonomic::cli_addon
