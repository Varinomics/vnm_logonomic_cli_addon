#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/limits.h"

#include <algorithm>
#include <array>
#include <random>

namespace logonomic::cli_addon {

namespace {

bool is_lower_alpha(unsigned char c) { return c >= 'a' && c <= 'z'; }
bool is_alpha(unsigned char c)       { return is_lower_alpha(c) || (c >= 'A' && c <= 'Z'); }
bool is_digit(unsigned char c)       { return c >= '0' && c <= '9'; }
bool is_lower_alnum(unsigned char c) { return is_lower_alpha(c) || is_digit(c); }
bool is_hex_lower(unsigned char c)   { return is_digit(c) || (c >= 'a' && c <= 'f'); }
bool is_control(unsigned char c)     { return c < 0x20 || c == 0x7F; }

bool all_of_bytes(std::string_view text, bool (*predicate)(unsigned char))
{
    return std::all_of(text.begin(), text.end(), [predicate](char c) {
        return predicate((unsigned char)c);
    });
}

// One reverse-domain segment: 1-63 bytes, letters/digits at both ends,
// interior letters, digits or hyphens.
bool is_valid_package_segment(std::string_view segment)
{
    if (segment.empty() || segment.size() > 63 || segment.find("--") != std::string_view::npos) {
        return false;
    }
    if (!is_lower_alnum((unsigned char)segment.front()) || !is_lower_alnum((unsigned char)segment.back())) {
        return false;
    }
    return all_of_bytes(segment, [](unsigned char c) { return is_lower_alnum(c) || c == '-'; });
}

bool is_valid_hex(std::string_view hex, std::size_t expected_size)
{
    return hex.size() == expected_size && all_of_bytes(hex, is_hex_lower);
}

constexpr std::array<const char*, 6> k_package_directories = {
    k_segment_adapter,
    k_segment_capabilities,
    k_segment_helpers,
    k_segment_assets,
    k_segment_notices,
    k_segment_provenance,
};

bool is_package_directory_segment(std::string_view segment)
{
    return std::any_of(k_package_directories.begin(), k_package_directories.end(), [segment](const char* name) {
        return segment == name;
    });
}

bool is_windows_device_segment(std::string_view segment)
{
    const std::string name = ascii_lowercase(segment.substr(0, segment.find('.')));
    if (name == "con" || name == "prn" || name == "aux" || name == "nul") {
        return true;
    }
    if (!(name.starts_with("com") || name.starts_with("lpt"))) {
        return false;
    }
    const std::string_view suffix = std::string_view(name).substr(3);
    // Win32 also treats the ISO-8859-1 superscript digits 1, 2 and 3 as
    // device-number aliases, including before an extension or directory slash.
    return (suffix.size() == 1 && suffix.front() >= '1' && suffix.front() <= '9') ||
        suffix == "\xC2\xB9" || suffix == "\xC2\xB2" || suffix == "\xC2\xB3";
}

} // namespace

bool is_valid_package_id(std::string_view id)
{
    if (id.size() < 3 || id.size() > limits::k_max_package_id_bytes) {
        return false;
    }

    std::size_t segment_count = 0;
    std::size_t start         = 0;
    while (start <= id.size()) {
        const std::size_t dot = id.find('.', start);
        const std::string_view segment = id.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start);
        if (!is_valid_package_segment(segment)) {
            return false;
        }
        ++segment_count;
        if (dot == std::string_view::npos) {
            break;
        }
        start = dot + 1;
    }
    return segment_count >= 2 && !is_windows_device_segment(id);
}

bool is_valid_capability_id(std::string_view id)
{
    if (id.empty() || id.size() > 128) {
        return false;
    }
    if (!is_lower_alnum((unsigned char)id.front()) || !is_lower_alnum((unsigned char)id.back())) {
        return false;
    }
    for (std::size_t i = 0; i < id.size(); ++i) {
        const unsigned char c = (unsigned char)id[i];
        if (!(is_lower_alnum(c) || c == '.' || c == '-')) {
            return false;
        }
        if (i > 0) {
            const unsigned char previous = (unsigned char)id[i - 1];
            const bool invalid_separator_pair =
                (c == '.' && (previous == '.' || previous == '-')) ||
                (c == '-' && previous == '.');
            if (invalid_separator_pair) {
                return false;
            }
        }
    }
    return true;
}

bool is_valid_operation_name(std::string_view name)
{
    if (name.empty() || name.size() > 64) {
        return false;
    }
    if (!is_lower_alpha((unsigned char)name.front())) {
        return false;
    }
    return all_of_bytes(name, [](unsigned char c) { return is_lower_alnum(c) || c == '_'; });
}

bool is_valid_reason_code(std::string_view code)
{
    return is_valid_operation_name(code);
}

bool is_valid_version(std::int64_t version)
{
    return version >= 0 && version <= limits::k_max_version;
}

bool is_valid_release_version(std::string_view release_version)
{
    if (release_version.empty() || release_version.size() > limits::k_max_release_version_bytes) {
        return false;
    }
    return all_of_bytes(release_version, [](unsigned char c) { return !is_control(c); });
}

bool is_valid_platform_id(std::string_view platform_id)
{
    const std::size_t dash = platform_id.find('-');
    if (dash == std::string_view::npos || dash == 0 || dash + 1 >= platform_id.size()) {
        return false;
    }
    const auto is_part_byte = [](unsigned char c) { return is_lower_alnum(c) || c == '_'; };
    return all_of_bytes(platform_id.substr(0, dash), is_part_byte) &&
           all_of_bytes(platform_id.substr(dash + 1), is_part_byte);
}

bool is_valid_request_id(std::string_view request_id)
{
    if (request_id.size() < 2 || request_id.size() > 64 || request_id.front() != 'r') {
        return false;
    }
    const std::string_view digits = request_id.substr(1);
    if (!all_of_bytes(digits, is_digit)) {
        return false;
    }
    return digits.size() == 1 || digits.front() != '0';
}

bool is_valid_environment_name(std::string_view name)
{
    if (name.empty() || name.size() > limits::k_max_environment_name_bytes) {
        return false;
    }
    const unsigned char first = (unsigned char)name.front();
    if (!(is_alpha(first) || first == '_')) {
        return false;
    }
    return all_of_bytes(name, [](unsigned char c) { return is_alpha(c) || is_digit(c) || c == '_'; });
}

bool is_reserved_environment_name(std::string_view name)
{
    constexpr std::string_view prefix = "LOGONOMIC_";
    if (name.size() < prefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        unsigned char character = static_cast<unsigned char>(name[i]);
        if (character >= 'a' && character <= 'z') {
            character = static_cast<unsigned char>(character - 'a' + 'A');
        }
        if (character != static_cast<unsigned char>(prefix[i])) {
            return false;
        }
    }
    return true;
}

std::vector<std::string_view> split_path_segments(std::string_view path)
{
    std::vector<std::string_view> segments;
    std::size_t start = 0;
    while (true) {
        const std::size_t slash = path.find('/', start);
        if (slash == std::string_view::npos) {
            segments.push_back(path.substr(start));
            return segments;
        }
        segments.push_back(path.substr(start, slash - start));
        start = slash + 1;
    }
}

std::string_view path_first_segment(std::string_view path)
{
    return path.substr(0, path.find('/'));
}

bool is_valid_package_relative_path(std::string_view path)
{
    if (path.empty() || path.size() > limits::k_max_package_relative_path_bytes) {
        return false;
    }

    const auto segments = split_path_segments(path);
    if (segments.size() > limits::k_max_package_path_segments) {
        return false;
    }

    for (const std::string_view segment : segments) {
        if (segment.empty() || segment.size() > 128) {
            return false;
        }
        if (segment == "." || segment == "..") {
            return false;
        }
        if (is_windows_device_segment(segment)) {
            return false;
        }
        if (segment.back() == '.' || segment.back() == ' ') {
            return false;
        }
        for (const char c : segment) {
            const unsigned char byte = (unsigned char)c;
            if (is_control(byte) || byte == '\\') {
                return false;
            }
            if (byte == '<' || byte == '>' || byte == ':' || byte == '"' ||
                byte == '|' || byte == '?' || byte == '*')
            {
                return false;
            }
        }
    }

    return is_package_directory_segment(segments.front());
}

std::string ascii_lowercase(std::string_view text)
{
    std::string lowered(text);
    for (char& c : lowered) {
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
    }
    return lowered;
}

bool is_valid_sha256_hex(std::string_view hex)
{
    return is_valid_hex(hex, 64);
}

bool is_valid_generation_nonce_hex(std::string_view hex)
{
    return is_valid_hex(hex, limits::k_max_generation_nonce_bytes);
}

bool is_valid_transition_nonce_hex(std::string_view hex)
{
    return is_valid_hex(hex, limits::k_transition_nonce_hex_chars);
}

std::string random_nonce_hex()
{
    constexpr char k_digits[] = "0123456789abcdef";
    std::random_device device;
    std::string hex;
    hex.reserve(limits::k_max_generation_nonce_bytes);
    while (hex.size() < limits::k_max_generation_nonce_bytes) {
        std::uint32_t word = device();
        for (int nibble = 0; nibble < 8 && hex.size() < limits::k_max_generation_nonce_bytes; ++nibble) {
            hex.push_back(k_digits[word & 0xFu]);
            word >>= 4;
        }
    }
    return hex;
}

bool is_valid_provider_id(std::string_view provider_id)
{
    if (provider_id.empty() || provider_id.size() > 64 || !is_lower_alpha((unsigned char)provider_id.front())) {
        return false;
    }
    return all_of_bytes(provider_id, [](unsigned char c) { return is_lower_alnum(c) || c == '-' || c == '_'; });
}

const char* host_platform_id()
{
#if defined(_WIN32)
    return k_platform_windows_x86_64;
#elif defined(__APPLE__) && defined(__aarch64__)
    return k_platform_macos_arm64;
#elif defined(__APPLE__)
    return "macos-x86_64";
#else
    return k_platform_linux_x86_64;
#endif
}

} // namespace logonomic::cli_addon
