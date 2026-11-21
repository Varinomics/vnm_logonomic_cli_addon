#include "logonomic_cli_addon_generation_custody_platform.h"

#include "logonomic_cli_addon/package_source.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <utility>

namespace logonomic::cli_addon_host::custody_platform {

namespace {

namespace sdk = logonomic::cli_addon;

std::string errno_text(int error)
{
    return std::strerror(error);
}

std::string describe(const std::filesystem::path& path)
{
    return sdk::path_to_utf8(path);
}

bool report(std::string* out_error, std::string message)
{
    *out_error = std::move(message);
    return false;
}

int fd_of(const Native_handle& handle)
{
    return (int)handle.value();
}

// Opens `parent`'s child relative to the parent descriptor. O_NOFOLLOW
// refuses a link in the final component; the parent descriptor pins every
// component above it.
bool open_child(
    const Native_handle& parent,
    const std::string&   name,
    int                  flags,
    Native_handle&       out_handle,
    std::string*         out_error)
{
    const std::filesystem::path path = child_path(parent, name);
    const int fd = ::openat(fd_of(parent), name.c_str(), flags | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        return report(out_error, "cannot open " + describe(path) + ": " + errno_text(errno));
    }
    out_handle = Native_handle(fd, path);
    return true;
}

} // namespace

void Native_handle::close()
{
    if (m_value != k_invalid_native_handle) {
        ::close((int)m_value);
        m_value = k_invalid_native_handle;
    }
}

bool open_directory(
    const std::filesystem::path& path,
    bool                         no_follow,
    Native_handle&               out_directory,
    std::string*                 out_error)
{
    const int flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | (no_follow ? O_NOFOLLOW : 0);
    const int fd    = ::open(path.c_str(), flags);
    if (fd < 0) {
        return report(out_error, "cannot open directory " + describe(path) + ": " + errno_text(errno));
    }
    out_directory = Native_handle(fd, path);
    return true;
}

bool reopen_directory(
    const Native_handle& directory,
    Native_handle&       out_directory,
    std::string*         out_error)
{
    const int fd = ::openat(fd_of(directory), ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        return report(out_error, "cannot reopen directory " + describe(directory.path()) + ": " + errno_text(errno));
    }
    out_directory = Native_handle(fd, directory.path());
    return true;
}

bool open_child_directory(
    const Native_handle& parent,
    const std::string&   name,
    Native_handle&       out_directory,
    std::string*         out_error)
{
    return open_child(parent, name, O_RDONLY | O_DIRECTORY, out_directory, out_error);
}

bool create_child_directory(
    const Native_handle& parent,
    const std::string&   name,
    std::string*         out_error)
{
    if (::mkdirat(fd_of(parent), name.c_str(), 0700) != 0 && errno != EEXIST) {
        return report(out_error,
            "cannot create directory " + describe(child_path(parent, name)) + ": " + errno_text(errno));
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
    // O_NONBLOCK keeps the open from blocking on a fifo that took the place
    // of a file; the kind check below then refuses it.
    Native_handle file;
    if (!open_child(parent, name, O_RDONLY | O_NONBLOCK, file, out_error)) {
        return false;
    }
    struct stat status {};
    if (::fstat(fd_of(file), &status) != 0) {
        return report(out_error, "cannot stat " + describe(file.path()) + ": " + errno_text(errno));
    }
    if (!S_ISREG(status.st_mode)) {
        return report(out_error, describe(file.path()) + " is not a regular file");
    }
    out_size = (std::uint64_t)status.st_size;
    out_file = std::move(file);
    return true;
}

bool create_child_file(
    const Native_handle& parent,
    const std::string&   name,
    Native_handle&       out_file,
    std::string*         out_error)
{
    const std::filesystem::path path = child_path(parent, name);
    const int fd = ::openat(fd_of(parent), name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        return report(out_error, "cannot create " + describe(path) + ": " + errno_text(errno));
    }
    out_file = Native_handle(fd, path);
    return true;
}

bool read_some(
    const Native_handle& file,
    char*                buffer,
    std::size_t          size,
    std::size_t&         out_count,
    std::string*         out_error)
{
    while (true) {
        const ssize_t count = ::read(fd_of(file), buffer, size);
        if (count >= 0) {
            out_count = (std::size_t)count;
            return true;
        }
        if (errno != EINTR) {
            return report(out_error, "cannot read " + describe(file.path()) + ": " + errno_text(errno));
        }
    }
}

bool write_all(
    const Native_handle& file,
    const char*          data,
    std::size_t          size,
    std::string*         out_error)
{
    while (size > 0) {
        const ssize_t written = ::write(fd_of(file), data, size);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return report(out_error, "cannot write " + describe(file.path()) + ": " + errno_text(errno));
        }
        data += written;
        size -= (std::size_t)written;
    }
    return true;
}

bool flush_file(const Native_handle& file, std::string* out_error)
{
    if (::fsync(fd_of(file)) != 0) {
        return report(out_error, "cannot flush " + describe(file.path()) + ": " + errno_text(errno));
    }
    return true;
}

bool publish_file(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    std::string*                 out_error)
{
    if (::rename(source.c_str(), destination.c_str()) != 0) {
        return report(out_error,
            "cannot publish " + describe(source) + " at " + describe(destination) + ": " + errno_text(errno));
    }
    return true;
}

bool set_child_mode(
    const Native_handle& parent,
    const std::string&   name,
    Entry_mode           mode,
    std::string*         out_error)
{
    // fchmodat follows a link in the final component, so the entry is opened
    // with O_NOFOLLOW and the mode applied through its descriptor.
    const bool   directory = mode == Entry_mode::READ_ONLY_DIRECTORY;
    const mode_t bits      = mode == Entry_mode::READ_ONLY_FILE ? 0400 : 0500;
    Native_handle entry;
    if (!open_child(parent, name, O_RDONLY | (directory ? O_DIRECTORY : 0), entry, out_error)) {
        return false;
    }
    if (::fchmod(fd_of(entry), bits) != 0) {
        return report(out_error, "cannot make " + describe(entry.path()) + " read-only: " + errno_text(errno));
    }
    return true;
}

bool publish_directory(
    Native_handle&               directory,
    const std::filesystem::path& destination,
    std::string*                 out_error)
{
    if (::rename(directory.path().c_str(), destination.c_str()) != 0) {
        return report(out_error,
            "cannot rename " + describe(directory.path()) + " to " + describe(destination) + ": " + errno_text(errno));
    }
    const bool read_only = ::fchmod(fd_of(directory), 0500) == 0;
    const int  error     = errno;
    directory.close();
    if (!read_only) {
        return report(out_error, "cannot make " + describe(destination) + " read-only: " + errno_text(error));
    }
    return true;
}

void flush_directory(const std::filesystem::path& path)
{
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
}

Lock_outcome acquire_lock(
    const std::filesystem::path& path,
    Lock_mode                    mode,
    Native_handle_value&         out_handle,
    std::string*                 out_error,
    bool                         create_if_missing,
    Lock_wait                    wait)
{
    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC | (create_if_missing ? O_CREAT : O_NOFOLLOW), 0600);
    if (fd < 0) {
        report(out_error, "cannot open lock file " + describe(path) + ": " + errno_text(errno));
        return Lock_outcome::FAILED;
    }
    const int operation = (mode == Lock_mode::EXCLUSIVE ? LOCK_EX : LOCK_SH) |
        (wait == Lock_wait::NON_BLOCKING ? LOCK_NB : 0);
    int locked;
    do {
        locked = ::flock(fd, operation);
    }
    while (locked != 0 && errno == EINTR);
    if (locked != 0) {
        const int error = errno;
        ::close(fd);
        if (error == EWOULDBLOCK) {
            return Lock_outcome::BUSY;
        }
        report(out_error, "cannot lock " + describe(path) + ": " + errno_text(error));
        return Lock_outcome::FAILED;
    }
    // Recovery can unlink the file after open() but before flock(). A lock on
    // that retired inode cannot protect the generation at this pathname.
    struct stat opened_status {};
    struct stat named_status  {};
    if (::fstat(fd, &opened_status) != 0 || ::stat(path.c_str(), &named_status) != 0) {
        const int error = errno;
        ::close(fd);
        if (error == ENOENT) {
            return Lock_outcome::BUSY;
        }
        report(out_error, "cannot inspect lock file " + describe(path) + ": " + errno_text(error));
        return Lock_outcome::FAILED;
    }
    if (opened_status.st_nlink == 0 ||
        opened_status.st_dev != named_status.st_dev ||
        opened_status.st_ino != named_status.st_ino)
    {
        ::close(fd);
        return Lock_outcome::BUSY;
    }
    out_handle = fd;
    return Lock_outcome::ACQUIRED;
}

void release_lock(Native_handle_value handle)
{
    ::close((int)handle);
}

} // namespace logonomic::cli_addon_host::custody_platform
