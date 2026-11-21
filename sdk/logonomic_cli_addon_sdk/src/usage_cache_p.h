#pragma once

#include "logonomic_cli_addon/usage_scan.h"

namespace logonomic::cli_addon {

// The DOM codec and streamed manifest I/O share the exact entry grammar.
Json_value encode_usage_cache_file(const Usage_cache_file& file);
std::optional<Usage_cache_file> decode_usage_cache_file(
    const Json_value& payload, Contract_failure* out_failure);

} // namespace logonomic::cli_addon
