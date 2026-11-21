#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace logonomic::cli_addon::launcher_detail {

std::optional<std::vector<std::string>> direct_argv(std::string_view script);

} // namespace logonomic::cli_addon::launcher_detail
