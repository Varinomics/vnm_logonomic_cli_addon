#include "logonomic_cli_addon/launcher_utilities.h"
#include "logonomic_cli_addon/launcher_discovery.h"
#include "logonomic_cli_addon/package_source.h"
#include "launcher_utilities_p.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <regex>
#include <sstream>
#include <system_error>
#include <utility>

namespace logonomic::cli_addon {
namespace fs = std::filesystem;

#ifndef _WIN32
bool open_cloexec_pipe(int (&descriptors)[2])
{
    descriptors[0] = -1;
    descriptors[1] = -1;
#if defined(__APPLE__)
    if (::pipe(descriptors) != 0) {
        return false;
    }
    if (::fcntl(descriptors[0], F_SETFD, FD_CLOEXEC) < 0 ||
        ::fcntl(descriptors[1], F_SETFD, FD_CLOEXEC) < 0)
    {
        const int error = errno;
        ::close(descriptors[0]);
        ::close(descriptors[1]);
        descriptors[0] = -1;
        descriptors[1] = -1;
        errno = error;
        return false;
    }
    return true;
#else
    return ::pipe2(descriptors, O_CLOEXEC) == 0;
#endif
}
#endif

namespace {

std::uint32_t read_scalar(std::string_view value, std::size_t& offset)
{
    const auto lead = static_cast<unsigned char>(value[offset++]);
    std::uint32_t scalar = lead;
    const auto continuation = lead >= 0xF0 ? 3u : lead >= 0xE0 ? 2u : lead >= 0xC0 ? 1u : 0u;
    if (continuation != 0 && offset + continuation <= value.size()) {
        scalar = lead & ((1u << (6u - continuation)) - 1u);
        for (unsigned index = 0; index < continuation; ++index) {
            scalar = (scalar << 6) | (static_cast<unsigned char>(value[offset++]) & 0x3F);
        }
    }
    return scalar;
}

bool unicode_whitespace(std::uint32_t scalar)
{
    return scalar == 0x20 || (scalar >= 9 && scalar <= 13) || scalar == 0x85 ||
        scalar == 0xA0 || scalar == 0x1680 || (scalar >= 0x2000 && scalar <= 0x200A) ||
        scalar == 0x2028 || scalar == 0x2029 || scalar == 0x202F || scalar == 0x205F || scalar == 0x3000;
}

struct Direct_arguments
{
    std::vector<std::string> words;
    bool valid = true;
};

Direct_arguments split_direct_arguments(std::string_view script, bool strict_host)
{
    Direct_arguments result;
    std::string current;
    bool quoted = false;
    int quotes = 0;
    for (std::size_t offset = 0; offset < script.size();) {
        const auto begin = offset;
        const char character = script[offset];
        if (character == '"') {
            ++offset;
            if (++quotes == 3) {
                current += '"';
                quotes = 0;
            }
            continue;
        }
        if (quotes == 1) {
            quoted = !quoted;
        }
        if (strict_host && quotes == 2) {
            result.valid = false;
        }
        quotes = 0;
        bool whitespace;
        if (strict_host) {
            whitespace = unicode_whitespace(read_scalar(script, offset));
        }
        else {
            ++offset;
            whitespace = std::isspace(static_cast<unsigned char>(character)) != 0;
        }
        if (!quoted && whitespace) {
            if (!current.empty()) {
                result.words.push_back(std::move(current));
                current.clear();
            }
        }
        else {
            current.append(script.substr(begin, offset - begin));
        }
    }
    if (strict_host) {
        if (quotes == 1) {
            quoted = !quoted;
        }
        result.valid = result.valid && quotes != 2 && !quoted;
    }
    if (!current.empty()) {
        result.words.push_back(std::move(current));
    }
    return result;
}

} // namespace

std::string trim(std::string_view value, Launcher_whitespace whitespace)
{
    if (whitespace == Launcher_whitespace::ASCII) {
        const auto first = value.find_first_not_of(" \t\r\n");
        const auto last  = value.find_last_not_of(" \t\r\n");
        return first == std::string_view::npos
            ? std::string{} : std::string(value.substr(first, last - first + 1));
    }
    std::size_t first = value.size(), last = 0;
    for (std::size_t offset = 0; offset < value.size();) {
        const auto begin = offset;
        const bool whitespace = unicode_whitespace(read_scalar(value, offset));
        if (!whitespace) { first = std::min(first, begin); last = offset; }
    }
    return last == 0 ? std::string{} : std::string(value.substr(first, last - first));
}

std::string lower(std::string value)
{
    for (char& character : value) {
        if (character >= 'A' && character <= 'Z') character += 'a' - 'A';
    }
    return value;
}

std::string unquote(std::string_view value, Launcher_whitespace whitespace)
{
    std::string result = trim(value, whitespace);
    if (result.size() >= 2 &&
        ((result.front() == '\'' && result.back() == '\'') ||
         (result.front() == '"'  && result.back() == '"')))
    {
        return result.substr(1, result.size() - 2);
    }
    return result;
}

std::string base_name(std::string_view value, Launcher_whitespace whitespace)
{
    std::string name = lower(unquote(value, whitespace));
    const auto separator = name.find_last_of("/\\");
    if (separator != std::string::npos) name.erase(0, separator + 1);
    for (const std::string_view suffix : {".exe", ".cmd", ".ps1", ".bat"}) {
        if (name.ends_with(suffix)) {
            name.resize(name.size() - suffix.size());
            break;
        }
    }
    return name;
}

namespace {
const std::string& semver_pattern()
{
    static const std::string pattern =
        R"((?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*))"
        R"((?:-(?:0|[1-9][0-9]*|[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*)(?:\.(?:0|[1-9][0-9]*|[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*))*)?)"
        R"((?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?)";
    return pattern;
}

bool environment_name(std::string_view name)
{
    if (name.empty() || (name.front() != '_' &&
        !std::isalpha(static_cast<unsigned char>(name.front())))) return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char character) {
        return character == '_' || (character >= 'A' && character <= 'Z') ||
            (character >= 'a' && character <= 'z') ||
            (character >= '0' && character <= '9');
    });
}

bool equal_name(std::string_view left, std::string_view right)
{
#ifdef _WIN32
    return lower(std::string(left)) == lower(std::string(right));
#else
    return left == right;
#endif
}

} // namespace

std::optional<std::pair<std::string, std::string>> environment_assignment(
    std::string_view text, Launcher_whitespace whitespace)
{
    const std::string value = unquote(text, whitespace);
    const auto separator = value.find('=');
    if (separator == std::string::npos) return std::nullopt;
    std::string name = trim(std::string_view(value).substr(0, separator), whitespace);
    if (!environment_name(name) || value.find('\0') != std::string::npos) return std::nullopt;
    return std::pair{std::move(name), unquote(std::string_view(value).substr(separator + 1), whitespace)};
}

std::vector<std::string> launcher_tokens(std::string_view text, std::string_view runner)
{
    static const std::regex bash_pattern(R"re("(?:\\.|[^"])*"|'[^']*'|\S+)re");
    static const std::regex powershell_pattern(R"re("(?:`.|[^"])*"|'(?:''|[^'])*'|\S+)re");
    static const std::regex cmd_pattern(R"re("[^"]*"|\S+)re");
    const auto& pattern = runner == "powershell" ? powershell_pattern
        : runner == "cmd" || runner == "direct" ? cmd_pattern : bash_pattern;
    std::vector<std::string> result;
    if (text.empty()) {
        return result;
    }
    for (std::cregex_iterator match(text.data(), text.data() + text.size(), pattern), end;
         match != end; ++match)
    {
        result.push_back(match->str());
    }
    return result;
}

std::vector<std::string> launcher_segments(
    std::string_view line, std::string_view runner, Launcher_whitespace whitespace)
{
    std::vector<std::string> result;
    std::string current;
    char quote = 0;
    for (std::size_t index = 0; index < line.size(); ++index) {
        const char character = line[index];
        const bool escape = runner == "cmd" ? character == '^' && !quote
            : runner == "powershell" ? character == '`' && quote != '\''
            : runner == "bash" && character == '\\' && quote != '\'';
        if (escape && index + 1 < line.size()) {
            current += character;
            current += line[++index];
            continue;
        }
        if (character == quote) quote = 0;
        else if (!quote && ((runner != "cmd" && runner != "direct" && character == '\'') ||
                            character == '"')) quote = character;
        else if (!quote && ((runner != "cmd" && character == ';') || character == '&' || character == '|')) {
            std::string segment = trim(current, whitespace);
            if (!segment.empty()) {
                result.push_back(std::move(segment));
            }
            current.clear();
            continue;
        }
        current += character;
    }
    std::string segment = trim(current, whitespace);
    if (!segment.empty()) {
        result.push_back(std::move(segment));
    }
    return result;
}

std::vector<std::string> literal_argv(std::string_view script)
{
    return split_direct_arguments(script, false).words;
}

std::optional<std::vector<std::string>> launcher_detail::direct_argv(std::string_view script)
{
    if (!is_valid_utf8(script)) {
        return std::nullopt;
    }
    auto result = split_direct_arguments(script, true);
    if (!result.valid || result.words.empty() || trim(result.words.front()).empty()) {
        return std::nullopt;
    }
    return std::move(result.words);
}

bool is_semantic_version(std::string_view value)
{
    static const std::regex pattern("^" + semver_pattern() + "$", std::regex::ECMAScript);
    return std::regex_match(value.begin(), value.end(), pattern);
}

std::optional<std::string> parse_version_stream(
    std::string_view value, std::span<const std::string_view> labels,
    Launcher_whitespace whitespace)
{
    if (value.size() > 65536 || value.find('\0') != std::string_view::npos || !is_valid_utf8(value)) {
        return std::nullopt;
    }
    static const std::regex labelled_suffix("^(?:\\s+version)?(?:\\s*[:=]\\s*|\\s+)v?(" +
        semver_pattern() + ")(?:\\s+\\([^\\r\\n()]{1,128}\\))?\\s*$", std::regex::icase);
    std::vector<std::string> lines;
    std::string text(value);
    std::replace(text.begin(), text.end(), '\r', '\n');
    std::istringstream input(text);
    std::string version;
    for (std::string line; std::getline(input, line);) {
        line = trim(line, whitespace);
        if (line.empty()) {
            continue;
        }
        lines.push_back(line);
        const std::string folded = lower(line);
        for (const auto label : labels) {
            if (!folded.starts_with(lower(std::string(label)))) {
                continue;
            }
            std::smatch match;
            if (std::regex_match(line.cbegin() + label.size(), line.cend(), match, labelled_suffix)) {
                if (!version.empty() && version != match[1].str()) {
                    return std::nullopt;
                }
                version = match[1].str();
                break;
            }
        }
    }
    if (!version.empty()) {
        return version;
    }
    if (lines.size() == 1) {
        auto candidate = lines.front();
        if (candidate.starts_with('v') || candidate.starts_with('V')) {
            candidate.erase(0, 1);
        }
        if (is_semantic_version(candidate)) {
            return candidate;
        }
    }
    return std::string{};
}

std::string environment_value(const Environment& environment, std::string_view name)
{
    for (const auto& [key, value] : environment) if (equal_name(key, name)) return value;
    return {};
}

void set_environment(Environment& environment, std::string name, std::string value)
{
    std::erase_if(environment, [&](const auto& entry) { return equal_name(entry.first, name); });
    environment.emplace_back(std::move(name), std::move(value));
}

std::optional<fs::path> normalized_resume_directory(std::string_view directory)
{
    if (!is_valid_utf8(directory) || directory.find('\0') != std::string_view::npos) return std::nullopt;
    const auto path = path_from_utf8(directory);
    if (!path.is_absolute()) return std::nullopt;
    auto normalized = path.lexically_normal();
    normalized.make_preferred();
    if (normalized != normalized.root_path() && normalized.filename().empty()) normalized = normalized.parent_path();
    return normalized;
}

std::optional<fs::path> regular_file(const fs::path& path)
{
    std::error_code error;
    const auto canonical = fs::canonical(path, error);
    return !error && fs::is_regular_file(canonical, error) && !error
        ? std::optional(canonical) : std::nullopt;
}

std::optional<fs::path> resolve_program(
    std::string_view program, const Environment& environment, std::string_view working_directory,
    [[maybe_unused]] Launcher_whitespace whitespace)
{
    const auto path = path_from_utf8(program);
    const bool has_working_directory = path_from_utf8(working_directory).is_absolute();
    const auto usable_program = [](const fs::path& candidate) -> std::optional<fs::path> {
        const auto file = regular_file(candidate);
#ifndef _WIN32
        if (file && access(file->c_str(), X_OK) != 0) return std::nullopt;
#endif
        return file;
    };
    if (path.is_absolute()) return usable_program(path);
    if (path.has_parent_path()) {
        return has_working_directory ? usable_program(path_from_utf8(working_directory) / path) : std::nullopt;
    }
    std::vector<fs::path> candidates;
    {
        const auto path_entry = std::find_if(environment.begin(), environment.end(), [](const auto& entry) {
            return equal_name(entry.first, "PATH");
        });
        if (path_entry == environment.end()) return std::nullopt;
        const auto& search_path = path_entry->second;
#ifdef _WIN32
        constexpr char separator = ';';
#else
        constexpr char separator = ':';
#endif
        std::istringstream parts(search_path);
        for (std::string part; std::getline(parts, part, separator);) {
#ifdef _WIN32
            if (part.empty()) continue;
            auto entry = trim(part, whitespace);
            if (entry.size() >= 2 &&
                ((entry.front() == '\'' && entry.back() == '\'') ||
                 (entry.front() == '"' && entry.back() == '"')))
            {
                entry = entry.substr(1, entry.size() - 2);
            }
#else
            const auto& entry = part;
#endif
            auto directory = path_from_utf8(entry);
            if (!directory.is_absolute()) {
                if (!has_working_directory) continue;
                directory = path_from_utf8(working_directory) / directory;
            }
            candidates.push_back(directory / path);
        }
#ifndef _WIN32
        // getline does not yield a final empty component, including PATH="".
        if (has_working_directory && (search_path.empty() || search_path.back() == separator)) {
            candidates.push_back(path_from_utf8(working_directory) / path);
        }
#endif
    }
    for (const auto& candidate : candidates) {
#ifdef _WIN32
        if (!candidate.has_extension()) {
            auto extensions = environment_value(environment, "PATHEXT");
            if (std::none_of(environment.begin(), environment.end(), [](const auto& entry) {
                    return equal_name(entry.first, "PATHEXT");
                })) extensions = ".COM;.EXE;.BAT;.CMD";
            std::istringstream parts(extensions);
            for (std::string extension; std::getline(parts, extension, ';');) {
                extension = trim(extension, whitespace);
                if (extension.empty()) continue;
                if (!extension.starts_with('.')) extension.insert(extension.begin(), '.');
                if (const auto file = usable_program(path_from_utf8(path_to_utf8(candidate) + extension))) return file;
            }
            continue;
        }
#endif
        if (const auto file = usable_program(candidate)) return file;
    }
    return std::nullopt;
}

bool native_image(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    std::array<unsigned char, 4> header{};
    file.read(reinterpret_cast<char*>(header.data()), header.size());
#ifdef _WIN32
    return file.gcount() >= 2 && header[0] == 'M' && header[1] == 'Z';
#else
    if (file.gcount() != 4) return false;
    const auto magic = (static_cast<std::uint32_t>(header[0]) << 24) |
        (static_cast<std::uint32_t>(header[1]) << 16) |
        (static_cast<std::uint32_t>(header[2]) << 8) | header[3];
    return magic == 0x7f454c46 || magic == 0xfeedface || magic == 0xfeedfacf ||
        magic == 0xcefaedfe || magic == 0xcffaedfe || magic == 0xcafebabe || magic == 0xbebafeca;
#endif
}

std::string preferred_discovery_runner(const fs::path& resolved_program, std::string_view fallback_runner)
{
    return native_image(resolved_program) ? k_runner_direct : std::string(fallback_runner);
}

#ifdef _WIN32
std::wstring wide(std::string_view text)
{
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0);
    if (size == 0) return {};
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), result.data(), size);
    return result;
}

std::wstring quote_argument(std::wstring_view argument)
{
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (wchar_t character : argument) {
        if (character == L'\\') { ++slashes; continue; }
        result.append(character == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        result += character;
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}

#endif

Environment environment_pairs(const Json_value& value)
{
    Environment result;
    for (const auto& entry : value.as_object()) result.emplace_back(entry.name, entry.value.as_string());
    return result;
}

} // namespace logonomic::cli_addon
