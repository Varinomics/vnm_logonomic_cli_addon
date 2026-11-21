#pragma once

#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif
#if defined(_WIN32)
#include <windows.h>
#endif

#include <filesystem>

namespace logonomic::cli_addon_host {

inline bool path_entry_is_link(
    const std::filesystem::path&        in_path,
    const std::filesystem::file_status& in_status)
{
    if (std::filesystem::is_symlink(in_status)) {
        return true;
    }
#if defined(_WIN32)
    const DWORD attributes = GetFileAttributesW(in_path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    (void)in_path;
    return false;
#endif
}

} // namespace logonomic::cli_addon_host
