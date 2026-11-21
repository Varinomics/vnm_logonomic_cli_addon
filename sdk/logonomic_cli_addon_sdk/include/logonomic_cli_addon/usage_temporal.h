#pragma once

#include "logonomic_cli_addon/capability_ids.h"
#include "logonomic_cli_addon/probe_contracts.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace logonomic::cli_addon {

inline constexpr std::size_t k_max_usage_temporal_portions = 4096;
inline constexpr std::size_t k_max_usage_temporal_bytes = 768u * 1024u;

struct Usage_temporal_parent
{
    std::string record_id;
    std::uint64_t total_tokens = 0;
    std::string snapshot_id;
    std::optional<std::string> source_account_key;
};

struct Usage_temporal_retained_from
{
    std::string snapshot_id;
    std::string manifest_sha256_hex;
};

// Each portion is a disjoint proved increment of its accounting parent.
// Source-calendar dates preserve their stated meaning and are never instants.
struct Usage_temporal_portion
{
    std::string basis = "instant";
    std::string instant;
    std::string date;
    std::string calendar;
    std::uint64_t total_tokens = 0;
};

struct Usage_temporal
{
    Usage_temporal_parent parent;
    std::vector<Usage_temporal_portion> portions;
    std::uint64_t unavailable_tokens = 0;
    std::string coverage_reason = "complete";
    std::optional<Usage_temporal_retained_from> retained_from;
};

/// Converts a native usage timestamp to milliseconds since the Unix epoch.
/// Explicit Z or +/-hh:mm zones use their supplied offset; an omitted zone
/// uses local mktime rules, including DST. Extra fractional digits truncate.
/// This conversion is separate from the usage-temporal instant validation.
std::optional<std::int64_t> parse_native_usage_timestamp(std::string_view iso);

bool decode_usage_temporal_declaration(const Json_value& payload, Contract_failure* out_failure);
Json_value encode_usage_temporal(const Usage_temporal& evidence);
// Unknown envelope versions remain opaque in Usage_record::capabilities.
std::optional<Usage_temporal> decode_usage_temporal(const Json_value& payload, Contract_failure* out_failure);

} // namespace logonomic::cli_addon
