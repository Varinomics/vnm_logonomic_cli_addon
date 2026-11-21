#include "logonomic_cli_addon/package_source.h"
#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/limits.h"

#include <windows.h>

#include <string>

#include <algorithm>
#include <utility>

namespace logonomic::cli_addon {

namespace {

constexpr std::size_t k_max_entries_per_directory =
    limits::k_max_files_per_package + 2;   // bootstrap and index are outside the index
constexpr std::size_t k_max_package_entries =
    limits::k_max_files_per_package * limits::k_max_package_path_segments + 2;

class Handle
{
public:
    explicit Handle(HANDLE handle = INVALID_HANDLE_VALUE) : m_handle(handle) {}
    ~Handle()
    {
        if (m_handle != INVALID_HANDLE_VALUE && m_handle != nullptr) {
            ::CloseHandle(m_handle);
        }
    }

    Handle(const Handle&)            = delete;
    Handle& operator=(const Handle&) = delete;

    HANDLE get() const { return m_handle; }
    bool valid() const { return m_handle != INVALID_HANDLE_VALUE && m_handle != nullptr; }

private:
    HANDLE m_handle;
};

std::string last_error_text(DWORD error)
{
    return "Win32 error " + std::to_string(error);
}

std::optional<std::wstring> to_wide(std::string_view utf8)
{
    if (utf8.empty()) {
        return std::wstring();
    }
    const int length = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), (int)utf8.size(), nullptr, 0);
    if (length <= 0) {
        return std::nullopt;
    }
    std::wstring wide((std::size_t)length, L'\0');
    ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), (int)utf8.size(), wide.data(), length);
    return wide;
}

std::optional<std::string> to_utf8(const wchar_t* wide)
{
    const int length = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return std::nullopt;
    }
    std::string utf8((std::size_t)length, '\0');
    ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, utf8.data(), length, nullptr, nullptr);
    utf8.resize((std::size_t)length - 1);   // drop the terminator
    return utf8;
}

std::optional<std::wstring> native_path(
    const std::filesystem::path& root,
    std::string_view             relative_path)
{
    std::wstring full = root.native();
    if (!full.empty() && full.back() != L'\\' && full.back() != L'/') {
        full.push_back(L'\\');
    }
    const std::optional<std::wstring> wide = to_wide(relative_path);
    if (!wide) {
        return std::nullopt;
    }
    std::wstring tail = *wide;
    std::replace(tail.begin(), tail.end(), L'/', L'\\');
    full += tail;
    return std::optional<std::wstring>(std::move(full));
}

bool set_error(Source_error* out_error, Source_error_kind kind, std::string detail)
{
    if (out_error != nullptr) {
        out_error->kind   = kind;
        out_error->detail = std::move(detail);
    }
    return false;
}

Source_entry_kind classify_attributes(DWORD attributes)
{
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) { return Source_entry_kind::LINK; }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)     { return Source_entry_kind::DIRECTORY; }
    if ((attributes & FILE_ATTRIBUTE_DEVICE) != 0)        { return Source_entry_kind::OTHER; }
    return Source_entry_kind::REGULAR_FILE;
}

bool list_directory(
    const std::filesystem::path& root,
    const std::string&           prefix,
    std::size_t                  depth,
    std::size_t&                 seen_entries,
    std::vector<Source_entry>&   out_entries,
    Source_error*                out_error,
    std::stop_token              cancellation)
{
    const std::optional<std::wstring> directory = native_path(root, prefix);
    if (!directory) {
        return set_error(out_error, Source_error_kind::INVALID_UTF8,
            "directory path is not valid UTF-8: " + prefix);
    }
    const std::wstring pattern = *directory + L"*";
    WIN32_FIND_DATAW data{};
    HANDLE find = ::FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
    if (find == INVALID_HANDLE_VALUE) {
        const DWORD error = ::GetLastError();
        if (error == ERROR_FILE_NOT_FOUND) {
            return true;   // empty directory
        }
        return set_error(out_error, Source_error_kind::IO_ERROR,
            "cannot list directory " + prefix + ": " + last_error_text(error));
    }

    std::vector<Source_entry> found;
    do {
        if (cancellation.stop_requested()) {
            ::FindClose(find);
            return set_error(out_error, Source_error_kind::IO_ERROR, "package listing was cancelled");
        }
        const std::wstring_view name(data.cFileName);
        if (name == L"." || name == L"..") {
            continue;
        }
        if (depth + 1 > limits::k_max_package_path_segments) {
            ::FindClose(find);
            return set_error(out_error, Source_error_kind::BOUND_EXCEEDED,
                "k_max_package_path_segments exceeded while listing " + prefix);
        }
        if (found.size() >= k_max_entries_per_directory ||
            seen_entries >= k_max_package_entries)
        {
            ::FindClose(find);
            return set_error(out_error, Source_error_kind::BOUND_EXCEEDED,
                "package entry bound derived from k_max_files_per_package exceeded while listing");
        }
        ++seen_entries;
        Source_entry entry;
        const std::optional<std::string> utf8_name = to_utf8(data.cFileName);
        if (!utf8_name.has_value()) {
            entry.relative_path = prefix + "?";
            entry.kind          = Source_entry_kind::OTHER;
            found.push_back(entry);
            continue;
        }
        entry.relative_path = prefix + *utf8_name;
        entry.kind          = classify_attributes(data.dwFileAttributes);
        if (entry.kind == Source_entry_kind::REGULAR_FILE) {
            entry.bytes = ((std::uint64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
        }
        found.push_back(entry);
    }
    while (!cancellation.stop_requested() && ::FindNextFileW(find, &data));
    const DWORD end_error = ::GetLastError();
    ::FindClose(find);
    if (cancellation.stop_requested()) {
        return set_error(out_error, Source_error_kind::IO_ERROR, "package listing was cancelled");
    }
    if (end_error != ERROR_NO_MORE_FILES) {
        return set_error(out_error, Source_error_kind::IO_ERROR,
            "cannot read directory " + prefix + ": " + last_error_text(end_error));
    }

    for (const Source_entry& entry : found) {
        if (cancellation.stop_requested()) {
            return set_error(out_error, Source_error_kind::IO_ERROR, "package listing was cancelled");
        }
        out_entries.push_back(entry);
        if (entry.kind == Source_entry_kind::DIRECTORY) {
            if (!list_directory(
                    root,
                    entry.relative_path + "/",
                    depth + 1,
                    seen_entries,
                    out_entries,
                    out_error,
                    cancellation))
            {
                return false;
            }
        }
    }
    return true;
}

} // namespace

Directory_package_source::Directory_package_source(
    std::filesystem::path root,
    std::stop_token cancellation)
:
    m_root(std::move(root)),
    m_cancellation(cancellation)
{}

std::optional<std::vector<Source_entry>> Directory_package_source::list(Source_error* out_error) const
{
    if (cancellation_requested(out_error)) {
        return std::nullopt;
    }
    const DWORD attributes = ::GetFileAttributesW(m_root.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        set_error(out_error, Source_error_kind::IO_ERROR,
            "cannot open package directory " + m_root.string() + ": " + last_error_text(::GetLastError()));
        return std::nullopt;
    }

    std::vector<Source_entry> entries;
    std::size_t seen_entries = 0;
    if (!list_directory(m_root, "", 0, seen_entries, entries, out_error, m_cancellation)) {
        return std::nullopt;
    }
    std::sort(entries.begin(), entries.end(), [](const Source_entry& a, const Source_entry& b) {
        return a.relative_path < b.relative_path;
    });
    return entries;
}

std::optional<std::string> Directory_package_source::read(
    std::string_view relative_path,
    std::uint64_t    max_bytes,
    Source_error*    out_error) const
{
    if (cancellation_requested(out_error)) {
        return std::nullopt;
    }
    // Every directory component is verified against reparse attributes: the
    // final component through the open handle, the intermediate ones by an
    // explicit attribute query, because CreateFileW follows junctions in the
    // middle of a path.
    const std::vector<std::string_view> segments = split_path_segments(relative_path);
    std::string walked;
    for (std::size_t i = 0; i + 1 < segments.size(); ++i) {
        if (cancellation_requested(out_error)) {
            return std::nullopt;
        }
        walked += std::string(segments[i]);
        const std::optional<std::wstring> directory = native_path(m_root, walked);
        if (!directory) {
            set_error(out_error, Source_error_kind::INVALID_UTF8,
                "path is not valid UTF-8: " + std::string(relative_path));
            return std::nullopt;
        }
        const DWORD attributes = ::GetFileAttributesW(directory->c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            set_error(out_error, Source_error_kind::MISSING, "cannot open " + std::string(relative_path));
            return std::nullopt;
        }
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            set_error(out_error, Source_error_kind::LINK, walked + " is a reparse point");
            return std::nullopt;
        }
        walked += "/";
    }

    const std::optional<std::wstring> full = native_path(m_root, relative_path);
    if (!full) {
        set_error(out_error, Source_error_kind::INVALID_UTF8,
            "path is not valid UTF-8: " + std::string(relative_path));
        return std::nullopt;
    }
    Handle file(::CreateFileW(
        full->c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr));
    if (!file.valid()) {
        const DWORD error = ::GetLastError();
        const Source_error_kind kind = (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            ? Source_error_kind::MISSING
            : Source_error_kind::IO_ERROR;
        set_error(out_error, kind, "cannot open " + std::string(relative_path) + ": " + last_error_text(error));
        return std::nullopt;
    }

    BY_HANDLE_FILE_INFORMATION information{};
    if (!::GetFileInformationByHandle(file.get(), &information)) {
        set_error(out_error, Source_error_kind::IO_ERROR, "cannot stat " + std::string(relative_path));
        return std::nullopt;
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        set_error(out_error, Source_error_kind::LINK, std::string(relative_path) + " is a reparse point");
        return std::nullopt;
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 || ::GetFileType(file.get()) != FILE_TYPE_DISK) {
        set_error(out_error, Source_error_kind::NOT_REGULAR, std::string(relative_path) + " is not a regular file");
        return std::nullopt;
    }
    const std::uint64_t size = ((std::uint64_t)information.nFileSizeHigh << 32) | information.nFileSizeLow;
    if (size > max_bytes) {
        set_error(out_error, Source_error_kind::TOO_LARGE, std::string(relative_path) + " exceeds the read bound");
        return std::nullopt;
    }

    std::string bytes;
    bytes.reserve((std::size_t)size);
    char buffer[65536];
    while (true) {
        if (cancellation_requested(out_error)) {
            return std::nullopt;
        }
        DWORD count = 0;
        if (!::ReadFile(file.get(), buffer, (DWORD)sizeof(buffer), &count, nullptr)) {
            set_error(out_error, Source_error_kind::IO_ERROR,
                "cannot read " + std::string(relative_path) + ": " + last_error_text(::GetLastError()));
            return std::nullopt;
        }
        if (count == 0) {
            break;
        }
        bytes.append(buffer, count);
        if (bytes.size() > max_bytes) {
            set_error(out_error, Source_error_kind::TOO_LARGE, std::string(relative_path) + " exceeds the read bound");
            return std::nullopt;
        }
    }
    if (cancellation_requested(out_error)) {
        return std::nullopt;
    }
    return bytes;
}

std::string Directory_package_source::describe() const
{
    return m_root.string();
}

} // namespace logonomic::cli_addon
