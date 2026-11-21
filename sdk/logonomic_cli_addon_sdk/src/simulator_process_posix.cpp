#include "simulator_process.h"
#include "logonomic_cli_addon/host_simulator.h"
#include "spawn_file_actions_p.h"
#if defined(__APPLE__)
#include "logonomic_cli_addon/launcher_utilities.h"
#endif

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#if defined(__APPLE__)
#include <sys/event.h>
#else
#include <sys/syscall.h>
#endif
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

extern char** environ;

namespace logonomic::cli_addon {

std::vector<std::pair<std::string, std::string>> current_process_environment()
{
    std::vector<std::pair<std::string, std::string>> pairs;
    for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
        const std::string_view text(*entry);
        const std::size_t equals = text.find('=');
        if (equals == std::string_view::npos || equals == 0) {
            continue;
        }
        pairs.emplace_back(std::string(text.substr(0, equals)), std::string(text.substr(equals + 1)));
    }
    return pairs;
}

bool kill_process(std::int64_t pid)
{
    return pid > 1 && ::kill((pid_t)pid, SIGKILL) == 0;
}

namespace process {

namespace {

void close_quietly(int& fd)
{
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

bool set_nonblocking(int fd)
{
    const int flags = ::fcntl(fd, F_GETFL);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

class Posix_child final : public Child_process
{
public:
    Posix_child(pid_t pid, int pidfd, int stdin_fd, int stdout_fd, int stderr_fd, bool isolated)
    :
        m_pid(pid),
        m_pidfd(pidfd),
        m_stdin_fd(stdin_fd),
        m_stdout_fd(stdout_fd),
        m_stderr_fd(stderr_fd),
        m_isolated(isolated)
    {}

    ~Posix_child() override
    {
#if defined(__APPLE__)
        // Keep the direct child unreaped through the last group signal. Its
        // retained PID prevents a post-exit cleanup from reaching a new group.
        kill();
        if (m_wait_owned) {
            int status = 0;
            pid_t waited;
            do { waited = ::waitpid(m_pid, &status, 0); } while (waited < 0 && errno == EINTR);
            m_wait_owned = false;
        }
#endif
        close_quietly(m_stdin_fd);
        close_quietly(m_stdout_fd);
        close_quietly(m_stderr_fd);
        close_quietly(m_pidfd);
    }

    std::int64_t pid() const override { return m_pid; }

    Write_result write_stdin(std::string_view bytes) override
    {
        Write_result result;
        if (m_stdin_fd < 0) {
            result.status = Write_status::CLOSED;
            return result;
        }
        ssize_t count = 0;
        do {
            count = ::write(m_stdin_fd, bytes.data(), bytes.size());
        }
        while (count < 0 && errno == EINTR);
        if (count < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                result.status = Write_status::WOULD_BLOCK;
            }
            else
            if (errno == EPIPE) {
                result.status = Write_status::CLOSED;
            }
            else {
                result.status = Write_status::FAILED;
            }
            return result;
        }
        result.status = count == 0 ? Write_status::CLOSED : Write_status::TRANSFERRED;
        result.bytes  = static_cast<std::size_t>(count);
        return result;
    }

    void close_stdin() override { close_quietly(m_stdin_fd); }

    Activity wait_activity(int timeout_ms, bool want_stdin_write) override
    {
        Activity activity;
        pollfd fds[4];
        int stdin_index  = -1;
        int stdout_index = -1;
        int stderr_index = -1;
        int pid_index    = -1;
        nfds_t count = 0;
        if (want_stdin_write && m_stdin_fd >= 0) { stdin_index = (int)count; fds[count++] = pollfd{m_stdin_fd, POLLOUT, 0}; }
        if (m_stdout_fd >= 0) { stdout_index = (int)count; fds[count++] = pollfd{m_stdout_fd, POLLIN, 0}; }
        if (m_stderr_fd >= 0) { stderr_index = (int)count; fds[count++] = pollfd{m_stderr_fd, POLLIN, 0}; }
        if (!m_exit.has_value() && m_pidfd >= 0) { pid_index = (int)count; fds[count++] = pollfd{m_pidfd, POLLIN, 0}; }
        if (count == 0) {
            return activity;
        }

        const int ready = ::poll(fds, count, timeout_ms);
        if (ready <= 0) {
            return activity;
        }

        if (stdin_index >= 0 && fds[stdin_index].revents != 0) {
            activity.stdin_writable = true;
        }

        if (stdout_index >= 0 && fds[stdout_index].revents != 0) {
            drain(m_stdout_fd, activity.stdout_bytes, activity.stdout_eof);
        }
        if (stderr_index >= 0 && fds[stderr_index].revents != 0) {
            drain(m_stderr_fd, activity.stderr_bytes, activity.stderr_eof);
        }
        if (pid_index >= 0 && fds[pid_index].revents != 0) {
#if defined(__APPLE__)
            siginfo_t info{};
            int observed;
            do {
                observed = ::waitid(P_PID, static_cast<id_t>(m_pid), &info, WEXITED | WNOHANG | WNOWAIT);
            } while (observed < 0 && errno == EINTR);
            if (observed == 0 && info.si_pid == m_pid) {
                Exit_status exit;
                exit.signaled = info.si_code != CLD_EXITED;
                exit.code = info.si_status;
                m_exit = exit;
                activity.exit = exit;
            }
            else
            if (observed < 0 && errno == ECHILD) {
                m_wait_owned = false;
                close_quietly(m_pidfd);
            }
#else
            int status = 0;
            const pid_t reaped = ::waitpid(m_pid, &status, WNOHANG);
            if (reaped == m_pid) {
                Exit_status exit;
                if (WIFSIGNALED(status)) {
                    exit.signaled = true;
                    exit.code     = WTERMSIG(status);
                }
                else {
                    exit.code = WEXITSTATUS(status);
                }
                m_exit        = exit;
                activity.exit = exit;
            }
#endif
        }
        return activity;
    }

    bool exited() const override { return m_exit.has_value(); }

    void terminate() override
    {
        if (!m_exit.has_value()
#if defined(__APPLE__)
            && m_wait_owned
#endif
        ) {
            ::kill(m_pid, SIGTERM);
        }
    }

    void kill() override
    {
#if defined(__APPLE__)
        if (!m_wait_owned) {
            return;
        }
#endif
        // The group can outlive its leader (an orphaned grandchild), so the
        // group signal is sent whether or not the adapter itself is gone.
        if (m_isolated) {
            ::kill(-m_pid, SIGKILL);
        }
        else
        if (!m_exit.has_value()) {
            ::kill(m_pid, SIGKILL);
        }
    }

    bool owns(std::int64_t pid) const override
    {
#if defined(__APPLE__)
        if (!m_wait_owned) {
            return false;
        }
#endif
        const pid_t group = ::getpgid((pid_t)pid);
        if (group < 0) {
            return false;
        }
        return group == (m_isolated ? m_pid : ::getpgid(0));
    }

private:
    // Reads what is available without blocking; EOF closes the descriptor.
    void drain(int& fd, std::string& out_bytes, bool& out_eof)
    {
        char buffer[65536];
        std::size_t total = 0;
        while (total < k_max_stream_bytes_per_activity) {
            const ssize_t count = ::read(fd, buffer, sizeof(buffer));
            if (count > 0) {
                out_bytes.append(buffer, (std::size_t)count);
                total += (std::size_t)count;
                continue;
            }
            if (count == 0) {
                out_eof = true;
                close_quietly(fd);
                return;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno != EAGAIN) {
                out_eof = true;
                close_quietly(fd);
            }
            return;
        }
    }

    pid_t                      m_pid;
    int                        m_pidfd;
    int                        m_stdin_fd;
    int                        m_stdout_fd;
    int                        m_stderr_fd;
    bool                       m_isolated;
    std::optional<Exit_status> m_exit;
#if defined(__APPLE__)
    bool                       m_wait_owned = true;
#endif
};

class Pipe_pair
{
public:
    int read_end  = -1;
    int write_end = -1;

    bool open()
    {
        int fds[2];
#if defined(__APPLE__)
        if (!open_cloexec_pipe(fds)) {
            return false;
        }
        for (int index = 0; index < 2; ++index) {
            // Keep sources away from 0/1/2 so the ordered dup2 actions cannot
            // overwrite another stream when a host has closed standard I/O.
            const int copy = ::fcntl(fds[index], F_DUPFD_CLOEXEC, 3);
            ::close(fds[index]);
            fds[index] = copy;
            if (copy < 0) {
                ::close(fds[1 - index]);
                return false;
            }
        }
#else
        if (::pipe2(fds, O_CLOEXEC) != 0) {
            return false;
        }
#endif
        read_end  = fds[0];
        write_end = fds[1];
        return true;
    }

    ~Pipe_pair()
    {
        close_quietly(read_end);
        close_quietly(write_end);
    }

    int release_read()  { const int fd = read_end;  read_end  = -1; return fd; }
    int release_write() { const int fd = write_end; write_end = -1; return fd; }
};

} // namespace

std::unique_ptr<Child_process> Child_process::spawn(const Spawn_request& request, std::string* out_error,
    std::uint32_t* out_native_error)
{
    if (out_native_error) *out_native_error = 0;
    const auto fail = [out_error, out_native_error](const std::string& operation, int code) -> std::unique_ptr<Child_process> {
        if (out_native_error) *out_native_error = (std::uint32_t)code;
        if (out_error != nullptr) {
            *out_error = operation + ": " + std::strerror(code);
        }
        return nullptr;
    };

    Pipe_pair stdin_pipe;
    Pipe_pair stdout_pipe;
    Pipe_pair stderr_pipe;
    if (!stdin_pipe.open() || !stdout_pipe.open() || !stderr_pipe.open()) {
        return fail("pipe2", errno);
    }

    posix_spawn_file_actions_t actions;
    int setup_error = ::posix_spawn_file_actions_init(&actions);
    if (setup_error != 0) {
        return fail("spawn file actions", setup_error);
    }
    setup_error = ::posix_spawn_file_actions_adddup2(&actions, stdin_pipe.read_end, 0);
    if (setup_error == 0) {
        setup_error = ::posix_spawn_file_actions_adddup2(&actions, stdout_pipe.write_end, 1);
    }
    if (setup_error == 0) {
        setup_error = ::posix_spawn_file_actions_adddup2(&actions, stderr_pipe.write_end, 2);
    }
    if (!request.working_directory.empty()) {
        if (setup_error == 0) {
            setup_error = detail::add_spawn_working_directory(
                &actions, request.working_directory.c_str());
        }
    }
    // Only the three explicitly duplicated streams cross the exec boundary.
#if !defined(__APPLE__)
    if (setup_error == 0) {
        setup_error = ::posix_spawn_file_actions_addclosefrom_np(&actions, 3);
    }
#endif
    if (setup_error != 0) {
        ::posix_spawn_file_actions_destroy(&actions);
        return fail("spawn file actions", setup_error);
    }

    posix_spawnattr_t attributes;
    setup_error = ::posix_spawnattr_init(&attributes);
    if (setup_error != 0) {
        ::posix_spawn_file_actions_destroy(&actions);
        return fail("spawn attributes", setup_error);
    }
    short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#if defined(__APPLE__)
    // Register the native exit event before the child can execute and exit.
    flags = static_cast<short>(flags | POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_START_SUSPENDED);
#endif
    if (request.isolate) {
        flags = (short)(flags | POSIX_SPAWN_SETPGROUP);
        setup_error = ::posix_spawnattr_setpgroup(&attributes, 0);
    }
    if (setup_error == 0) {
        setup_error = ::posix_spawnattr_setflags(&attributes, flags);
    }
    sigset_t empty_set;
    sigemptyset(&empty_set);
    if (setup_error == 0) {
        setup_error = ::posix_spawnattr_setsigmask(&attributes, &empty_set);
    }
    sigset_t all_signals;
    sigfillset(&all_signals);
    if (setup_error == 0) {
        setup_error = ::posix_spawnattr_setsigdefault(&attributes, &all_signals);
    }
    if (setup_error != 0) {
        ::posix_spawn_file_actions_destroy(&actions);
        ::posix_spawnattr_destroy(&attributes);
        return fail("spawn attributes", setup_error);
    }

    std::vector<char*> argv;
    for (const std::string& argument : request.argv) {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    std::vector<char*> envp;
    for (const std::string& entry : request.environment) {
        envp.push_back(const_cast<char*>(entry.c_str()));
    }
    envp.push_back(nullptr);

    pid_t pid = 0;
    const int spawn_error = ::posix_spawn(&pid, request.executable.c_str(), &actions, &attributes, argv.data(), envp.data());
    ::posix_spawn_file_actions_destroy(&actions);
    ::posix_spawnattr_destroy(&attributes);
    if (spawn_error != 0) {
        return fail("posix_spawn " + request.executable, spawn_error);
    }

#if defined(__APPLE__)
    int pidfd = ::kqueue();
    struct kevent event{};
    EV_SET(&event, pid, EVFILT_PROC, EV_ADD | EV_ENABLE, NOTE_EXIT, 0, nullptr);
    if (pidfd >= 0 &&
        (::fcntl(pidfd, F_SETFD, FD_CLOEXEC) < 0 ||
         ::kevent(pidfd, &event, 1, nullptr, 0, nullptr) < 0))
    {
        const int error = errno;
        close_quietly(pidfd);
        errno = error;
    }
#else
    const int pidfd = (int)::syscall(SYS_pidfd_open, pid, 0);
#endif
    if (pidfd < 0) {
        const int error = errno;
        ::kill(pid, SIGKILL);
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
#if defined(__APPLE__)
        return fail("native exit observer", error);
#else
        return fail("pidfd_open", error);
#endif
    }

    const int stdin_fd  = stdin_pipe.release_write();
    const int stdout_fd = stdout_pipe.release_read();
    const int stderr_fd = stderr_pipe.release_read();
#if defined(__APPLE__)
    auto child = std::make_unique<Posix_child>(pid, pidfd, stdin_fd, stdout_fd, stderr_fd, request.isolate);
    if (!set_nonblocking(stdin_fd) || !set_nonblocking(stdout_fd) || !set_nonblocking(stderr_fd) ||
        ::fcntl(stdin_fd, F_SETNOSIGPIPE, 1) < 0 || ::kill(pid, SIGCONT) < 0)
    {
        return fail("native child stream or resume", errno);
    }
    return child;
#else
    set_nonblocking(stdin_fd);
    set_nonblocking(stdout_fd);
    set_nonblocking(stderr_fd);
    return std::make_unique<Posix_child>(pid, pidfd, stdin_fd, stdout_fd, stderr_fd, request.isolate);
#endif
}

} // namespace process

} // namespace logonomic::cli_addon
