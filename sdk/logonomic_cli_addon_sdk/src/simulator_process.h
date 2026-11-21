#pragma once

// Internal: the minimal child-process surface the host simulator needs,
// implemented per platform (simulator_process_posix.cpp,
// simulator_process_win.cpp). Deliberately without custody semantics; the
// Logonomic host owns those in its own library.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace logonomic::cli_addon::process {

// One readiness handoff never transfers more than this many bytes from either
// child stream. POSIX enforces it while draining the non-blocking descriptor;
// Windows applies producer backpressure at the same boundary.
inline constexpr std::size_t k_max_stream_bytes_per_activity = 1u << 20;

struct Spawn_request
{
    std::string              executable;        // absolute, native form
    std::vector<std::string> argv;              // argv[0] included
    std::vector<std::string> environment;       // NAME=VALUE, complete
    std::string              working_directory;
    bool                     isolate = true;    // own process group (POSIX) / Job (Windows)
};

struct Exit_status
{
    bool signaled = false;
    int  code     = 0;   // exit code, or the signal number when signaled
};

struct Activity
{
    std::string                stdout_bytes;
    std::string                stderr_bytes;
    bool                       stdout_eof = false;
    bool                       stderr_eof = false;
    bool                       stdin_writable = false;
    std::optional<Exit_status> exit;   // reported once
};

enum class Write_status
{
    TRANSFERRED,
    WOULD_BLOCK,
    CLOSED,
    FAILED,
};

struct Write_result
{
    Write_status status = Write_status::FAILED;
    std::size_t  bytes  = 0;
};

class Child_process
{
public:
    static std::unique_ptr<Child_process> spawn(const Spawn_request& request, std::string* out_error);

    virtual ~Child_process() = default;

    virtual std::int64_t pid() const = 0;

    /// Non-blocking partial write. The caller retains and retries the
    /// unwritten tail after wait_activity() reports stdin_writable.
    virtual Write_result write_stdin(std::string_view bytes) = 0;
    virtual void close_stdin() = 0;

    /// Waits up to timeout_ms for output, end of file or exit.
    virtual Activity wait_activity(int timeout_ms, bool want_stdin_write) = 0;

    virtual bool exited() const = 0;
    virtual void terminate() = 0;   // SIGTERM / TerminateProcess
    virtual void kill() = 0;        // SIGKILL to the group / TerminateJobObject
    virtual bool owns(std::int64_t pid) const = 0;
};

} // namespace logonomic::cli_addon::process
