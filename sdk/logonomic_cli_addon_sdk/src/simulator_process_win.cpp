#include "simulator_process.h"
#include "logonomic_cli_addon/host_simulator.h"
#include "logonomic_cli_addon/limits.h"

#include <windows.h>

#include <algorithm>
#include <limits>
#include <mutex>
#include <string_view>
#include <utility>

namespace logonomic::cli_addon {

namespace {

bool to_wide(std::string_view utf8, std::wstring* out_wide)
{
    if (utf8.empty()) {
        out_wide->clear();
        return true;
    }
    if (utf8.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    const int length = ::MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        utf8.data(),
        static_cast<int>(utf8.size()),
        nullptr,
        0);
    if (length <= 0) {
        return false;
    }
    out_wide->resize(static_cast<std::size_t>(length));
    return ::MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        utf8.data(),
        static_cast<int>(utf8.size()),
        out_wide->data(),
        length) == length;
}

std::string to_utf8(std::wstring_view wide)
{
    if (wide.empty()) {
        return std::string();
    }
    const int length = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), nullptr, 0, nullptr, nullptr);
    std::string utf8(length > 0 ? (std::size_t)length : 0, '\0');
    if (length > 0) {
        ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), utf8.data(), length, nullptr, nullptr);
    }
    return utf8;
}

} // namespace

std::vector<std::pair<std::string, std::string>> current_process_environment()
{
    std::vector<std::pair<std::string, std::string>> pairs;
    wchar_t* block = ::GetEnvironmentStringsW();
    if (block == nullptr) {
        return pairs;
    }
    for (const wchar_t* entry = block; *entry != L'\0'; entry += ::wcslen(entry) + 1) {
        const std::wstring_view text(entry);
        const std::size_t equals = text.find(L'=');
        // Entries beginning with '=' are drive-letter bookkeeping, not variables.
        if (equals == std::wstring_view::npos || equals == 0) {
            continue;
        }
        pairs.emplace_back(to_utf8(text.substr(0, equals)), to_utf8(text.substr(equals + 1)));
    }
    ::FreeEnvironmentStringsW(block);
    return pairs;
}

bool kill_process(std::int64_t pid)
{
    HANDLE process = ::OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)pid);
    if (process == nullptr) {
        return false;
    }
    const BOOL ok = ::TerminateProcess(process, 1);
    ::CloseHandle(process);
    return ok != 0;
}

namespace process {

namespace {

void close_handle(HANDLE& handle)
{
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
        ::CloseHandle(handle);
    }
    handle = nullptr;
}

// Quotes one argument the way the Microsoft C runtime parses a command line.
void append_quoted_argument(std::wstring& command_line, const std::wstring& argument)
{
    if (!command_line.empty()) {
        command_line.push_back(L' ');
    }
    const bool needs_quotes = argument.empty() ||
        argument.find_first_of(L" \t\n\v\"") != std::wstring::npos;
    if (!needs_quotes) {
        command_line += argument;
        return;
    }
    command_line.push_back(L'"');
    std::size_t backslashes = 0;
    for (const wchar_t c : argument) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            command_line.append(backslashes * 2 + 1, L'\\');
            command_line.push_back(L'"');
        }
        else {
            command_line.append(backslashes, L'\\');
            command_line.push_back(c);
        }
        backslashes = 0;
    }
    command_line.append(backslashes * 2, L'\\');
    command_line.push_back(L'"');
}

bool build_environment_block(
    const std::vector<std::string>& environment,
    std::wstring*                   out_block,
    std::string*                    out_error)
{
    std::vector<std::wstring> entries;
    entries.reserve(environment.size());
    for (const std::string& entry : environment) {
        const std::size_t equal = entry.find('=');
        if (equal == std::string::npos || equal == 0 || entry.find('\0') != std::string::npos) {
            *out_error = "spawn: an environment entry is not NAME=VALUE or contains NUL";
            return false;
        }
        const std::string_view name(entry.data(), equal);
        const std::string_view value(entry.data() + equal + 1, entry.size() - equal - 1);
        std::wstring wide_name;
        std::wstring wide_value;
        if (!to_wide(name, &wide_name) || !to_wide(value, &wide_value)) {
            *out_error = "spawn: an environment entry is not valid UTF-8";
            return false;
        }
        if (wide_value.size() > limits::k_max_windows_environment_value_units) {
            *out_error = "spawn: environment value \"" + std::string(name) + "\" encodes to " +
                std::to_string(wide_value.size()) + " UTF-16 code units, above the native ceiling of " +
                std::to_string(limits::k_max_windows_environment_value_units);
            return false;
        }
        entries.push_back(std::move(wide_name) + L"=" + std::move(wide_value));
    }
    std::sort(entries.begin(), entries.end(), [](const std::wstring& a, const std::wstring& b) {
        return ::CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    out_block->clear();
    for (const std::wstring& entry : entries) {
        *out_block += entry;
        out_block->push_back(L'\0');
    }
    if (entries.empty()) {
        out_block->push_back(L'\0');
    }
    out_block->push_back(L'\0');
    return true;
}

struct Reader_state
{
    HANDLE       pipe   = nullptr;
    HANDLE       wake   = nullptr;
    HANDLE       space  = nullptr;
    HANDLE       stop   = nullptr;
    std::mutex*  mutex  = nullptr;
    std::string* buffer = nullptr;
    bool*        eof    = nullptr;
};

struct Writer_state
{
    HANDLE       pipe    = nullptr;
    HANDLE       wake    = nullptr;
    HANDLE       request = nullptr;
    HANDLE       stop    = nullptr;
    std::mutex*  mutex   = nullptr;
    std::string  buffer;
    bool         pending          = false;
    bool         completion_ready = false;
    std::size_t  completed        = 0;
    bool         failed           = false;
};

DWORD WINAPI reader_thread(LPVOID parameter)
{
    Reader_state* state = static_cast<Reader_state*>(parameter);
    char chunk[65536];
    while (true) {
        if (::WaitForSingleObject(state->stop, 0) == WAIT_OBJECT_0) {
            return 0;
        }
        DWORD count = 0;
        const BOOL ok = ::ReadFile(state->pipe, chunk, (DWORD)sizeof(chunk), &count, nullptr);
        if (!ok || count == 0) {
            std::lock_guard<std::mutex> lock(*state->mutex);
            *state->eof = true;
            ::SetEvent(state->wake);
            return 0;
        }
        std::size_t consumed = 0;
        while (consumed < count) {
            bool buffer_full = false;
            {
                std::lock_guard<std::mutex> lock(*state->mutex);
                const std::size_t available =
                    k_max_stream_bytes_per_activity - state->buffer->size();
                const std::size_t accepted = std::min<std::size_t>(count - consumed, available);
                state->buffer->append(chunk + consumed, accepted);
                consumed += accepted;
                buffer_full = state->buffer->size() == k_max_stream_bytes_per_activity;
                if (buffer_full) {
                    ::ResetEvent(state->space);
                }
            }
            ::SetEvent(state->wake);
            if (buffer_full) {
                HANDLE waitables[2] = {state->stop, state->space};
                const DWORD ready = ::WaitForMultipleObjects(2, waitables, FALSE, INFINITE);
                if (ready == WAIT_OBJECT_0 || ready == WAIT_FAILED) {
                    return 0;
                }
            }
        }
    }
}

DWORD WINAPI writer_thread(LPVOID parameter)
{
    Writer_state* state = static_cast<Writer_state*>(parameter);
    HANDLE waitables[2] = {state->stop, state->request};
    while (true) {
        const DWORD ready = ::WaitForMultipleObjects(2, waitables, FALSE, INFINITE);
        if (ready == WAIT_OBJECT_0 || ready == WAIT_FAILED) {
            return 0;
        }
        ::ResetEvent(state->request);

        std::size_t written = 0;
        while (written < state->buffer.size()) {
            DWORD count = 0;
            const DWORD chunk = static_cast<DWORD>(
                std::min<std::size_t>(state->buffer.size() - written, 1u << 20));
            if (!::WriteFile(state->pipe, state->buffer.data() + written, chunk, &count, nullptr)) {
                const bool stopping = ::WaitForSingleObject(state->stop, 0) == WAIT_OBJECT_0;
                std::lock_guard<std::mutex> lock(*state->mutex);
                state->pending = false;
                state->failed  = !stopping;
                ::SetEvent(state->wake);
                if (stopping) {
                    return 0;
                }
                break;
            }
            written += count;
        }
        if (written == state->buffer.size()) {
            std::lock_guard<std::mutex> lock(*state->mutex);
            state->pending          = false;
            state->completion_ready = true;
            state->completed        = written;
            ::SetEvent(state->wake);
        }
    }
}

class Win_child final : public Child_process
{
public:
    Win_child(HANDLE process, DWORD pid, HANDLE job, HANDLE stdin_write, HANDLE stdout_read, HANDLE stderr_read)
    :
        m_process(process),
        m_pid(pid),
        m_job(job),
        m_stdin_write(stdin_write),
        m_stdout_read(stdout_read),
        m_stderr_read(stderr_read)
    {
        m_wake          = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        m_reader_stop   = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        m_stdout_space  = ::CreateEventW(nullptr, TRUE, TRUE, nullptr);
        m_stderr_space  = ::CreateEventW(nullptr, TRUE, TRUE, nullptr);
        m_stdin_request = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        m_stdin_stop    = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        m_stdout_state = Reader_state{
            m_stdout_read, m_wake, m_stdout_space, m_reader_stop, &m_mutex, &m_stdout_buffer, &m_stdout_eof};
        m_stderr_state = Reader_state{
            m_stderr_read, m_wake, m_stderr_space, m_reader_stop, &m_mutex, &m_stderr_buffer, &m_stderr_eof};
        m_stdin_state  = Writer_state{
            .pipe             = m_stdin_write,
            .wake             = m_wake,
            .request          = m_stdin_request,
            .stop             = m_stdin_stop,
            .mutex            = &m_mutex,
            .buffer           = {},
            .pending          = false,
            .completion_ready = false,
            .completed        = 0,
            .failed           = false};
        m_stdout_thread = ::CreateThread(nullptr, 0, reader_thread, &m_stdout_state, 0, nullptr);
        m_stderr_thread = ::CreateThread(nullptr, 0, reader_thread, &m_stderr_state, 0, nullptr);
        m_stdin_thread  = ::CreateThread(nullptr, 0, writer_thread, &m_stdin_state, 0, nullptr);
    }

    ~Win_child() override
    {
        if (!m_exit.has_value()) {
            kill();
        }
        ::SetEvent(m_reader_stop);
        ::SetEvent(m_stdout_space);
        ::SetEvent(m_stderr_space);
        for (HANDLE thread : {m_stdout_thread, m_stderr_thread}) {
            if (thread != nullptr) {
                // Cancellation can race the reader between consecutive
                // synchronous reads. The stop check prevents another read;
                // retrying cancellation closes the remaining race window.
                DWORD stopped = WAIT_TIMEOUT;
                while (stopped == WAIT_TIMEOUT) {
                    ::CancelSynchronousIo(thread);
                    stopped = ::WaitForSingleObject(thread, 100);
                }
                ::CloseHandle(thread);
            }
        }
        close_stdin();
        close_handle(m_stdout_read);
        close_handle(m_stderr_read);
        close_handle(m_wake);
        close_handle(m_reader_stop);
        close_handle(m_stdout_space);
        close_handle(m_stderr_space);
        close_handle(m_stdin_request);
        close_handle(m_stdin_stop);
        close_handle(m_process);
        close_handle(m_job);   // kill-on-close removes whatever is left in the Job
    }

    std::int64_t pid() const override { return m_pid; }

    Write_result write_stdin(std::string_view bytes) override
    {
        Write_result result;
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stdin_write == nullptr || m_stdin_thread == nullptr) {
            result.status = Write_status::CLOSED;
            return result;
        }
        if (m_stdin_state.failed) {
            result.status = Write_status::FAILED;
            return result;
        }
        if (m_stdin_state.completion_ready) {
            result.status = Write_status::TRANSFERRED;
            result.bytes  = m_stdin_state.completed;
            m_stdin_state.completion_ready = false;
            m_stdin_state.completed        = 0;
            m_stdin_state.buffer.clear();
            return result;
        }
        if (m_stdin_state.pending) {
            result.status = Write_status::WOULD_BLOCK;
            return result;
        }
        m_stdin_state.buffer.assign(bytes.data(), bytes.size());
        m_stdin_state.pending = true;
        ::SetEvent(m_stdin_request);
        result.status = Write_status::WOULD_BLOCK;
        return result;
    }

    void close_stdin() override
    {
        if (m_stdin_thread != nullptr) {
            ::SetEvent(m_stdin_stop);
            ::CancelSynchronousIo(m_stdin_thread);
            ::WaitForSingleObject(m_stdin_thread, INFINITE);
            close_handle(m_stdin_thread);
        }
        close_handle(m_stdin_write);
        m_stdin_state.pipe = nullptr;
    }

    Activity wait_activity(int timeout_ms, bool want_stdin_write) override
    {
        Activity activity;
        HANDLE handles[2];
        DWORD count = 0;
        handles[count++] = m_wake;
        if (!m_exit.has_value()) {
            handles[count++] = m_process;
        }
        const DWORD waited = ::WaitForMultipleObjects(count, handles, FALSE, (DWORD)std::max(0, timeout_ms));
        if (waited == WAIT_TIMEOUT || waited == WAIT_FAILED) {
            return activity;
        }
        ::ResetEvent(m_wake);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            activity.stdout_bytes.swap(m_stdout_buffer);
            activity.stderr_bytes.swap(m_stderr_buffer);
            activity.stdout_eof = m_stdout_eof;
            activity.stderr_eof = m_stderr_eof;
            activity.stdin_writable = want_stdin_write &&
                (m_stdin_state.completion_ready || m_stdin_state.failed);
        }
        ::SetEvent(m_stdout_space);
        ::SetEvent(m_stderr_space);
        if (!m_exit.has_value() && ::WaitForSingleObject(m_process, 0) == WAIT_OBJECT_0) {
            DWORD code = 0;
            ::GetExitCodeProcess(m_process, &code);
            Exit_status exit;
            exit.code     = (int)code;
            m_exit        = exit;
            activity.exit = exit;
        }
        return activity;
    }

    bool exited() const override { return m_exit.has_value(); }

    void terminate() override
    {
        if (!m_exit.has_value()) {
            ::TerminateProcess(m_process, 1);
        }
    }

    void kill() override
    {
        if (m_job != nullptr) {
            ::TerminateJobObject(m_job, 1);
        }
        else
        if (!m_exit.has_value()) {
            ::TerminateProcess(m_process, 1);
        }
    }

    bool owns(std::int64_t pid) const override
    {
        if (m_job == nullptr) {
            return false;
        }
        HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
        if (process == nullptr) {
            return false;
        }
        BOOL in_job = FALSE;
        const BOOL ok = ::IsProcessInJob(process, m_job, &in_job);
        ::CloseHandle(process);
        return ok && in_job;
    }

private:
    HANDLE                     m_process;
    DWORD                      m_pid;
    HANDLE                     m_job;
    HANDLE                     m_stdin_write;
    HANDLE                     m_stdout_read;
    HANDLE                     m_stderr_read;
    HANDLE                     m_wake          = nullptr;
    HANDLE                     m_reader_stop   = nullptr;
    HANDLE                     m_stdout_space  = nullptr;
    HANDLE                     m_stderr_space  = nullptr;
    HANDLE                     m_stdin_request = nullptr;
    HANDLE                     m_stdin_stop    = nullptr;
    HANDLE                     m_stdout_thread = nullptr;
    HANDLE                     m_stderr_thread = nullptr;
    HANDLE                     m_stdin_thread  = nullptr;
    Reader_state               m_stdout_state;
    Reader_state               m_stderr_state;
    Writer_state               m_stdin_state;
    std::mutex                 m_mutex;
    std::string                m_stdout_buffer;
    std::string                m_stderr_buffer;
    bool                       m_stdout_eof = false;
    bool                       m_stderr_eof = false;
    std::optional<Exit_status> m_exit;
};

struct Inheritable_pipe
{
    HANDLE read  = nullptr;
    HANDLE write = nullptr;

    bool open(bool child_reads)
    {
        SECURITY_ATTRIBUTES attributes{};
        attributes.nLength        = sizeof(attributes);
        attributes.bInheritHandle = TRUE;
        if (!::CreatePipe(&read, &write, &attributes, 0)) {
            return false;
        }
        // Only the child's end stays inheritable.
        return ::SetHandleInformation(child_reads ? write : read, HANDLE_FLAG_INHERIT, 0) != 0;
    }

    ~Inheritable_pipe()
    {
        close_handle(read);
        close_handle(write);
    }
};

} // namespace

std::unique_ptr<Child_process> Child_process::spawn(const Spawn_request& request, std::string* out_error,
    std::uint32_t* out_native_error)
{
    if (out_native_error) *out_native_error = 0;
    const auto fail = [out_error, out_native_error](const std::string& text, DWORD code) -> std::unique_ptr<Child_process> {
        if (out_native_error) *out_native_error = code;
        if (out_error != nullptr) {
            *out_error = text + " (Win32 error " + std::to_string(code) + ")";
        }
        return nullptr;
    };
    const auto preflight_fail = [out_error](const std::string& text) -> std::unique_ptr<Child_process> {
        if (out_error != nullptr) {
            *out_error = text;
        }
        return nullptr;
    };

    // Validate exactly what CreateProcessW will receive before allocating any
    // child-side resources. The native ceiling includes its terminating NUL,
    // so the assembled UTF-16 command line itself must stay at 32766 units.
    std::wstring executable;
    std::wstring working_directory;
    if (request.executable.find('\0') != std::string::npos ||
        request.working_directory.find('\0') != std::string::npos ||
        !to_wide(request.executable, &executable) ||
        !to_wide(request.working_directory, &working_directory))
    {
        return preflight_fail("spawn: executable or working directory is not valid UTF-8 or contains NUL");
    }
    std::wstring command_line;
    for (const std::string& argument : request.argv) {
        std::wstring wide_argument;
        if (argument.find('\0') != std::string::npos || !to_wide(argument, &wide_argument)) {
            return preflight_fail("spawn: an argument is not valid UTF-8 or contains NUL");
        }
        append_quoted_argument(command_line, wide_argument);
    }
    if (command_line.size() > limits::k_max_windows_native_command_line_units) {
        return preflight_fail(
            "spawn: assembled native command line is " + std::to_string(command_line.size()) +
            " UTF-16 code units, above the native ceiling of " +
            std::to_string(limits::k_max_windows_native_command_line_units));
    }
    std::wstring environment_block;
    std::string environment_error;
    if (!build_environment_block(request.environment, &environment_block, &environment_error)) {
        return preflight_fail(environment_error);
    }

    Inheritable_pipe stdin_pipe;
    Inheritable_pipe stdout_pipe;
    Inheritable_pipe stderr_pipe;
    if (!stdin_pipe.open(true) || !stdout_pipe.open(false) || !stderr_pipe.open(false)) {
        return fail("CreatePipe", ::GetLastError());
    }

    HANDLE job = nullptr;
    if (request.isolate) {
        job = ::CreateJobObjectW(nullptr, nullptr);
        if (job == nullptr) {
            return fail("CreateJobObjectW", ::GetLastError());
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            const DWORD code = ::GetLastError();
            ::CloseHandle(job);
            return fail("SetInformationJobObject", code);
        }
    }

    SIZE_T attribute_size = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
    std::vector<unsigned char> attribute_storage(attribute_size);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
    if (!::InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_size)) {
        const DWORD code = ::GetLastError();
        close_handle(job);
        return fail("InitializeProcThreadAttributeList", code);
    }
    HANDLE inherited[3] = {stdin_pipe.read, stdout_pipe.write, stderr_pipe.write};
    if (!::UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr)) {
        const DWORD code = ::GetLastError();
        ::DeleteProcThreadAttributeList(attributes);
        close_handle(job);
        return fail("UpdateProcThreadAttribute", code);
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb          = sizeof(startup);
    startup.StartupInfo.dwFlags     = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput   = stdin_pipe.read;
    startup.StartupInfo.hStdOutput  = stdout_pipe.write;
    startup.StartupInfo.hStdError   = stderr_pipe.write;
    startup.lpAttributeList         = attributes;

    PROCESS_INFORMATION information{};
    const BOOL created = ::CreateProcessW(
        executable.c_str(),
        command_line.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
        environment_block.data(),
        working_directory.empty() ? nullptr : working_directory.c_str(),
        &startup.StartupInfo,
        &information);
    const DWORD create_error = created ? ERROR_SUCCESS : ::GetLastError();
    ::DeleteProcThreadAttributeList(attributes);
    if (!created) {
        close_handle(job);
        return fail("CreateProcessW " + request.executable, create_error);
    }

    if (job != nullptr && !::AssignProcessToJobObject(job, information.hProcess)) {
        const DWORD code = ::GetLastError();
        ::TerminateProcess(information.hProcess, 1);
        ::CloseHandle(information.hThread);
        ::CloseHandle(information.hProcess);
        close_handle(job);
        return fail("AssignProcessToJobObject", code);
    }
    ::ResumeThread(information.hThread);
    ::CloseHandle(information.hThread);

    HANDLE stdin_write = stdin_pipe.write;
    HANDLE stdout_read = stdout_pipe.read;
    HANDLE stderr_read = stderr_pipe.read;
    stdin_pipe.write = nullptr;
    stdout_pipe.read = nullptr;
    stderr_pipe.read = nullptr;
    return std::make_unique<Win_child>(information.hProcess, information.dwProcessId, job, stdin_write, stdout_read, stderr_read);
}

} // namespace process

} // namespace logonomic::cli_addon
