#pragma once

#include "logonomic_cli_addon/accounts.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace logonomic::cli_addon {

inline constexpr const char* k_usage_lifetime_summary_read_operation = "read";
inline constexpr std::size_t k_max_usage_lifetime_models = 256;

struct Usage_lifetime_summary_request
{
    // accounts@2 freezes the source profile path and its adapter key.
    Account_context account_context;
};

struct Usage_lifetime_model
{
    std::string model;
    std::uint64_t input_tokens = 0;
    std::uint64_t output_tokens = 0;
    std::uint64_t cache_read_tokens = 0;
    std::uint64_t cache_creation_tokens = 0;
};

struct Usage_lifetime_summary
{
    Account_context account_context;
    std::string source_kind;
    std::optional<std::int64_t> source_format_version;
    std::string source_content_digest;
    std::optional<std::string> cache_stated_date;
    // The source profile identifies custody; it does not identify the
    // historical provider principal that produced these counters.
    std::string historical_principal = "unknown";
    std::vector<Usage_lifetime_model> models;
};

bool decode_usage_lifetime_summary_declaration(
    const Json_value& payload, Contract_failure* out_failure);

Json_value encode_usage_lifetime_summary_request(const Usage_lifetime_summary_request& request);
std::optional<Usage_lifetime_summary_request> decode_usage_lifetime_summary_request(
    const Json_value& payload, Contract_failure* out_failure);

Json_value encode_usage_lifetime_summary_result(const Usage_lifetime_summary& summary);
std::optional<Usage_lifetime_summary> decode_usage_lifetime_summary_result(
    const Json_value& payload, Contract_failure* out_failure);

} // namespace logonomic::cli_addon
