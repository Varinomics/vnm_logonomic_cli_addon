#include "logonomic_cli_addon_generation_custody_platform.h"

#include "logonomic_cli_addon/package_source.h"

#include <windows.h>

#include <algorithm>
#include <string>
#include <utility>

namespace logonomic::cli_addon_host::custody_platform {

namespace {

namespace sdk = logonomic::cli_addon;

std::string describe(const std::filesystem::path& path)
{
    return sdk::path_to_utf8(path);
}

std::string system_error_text(DWORD code)
{
    wchar_t*    buffer = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::string text = "error " + std::to_string(code);
    if (length > 0) {
        const int utf8_length = ::WideCharToMultiByte(CP_UTF8, 0, buffer, (int)length, nullptr, 0, nullptr, nullptr);
        if (utf8_length > 0) {
            text.assign((std::size_t)utf8_length, '\0');
            ::WideCharToMultiByte(CP_UTF8, 0, buffer, (int)length, text.data(), utf8_length, nullptr, nullptr);
            while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ')) {
                text.pop_back();
            }
        }
    }
    if (buffer != nullptr) {
        ::LocalFree(buffer);
    }
    return text;
}

std::string last_error_text()
{
    return system_error_text(::GetLastError());
}

bool report(std::string* out_error, std::string message)
{
    *out_error = std::move(message);
    return false;
}

HANDLE handle_of(const Native_handle& handle)
{
    return (HANDLE)handle.value();
}

// Rejects a reparse point and the wrong entry kind on an already open
// handle: FILE_FLAG_OPEN_REPARSE_POINT opened the link itself, so the
// attributes describe the entry that was named, not a target.
bool check_opened_kind(HANDLE handle, bool want_directory, const std::filesystem::path& path, std::string* out_error)
{
    BY_HANDLE_FILE_INFORMATION info{};
    if (!::GetFileInformationByHandle(handle, &info)) {
        return report(out_error, "cannot query " + describe(path) + ": " + last_error_text());
    }
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return report(out_error, describe(path) + " is a reparse point");
    }
    const bool is_directory = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (is_directory != want_directory) {
        return report(out_error, describe(path) + (want_directory ? " is not a directory" : " is not a regular file"));
    }
    return true;
}

bool open_directory_no_follow(const std::filesystem::path& path, Native_handle& out_directory, std::string* out_error)
{
    const HANDLE handle = ::CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return report(out_error, "cannot open directory " + describe(path) + ": " + last_error_text());
    }
    Native_handle opened((Native_handle_value)handle, path);
    if (!check_opened_kind(handle, true, path, out_error)) {
        return false;
    }
    out_directory = std::move(opened);
    return true;
}

} // namespace

void Native_handle::close()
{
    if (m_value != k_invalid_native_handle) {
        ::CloseHandle((HANDLE)m_value);
        m_value = k_invalid_native_handle;
    }
}

bool open_directory(
    const std::filesystem::path& path,
    bool                         no_follow,
    Native_handle&               out_directory,
    std::string*                 out_error)
{
    if (no_follow) {
        return open_directory_no_follow(path, out_directory, out_error);
    }
    const HANDLE handle = ::CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return report(out_error, "cannot open directory " + describe(path) + ": " + last_error_text());
    }
    out_directory = Native_handle((Native_handle_value)handle, path);
    return true;
}

bool reopen_directory(
    const Native_handle& directory,
    Native_handle&       out_directory,
    std::string*         out_error)
{
    // Every directory this layer reopens was opened without following links,
    // so reopening it by path under the same rule is equivalent.
    return open_directory_no_follow(directory.path(), out_directory, out_error);
}

bool open_child_directory(
    const Native_handle& parent,
    const std::string&   name,
    Native_handle&       out_directory,
    std::string*         out_error)
{
    return open_directory_no_follow(child_path(parent, name), out_directory, out_error);
}

bool create_child_directory(
    const Native_handle& parent,
    const std::string&   name,
    std::string*         out_error)
{
    const std::filesystem::path path = child_path(parent, name);
    if (!::CreateDirectoryW(path.c_str(), nullptr) && ::GetLastError() != ERROR_ALREADY_EXISTS) {
        return report(out_error, "cannot create directory " + describe(path) + ": " + last_error_text());
    }
    return true;
}

bool open_child_file_for_read(
    const Native_handle& parent,
    const std::string&   name,
    Native_handle&       out_file,
    std::uint64_t&       out_size,
    std::string*         out_error)
{
    const std::filesystem::path path = child_path(parent, name);
    const HANDLE handle = ::CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return report(out_error, "cannot open " + describe(path) + ": " + last_error_text());
    }
    Native_handle opened((Native_handle_value)handle, path);
    if (!check_opened_kind(handle, false, path, out_error)) {
        return false;
    }
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(handle, &size)) {
        return report(out_error, "cannot query " + describe(path) + ": " + last_error_text());
    }
    out_size = (std::uint64_t)size.QuadPart;
    out_file = std::move(opened);
    return true;
}

bool create_child_file(
    const Native_handle& parent,
    const std::string&   name,
    Native_handle&       out_file,
    std::string*         out_error)
{
    const std::filesystem::path path = child_path(parent, name);
    const HANDLE handle = ::CreateFileW(
        path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return report(out_error, "cannot create " + describe(path) + ": " + last_error_text());
    }
    out_file = Native_handle((Native_handle_value)handle, path);
    return true;
}

bool read_some(
    const Native_handle& file,
    char*                buffer,
    std::size_t          size,
    std::size_t&         out_count,
    std::string*         out_error)
{
    const DWORD request = (DWORD)std::min<std::size_t>(size, MAXDWORD);
    DWORD       count   = 0;
    if (!::ReadFile(handle_of(file), buffer, request, &count, nullptr)) {
        return report(out_error, "cannot read " + describe(file.path()) + ": " + last_error_text());
    }
    out_count = count;
    return true;
}

bool write_all(
    const Native_handle& file,
    const char*          data,
    std::size_t          size,
    std::string*         out_error)
{
    while (size > 0) {
        const DWORD request = (DWORD)std::min<std::size_t>(size, MAXDWORD);
        DWORD       written = 0;
        if (!::WriteFile(handle_of(file), data, request, &written, nullptr)) {
            return report(out_error, "cannot write " + describe(file.path()) + ": " + last_error_text());
        }
        data += written;
        size -= written;
    }
    return true;
}

bool flush_file(const Native_handle& file, std::string* out_error)
{
    if (!::FlushFileBuffers(handle_of(file))) {
        return report(out_error, "cannot flush " + describe(file.path()) + ": " + last_error_text());
    }
    return true;
}

bool publish_file(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    std::string*                 out_error)
{
    if (!::MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
        return report(out_error,
            "cannot publish " + describe(source) + " at " + describe(destination) + ": " + last_error_text());
    }
    return true;
}

bool set_child_mode(
    const Native_handle& parent,
    const std::string&   name,
    Entry_mode           mode,
    std::string*         out_error)
{
    // Directory read-only is advisory on Windows; the contract is that
    // Logonomic never writes below a published generation.
    if (mode == Entry_mode::READ_ONLY_DIRECTORY) {
        return true;
    }
    const std::filesystem::path path = child_path(parent, name);
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        !::SetFileAttributesW(path.c_str(), attributes | FILE_ATTRIBUTE_READONLY))
    {
        return report(out_error, "cannot make " + describe(path) + " read-only: " + last_error_text());
    }
    return true;
}

bool publish_directory(
    Native_handle&               directory,
    const std::filesystem::path& destination,
    std::string*                 out_error)
{
    // A directory with an open handle cannot be moved, and its read-only
    // state is advisory, so the handle is released before the rename.
    const std::filesystem::path source = directory.path();
    directory.close();
    if (!::MoveFileExW(source.c_str(), destination.c_str(), 0)) {
        return report(out_error,
            "cannot rename " + describe(source) + " to " + describe(destination) + ": " + last_error_text());
    }
    return true;
}

void flush_directory(const std::filesystem::path& path)
{
    // Directory entries are made durable by the file flushes on NTFS; there
    // is no per-directory flush to issue.
    (void)path;
}

Lock_outcome acquire_lock(
    const std::filesystem::path& path,
    Lock_mode                    mode,
    Native_handle_value&         out_handle,
    std::string*                 out_error,
    bool                         create_if_missing,
    Lock_wait                    wait)
{
    const HANDLE handle = ::CreateFileW(
        path.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, create_if_missing ? OPEN_ALWAYS : OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | (create_if_missing ? 0 : FILE_FLAG_OPEN_REPARSE_POINT), nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        report(out_error, "cannot open lock file " + describe(path) + ": " + last_error_text());
        return Lock_outcome::FAILED;
    }
    if (!create_if_missing && !check_opened_kind(handle, false, path, out_error)) {
        ::CloseHandle(handle);
        return Lock_outcome::FAILED;
    }
    OVERLAPPED overlapped{};
    const DWORD flags = (wait == Lock_wait::NON_BLOCKING ? LOCKFILE_FAIL_IMMEDIATELY : 0) |
        (mode == Lock_mode::EXCLUSIVE ? LOCKFILE_EXCLUSIVE_LOCK : 0);
    if (!::LockFileEx(handle, flags, 0, 1, 0, &overlapped)) {
        const DWORD code = ::GetLastError();
        ::CloseHandle(handle);
        if (code == ERROR_LOCK_VIOLATION) {
            return Lock_outcome::BUSY;
        }
        report(out_error, "cannot lock " + describe(path) + ": " + system_error_text(code));
        return Lock_outcome::FAILED;
    }
    out_handle = (Native_handle_value)handle;
    return Lock_outcome::ACQUIRED;
}

void release_lock(Native_handle_value handle)
{
    OVERLAPPED overlapped{};
    ::UnlockFileEx((HANDLE)handle, 0, 1, 0, &overlapped);
    ::CloseHandle((HANDLE)handle);
}

} // namespace logonomic::cli_addon_host::custody_platform
