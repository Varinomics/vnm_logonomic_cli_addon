#pragma once

#include "logonomic_cli_addon/accounts.h"
#include "logonomic_cli_addon/profile_storage.h"
#include "logonomic_cli_addon/probe_contracts.h"
#include "logonomic_cli_addon/usage_cross_source_event.h"
#include "logonomic_cli_addon/usage_temporal.h"

#include <filesystem>
#include <functional>

namespace logonomic::cli_addon {

inline constexpr const char* k_usage_scan_operation = "scan";
inline constexpr std::size_t k_max_usage_page_records = 256;

enum class Usage_counter_semantics
{
    CUMULATIVE_FINAL,
    DELTA_EVENT,
};

enum class Usage_support
{
    UNKNOWN,
    SUPPORTED,
    UNSUPPORTED,
};

enum class Usage_coverage_unit
{
    RECORDS,
    SESSIONS,
};

struct Usage_field_coverage
{
    std::uint64_t covered_records = 0;
    std::uint64_t total_records = 0;
    bool overflow = false;
    Usage_support support = Usage_support::UNKNOWN;
};

struct Usage_repository_allocation
{
    std::string repository_path;
    std::uint64_t total_tokens = 0;
};

struct Usage_repository
{
    std::vector<Usage_repository_allocation> allocations;
    std::uint64_t unattributed_tokens = 0;
};

struct Usage_amount
{
    bool                         applicable = false;
    std::optional<std::uint64_t> value;
    std::optional<Usage_field_coverage> coverage = std::nullopt;
};

struct Usage_record
{
    std::string                record_id;
    std::optional<std::string> session_key;
    std::string                completed_date;
    std::uint64_t              total_tokens = 0;
    std::optional<std::string> source_path;
    std::optional<std::string> repository_path;
    Usage_counter_semantics    semantics = Usage_counter_semantics::CUMULATIVE_FINAL;
    Usage_amount               input;
    Usage_amount               output;
    Usage_amount               cache_read;
    Usage_amount               cache_creation;
    Usage_coverage_unit coverage_unit = Usage_coverage_unit::RECORDS;
    std::optional<Usage_repository> repository_usage = std::nullopt;
    std::optional<std::string> model = std::nullopt;
    std::optional<std::string> counter_field = std::nullopt;
    std::optional<std::string> source_counter_semantics = std::nullopt;
    // Required on usage-scan@3; absence cannot assert complete aggregate evidence.
    std::optional<std::string> aggregate_evidence = std::nullopt;
    // Independently versioned optional proof. Missing or unrecognized proof
    // never changes the base usage record's interpretation.
    std::optional<Usage_cross_source_event> cross_source_event = std::nullopt;
    std::optional<Usage_temporal> temporal = std::nullopt;
    // Preserve envelopes this SDK cannot interpret through typed cache replay.
    Json_value capabilities = Json_value::object();
};

struct Usage_cache_reference
{
    std::string root;
    std::string snapshot_id;
    std::string manifest_sha256_hex;

    bool operator==(const Usage_cache_reference&) const = default;
};

struct Usage_cache_selection
{
    std::optional<Usage_cache_reference> accepted;
    std::string candidate_root;
};

struct Usage_cache_file
{
    std::string path;
    std::uint64_t bytes = 0;
    std::string sha256_hex;
};

struct Usage_cache_manifest
{
    std::optional<Account_context> account_context;
    std::string snapshot_id;
    std::string format;
    std::vector<Usage_cache_file> files;
};

struct Usage_scan_request
{
    Session_launch_context     context;
    std::string                mode = "incremental";
    std::optional<std::string> snapshot_id = std::nullopt;
    std::optional<std::string> cursor = std::nullopt;
    std::optional<Usage_cache_selection> cache_selection = std::nullopt;
};

struct Usage_scan_page
{
    Observation_scope          scope;
    std::string                snapshot_id;
    std::optional<std::string> next_cursor;
    bool                       complete                       = false;
    bool                       incomplete_historical_coverage = false;
    std::vector<Usage_record>  records;
    std::optional<Usage_cache_reference> cache_candidate = std::nullopt;
    std::optional<Account_context> account_context = std::nullopt;
    std::optional<Profile_storage_context> profile_storage = std::nullopt;
};

Json_value encode_usage_cache_reference(const Usage_cache_reference& reference);
std::optional<Usage_cache_reference> decode_usage_cache_reference(
    const Json_value& payload, Contract_failure* out_failure);
Json_value encode_usage_cache_manifest(const Usage_cache_manifest& manifest);
std::optional<Usage_cache_manifest> decode_usage_cache_manifest(
    const Json_value& payload, Contract_failure* out_failure);
// Lexical scope validation. Callers must additionally reject filesystem links/reparse points.
bool validate_usage_cache_selection(const Usage_scan_request& request, Contract_failure* out_failure);
bool validate_usage_cache_root(
    const std::string& root, const std::string& profile_root, Contract_failure* out_failure);
bool validate_usage_cache_allocation(
    const std::string& candidate_root, const std::string& profile_root, Contract_failure* out_failure);
// I/O spelling for an already validated absolute cache path. Never use this
// spelling as a wire identity or as a substitute for scope/link validation.
std::filesystem::path usage_cache_io_path(const std::filesystem::path& path);
// Incremental readers verify the sealed identity, then only the payloads they consume.
std::optional<Usage_cache_manifest> validate_usage_cache_manifest(
    const Usage_cache_reference& reference,
    const std::string& profile_root,
    const std::optional<Account_context>& account,
    const std::function<bool()>& cancelled,
    Contract_failure* out_failure);
bool validate_usage_cache_file(
    const Usage_cache_reference& reference,
    const Usage_cache_manifest& manifest,
    const std::string& relative_path,
    const std::function<bool()>& cancelled,
    Contract_failure* out_failure);
// Stream-verifies all files and exact inventory; never modifies the selected cache.
std::optional<Usage_cache_manifest> validate_usage_cache_candidate(
    const Usage_cache_reference& reference,
    const std::string& profile_root,
    const std::optional<Account_context>& account,
    const std::function<bool()>& cancelled,
    Contract_failure* out_failure);
// Seals an exclusively owned flat candidate directory. No acceptance or reclamation occurs.
std::optional<Usage_cache_reference> seal_usage_cache_candidate(
    const std::string& candidate_root,
    const std::string& profile_root,
    const std::optional<Account_context>& account,
    const std::string& snapshot_id,
    const std::string& format,
    const std::function<bool()>& cancelled,
    Contract_failure* out_failure);

const char* usage_counter_semantics_name(Usage_counter_semantics semantics);
// The current scan owns its frozen environment projection. An empty declaration
// requests only the baseline and explicitly authored environment names.
std::optional<Launcher_discovery_declaration> decode_usage_scan_declaration(
    const Json_value& payload, Contract_failure* out_failure);

Json_value encode_usage_scan_request(const Usage_scan_request& request, std::uint16_t version = 3);
std::optional<Usage_scan_request> decode_usage_scan_request(
    const Json_value& payload, Contract_failure* out_failure, std::uint16_t version = 3);

Json_value encode_usage_record(const Usage_record& record, std::uint16_t version = 3);
// Append only when the existing count and transport payload budgets fit. False
// leaves the page and byte counter unchanged, so the producer retains its next
// spool record. Start encoded_records_bytes at zero with an empty page; it tracks
// each record's encoded bytes plus one separator byte. Keep it in sync when
// removing or changing records. Other page fields may change between appends.
bool append_usage_page_record(Usage_scan_page& page, const Usage_record& record,
    std::size_t& encoded_records_bytes, std::uint16_t version = 3, std::size_t reserved_payload_bytes = 0);
std::optional<Usage_record> decode_usage_record(
    const Json_value& payload, Contract_failure* out_failure, std::uint16_t version = 3);

Json_value encode_usage_scan_result(const Usage_scan_page& page, std::uint16_t version = 3);
std::optional<Usage_scan_page> decode_usage_scan_result(
    const Json_value& payload, Contract_failure* out_failure, std::uint16_t version = 3);

} // namespace logonomic::cli_addon
