#pragma once

#include <charconv>
#include <string_view>

namespace logonomic::cli_addon::detail {

// The caller has already validated the complete decimal token's grammar.
std::from_chars_result parse_decimal(std::string_view text, double& value);

} // namespace logonomic::cli_addon::detail
