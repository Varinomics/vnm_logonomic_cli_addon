#pragma once

#include <string>

namespace logonomic::cli_addon::tools {

int inspect_desktop(
    const std::string& directory, const std::string& declaration_path,
    const std::string& platform, bool live);

} // namespace logonomic::cli_addon::tools
