#pragma once

#include "logonomic_cli_addon/usage_scan.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace logonomic::cli_addon {

inline constexpr const char* k_usage_owned_event_correction_scan_operation = "scan";

struct Usage_owned_event_predecessor_reference
{
    std::string path;
    std::string sha256_hex;
};

struct Usage_owned_event_correction_request
{
    Usage_scan_request scan;
    Usage_owned_event_predecessor_reference predecessors;
};

struct Usage_owned_event_predecessor_header
{
    std::string provider_id;
    std::optional<Account_context> account_context;
    std::string profile_root;
    std::optional<Usage_cache_reference> accepted_cache;
};

// The digest binds the complete host row, not this accounting projection.
// Temporal acceptance is opaque; decoding it grants no subtraction authority.
struct Usage_owned_event_predecessor
{
    Usage_record record;
    std::string predecessor_sha256_hex;
    Json_value temporal_acceptance;
};

struct Usage_owned_event_counter_removal
{
    std::uint64_t value = 0;
    std::uint64_t covered_records = 0;
};

struct Usage_owned_event_temporal_removal
{
    std::vector<Usage_temporal_portion> portions;
    std::uint64_t unavailable_tokens = 0;
};

struct Usage_owned_event_removal
{
    std::uint64_t total_tokens = 0;
    std::uint64_t total_records = 0;
    Usage_owned_event_counter_removal input, output, cache_read, cache_creation;
    Usage_repository repository_usage;
    std::optional<Usage_owned_event_temporal_removal> temporal;
};

struct Usage_owned_event_correction
{
    std::string disposition;
    std::string predecessor_sha256_hex;
    std::string coverage_policy;
    std::optional<Usage_owned_event_removal> removed;
    std::vector<std::string> original_record_ids;
};

bool decode_usage_owned_event_correction_declaration(
    const Json_value& payload, Contract_failure* out_failure);

Json_value encode_usage_owned_event_correction_request(const Usage_owned_event_correction_request& request);
std::optional<Usage_owned_event_correction_request> decode_usage_owned_event_correction_request(
    const Json_value& payload, Contract_failure* out_failure);

Json_value encode_usage_owned_event_predecessor_header(const Usage_owned_event_predecessor_header& header);
std::optional<Usage_owned_event_predecessor_header> decode_usage_owned_event_predecessor_header(
    const Json_value& payload, Contract_failure* out_failure);

Json_value encode_usage_owned_event_predecessor(const Usage_owned_event_predecessor& predecessor);
std::optional<Usage_owned_event_predecessor> decode_usage_owned_event_predecessor(
    const Json_value& payload, Contract_failure* out_failure);

Json_value encode_usage_owned_event_correction(const Usage_owned_event_correction& correction);
// Only a positively selected correction scan may interpret this envelope.
// Unknown versions are unavailable locally and do not invalidate base records.
std::optional<Usage_owned_event_correction> decode_usage_owned_event_correction(
    const Json_value& payload, Contract_failure* out_failure);

} // namespace logonomic::cli_addon
