#pragma once

#include <algorithm>
#include <filesystem>

namespace logonomic::cli_addon_host {

// Convert an admitted absolute logical path only at the file-I/O boundary.
// Callers retain the logical path for protocol messages and diagnostics.
inline std::filesystem::path native_file_path(const std::filesystem::path& path)
{
#if defined(_WIN32)
    if (!path.is_absolute()) {
        return path;
    }
    std::wstring native = path.native();
    std::replace(native.begin(), native.end(), L'/', L'\\');
    if (native.starts_with(L"\\\\?\\") || native.starts_with(L"\\\\.\\")) {
        return native;
    }
    if (native.size() >= 3 && native[1] == L':' && native[2] == L'\\') {
        return L"\\\\?\\" + native;
    }
    if (native.starts_with(L"\\\\")) {
        return L"\\\\?\\UNC\\" + native.substr(2);
    }
#endif
    return path;
}

} // namespace logonomic::cli_addon_host
