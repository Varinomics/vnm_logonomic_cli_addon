#pragma once

#include <filesystem>
#include <string>

namespace logonomic::cli_addon {

// Use the extended spelling for filesystem calls on absolute native paths.
// Persisted identities and protocol fields keep their canonical spelling.
inline std::filesystem::path native_io_path(const std::filesystem::path& absolute_path)
{
#ifdef _WIN32
    const auto path = std::filesystem::path(absolute_path).make_preferred().native();
    if (path.starts_with(LR"(\\?\)") || path.starts_with(LR"(\\.\)")) {
        return path;
    }
    if (path.starts_with(LR"(\\)")) {
        return LR"(\\?\UNC\)" + path.substr(2);
    }
    return LR"(\\?\)" + path;
#else
    return absolute_path;
#endif
}

} // namespace logonomic::cli_addon
