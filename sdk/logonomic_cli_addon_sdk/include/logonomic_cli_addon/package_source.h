#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace logonomic::cli_addon {

enum class Source_entry_kind
{
    REGULAR_FILE,
    DIRECTORY,
    LINK,    // symbolic link or reparse point, never followed
    OTHER,   // device, fifo, socket, or an entry whose name is not valid UTF-8
};

struct Source_entry
{
    std::string       relative_path;   // '/'-separated, relative to the package directory
    Source_entry_kind kind  = Source_entry_kind::OTHER;
    std::uint64_t     bytes = 0;       // regular files only
};

enum class Source_error_kind
{
    MISSING,
    LINK,
    NOT_REGULAR,
    TOO_LARGE,
    BOUND_EXCEEDED,
    INVALID_UTF8,
    IO_ERROR,
};

struct Source_error
{
    Source_error_kind kind = Source_error_kind::IO_ERROR;
    std::string       detail;
};

/**
 * @brief Read-only byte source over one package directory. The validator
 * and the host's custody copy read a package only through this interface,
 * so link rejection and bounded reads are decided in one place.
 */
class Package_source
{
public:
    virtual ~Package_source() = default;

    /// Every entry below the package directory, recursively, sorted bytewise
    /// by relative path. Directories that are links are reported as LINK and
    /// not descended.
    virtual std::optional<std::vector<Source_entry>> list(Source_error* out_error) const = 0;

    /// Reads one regular file without following links in any path component.
    /// A file larger than max_bytes is TOO_LARGE, not truncated.
    virtual std::optional<std::string> read(
        std::string_view relative_path,
        std::uint64_t    max_bytes,
        Source_error*    out_error) const = 0;

    /// Human-readable location for diagnostics.
    virtual std::string describe() const = 0;
};

/**
 * @brief Package_source over a directory on the local file system. Every
 * component below the root is opened with O_NOFOLLOW (POSIX) or rejected
 * when it carries a reparse-point attribute (Windows). The root path itself
 * is the caller's choice and is not subject to the link rule.
 * Cancellation is checked between directory entries and read chunks and
 * reports IO_ERROR with a cancellation diagnostic. It does not interrupt a
 * filesystem call already in progress.
 */
class Directory_package_source final : public Package_source
{
public:
    explicit Directory_package_source(
        std::filesystem::path root,
        std::stop_token cancellation = {});

    std::optional<std::vector<Source_entry>> list(Source_error* out_error) const override;
    std::optional<std::string> read(
        std::string_view relative_path,
        std::uint64_t    max_bytes,
        Source_error*    out_error) const override;
    std::string describe() const override;

    const std::filesystem::path& root() const { return m_root; }

private:
    bool cancellation_requested(Source_error* out_error) const;

    std::filesystem::path m_root;
    std::stop_token m_cancellation;
};

/// UTF-8 bytes of a path in native form (the form the protocol carries).
std::string path_to_utf8(const std::filesystem::path& path);

/// A path from UTF-8 bytes, on every platform.
std::filesystem::path path_from_utf8(std::string_view utf8);

/// Exact signed native file-clock ticks in decimal, without epoch or precision conversion.
std::string file_time_ticks_text(std::filesystem::file_time_type modified);

} // namespace logonomic::cli_addon
