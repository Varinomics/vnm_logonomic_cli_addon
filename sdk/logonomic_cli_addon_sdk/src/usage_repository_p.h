#pragma once

#include "logonomic_cli_addon/usage_scan.h"

namespace logonomic::cli_addon::usage_repository_detail {

Json_value encode(const Usage_repository& repository);
std::optional<Usage_repository> decode(
    const Json_value& payload, std::uint64_t total_tokens, Contract_failure* out_failure);

} // namespace logonomic::cli_addon::usage_repository_detail
