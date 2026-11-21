#include "logonomic_cli_addon/launcher_utilities.h"
#include "logonomic_cli_addon/host_simulator.h"
#include "logonomic_cli_addon/package_source.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef LOGONOMIC_TEST_WRAP_PROBE_IO
namespace {

// Observe the native calls rather than making CPU timing a test oracle.
class Probe_io_observation
{
public:
    Probe_io_observation() { s_active = this; }
    ~Probe_io_observation() { s_active = nullptr; }

    bool saw_eof(int descriptor) const
    {
        return descriptor >= 0 &&
            std::find(m_eof.begin(), m_eof.end(), descriptor) != m_eof.end();
    }

    inline static thread_local Probe_io_observation* s_active = nullptr;
    std::array<int, 2> m_eof{-1, -1};

    bool m_read_after_eof = false;
    bool m_poll_after_eof = false;
    int m_read_error     = 0;
    bool m_interrupt_poll = false;
};

} // namespace

extern "C" ssize_t __real_read(int, void*, size_t);
extern "C" int __real_poll(pollfd*, nfds_t, int);

extern "C" ssize_t __wrap_read(int descriptor, void* buffer, size_t size)
{
    auto* observation = Probe_io_observation::s_active;
    if (observation) {
        observation->m_read_after_eof |= observation->saw_eof(descriptor);
        if (observation->m_read_error != 0) {
            errno = std::exchange(observation->m_read_error, 0);
            return -1;
        }
    }
    const auto count = __real_read(descriptor, buffer, size);
    const int error = errno;
    if (observation && count == 0 && !observation->saw_eof(descriptor)) {
        const auto empty = std::find(observation->m_eof.begin(), observation->m_eof.end(), -1);
        if (empty != observation->m_eof.end()) {
            *empty = descriptor;
        }
    }
    errno = error;
    return count;
}

extern "C" int __wrap_poll(pollfd* descriptors, nfds_t count, int timeout)
{
    if (auto* observation = Probe_io_observation::s_active) {
        for (nfds_t index = 0; index < count; ++index) {
            observation->m_poll_after_eof |= observation->saw_eof(descriptors[index].fd);
        }
        if (std::exchange(observation->m_interrupt_poll, false)) {
            errno = EINTR;
            return -1;
        }
    }
    return __real_poll(descriptors, count, timeout);
}
#endif

namespace {
namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class Scratch
{
public:
    Scratch()
    :
        m_path(fs::temp_directory_path() / addon::path_from_utf8("launcher ü " + addon::random_nonce_hex()))
    {
        fs::create_directories(m_path);
        m_path = fs::canonical(m_path);
    }
    ~Scratch() { std::error_code error; fs::remove_all(m_path, error); }
    const fs::path& path() const { return m_path; }
private:
    fs::path m_path;
};

class Context final : public addon::Invocation_context
{
public:
    const addon::Invoke_request& request() const override { return m_request; }
    bool cancel_requested() const override
    {
        return m_cancelled || (!m_marker.empty() && fs::exists(m_marker) && m_marker_checks-- <= 0);
    }
    bool wait_for_cancel(int milliseconds) override
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
        return cancel_requested();
    }
    void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override {}
    void succeed(addon::Json_value) override {}
    void fail(addon::Result_status, std::string, std::string) override {}
    bool finished() const override { return false; }

    bool m_cancelled = false;
    fs::path m_marker;
    mutable int m_marker_checks = 0;
private:
    addon::Invoke_request m_request;
};

int child(const std::vector<std::string>& args)
{
    const auto& mode = args.at(2);
    if (mode == "echo") {
#ifdef _WIN32
        // A Windows option parser can inspect the raw command line before
        // the CRT's argv decoding. Simple switches must keep their spelling.
        if (std::wstring_view(GetCommandLineW()).find(L" --child echo ") == std::wstring_view::npos) return 1;
#endif
        for (std::size_t i = 3; i < args.size(); ++i) {
            std::cout << args[i].size() << ':' << args[i];
        }
        std::cerr << "stderr-canary";
        return std::getchar() == EOF ? 0 : 9;
    }
    if (mode == "environment") {
        const auto environment = addon::current_process_environment();
        const auto find = [&](const char* name) { return addon::environment_value(environment, name); };
        return find("LAUNCHER_TEST_VALUE") == "frozen ü" && find("LAUNCHER_AMBIENT_CANARY").empty() &&
            fs::equivalent(fs::current_path(), addon::path_from_utf8(args.at(3))) ? 0 : 8;
    }
    if (mode == "output") {
        const std::string bytes(std::stoul(args.at(3)), 'x');
        (args.at(4) == "stdout" ? std::cout : std::cerr) << bytes;
        return 0;
    }
    if (mode == "failed") {
        return 7;
    }
    if (mode == "final-output") {
#ifdef _WIN32
        std::cout << GetCurrentProcessId();
#else
        std::cout << getpid();
#endif
        std::cout << '|' << std::flush;
        const auto release = addon::path_from_utf8(args.at(3));
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        while (!fs::exists(release)) {
            if (Clock::now() >= deadline) return 9;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::cout << "result";
        return std::stoi(args.at(4));
    }
    if (mode == "sleep") {
        const auto marker_path = addon::path_from_utf8(args.at(3));
        auto pending_path = marker_path;
        pending_path += ".pending";
        std::ofstream marker(pending_path);
#ifdef _WIN32
        marker << GetCurrentProcessId();
#else
        marker << getpid();
#endif
        marker.close();
        // Cancellation observes only a completely published PID, never an empty file.
        fs::rename(pending_path, marker_path);
        std::this_thread::sleep_for(std::chrono::seconds(20));
        return 0;
    }
#ifdef _WIN32
    if (mode == "descendant") {
        const auto program = addon::wide(args.at(0));
        auto command = addon::quote_argument(program) + L" --child sleep " +
            addon::quote_argument(addon::wide(args.at(3)));
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        require(CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, FALSE, 0,
            nullptr, nullptr, &startup, &process) != FALSE, "spawn job descendant");
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        while (!fs::exists(addon::path_from_utf8(args.at(3))) && Clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return fs::exists(addon::path_from_utf8(args.at(3))) ? 0 : 5;
    }
    if (mode == "handle") {
        SetEvent(reinterpret_cast<HANDLE>(std::stoull(args.at(3))));
        return 0;
    }
#else
    if (mode == "closed-output") {
        const auto& streams = args.at(3);
        const auto marker_path = addon::path_from_utf8(args.at(4));
        const auto& action = args.at(5);
        std::cout << std::string(8192, 'o') << std::flush;
        std::cerr << std::string(8192, 'e') << std::flush;
        if (streams != "stderr") { close(STDOUT_FILENO); }
        if (streams != "stdout") { close(STDERR_FILENO); }
        auto pending_path = marker_path;
        pending_path += ".pending";
        std::ofstream(pending_path) << getpid();
        fs::rename(pending_path, marker_path);
        if (action == "exchange") {
            auto read_marker = marker_path;
            read_marker += ".read";
            while (!fs::exists(read_marker)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            std::string input(256 * 1024, '\0');
            std::cin.read(input.data(), (std::streamsize)input.size());
            if (!std::cin || input != std::string(input.size(), 'i')) {
                return 8;
            }
            auto receipt = marker_path;
            receipt += ".received";
            std::ofstream(receipt) << input.size();
        }
        if (action == "finish" || action == "failed") {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (streams == "stderr") { std::cout << "remaining-stdout" << std::flush; }
            if (streams == "stdout") { std::cerr << "remaining-stderr" << std::flush; }
            auto finished_path = marker_path;
            finished_path += ".finished";
            std::ofstream(finished_path) << "finished";
            return action == "failed" ? 7 : 0;
        }
        std::this_thread::sleep_for(std::chrono::seconds(20));
        return 0;
    }
    if (mode == "group") {
        std::cout << getpgrp();
        return 0;
    }
#endif
    return 6;
}

void resume_directories(const fs::path& root)
{
    const auto uncreated = root / "uncreated-link" / "session";
    const auto lexical = addon::normalized_resume_directory(
        addon::path_to_utf8(root / "uncreated-link" / "child" / ".." / "session" / "."));
    require(lexical && *lexical == uncreated, "resume folders normalize without requiring a filesystem target");
    require(!addon::normalized_resume_directory("relative/folder") && !addon::normalized_resume_directory(""),
        "resume folders must be absolute");
    require(!addon::normalized_resume_directory(addon::path_to_utf8(root) + std::string("\0child", 6)) &&
        !addon::normalized_resume_directory(addon::path_to_utf8(root) + "\xFF"),
        "resume folders are NUL-free UTF-8");
#ifdef _WIN32
    const std::vector<std::pair<std::string, std::string>> spellings{
        {R"(C:\work\.\project\..\session\\)", R"(C:\work\session)"},
        {"C:/work//session/", R"(C:\work\session)"},
        {"C:/work/../../", "C:\\"},
        {"C:/", "C:\\"},
        {"C:/A Space/会話/child/../", "C:\\A Space\\会話"},
        {R"(S:\alias\child\..\session)", R"(S:\alias\session)"},
    };
    require(!addon::normalized_resume_directory(R"(C:drive-relative)") &&
        !addon::normalized_resume_directory(R"(\root-relative)"), "Windows resume folders need an absolute root");
#else
    const std::vector<std::pair<std::string, std::string>> spellings{
        {"/work/./project/../session//", "/work/session"},
        {"/work/../../", "/"},
        {"/", "/"},
        {"/A Space/会話/child/../", "/A Space/会話"},
        {R"(/work/back\slash/child/..)", R"(/work/back\slash)"},
    };
#endif
    for (const auto& [spelling, expected] : spellings) {
        const auto normalized = addon::normalized_resume_directory(spelling);
        require(normalized && addon::path_to_utf8(*normalized) == expected,
            "native resume folder spelling normalizes lexical segments");
    }
}

void primitives(const fs::path& executable, const fs::path& root)
{
    resume_directories(root);
    require(addon::trim("\xc2\xa0 text \xe3\x80\x80") == "text", "UTF-8 whitespace");
    require(addon::trim(" \ttext\r\n", addon::Launcher_whitespace::ASCII) == "text",
        "ASCII launcher whitespace");
    require(addon::trim("\xc2\xa0 text\v", addon::Launcher_whitespace::ASCII) == "\xc2\xa0 text\v",
        "ASCII launcher grammar retains other whitespace");
    require(addon::lower("CLI-Ä") == "cli-Ä", "ASCII case folding");
    require(addon::unquote(" 'a b' ") == "a b", "matching outer quotes");
    require(addon::unquote("\xc2\xa0'a b'\xc2\xa0") == "a b" &&
        addon::unquote("\xc2\xa0'a b'\xc2\xa0", addon::Launcher_whitespace::ASCII) == "\xc2\xa0'a b'\xc2\xa0",
        "unquote preserves the selected whitespace grammar");
    require(addon::base_name("\"C:\\tools\\KIMI.EXE\"") == "kimi", "launcher basename");
    require(addon::base_name("\xc2\xa0'KIMI.EXE'\xc2\xa0") == "kimi" &&
        addon::base_name("\xc2\xa0'KIMI.EXE'\xc2\xa0", addon::Launcher_whitespace::ASCII) == "\xc2\xa0'kimi.exe'\xc2\xa0",
        "basename preserves the selected whitespace grammar");
    for (const auto whitespace : {addon::Launcher_whitespace::ASCII, addon::Launcher_whitespace::UTF8}) {
        const auto assignment = addon::environment_assignment("\" _NAME = 'a=b' \"", whitespace);
        require(assignment && *assignment == std::pair<std::string, std::string>{"_NAME", "a=b"},
            "assignment retains equals inside a quoted value");
        const auto empty = addon::environment_assignment("NAME=", whitespace);
        require(empty && empty->second.empty(), "assignment preserves empty values");
        for (const std::string_view invalid : {"", "NAME", "=value", "1NAME=value", "N-AME=value", "N AME=value"}) {
            require(!addon::environment_assignment(invalid, whitespace), "invalid assignment name or separator");
        }
        require(!addon::environment_assignment(std::string_view("NAME=a\0b", 8), whitespace),
            "assignment rejects an embedded NUL");
    }
    require(addon::environment_assignment("\xc2\xa0NAME=value", addon::Launcher_whitespace::UTF8).has_value() &&
        !addon::environment_assignment("\xc2\xa0NAME=value", addon::Launcher_whitespace::ASCII),
        "assignment preserves provider whitespace selection");
    require(addon::launcher_tokens(R"cmd(tool "a \"b" 'c d')cmd", "bash") ==
        std::vector<std::string>{"tool", R"cmd("a \"b")cmd", "'c d'"},
        "inspection tokens retain quote spelling");
    require(addon::launcher_tokens("\"a b", "bash") == std::vector<std::string>{"\"a", "b"},
        "inspection tokens retain the unmatched-quote fallback");
    require(addon::launcher_segments(R"cmd($env:CODEX_HOME = "D:\profiles\work\"; codex)cmd",
        "powershell", addon::Launcher_whitespace::ASCII) ==
        std::vector<std::string>{R"cmd($env:CODEX_HOME = "D:\profiles\work\")cmd", "codex"},
        "PowerShell quoted Windows path does not absorb the following command");
    require(addon::launcher_segments(R"cmd( ; one "a;\"b"| two 'c&d' && three\;four ;)cmd",
        "bash", addon::Launcher_whitespace::ASCII) ==
        std::vector<std::string>{R"cmd(one "a;\"b")cmd", "two 'c&d'", "three\\;four"},
        "segments split only outside quotes and omit empty parts");
    require(addon::launcher_segments("one 'unfinished;two", "bash", addon::Launcher_whitespace::ASCII) ==
        std::vector<std::string>{"one 'unfinished;two"}, "segments retain an unmatched quoted tail");
    require(addon::launcher_segments("\xc2\xa0one\xc2\xa0;two", "bash", addon::Launcher_whitespace::UTF8) ==
        std::vector<std::string>{"one", "two"} &&
        addon::launcher_segments("\xc2\xa0one\xc2\xa0;two", "bash", addon::Launcher_whitespace::ASCII) ==
        std::vector<std::string>{"\xc2\xa0one\xc2\xa0", "two"}, "segments preserve provider whitespace selection");
    for (const auto* runner : {"cmd", "powershell"}) {
        require(addon::launcher_tokens(R"cmd(tool "D:\profiles\work\" next)cmd", runner) ==
            std::vector<std::string>{"tool", R"cmd("D:\profiles\work\")cmd", "next"},
            "Windows shell backslashes do not escape closing quotes");
        require(addon::launcher_segments(R"cmd(tool D:\profiles\work\)cmd", runner,
            addon::Launcher_whitespace::ASCII) == std::vector<std::string>{R"cmd(tool D:\profiles\work\)cmd"},
            "Windows shell inspection retains an unquoted trailing backslash");
    }
    require(addon::launcher_segments(R"cmd(tool D:\O'Brien\;notes & next)cmd", "cmd",
        addon::Launcher_whitespace::ASCII) == std::vector<std::string>{R"cmd(tool D:\O'Brien\;notes)cmd", "next"},
        "cmd treats apostrophes and semicolons as path characters");
    require(addon::launcher_segments("tool one^&two & next", "cmd", addon::Launcher_whitespace::ASCII) ==
        std::vector<std::string>{"tool one^&two", "next"}, "cmd caret protects a command separator");
    require(addon::launcher_segments(R"cmd(tool "a`";b"; next)cmd", "powershell",
        addon::Launcher_whitespace::ASCII) == std::vector<std::string>{R"cmd(tool "a`";b")cmd", "next"},
        "PowerShell backtick protects a quote before a literal separator");
    const std::vector<std::pair<std::string, std::vector<std::string>>> literal_cases{
        {" \ttool  arg\n", {"tool", "arg"}},
        {R"cmd(tool "two words" "")cmd", {"tool", "two words"}},
        {R"cmd(tool """quoted""")cmd", {"tool", "\"quoted\""}},
        {R"cmd(tool 'two words')cmd", {"tool", "'two", "words'"}},
        {R"cmd(tool one\"two)cmd", {"tool", "one\\two"}},
        {R"cmd(tool "unfinished value)cmd", {"tool", "unfinished value"}},
        {"tool tail\"", {"tool", "tail"}},
        {"\"\"", {}},
    };
    for (const auto& [script, expected] : literal_cases) {
        require(addon::literal_argv(script) == expected, "literal launcher argument grammar");
    }
    for (const std::string_view version : {"0.0.0", "1.2.3-rc.1+build.7", "1.2.3-01a", "1.2.3+007"}) {
        require(addon::is_semantic_version(version), "semantic version grammar");
    }
    for (const std::string_view version : {"v1.2.3", "1.02.3", "1.2", "1.2.3-01", "1.2.3+", "1.2.3 "}) {
        require(!addon::is_semantic_version(version), "malformed semantic version");
    }
    constexpr std::string_view labels[] = {"Tool CLI", "tool"};
    const std::vector<std::pair<std::string, std::optional<std::string>>> version_cases{
        {"tool 1.2.3",                            "1.2.3"},
        {"TOOL CLI VERSION: V1.2.3-rc.1+build.7", "1.2.3-rc.1+build.7"},
        {"tool=1.2.3 (native build)",             "1.2.3"},
        {"v1.2.3\r\n",                            "1.2.3"},
        {"\r\n V1.2.3 \r",                        "1.2.3"},
        {"notice\rtool 1.2.3\r1.9.0",             "1.2.3"},
        {"tool 1.2.3\nTool CLI 1.2.3",            "1.2.3"},
        {"tool 1.2.3\ntool 1.2.4",                std::nullopt},
        {"tool 1.2.3\ntool 1.2.3+build.7",        std::nullopt},
        {"",                                      ""},
        {"notice",                                ""},
        {"notice\n1.2.3",                         ""},
        {"1.2.3\n1.2.3",                          ""},
        {"toolbox 1.2.3",                         ""},
        {"tool v1.02.3",                          ""},
        {"tool 1.2.3 ()",                         ""},
        {"tool 1.2.3 (nested (build))",           ""},

        {"tool 1.2.3 (" + std::string(128, 'x') + ")", "1.2.3"},
        {"tool 1.2.3 (" + std::string(129, 'x') + ")", ""},

        {std::string(65536, 'x'),         ""},
        {std::string(65537, 'x'),         std::nullopt},
        {std::string("tool 1.2.3\0", 11), std::nullopt},
        {std::string("\xff", 1),          std::nullopt},
    };
    for (const auto whitespace : {addon::Launcher_whitespace::ASCII, addon::Launcher_whitespace::UTF8}) {
        for (const auto& [output, expected] : version_cases) {
            require(addon::parse_version_stream(output, labels, whitespace) == expected,
                "version stream recognition and rejection");
        }
    }
    for (const std::string_view output : {"\xc2\xa0tool 1.2.3\xe3\x80\x80", "\xc2\xa0v1.2.3\xc2\xa0"}) {
        require(addon::parse_version_stream(output, labels) == "1.2.3" &&
            addon::parse_version_stream(output, labels, addon::Launcher_whitespace::ASCII) == "",
            "version streams preserve provider whitespace selection");
    }
    addon::Environment environment{{"TOKEN", "first"}, {"TOKEN", "second"}, {"token", "lower"}};
    addon::set_environment(environment, "TOKEN", "");
#ifdef _WIN32
    require(environment.size() == 1 && addon::environment_value(environment, "token").empty(),
        "Windows environment replacement is case insensitive and preserves empty values");
#else
    require(environment.size() == 2 && addon::environment_value(environment, "token") == "lower",
        "POSIX environment replacement is case sensitive");
#endif
    auto object = addon::Json_value::object();
    object.set("NAME", addon::Json_value("value"));
    require(addon::environment_pairs(object) == addon::Environment{{"NAME", "value"}}, "JSON environment pairs");
    require(addon::native_image(executable), "running fixture is a native image");
    require(addon::preferred_discovery_runner(executable, "powershell") == "direct",
        "a selected native executable prefers Direct over a shell");
    const auto wrapper = root / "selected-wrapper.cmd";
    std::ofstream(wrapper) << "@echo off\n";
    require(addon::preferred_discovery_runner(wrapper, "cmd") == "cmd",
        "a selected script wrapper retains its required shell");
    require(!addon::regular_file(root), "directory is not a regular file");
    require(!addon::regular_file(root / "missing"), "missing file is unavailable");

    fs::create_directory(root / "bin");
#ifdef _WIN32
    const std::string name = "tool.exe";
    constexpr char separator = ';';
#else
    const std::string name = "tool";
#endif
    const auto program = root / "bin" / name;
    fs::copy_file(executable, program);
    const auto cwd = addon::path_to_utf8(root);
    environment = {{"PATH", "bin"}};
    require(!addon::resolve_program(name, environment, {}), "relative PATH requires explicit cwd");
    require(addon::resolve_program(name, environment, cwd) == program, "relative PATH uses supplied cwd");
    require(addon::resolve_program("bin/" + name, {}, cwd) == program, "relative executable uses cwd");
    require(addon::resolve_program(addon::path_to_utf8(program), {}, {}) == program, "absolute executable");
#ifdef _WIN32
    addon::set_environment(environment, "PATH", addon::path_to_utf8(root / "missing") + separator +
        "\"" + addon::path_to_utf8(root / "bin") + "\"");
    require(addon::resolve_program(name, environment, {}) == program, "quoted PATH and missing entry");
    addon::set_environment(environment, "PATH", "\xc2\xa0" + addon::path_to_utf8(root / "bin") + "\xc2\xa0");
    require(addon::resolve_program(name, environment, {}) == program, "UTF-8 PATH trimming");
    require(!addon::resolve_program(name, environment, {}, addon::Launcher_whitespace::ASCII),
        "ASCII PATH grammar retains non-ASCII whitespace");
#else
    fs::create_directory(root / "tools");
    fs::copy_file(executable, root / "tools" / name);
    for (const std::string directory_name : {" tools", "tools ", "'tools'", "\"tools\"", "\xc2\xa0tools\xc2\xa0"}) {
        const auto directory = root / addon::path_from_utf8(directory_name);
        fs::create_directory(directory);
        const auto expected = directory / name;
        fs::copy_file(executable, expected);
        addon::set_environment(environment, "PATH", directory_name);
        require(addon::resolve_program(name, environment, cwd) == expected,
            "POSIX relative PATH preserves spaces and literal quotes");
        require(addon::resolve_program(name, environment, cwd, addon::Launcher_whitespace::ASCII) == expected,
            "provider token whitespace policy does not change POSIX PATH");
        addon::set_environment(environment, "PATH", addon::path_to_utf8(directory));
        require(addon::resolve_program(name, environment, {}) == expected,
            "POSIX absolute PATH preserves filename characters");
    }
    const auto cwd_program = root / name;
    fs::copy_file(executable, cwd_program);
    for (const std::string search_path : {"", ":bin", "missing::bin", "missing:"}) {
        addon::set_environment(environment, "PATH", search_path);
        require(addon::resolve_program(name, environment, cwd) == cwd_program,
            "empty POSIX PATH component uses explicit cwd at its declared position");
    }
    addon::set_environment(environment, "PATH", "bin:");
    require(addon::resolve_program(name, environment, cwd) == program,
        "earlier POSIX PATH component precedes trailing cwd component");
    addon::set_environment(environment, "PATH", "");
    const auto prior_directory = fs::current_path();
    fs::current_path(root);
    const bool used_ambient_directory = addon::resolve_program(name, environment, {}).has_value();
    fs::current_path(prior_directory);
    require(!used_ambient_directory, "empty POSIX PATH component requires explicit cwd");
    require(!addon::resolve_program(name, {}, cwd), "missing PATH does not imply a cwd component");
#endif
    addon::set_environment(environment, "PATH", addon::path_to_utf8(root / "bin"));
#ifdef _WIN32
    require(addon::resolve_program("tool", environment, {}) == program, "absent PATHEXT uses defaults");
    addon::set_environment(environment, "PATHEXT", "");
    require(!addon::resolve_program("tool", environment, {}), "empty PATHEXT disables suffix search");
    fs::copy_file(executable, root / "bin" / "tool.cmd");
    addon::set_environment(environment, "PATHEXT", "CMD;EXE");
    require(addon::resolve_program("tool", environment, {}) == root / "bin" / "tool.cmd",
        "PATHEXT ordering and omitted dot");
#else
    fs::permissions(program, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
    require(!addon::resolve_program(name, environment, {}), "POSIX discovery requires executable permission");
#endif
}

void final_probe_output(const fs::path& executable, const fs::path& root)
{
    enum class Reply { OBSERVE, REJECT, COMPLETE, REQUEST_MORE, CANCEL, LATE_COMPLETION };
    struct Case {
        int   exit_code;
        Reply reply;
        bool  success;
    };
    constexpr Case cases[] = {
        {0, Reply::OBSERVE,         true},
        {0, Reply::REJECT,          false},
        {7, Reply::OBSERVE,         false},
        {7, Reply::COMPLETE,        true},
        {0, Reply::REQUEST_MORE,    false},
        {0, Reply::CANCEL,          false},
        {0, Reply::LATE_COMPLETION, false},
    };
    const auto release = root / "release-probe";
    for (const auto& test : cases) {
        const int budget_ms = test.reply == Reply::LATE_COMPLETION ? 5000 : 10000;
        const auto after_deadline = Clock::now() + std::chrono::milliseconds(budget_ms + 500);
        Context context;
        addon::Probe_exchange exchange;
        int received = 0;
        exchange.on_output = [&](std::string_view bytes) -> std::optional<std::string> {
            if (bytes.ends_with("|")) {
                // The child writes its final record and exits between the probe's
                // first drain and its process-status check. Leave reaping to the probe.
                const auto pid = std::stoul(std::string(bytes));
#ifdef _WIN32
                HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
                if (!process) return {};
                std::ofstream(release).close();
                const auto wait = WaitForSingleObject(process, 5000);
                CloseHandle(process);
                if (wait != WAIT_OBJECT_0) return {};
#else
                std::ofstream(release).close();
                siginfo_t status{};
                int wait;
                do {
                    wait = waitid(P_PID, static_cast<id_t>(pid), &status, WEXITED | WNOWAIT);
                } while (wait < 0 && errno == EINTR);
                if (wait < 0) return {};
#endif
            }
            if (bytes.ends_with("|result")) {
                ++received;
                if (test.reply == Reply::REJECT) return {};
                if (test.reply == Reply::REQUEST_MORE) return "next request";
                context.m_cancelled = test.reply == Reply::CANCEL;
                if (test.reply == Reply::LATE_COMPLETION) std::this_thread::sleep_until(after_deadline);
                exchange.complete = test.reply == Reply::COMPLETE || test.reply == Reply::LATE_COMPLETION;
            }
            return std::string{};
        };
        const std::vector<std::string> command{addon::path_to_utf8(executable), "--child",
            "final-output", addon::path_to_utf8(release), std::to_string(test.exit_code)};
        bool timed_out = false;
        const auto output = addon::probe_with_timeout_status(
            command, {}, addon::path_to_utf8(root), budget_ms, context, timed_out, &exchange);
        require(received == 1 && output.has_value() == test.success,
            "the exchange must receive and validate the child's final stdout exactly once");
        require(timed_out == (test.reply == Reply::LATE_COMPLETION),
            "only exhaustion of the final exchange deadline reports a probe timeout");
        fs::remove(release);
    }
}

void probes(const fs::path& executable, const fs::path& root)
{
    const auto program = addon::path_to_utf8(executable);
    const auto cwd = addon::path_to_utf8(root);
    Context context;
    addon::Environment environment{{"LAUNCHER_TEST_VALUE", "frozen ü"}};
    // Preserve Windows loader configuration, not the adapter's ambient variables.
#ifdef _WIN32
    if (const auto* system_root = std::getenv("SystemRoot")) {
        environment.emplace_back("SystemRoot", system_root);
    }
    SetEnvironmentVariableW(L"LAUNCHER_AMBIENT_CANARY", L"must-not-leak");
#else
    setenv("LAUNCHER_AMBIENT_CANARY", "must-not-leak", 1);
#endif
    bool timed_out = false;
    const auto run = [&](std::vector<std::string> args, int timeout = 5000) {
        args.insert(args.begin(), {program, "--child"});
        return addon::probe_with_timeout_status(args, environment, cwd, timeout, context, timed_out);
    };
    const std::vector<std::string> values{"", "a b", "ü中", "a\"b", "a\\", "a\\\"b", "&|$()"};
    std::vector<std::string> args{"echo"};
    std::string expected;
    for (const auto& value : values) {
        args.push_back(value);
        expected += std::to_string(value.size()) + ':' + value;
    }
    const auto echoed = run(args);
    require(echoed && echoed->output == expected && echoed->errors == "stderr-canary",
        "native argument round-trip, separate streams and EOF stdin");
    require(run({"environment", cwd}).has_value(), "frozen environment and cwd reach child");
    require(!run({"failed"}) && !timed_out, "nonzero child exit is unavailable");
    require(!addon::probe_with_timeout_status({cwd + "/missing"}, environment, cwd, 5000, context, timed_out) &&
        !timed_out, "spawn failure is distinct from deadline expiry");
    for (const auto* stream : {"stdout", "stderr"}) {
        const auto output = run({"output", "65536", stream});
        require(output && output->output.size() + output->errors.size() == 65536, "exact stream limit accepted");
        require(!run({"output", "65537", stream}), "stream overflow rejected");
    }
    const auto marker = root / "started";
    context.m_cancelled = true;
    require(!run({"sleep", addon::path_to_utf8(marker)}) && !fs::exists(marker), "pre-cancel spawns nothing");
    context.m_cancelled = false;
    for (const bool cancel : {true, false}) {
        context.m_marker = cancel ? marker : fs::path{};
        const auto started = Clock::now();
        require(!run({"sleep", addon::path_to_utf8(marker)}, cancel ? 5000 : 3000), "cancel/timeout rejected");
        require(timed_out == !cancel, "only expiry of the probe deadline reports timeout");
        require(Clock::now() - started < std::chrono::seconds(10), "bounded probe returns promptly");
        unsigned long process = 0;
        std::ifstream(marker) >> process;
        require(process != 0, "child started before cancellation/timeout");
#ifdef _WIN32
        HANDLE handle = OpenProcess(SYNCHRONIZE, FALSE, process);
        const bool exited = !handle || WaitForSingleObject(handle, 1000) == WAIT_OBJECT_0;
        if (handle) {
            CloseHandle(handle);
        }
        require(exited, "probe leader terminated");
#else
        int status = 0;
        require(waitpid((pid_t)process, &status, WNOHANG) == -1 && errno == ECHILD, "probe leader reaped");
#endif
        fs::remove(marker);
    }
    context.m_marker.clear();
#ifdef _WIN32
    const auto descendant = run({"descendant", addon::path_to_utf8(marker)});
    unsigned long child_process = 0;
    std::ifstream(marker) >> child_process;
    require(descendant && child_process != 0, "descendant started before probe leader exits");
    HANDLE child_handle = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, child_process);
    const bool retired = !child_handle || WaitForSingleObject(child_handle, 1000) == WAIT_OBJECT_0;
    if (child_handle) {
        if (!retired) {
            TerminateProcess(child_handle, 1);
            WaitForSingleObject(child_handle, 5000);
        }
        CloseHandle(child_handle);
    }
    require(retired, "probe job retires surviving descendant");
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE event = CreateEventW(&security, TRUE, FALSE, nullptr);
    require(event != nullptr, "create inheritable canary");
    const auto result = run({"handle", std::to_string(reinterpret_cast<std::uintptr_t>(event))});
    const bool excluded = WaitForSingleObject(event, 0) == WAIT_TIMEOUT;
    CloseHandle(event);
    require(result && excluded, "unrelated inheritable handle excluded");
#else
    const auto group = run({"group"});
    require(group && group->output == std::to_string(getpgrp()), "probe retains custodian process group");
#endif
}

#ifndef _WIN32
void closed_output_probes(const fs::path& executable, const fs::path& root)
{
    const auto program = addon::path_to_utf8(executable);
    const auto cwd = addon::path_to_utf8(root);
    Context context;
    const auto run = [
            &program,
            &cwd,
            &context
        ](
            std::vector<std::string> args,
            int timeout = 5000,
            addon::Probe_exchange* exchange = nullptr,
            int read_error = 0,
            bool interrupt_poll = false)
        {
#ifdef LOGONOMIC_TEST_WRAP_PROBE_IO
            Probe_io_observation observation;
            observation.m_read_error     = read_error;
            observation.m_interrupt_poll = interrupt_poll;
#else
            (void)read_error;
            (void)interrupt_poll;
#endif
            args.insert(args.begin(), {program, "--child", "closed-output"});
            const auto result = addon::probe(args, {}, cwd, timeout, context, exchange);
#ifdef LOGONOMIC_TEST_WRAP_PROBE_IO
            require(!observation.m_poll_after_eof, "EOF descriptor must leave subsequent poll sets");
            require(!observation.m_read_after_eof, "EOF descriptor must not be drained again");
            require(observation.m_read_error == 0 && !observation.m_interrupt_poll,
                "requested I/O interruption must be exercised");
#endif
            return result;
        };
    const auto reaped = [](
            const fs::path& marker)
        {
            pid_t process = 0;
            std::ifstream(marker) >> process;
            require(process > 0, "closed-output child published its PID");
            int status = 0;
            require(waitpid(process, &status, WNOHANG) == -1 && errno == ECHILD,
                "closed-output probe leader reaped");
        };
    for (const std::string streams : {"stdout", "stderr", "both"}) {
        const auto marker = root / ("closed-" + streams);
        const auto marker_text = addon::path_to_utf8(marker);
        const auto output = run({streams, marker_text, "finish"});
        const auto expected_output = std::string(8192, 'o') + (streams == "stderr" ? "remaining-stdout" : "");
        const auto expected_errors = std::string(8192, 'e') + (streams == "stdout" ? "remaining-stderr" : "");
        require(output && output->output == expected_output && output->errors == expected_errors,
            "closed stream keeps buffered tail and the other stream keeps draining");
        require(fs::exists(root / ("closed-" + streams + ".finished")),
            "output EOF must not substitute for child completion");
        reaped(marker);
        fs::remove(marker);
        for (const bool cancel : {true, false}) {
            context.m_marker = cancel ? marker : fs::path{};
            // Admit several complete loop iterations after the child closes its streams.
            context.m_marker_checks = 8;
            require(!run({streams, marker_text, "wait"}, cancel ? 5000 : 3000),
                "cancellation and deadline still stop a live child after output EOF");
            reaped(marker);
            fs::remove(marker);
        }
        context.m_marker.clear();
    }
    require(!run({"both", addon::path_to_utf8(root / "failed-output"), "failed"}),
        "nonzero child exit remains unavailable after output EOF");

#ifdef LOGONOMIC_TEST_WRAP_PROBE_IO
    {
        const auto marker = root / "interrupted-output";
        context.m_marker = marker;
        context.m_marker_checks = 8;
        require(!run({"both", addon::path_to_utf8(marker), "wait"}, 5000, nullptr, EINTR, true),
            "interrupted reads and waits preserve cancellation");
        reaped(marker);
        context.m_marker.clear();
        for (const int read_error : {EINTR, EIO}) {
            const auto output = run({"both", addon::path_to_utf8(marker), "finish"}, 5000, nullptr, read_error);
            require(read_error == EINTR ? output.has_value() : !output,
                "interrupted reads retry while terminal read failures reject the probe");
        }
    }
#endif

    const auto marker = root / "exchange-output";
    auto read_marker = marker;
    auto receipt = marker;
    read_marker += ".read";
    receipt += ".received";
    addon::Probe_exchange exchange;
    exchange.initial_input.assign(256 * 1024, 'i');
    exchange.on_output = [](std::string_view) { return std::optional(std::string{}); };
    int ready_ticks = 0;
    auto completed_at = Clock::time_point{};
    exchange.tick = [
            &marker,
            &read_marker,
            &receipt,
            &ready_ticks,
            &completed_at,
            &exchange
        ]()
        {
            if (fs::exists(marker) && ++ready_ticks == 3) {
                // The child cannot drain stdin until the probe has retried a full pipe after EOF.
                std::ofstream(read_marker) << "read";
            }
            if (!exchange.complete && fs::exists(receipt)) {
                completed_at = Clock::now();
                exchange.complete = true;
            }
        };
    const auto output = run({"both", addon::path_to_utf8(marker), "exchange"}, 5000, &exchange);
    require(output && output->output == std::string(8192, 'o') && output->errors == std::string(8192, 'e'),
        "exchange drains both tails while delivering pending stdin after EOF");
    std::size_t received = 0;
    std::ifstream(receipt) >> received;
    require(received == exchange.initial_input.size(), "exchange receives the complete backpressured input");
    require(exchange.complete && Clock::now() - completed_at >= std::chrono::milliseconds(250),
        "completed exchange retains its settling interval after output EOF");
    reaped(marker);
}
#endif

int exercise(const std::vector<std::string>& args)
{
    try {
        if (args.size() > 1 && args[1] == "--child") {
            return child(args);
        }
        Scratch scratch;
        const auto executable = fs::canonical(addon::path_from_utf8(args.at(0)));
        primitives(executable, scratch.path());
        const auto copy = scratch.path() / addon::path_from_utf8("probe ü" + executable.extension().string());
        fs::copy_file(executable, copy);
        probes(copy, scratch.path());
        final_probe_output(copy, scratch.path());
#ifndef _WIN32
        closed_output_probes(copy, scratch.path());
#endif
        std::cout << "launcher utility contracts passed\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
{
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) {
        args.push_back(addon::path_to_utf8(fs::path(argv[i])));
    }
    return exercise(args);
}
#else
int main(int argc, char** argv)
{
    return exercise(std::vector<std::string>(argv, argv + argc));
}
#endif
