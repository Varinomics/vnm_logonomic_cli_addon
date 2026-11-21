#pragma once

#include "logonomic_cli_addon/lifecycle_events.h"

#include <optional>
#include <string>
#include <vector>

namespace logonomic::cli_addon {

inline constexpr const char* k_lifecycle_correlation_lifetime_reserve_operation = "reserve";
inline constexpr const char* k_lifecycle_correlation_lifetime_retire_operation  = "retire";

struct Lifecycle_correlation_reserve_request
{
    Session_correlation correlation;
};

enum class Lifecycle_correlation_retire_disposition
{
    ABANDONED,
    TERMINAL_CONSUMED,
};

struct Lifecycle_correlation_retire_request
{
    Session_correlation                       correlation;
    Lifecycle_correlation_retire_disposition  disposition =
        Lifecycle_correlation_retire_disposition::ABANDONED;
    std::optional<std::string>                consumed_cursor;
};

/// Both lifecycle-correlation-lifetime operations acknowledge success with
/// the exact empty object. The invocation envelope already identifies it.
struct Lifecycle_correlation_lifetime_ack {};

Json_value encode_lifecycle_correlation_reserve_request(
    const Lifecycle_correlation_reserve_request& request);
std::optional<Lifecycle_correlation_reserve_request>
decode_lifecycle_correlation_reserve_request(
    const Json_value& payload,
    Contract_failure* out_failure);

Json_value encode_lifecycle_correlation_retire_request(
    const Lifecycle_correlation_retire_request& request);
std::optional<Lifecycle_correlation_retire_request>
decode_lifecycle_correlation_retire_request(
    const Json_value& payload,
    Contract_failure* out_failure);

Json_value encode_lifecycle_correlation_lifetime_ack(
    const Lifecycle_correlation_lifetime_ack& ack = {});
std::optional<Lifecycle_correlation_lifetime_ack>
decode_lifecycle_correlation_lifetime_ack(
    const Json_value& payload,
    Contract_failure* out_failure);

/// Requires lifecycle-events@2 and any host-known live helper-ingress pair
/// (@1, @2, or @3) in this generation.
bool validate_lifecycle_correlation_lifetime_dependency(
    const std::vector<Capability_pair>& live_pairs,
    Contract_failure*                   out_failure);

} // namespace logonomic::cli_addon
