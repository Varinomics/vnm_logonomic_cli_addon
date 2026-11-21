#pragma once

#include "logonomic_cli_addon/accounts.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace logonomic::cli_addon {

inline constexpr const char* k_usage_saved_daily_tokens_read_operation = "read";
inline constexpr std::size_t k_max_usage_saved_daily_days = 4096;
inline constexpr std::size_t k_max_usage_saved_daily_models_per_day = 256;
inline constexpr std::size_t k_max_usage_saved_daily_model_rows = 16384;

struct Usage_saved_daily_tokens_request
{
    Account_context account_context;
};

struct Usage_saved_daily_model
{
    std::string model;
    std::uint64_t tokens = 0;
};

struct Usage_saved_daily_day
{
    std::string date;
    std::vector<Usage_saved_daily_model> models;
};

struct Usage_saved_daily_tokens
{
    Account_context account_context;
    std::string source_kind;
    std::optional<std::int64_t> source_format_version;
    std::optional<std::int64_t> daily_series_format_version;
    std::string source_content_digest;
    std::optional<std::string> cache_stated_date;
    std::string historical_principal = "unknown";
    std::vector<Usage_saved_daily_day> days;
};

bool decode_usage_saved_daily_tokens_declaration(
    const Json_value& payload, Contract_failure* out_failure);

Json_value encode_usage_saved_daily_tokens_request(const Usage_saved_daily_tokens_request& request);
std::optional<Usage_saved_daily_tokens_request> decode_usage_saved_daily_tokens_request(
    const Json_value& payload, Contract_failure* out_failure);

Json_value encode_usage_saved_daily_tokens_result(const Usage_saved_daily_tokens& summary);
std::optional<Usage_saved_daily_tokens> decode_usage_saved_daily_tokens_result(
    const Json_value& payload, Contract_failure* out_failure);

} // namespace logonomic::cli_addon
