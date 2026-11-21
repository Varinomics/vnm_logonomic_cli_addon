#include "logonomic_cli_addon/package_source.h"
#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/json.h"
#include "logonomic_cli_addon/limits.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <utility>

namespace logonomic::cli_addon {

namespace {

constexpr std::size_t k_max_entries_per_directory =
    limits::k_max_files_per_package + 2;   // bootstrap and index are outside the index
constexpr std::size_t k_max_package_entries =
    limits::k_max_files_per_package * limits::k_max_package_path_segments + 2;

class Descriptor
{
public:
    explicit Descriptor(int fd = -1) : m_fd(fd) {}
    ~Descriptor() { reset(); }

    Descriptor(const Descriptor&)            = delete;
    Descriptor& operator=(const Descriptor&) = delete;

    int  get() const { return m_fd; }
    bool valid() const { return m_fd >= 0; }
    int  release() { const int fd = m_fd; m_fd = -1; return fd; }

    void reset(int fd = -1)
    {
        if (m_fd >= 0) {
            ::close(m_fd);
        }
        m_fd = fd;
    }

private:
    int m_fd;
};

std::string errno_text(int error)
{
    return std::strerror(error);
}

Source_error_kind classify_open_error(int error)
{
    switch (error) {
        case ELOOP:   return Source_error_kind::LINK;
        case ENOENT:
        case ENOTDIR: return Source_error_kind::MISSING;
        default:      return Source_error_kind::IO_ERROR;
    }
}

bool set_error(Source_error* out_error, Source_error_kind kind, std::string detail)
{
    if (out_error != nullptr) {
        out_error->kind   = kind;
        out_error->detail = std::move(detail);
    }
    return false;
}

Source_entry_kind classify_mode(mode_t mode)
{
    if (S_ISLNK(mode)) { return Source_entry_kind::LINK; }
    if (S_ISDIR(mode)) { return Source_entry_kind::DIRECTORY; }
    if (S_ISREG(mode)) { return Source_entry_kind::REGULAR_FILE; }
    return Source_entry_kind::OTHER;
}

bool list_directory(
    int                        dir_fd,
    const std::string&         prefix,
    std::size_t                depth,
    std::size_t&               seen_entries,
    std::vector<Source_entry>& out_entries,
    Source_error*              out_error,
    std::stop_token            cancellation)
{
    // fdopendir takes ownership of the descriptor it is given, so open a
    // fresh one for the stream and keep dir_fd for openat/fstatat.
    Descriptor stream_fd(::openat(dir_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!stream_fd.valid()) {
        return set_error(out_error, Source_error_kind::IO_ERROR,
            "cannot reopen directory " + prefix + ": " + errno_text(errno));
    }
    DIR* stream = ::fdopendir(stream_fd.get());
    if (stream == nullptr) {
        return set_error(out_error, Source_error_kind::IO_ERROR,
            "cannot list directory " + prefix + ": " + errno_text(errno));
    }
    stream_fd.release();

    std::vector<std::string> names;
    while (true) {
        if (cancellation.stop_requested()) {
            ::closedir(stream);
            return set_error(out_error, Source_error_kind::IO_ERROR, "package listing was cancelled");
        }
        errno = 0;
        const dirent* entry = ::readdir(stream);
        if (entry == nullptr) {
            const int read_error = errno;
            ::closedir(stream);
            if (read_error != 0) {
                return set_error(out_error, Source_error_kind::IO_ERROR,
                    "cannot read directory " + prefix + ": " + errno_text(read_error));
            }
            break;
        }
        const std::string_view name(entry->d_name);
        if (name == "." || name == "..") {
            continue;
        }
        if (depth + 1 > limits::k_max_package_path_segments) {
            ::closedir(stream);
            return set_error(out_error, Source_error_kind::BOUND_EXCEEDED,
                "k_max_package_path_segments exceeded while listing " + prefix + std::string(name));
        }
        if (names.size() >= k_max_entries_per_directory ||
            seen_entries >= k_max_package_entries)
        {
            ::closedir(stream);
            return set_error(out_error, Source_error_kind::BOUND_EXCEEDED,
                "package entry bound derived from k_max_files_per_package exceeded while listing");
        }
        ++seen_entries;
        names.emplace_back(name);
    }

    for (const std::string& name : names) {
        if (cancellation.stop_requested()) {
            return set_error(out_error, Source_error_kind::IO_ERROR, "package listing was cancelled");
        }
        struct stat status {};
        if (::fstatat(dir_fd, name.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
            return set_error(out_error, Source_error_kind::IO_ERROR,
                "cannot stat " + prefix + name + ": " + errno_text(errno));
        }

        Source_entry source_entry;
        source_entry.relative_path = prefix + name;
        source_entry.kind          = is_valid_utf8(name) ? classify_mode(status.st_mode) : Source_entry_kind::OTHER;
        source_entry.bytes         = source_entry.kind == Source_entry_kind::REGULAR_FILE ? (std::uint64_t)status.st_size : 0;
        out_entries.push_back(source_entry);

        if (source_entry.kind == Source_entry_kind::DIRECTORY) {
            Descriptor child_fd(::openat(dir_fd, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            if (!child_fd.valid()) {
                return set_error(out_error, Source_error_kind::IO_ERROR,
                    "cannot open directory " + prefix + name + ": " + errno_text(errno));
            }
            if (!list_directory(
                    child_fd.get(),
                    prefix + name + "/",
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
    Descriptor root_fd(::open(m_root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!root_fd.valid()) {
        set_error(out_error, Source_error_kind::IO_ERROR,
            "cannot open package directory " + m_root.string() + ": " + errno_text(errno));
        return std::nullopt;
    }

    std::vector<Source_entry> entries;
    std::size_t seen_entries = 0;
    if (!list_directory(root_fd.get(), "", 0, seen_entries, entries, out_error, m_cancellation)) {
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
    Descriptor current(::open(m_root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!current.valid()) {
        set_error(out_error, Source_error_kind::IO_ERROR, "cannot open package directory: " + errno_text(errno));
        return std::nullopt;
    }

    const std::vector<std::string_view> segments = split_path_segments(relative_path);
    for (std::size_t i = 0; i < segments.size(); ++i) {
        if (cancellation_requested(out_error)) {
            return std::nullopt;
        }
        const std::string segment(segments[i]);
        const bool last = (i + 1 == segments.size());
        const int flags = last
            ? O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK
            : O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
        const int next = ::openat(current.get(), segment.c_str(), flags);
        if (next < 0) {
            const int error = errno;
            set_error(out_error, classify_open_error(error),
                "cannot open " + std::string(relative_path) + ": " + errno_text(error));
            return std::nullopt;
        }
        current.reset(next);
    }

    struct stat status {};
    if (::fstat(current.get(), &status) != 0) {
        set_error(out_error, Source_error_kind::IO_ERROR, "cannot stat " + std::string(relative_path) + ": " + errno_text(errno));
        return std::nullopt;
    }
    if (!S_ISREG(status.st_mode)) {
        set_error(out_error, Source_error_kind::NOT_REGULAR, std::string(relative_path) + " is not a regular file");
        return std::nullopt;
    }
    if ((std::uint64_t)status.st_size > max_bytes) {
        set_error(out_error, Source_error_kind::TOO_LARGE, std::string(relative_path) + " exceeds the read bound");
        return std::nullopt;
    }

    std::string bytes;
    bytes.reserve((std::size_t)status.st_size);
    char buffer[65536];
    while (true) {
        if (cancellation_requested(out_error)) {
            return std::nullopt;
        }
        const ssize_t count = ::read(current.get(), buffer, sizeof(buffer));
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            set_error(out_error, Source_error_kind::IO_ERROR, "cannot read " + std::string(relative_path) + ": " + errno_text(errno));
            return std::nullopt;
        }
        if (count == 0) {
            break;
        }
        bytes.append(buffer, (std::size_t)count);
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
