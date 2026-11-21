#include "logonomic_cli_addon/accounts.h"
#include "logonomic_cli_addon/package_source.h"
#include "native_io_path.h"

#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace logonomic::cli_addon {

namespace fs = std::filesystem;

#ifdef _WIN32
namespace {

enum class Directory_case
{
    INSENSITIVE,
    SENSITIVE,
    MISSING,
    UNAVAILABLE,
};

enum class Path_use { DERIVE_KEY, COMPARE_GRANT, VERIFY_HISTORICAL_KEY };

Directory_case directory_case(const std::wstring& path)
{
    const HANDLE directory = CreateFileW(native_io_path(path).c_str(), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (directory == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
            ? Directory_case::MISSING : Directory_case::UNAVAILABLE;
    }

    FILE_CASE_SENSITIVE_INFO info{};
    Directory_case mode = Directory_case::UNAVAILABLE;
    if (GetFileInformationByHandleEx(directory, FileCaseSensitiveInfo, &info, sizeof(info))) {
        mode = (info.Flags & FILE_CS_FLAG_CASE_SENSITIVE_DIR) ? Directory_case::SENSITIVE : Directory_case::INSENSITIVE;
    }
    else {
        // Some volumes guarantee insensitive lookup without exposing NTFS's
        // per-directory setting. The capability flags distinguish these volumes.
        DWORD flags = 0;
        if (GetVolumeInformationByHandleW(directory, nullptr, 0, nullptr, nullptr, &flags, nullptr, 0) &&
            !(flags & FILE_CASE_SENSITIVE_SEARCH))
        {
            mode = Directory_case::INSENSITIVE;
        }
    }
    CloseHandle(directory);
    return mode;
}

std::optional<std::wstring> mapped_case(std::wstring_view text, DWORD mapping)
{
    const int size  = (int)text.size();
    const int count = LCMapStringEx(LOCALE_NAME_INVARIANT, mapping, text.data(), size, nullptr, 0, nullptr, nullptr, 0);
    if (count <= 0) return std::nullopt;
    std::wstring mapped(count, L'\0');
    if (LCMapStringEx(LOCALE_NAME_INVARIANT, mapping, text.data(), size, mapped.data(), count, nullptr, nullptr, 0) != count) {
        return std::nullopt;
    }
    return mapped;
}

wchar_t ascii_lower(wchar_t c)
{
    return c >= L'A' && c <= L'Z' ? (wchar_t)(c + L'a' - L'A') : c;
}

struct Folded_component
{
    bool        cased = false;
    std::string exact_offsets;
};

bool append_folded(
    std::wstring& output, std::wstring_view text, Account_profile_case_fold fold, Folded_component& component)
{
    const auto size = text.size();
    while (!text.empty()) {
        const wchar_t first = text.front();
        if (first < 128 || fold == Account_profile_case_fold::ASCII) {
            const wchar_t lower = ascii_lower(first);
            output += lower;
            if (lower >= L'a' && lower <= L'z') component.cased = true;
            text.remove_prefix(1);
            continue;
        }
        const auto scalar = text.substr(0,
            text.size() >= 2 && IS_HIGH_SURROGATE(first) && IS_LOW_SURROGATE(text[1]) ? 2 : 1);
        const auto lower = mapped_case(scalar, LCMAP_LOWERCASE);
        const auto upper = mapped_case(scalar, LCMAP_UPPERCASE);
        if (!lower || !upper) return false;
        if (*lower != *upper) component.cased = true;

        const auto same_ordinal = [scalar](const std::wstring& other) {
            return CompareStringOrdinal(scalar.data(), (int)scalar.size(), other.data(), (int)other.size(), TRUE) == CSTR_EQUAL;
        };
        // Linguistic case mappings can connect two distinct NTFS names.
        // Check both directions so the lowercase spelling cannot reuse their old key.
        if (same_ordinal(*lower) && same_ordinal(*upper)) {
            output += *lower;
        }
        else {
            output += scalar;
            component.exact_offsets += ',' + std::to_string(size - text.size());
        }
        text.remove_prefix(scalar.size());
    }
    return true;
}

std::size_t root_length(const fs::path& path)
{
    const auto& name = path.native();
    if (!name.starts_with(LR"(\\)")) return path.root_path().native().size();

    // The share name is part of an ordinary or extended UNC root. A drive or
    // volume using the extended prefix also has two namespace components.
    const bool extended_unc = name.size() >= 8 &&
        CompareStringOrdinal(name.data(), 8, LR"(\\?\UNC\)", 8, TRUE) == CSTR_EQUAL;
    const auto server = name.find(L'\\', extended_unc ? 8 : 2);
    const auto share  = server == name.npos ? name.npos : name.find(L'\\', server + 1);
    return share == name.npos ? name.size() : share + 1;
}

std::optional<std::string> windows_identity(
    const fs::path& path, Account_profile_case_fold fold, Path_use use, bool* out_key_only = nullptr)
{
    const auto  native = fs::path(path).make_preferred().native();
    const auto  root   = root_length(native);
    std::wstring identity;
    Folded_component root_fold;
    if (!append_folded(identity, std::wstring_view(native).substr(0, root), fold, root_fold)) {
        return std::nullopt;
    }
    bool native_distinction = !root_fold.exact_offsets.empty();
    std::string edges = "R" + root_fold.exact_offsets;

    bool missing    = false;
    bool sensitive  = false;
    bool unverified = false;
    bool inherited_fold = false;
    if (root == native.size()) {
        const auto mode = directory_case(native);
        unverified = mode == Directory_case::MISSING || mode == Directory_case::UNAVAILABLE;
        if (unverified && use == Path_use::VERIFY_HISTORICAL_KEY) return std::nullopt;
    }
    for (auto begin = root; begin < native.size();) {
        if (!missing) {
            // Keep the separator on a drive/share root; opening the drive
            // designator alone would consult that drive's working directory.
            const auto mode = directory_case(native.substr(0, begin == root ? root : begin - 1));
            unverified = mode == Directory_case::UNAVAILABLE || (mode == Directory_case::MISSING && begin == root);
            missing = unverified || mode == Directory_case::MISSING;
            if (missing && use == Path_use::VERIFY_HISTORICAL_KEY) return std::nullopt;
            if (!missing) sensitive = mode == Directory_case::SENSITIVE;
        }
        const auto end       = native.find(L'\\', begin);
        const auto component = std::wstring_view(native).substr(begin, end == native.npos ? end : end - begin);
        edges.push_back('\0');
        if (sensitive || unverified || (missing && use == Path_use::COMPARE_GRANT)) {
            identity += component;
            native_distinction = true;
            edges += unverified ? 'U' : 'E';
        }
        else {
            Folded_component folded;
            if (!append_folded(identity, component, fold, folded)) return std::nullopt;
            native_distinction |= !folded.exact_offsets.empty();
            if (missing && folded.cased) {
                // A separated key cannot inherit authority for the alternate
                // spelling of a child whose actual lookup rules disappeared.
                inherited_fold = true;
                edges += 'M';
                edges += path_to_utf8(fs::path(component));
            }
            else {
                edges += 'I';
                edges += folded.exact_offsets;
            }
        }
        if (end == native.npos) break;
        identity += L'\\';
        begin = end + 1;
    }
    auto bytes = path_to_utf8(fs::path(identity));
    // Even an all-lowercase child must not claim a key previously shared
    // with its case-distinct sibling. NUL cannot occur in a native path.
    if (unverified || native_distinction) {
        bytes.insert(0, unverified ? 2 : 1, '\0');
        if (use == Path_use::DERIVE_KEY) {
            // A global marker alone lets an exact edge transfer its authority
            // to a different insensitive edge when a directory changes mode.
            // NUL separates component evidence from all valid path characters.
            bytes.push_back('\0');
            bytes += edges;
        }
    }
    if (out_key_only) *out_key_only = !inherited_fold || unverified || native_distinction;
    return bytes;
}

} // namespace
#endif

std::optional<std::string> native_account_profile_identity(const fs::path& path, Account_profile_case_fold fold)
{
    if (!path.is_absolute()) return std::nullopt;
#ifdef _WIN32
    return windows_identity(path, fold, Path_use::DERIVE_KEY);
#else
    (void)fold;
    return path_to_utf8(path);
#endif
}

bool same_native_account_profile(const fs::path& granted, const fs::path& selected)
{
    if (granted.native() == selected.native()) return true;
#ifdef _WIN32
    if (!granted.is_absolute() || !selected.is_absolute()) return false;
    const auto a = fs::path(granted).make_preferred().native();
    const auto b = fs::path(selected).make_preferred().native();
    if (CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) != CSTR_EQUAL) return false;
    if (a == b) return true;
    const auto first  = windows_identity(a, Account_profile_case_fold::WINDOWS_INVARIANT, Path_use::COMPARE_GRANT);
    const auto second = windows_identity(b, Account_profile_case_fold::WINDOWS_INVARIANT, Path_use::COMPARE_GRANT);
    if (!first || first != second) return false;

    std::error_code error;
    const fs::path left(native_io_path(a)), right(native_io_path(b));
    if (fs::equivalent(left, right, error)) return true;
    return fs::symlink_status(left, error).type() == fs::file_type::not_found &&
           fs::symlink_status(right, error).type() == fs::file_type::not_found;
#else
    return false;
#endif
}

bool account_profile_allows_key_only(const fs::path& selected, Account_profile_case_fold fold)
{
    if (!selected.is_absolute()) return false;
#ifdef _WIN32
    bool eligible = false;
    return windows_identity(selected, fold, Path_use::DERIVE_KEY, &eligible) && eligible;
#else
    (void)fold;
    return true;
#endif
}

std::optional<std::string> historical_account_profile_identity(
    const fs::path& granted, const fs::path& selected, Account_profile_case_fold fold)
{
    if (!granted.is_absolute() || !same_native_account_profile(granted, selected)) return std::nullopt;
#ifdef _WIN32
    if (!windows_identity(granted, fold, Path_use::VERIFY_HISTORICAL_KEY)) return std::nullopt;
    auto native = granted.native();
    if (fold == Account_profile_case_fold::ASCII) {
        for (auto& c : native) c = ascii_lower(c);
    }
    else {
        auto lower = mapped_case(native, LCMAP_LOWERCASE);
        if (!lower) return std::nullopt;
        native = std::move(*lower);
    }
    return path_to_utf8(fs::path(native));
#else
    (void)fold;
    return path_to_utf8(granted);
#endif
}

} // namespace logonomic::cli_addon
