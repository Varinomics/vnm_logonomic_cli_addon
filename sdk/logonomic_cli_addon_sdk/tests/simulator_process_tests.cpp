#include "sdk_test_support.h"

#include "simulator_process.h"

#include "logonomic_cli_addon/host_simulator.h"
#include "logonomic_cli_addon/limits.h"
#include "logonomic_cli_addon/package_source.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <cerrno>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace sdk_tests {

namespace {

namespace addon   = logonomic::cli_addon;
namespace process = logonomic::cli_addon::process;

std::vector<std::string> inherited_environment()
{
    std::vector<std::string> environment;
    for (const auto& [name, value] : addon::current_process_environment()) {
        environment.push_back(name + "=" + value);
    }
    return environment;
}

#if defined(_WIN32)

bool to_wide(std::string_view in_utf8, std::wstring* out_wide)
{
    const int units = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        in_utf8.data(),
        static_cast<int>(in_utf8.size()),
        nullptr,
        0);
    if (units <= 0) {
        return false;
    }
    out_wide->resize(static_cast<std::size_t>(units));
    return MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        in_utf8.data(),
        static_cast<int>(in_utf8.size()),
        out_wide->data(),
        units) == units;
}

void append_quoted_argument(std::wstring* in_out_line, const std::wstring& in_argument)
{
    if (!in_out_line->empty()) {
        in_out_line->push_back(L' ');
    }
    const bool needs_quotes = in_argument.empty() ||
        in_argument.find_first_of(L" \t\n\v\"") != std::wstring::npos;
    if (!needs_quotes) {
        in_out_line->append(in_argument);
        return;
    }
    in_out_line->push_back(L'\"');
    std::size_t backslashes = 0;
    for (const wchar_t character : in_argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'\"') {
            in_out_line->append(backslashes * 2 + 1, L'\\');
        }
        else {
            in_out_line->append(backslashes, L'\\');
        }
        in_out_line->push_back(character);
        backslashes = 0;
    }
    in_out_line->append(backslashes * 2, L'\\');
    in_out_line->push_back(L'\"');
}

std::size_t native_command_line_units(const std::vector<std::string>& in_argv)
{
    std::wstring line;
    for (const std::string& argument : in_argv) {
        std::wstring wide;
        if (!to_wide(argument, &wide)) {
            return 0;
        }
        append_quoted_argument(&line, wide);
    }
    return line.size();
}

process::Spawn_request native_limit_request()
{
    std::error_code error;
    process::Spawn_request request;
    request.executable = addon::path_to_utf8(test_executable());
    request.argv        = {request.executable, "--native-preflight-child"};
    request.environment = {"NATIVE_LIMIT=x"};
    request.working_directory = addon::path_to_utf8(std::filesystem::current_path(error));
    return request;
}

bool starts(process::Spawn_request request, std::string* out_error)
{
    std::unique_ptr<process::Child_process> child = process::Child_process::spawn(request, out_error);
    if (!child) {
        return false;
    }
    child->kill();
    return true;
}

#endif

} // namespace

bool test_simulator_stream_backpressure()
{
    const char* id = "simulator_stream_backpressure";
    std::error_code path_error;
    const std::filesystem::path working_directory = std::filesystem::current_path(path_error);
    SDK_CHECK(!path_error, "current working directory is available");
    SDK_CHECK(!test_executable().empty(), "test executable path is available");

    process::Spawn_request request;
    request.executable        = addon::path_to_utf8(test_executable());
    request.argv              = {request.executable, "--stream-flood-child"};
    request.environment       = inherited_environment();
    request.working_directory = addon::path_to_utf8(working_directory);
    request.isolate           = true;

    std::string error;
    std::unique_ptr<process::Child_process> child = process::Child_process::spawn(request, &error);
    SDK_CHECK(child != nullptr, "continuous-output child starts: " + error);

    // Let each dedicated reader reach its bound before consuming. On Windows
    // this exercises the staging buffer rather than the much smaller kernel
    // pipe; on POSIX the same observable per-activity ceiling applies.
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    // The contract (simulator_process.h): one handoff never carries more than
    // k_max_stream_bytes_per_activity from either stream, and the bound is a
    // per-handoff ceiling, not a cap on the stream - a producer held at the
    // bound is released by the next handoff, so each stream eventually
    // delivers more than one bound's worth. "Eventually" is the oracle; the
    // deadline only turns a hang into a failure. No fixed handoff budget can
    // stand in for it: a handoff carries the streams that are readable when
    // wait_activity() polls, and the stream drained last, microseconds
    // earlier, is routinely not refilled yet, so a handoff legitimately
    // carries one stream. The budget this replaces allowed exactly 17
    // handoffs, the number of 64 KiB pipe loads it takes on Linux to exceed
    // the bound, and so measured how the child's two writers were scheduled
    // against poll(2): it failed whenever the stream drained last missed a
    // handoff and no multi-load drain made the load up.
    const auto  deadline     = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    std::size_t stdout_bytes = 0;
    std::size_t stderr_bytes = 0;
    bool        ok           = true;
    bool        streaming    = true;
    while (streaming &&
           (stdout_bytes <= process::k_max_stream_bytes_per_activity ||
            stderr_bytes <= process::k_max_stream_bytes_per_activity) &&
           std::chrono::steady_clock::now() < deadline)
    {
        const process::Activity activity = child->wait_activity(250, false);
        ok &= check(activity.stdout_bytes.size() <= process::k_max_stream_bytes_per_activity, id,
            "every stdout handoff stays within the process staging bound");
        ok &= check(activity.stderr_bytes.size() <= process::k_max_stream_bytes_per_activity, id,
            "every stderr handoff stays within the process staging bound");
        stdout_bytes += activity.stdout_bytes.size();
        stderr_bytes += activity.stderr_bytes.size();
        streaming = !activity.stdout_eof && !activity.stderr_eof && !activity.exit.has_value();
    }
    ok &= check(streaming, id,
        "continuous-output child keeps both streams open while it is consumed");
    ok &= check(stdout_bytes > process::k_max_stream_bytes_per_activity, id,
        "stdout producer resumes after bounded-buffer backpressure");
    ok &= check(stderr_bytes > process::k_max_stream_bytes_per_activity, id,
        "stderr producer resumes after bounded-buffer backpressure");

    child->kill();
    const auto reap_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!child->exited() && std::chrono::steady_clock::now() < reap_deadline) {
        (void)child->wait_activity(100, false);
    }
    ok &= check(child->exited(), id, "continuous-output child is reaped after termination");
    return ok;
}

bool test_simulator_darwin_retains_wait_until_cleanup()
{
#if !defined(__APPLE__)
    return true;
#else
    process::Spawn_request request;
    request.executable = "/bin/sh";
    request.argv = {request.executable, "-c", "sleep 30 & exit 7"};
    request.environment = inherited_environment();
    request.isolate = true;
    std::string error;
    auto child = process::Child_process::spawn(request, &error);
    SDK_CHECK(child != nullptr, "isolated child starts: " + error);
    const auto pid = static_cast<pid_t>(child->pid());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::optional<process::Exit_status> exit;
    while (!exit && std::chrono::steady_clock::now() < deadline) {
        exit = child->wait_activity(50, false).exit;
    }
    SDK_CHECK(exit && !exit->signaled && exit->code == 7, "original root exit is observed");
    siginfo_t info{};
    SDK_CHECK(::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG | WNOWAIT) == 0 &&
        info.si_pid == pid, "exit observation has not consumed the root wait");
    child->kill();
    bool stdout_eof = false;
    bool stderr_eof = false;
    while ((!stdout_eof || !stderr_eof) && std::chrono::steady_clock::now() < deadline) {
        const auto activity = child->wait_activity(50, false);
        stdout_eof = stdout_eof || activity.stdout_eof;
        stderr_eof = stderr_eof || activity.stderr_eof;
    }
    SDK_CHECK(stdout_eof && stderr_eof, "post-exit cleanup closes the original group's inherited streams");
    child->kill();
    info = {};
    SDK_CHECK(::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG | WNOWAIT) == 0 &&
        info.si_pid == pid, "the final repeated signal still precedes the sole consuming wait");
    child.reset();
    int status = 0;
    SDK_CHECK(::waitpid(pid, &status, WNOHANG) == -1 && errno == ECHILD,
        "destruction consumes exactly the retained direct-child wait");
    return true;
#endif
}

bool test_simulator_windows_native_limits()
{
#if !defined(_WIN32)
    return true;
#else
    const char* id = "simulator_windows_native_limits";
    process::Spawn_request command = native_limit_request();
    const std::size_t command_base = native_command_line_units(command.argv);
    SDK_CHECK(command_base + 1 <= addon::limits::k_max_windows_native_command_line_units,
        "native-limit child command has room for a sizing argument");
    const auto command_with_size = [&](std::size_t in_total_units) {
        process::Spawn_request request = command;
        request.argv.push_back(std::string(in_total_units - command_base - 1, 'x'));
        return request;
    };

    bool ok = true;
    for (const std::size_t units : {
             addon::limits::k_max_windows_native_command_line_units - 1,
             addon::limits::k_max_windows_native_command_line_units,
         })
    {
        process::Spawn_request request = command_with_size(units);
        std::string error;
        ok &= check(native_command_line_units(request.argv) == units, id,
            "command sizing oracle reaches the requested UTF-16 boundary");
        ok &= check(starts(std::move(request), &error), id,
            units == addon::limits::k_max_windows_native_command_line_units
                ? "simulator starts at the native command-line boundary: " + error
                : "simulator starts below the native command-line boundary: " + error);
    }
    {
        process::Spawn_request request = command_with_size(
            addon::limits::k_max_windows_native_command_line_units + 1);
        std::string error;
        ok &= check(!starts(std::move(request), &error) &&
                error.find("assembled native command line") != std::string::npos,
            id,
            "simulator rejects one UTF-16 command-line unit above the native boundary");
    }

    for (const std::size_t value_bytes : {
             addon::limits::k_max_windows_environment_value_units - 1,
             addon::limits::k_max_windows_environment_value_units,
         })
    {
        process::Spawn_request request = native_limit_request();
        request.environment = {"NATIVE_LIMIT=" + std::string(value_bytes, 'x')};
        std::string error;
        ok &= check(starts(std::move(request), &error), id,
            value_bytes == addon::limits::k_max_windows_environment_value_units
                ? "simulator starts at the native environment-value boundary: " + error
                : "simulator starts below the native environment-value boundary: " + error);
    }
    {
        process::Spawn_request request = native_limit_request();
        request.environment = {
            "NATIVE_LIMIT=" + std::string(addon::limits::k_max_windows_environment_value_units + 1, 'x')};
        std::string error;
        ok &= check(!starts(std::move(request), &error) &&
                error.find("environment value") != std::string::npos,
            id,
            "simulator rejects one UTF-16 environment-value unit above the native boundary");
    }
    return ok;
#endif
}

} // namespace sdk_tests
