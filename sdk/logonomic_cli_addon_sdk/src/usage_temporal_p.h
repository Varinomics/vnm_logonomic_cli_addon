#pragma once

#include "logonomic_cli_addon/usage_temporal.h"

namespace logonomic::cli_addon::usage_temporal_detail {

Json_value encode_portions(const std::vector<Usage_temporal_portion>& portions);
std::optional<std::vector<Usage_temporal_portion>> decode_portions(
    const Json_value& portions,
    std::uint64_t total_tokens,
    std::uint64_t unavailable_tokens,
    Contract_failure* out_failure);

} // namespace logonomic::cli_addon::usage_temporal_detail
