#pragma once

#include "logonomic_cli_addon/capability_ids.h"
#include "logonomic_cli_addon/usage_scan.h"

namespace logonomic::cli_addon {

inline constexpr const char* k_usage_repository_attribution_scan_operation = "scan";
inline constexpr const char* k_usage_repository_attribution_incremental_operation = "incremental";

enum class Usage_repository_attribution_tag_status
{
    AVAILABLE,
    UNSUPPORTED,
    INVALID
};

// The operation selects authority; the tag identifies only a fresh repository
// correction. Neither changes the accounting authority of usage-scan@3.
bool decode_usage_repository_attribution_declaration(const Json_value& payload, Contract_failure* out_failure);
Json_value encode_usage_repository_attribution_tag();
Usage_repository_attribution_tag_status decode_usage_repository_attribution_tag(
    const Json_value& payload, Contract_failure* out_failure);

} // namespace logonomic::cli_addon
