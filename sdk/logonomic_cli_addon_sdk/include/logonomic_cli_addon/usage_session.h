#pragma once

#include "logonomic_cli_addon/capability_ids.h"
#include "logonomic_cli_addon/usage_scan.h"

#include <optional>
#include <string>
#include <vector>

namespace logonomic::cli_addon {

inline constexpr const char* k_usage_session_describe_operation = "describe";

struct Usage_session_identity
{
    std::string record_id;
    std::optional<std::string> session_key;

    bool operator==(const Usage_session_identity&) const = default;
};

// Correlates an opaque ledger key with the identifier lifecycle events report.
// The cutoff describes the enclosing record's source, independently of its amount.
struct Usage_session_fact
{
    std::string provider_session_id;
    bool excludes_turn_in_flight = false;

    bool operator==(const Usage_session_fact&) const = default;
};

struct Usage_session_description
{
    Usage_session_identity identity;
    std::optional<Usage_session_fact> session;
};

bool decode_usage_session_declaration(const Json_value& payload, Contract_failure* out_failure);
Json_value encode_usage_session_fact(const Usage_session_fact& fact);
std::optional<Usage_session_fact> decode_usage_session_fact(
    const Json_value& payload, Contract_failure* out_failure);
Json_value encode_usage_session_request(const std::vector<Usage_session_identity>& records);
std::optional<std::vector<Usage_session_identity>> decode_usage_session_request(
    const Json_value& payload, Contract_failure* out_failure);
Json_value encode_usage_session_result(const std::vector<Usage_session_description>& records);
std::optional<std::vector<Usage_session_description>> decode_usage_session_result(
    const Json_value& payload, Contract_failure* out_failure);

} // namespace logonomic::cli_addon
