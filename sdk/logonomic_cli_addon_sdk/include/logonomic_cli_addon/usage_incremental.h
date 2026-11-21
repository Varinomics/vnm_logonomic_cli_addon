#pragma once

#include "logonomic_cli_addon/usage_scan.h"

namespace logonomic::cli_addon {

struct Usage_incremental_result
{
    Usage_cache_reference baseline;
    // Absence retains the accepted accounting and source metadata unchanged.
    // Present pages contain sorted changed absolute observations; omitted IDs
    // retain their accepted values. An empty complete page updates metadata only.
    std::optional<Usage_scan_page> changes = std::nullopt;
    std::optional<Account_context> account_context = std::nullopt;
    std::optional<Profile_storage_context> profile_storage = std::nullopt;
};

bool decode_usage_incremental_declaration(const Json_value& payload, Contract_failure* out_failure);
Json_value encode_usage_incremental_request(const Usage_scan_request& request);
std::optional<Usage_scan_request> decode_usage_incremental_request(
    const Json_value& payload, Contract_failure* out_failure);
Json_value encode_usage_incremental_result(const Usage_incremental_result& result);
std::optional<Usage_incremental_result> decode_usage_incremental_result(
    const Json_value& payload, Contract_failure* out_failure);

// A candidate owns a self-contained link to each inherited immutable file.
// Files copied when hard links are unavailable are hashed again when sealed.
bool inherit_usage_cache_file(
    const Usage_cache_reference& baseline,
    const Usage_cache_manifest& baseline_manifest,
    const std::string& relative_path,
    const std::string& candidate_root,
    const std::string& profile_root,
    const std::function<bool()>& cancelled,
    Contract_failure* out_failure);
std::optional<Usage_cache_reference> seal_usage_incremental_cache_candidate(
    const std::string& candidate_root,
    const std::string& profile_root,
    const std::optional<Account_context>& account,
    const std::string& snapshot_id,
    const std::string& format,
    const Usage_cache_reference& baseline,
    const std::function<bool()>& cancelled,
    Contract_failure* out_failure);
// Only exact baseline descriptors referring to the same immutable file may
// reuse a hash; every new or copied file is stream-verified normally.
std::optional<Usage_cache_manifest> validate_usage_incremental_cache_candidate(
    const Usage_cache_reference& candidate,
    const Usage_cache_reference& baseline,
    const std::string& profile_root,
    const std::optional<Account_context>& account,
    const std::function<bool()>& cancelled,
    Contract_failure* out_failure);

} // namespace logonomic::cli_addon
