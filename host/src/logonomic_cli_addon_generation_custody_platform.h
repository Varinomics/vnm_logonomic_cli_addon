#pragma once

#include "logonomic_cli_addon/package_source.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>

/**
 * @brief Platform half of immutable generation custody: every operation that
 * needs a descriptor or handle. The Qt half
 * (logonomic_cli_addon_generation_custody.cpp) decides the order of the
 * phases and the outcome; this layer only touches files. It is Qt-free so
 * that the Windows unit type-checks under a cross-compiler without Windows
 * Qt, and it takes UTF-8 names and std::filesystem paths, which are native
 * on both platforms.
 *
 * Children are always reached through an open parent - openat with O_NOFOLLOW
 * on POSIX; on Windows a path composed from the parent's path, opened with
 * FILE_FLAG_OPEN_REPARSE_POINT and checked for the reparse attribute - so a
 * component swapped for a link after the validator saw it is refused instead
 * of followed. Every function reports failure as false with a UTF-8 message
 * in *out_error.
 */
namespace logonomic::cli_addon_host::custody_platform {

/// A POSIX descriptor or a Windows HANDLE.
using Native_handle_value = std::intptr_t;

inline constexpr Native_handle_value k_invalid_native_handle = -1;

/// The modes a published generation is left in. Directory read-only is
/// enforced on POSIX and advisory on Windows.
enum class Entry_mode
{
    READ_ONLY_FILE,         // POSIX 0400
    READ_ONLY_EXECUTABLE,   // POSIX 0500: the adapter and helpers
    READ_ONLY_DIRECTORY,    // POSIX 0500
};

/**
 * @brief An open directory or file plus the path it was opened at. The path
 * is for diagnostics and, on Windows, for composing child paths; every
 * operation goes through the handle.
 */
class Native_handle
{
public:
    Native_handle() = default;

    Native_handle(Native_handle_value value, std::filesystem::path path)
    :
        m_value(value),
        m_path(std::move(path))
    {}

    ~Native_handle() { close(); }

    Native_handle(const Native_handle&)            = delete;
    Native_handle& operator=(const Native_handle&) = delete;

    Native_handle(Native_handle&& other) noexcept
    :
        m_value(other.m_value),
        m_path(std::move(other.m_path))
    {
        other.m_value = k_invalid_native_handle;
    }

    Native_handle& operator=(Native_handle&& other) noexcept
    {
        if (this != &other) {
            close();
            m_value       = other.m_value;
            m_path        = std::move(other.m_path);
            other.m_value = k_invalid_native_handle;
        }
        return *this;
    }

    Native_handle_value          value() const { return m_value; }
    const std::filesystem::path& path()  const { return m_path; }
    bool                         valid() const { return m_value != k_invalid_native_handle; }

    /// Closes the descriptor or handle; the object is invalid afterwards.
    void close();

private:
    Native_handle_value   m_value = k_invalid_native_handle;
    std::filesystem::path m_path;
};

/// The path of `parent`'s child named by UTF-8 `name`.
inline std::filesystem::path child_path(const Native_handle& parent, const std::string& name)
{
    return parent.path() / logonomic::cli_addon::path_from_utf8(name);
}

/// Opens a directory by path; with no_follow a link at the final component
/// is refused.
bool open_directory(
    const std::filesystem::path& path,
    bool                         no_follow,
    Native_handle&               out_directory,
    std::string*                 out_error);

/// A second handle on an already open directory.
bool reopen_directory(
    const Native_handle& directory,
    Native_handle&       out_directory,
    std::string*         out_error);

bool open_child_directory(
    const Native_handle& parent,
    const std::string&   name,
    Native_handle&       out_directory,
    std::string*         out_error);

/// Creates the child directory; an existing directory is not an error.
bool create_child_directory(
    const Native_handle& parent,
    const std::string&   name,
    std::string*         out_error);

/// Opens a regular file for reading; a link, directory or other entry kind
/// is refused. out_size is the file's current size.
bool open_child_file_for_read(
    const Native_handle& parent,
    const std::string&   name,
    Native_handle&       out_file,
    std::uint64_t&       out_size,
    std::string*         out_error);

/// Creates a new file for writing; an existing entry of that name is an
/// error.
bool create_child_file(
    const Native_handle& parent,
    const std::string&   name,
    Native_handle&       out_file,
    std::string*         out_error);

/// Reads up to `size` bytes; out_count is 0 at the end of the file.
bool read_some(
    const Native_handle& file,
    char*                buffer,
    std::size_t          size,
    std::size_t&         out_count,
    std::string*         out_error);

bool write_all(
    const Native_handle& file,
    const char*          data,
    std::size_t          size,
    std::string*         out_error);

/// Forces the file's bytes to stable storage.
bool flush_file(const Native_handle& file, std::string* out_error);

/// Publishes a closed, flushed file at its final name in the same directory.
bool publish_file(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    std::string*                 out_error);

/// Applies the mode to `parent`'s child without following a link.
bool set_child_mode(
    const Native_handle& parent,
    const std::string&   name,
    Entry_mode           mode,
    std::string*         out_error);

/// Renames the open directory to `destination`, applies the POSIX read-only
/// directory mode after the move, and closes the handle. Windows closes its
/// open handle before the rename and leaves the directory ACL inherited from
/// the host-owned custody root; indexed files already carry their read-only
/// attribute. When rename fails the directory remains at its old path; on
/// POSIX a mode failure leaves it at `destination`.
bool publish_directory(
    Native_handle&               directory,
    const std::filesystem::path& destination,
    std::string*                 out_error);

/// Best effort: forces a directory's entries to stable storage. A failure
/// only decides what a power loss leaves for the next start's recovery, so
/// there is no outcome to report.
void flush_directory(const std::filesystem::path& path);

enum class Lock_outcome
{
    ACQUIRED,
    BUSY,     // another owner holds the lock, or recovery changed its pathname
    FAILED,   // the file could not be opened or locked; *out_error says why
};

enum class Lock_mode
{
    SHARED,
    EXCLUSIVE,
};

enum class Lock_wait
{
    NON_BLOCKING,
    BLOCKING,
};

/// Takes an advisory lock (flock on POSIX, LockFileEx on Windows).
/// Closed registries pass create_if_missing=false so stale admission cannot
/// recreate a lock that its authoritative closer already removed.
Lock_outcome acquire_lock(
    const std::filesystem::path& path,
    Lock_mode                    mode,
    Native_handle_value&         out_handle,
    std::string*                 out_error,
    bool                         create_if_missing = true,
    Lock_wait                    wait = Lock_wait::NON_BLOCKING);

/// Releases a lock returned by acquire_lock and closes its handle.
void release_lock(Native_handle_value handle);

} // namespace logonomic::cli_addon_host::custody_platform
