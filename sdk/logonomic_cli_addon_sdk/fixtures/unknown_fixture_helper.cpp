// The packaged helper of the provider-session fixture packages
// (docs/conformance.md, "Fixture roster"): the one executable the packages
// ship under helpers/, Qt-free and built on the SDK alone. It is at once
//
// - the package's hook and terminal helper: `event`, `set-title`,
//   `changed-files` and `show-diff` are Helper_client over the vnmctl
//   executable the bound terminal names in LOGONOMIC_VNMCTL_PATH, so the
//   helper knows nothing of the control protocol, the endpoint or the token;
// - the package's MCP server command: `coordination-mcp` and `delegation-mcp`
//   start the named server with the inherited bound environment and hand it
//   the helper's own standard streams, which is the byte-exact proxy;
// - the process the fixture adapter's structured and delegated proposals
//   launch: `structured` and `delegated` read the prompt on stdin and write
//   the line protocol the adapter's `inspect` parses.
//
// Exit codes: the wrapper's for the host calls (0 committed, 1 transport
// failure, 2 invalid arguments or environment, 4 pending at the deadline), the
// server's own for the MCP modes, 0 for a completed structured or delegated
// run, and 2 for a usage error.

#include "logonomic_cli_addon/helper_client.h"
#include "logonomic_cli_addon/host_simulator.h"
#include "logonomic_cli_addon/probe_contracts.h"
#include "logonomic_cli_addon/auth_status.h"
#include "logonomic_cli_addon/quota.h"
#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/json.h"
#include "logonomic_cli_addon/limits.h"
#include "logonomic_cli_addon/package_source.h"
#include "logonomic_cli_addon/session_contracts.h"
#include "logonomic_cli_addon/sha256.h"
#include "../src/simulator_process.h"

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#include <algorithm>
#include <cstdio>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace addon = logonomic::cli_addon;
namespace fs    = std::filesystem;

// The line protocol a structured or delegated run writes; the adapter's
// `inspect` parses exactly these lines and ignores every other one.
constexpr const char* k_protocol_header       = "unknown-fixture-helper 1";
constexpr const char* k_protocol_result_begin = "result-begin";
constexpr const char* k_protocol_result_end   = "result-end";
constexpr const char* k_protocol_result_file_prefix = "result-file ";

// Where a delegated run finds the servers the host materialized from the
// adapter's private artifact, and where it writes its own.
constexpr const char* k_servers_artifact = "mcp/servers.json";
constexpr const char* k_summary_artifact = "unknown-fixture-helper/summary.json";

int usage(const char* message)
{
    if (message != nullptr) {
        std::fprintf(stderr, "unknown_fixture_helper: %s\n", message);
    }
    std::fprintf(stderr,
        "usage: unknown_fixture_helper event <name> [--body-file F] [--key K] [--nonce N]\n"
        "       unknown_fixture_helper set-title <title>\n"
        "       unknown_fixture_helper changed-files <paths...>\n"
        "       unknown_fixture_helper show-diff --manifest-file F\n"
        "       unknown_fixture_helper coordination-mcp <server> [arguments...]\n"
        "       unknown_fixture_helper delegation-mcp <server> [arguments...]\n"
        "       unknown_fixture_helper structured --purpose <purpose>\n"
        "       unknown_fixture_helper delegated --purpose <purpose> --artifact-root <dir>\n");
    return addon::k_helper_exit_invalid_arguments;
}

std::string read_stdin()
{
    std::string bytes;
    char        buffer[4096];
    std::size_t count = 0;
    while ((count = std::fread(buffer, 1, sizeof(buffer), stdin)) > 0) {
        bytes.append(buffer, count);
    }
    return bytes;
}

std::optional<std::string> read_file(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return bytes;
}

bool write_file(const fs::path& path, std::string_view bytes)
{
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), (std::streamsize)bytes.size());
    output.close();
    return output.good();
}

bool is_orchestration_result_prompt(std::string_view prompt)
{
    return prompt == "orchestration-large-result" || prompt == "orchestration-invalid-utf8";
}

std::optional<std::string> read_bounded_file(const fs::path& path, std::size_t limit)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    std::string bytes(limit + 1, '\0');
    input.read(bytes.data(), (std::streamsize)bytes.size());
    const auto count = (std::size_t)input.gcount();
    if (input.bad() || count > limit) {
        return std::nullopt;
    }
    bytes.resize(count);
    return bytes;
}

std::optional<std::string> read_tool_request(const fs::path& path)
{
    constexpr std::size_t k_request_bytes = 64u * 1024u;
#if defined(_WIN32)
    std::string bytes(k_request_bytes + 1, '\0');
    // The parent replaces this file atomically. Holding a reader must not
    // refuse the next request publication or the owner's artifact cleanup.
    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    DWORD count = 0;
    const BOOL read = ::ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr);
    ::CloseHandle(file);
    if (!read || count > k_request_bytes) {
        return std::nullopt;
    }
    bytes.resize(count);
    return bytes;
#else
    return read_bounded_file(path, k_request_bytes);
#endif
}

bool publish_tool_control(const fs::path& path, const addon::Json_value& value)
{
    const fs::path temporary = fs::path(path).concat(".tmp");
    if (!write_file(temporary, addon::serialize_json(value))) {
        return false;
    }
#if defined(_WIN32)
    return ::MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
#else
    std::error_code error;
    fs::rename(temporary, path, error);
    return !error;
#endif
}

// Reuse the SDK's bounded pipe pump without creating another Job/process
// group: the shipped relay must remain a descendant in the host's Task custody.
class Fixture_tool_client
{
public:
    explicit Fixture_tool_client(std::unique_ptr<addon::process::Child_process> child)
    :
        m_child(std::move(child))
    {}

    ~Fixture_tool_client()
    {
        trace("destructor-enter");
        if (!m_child->exited()) {
            m_child->kill();
            drain_exit();
        }
        trace("destructor-settled");
    }

    bool send(
        std::string_view bytes,
        std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15))
    {
        if (bytes.size() > 64u * 1024u) {
            return false;
        }
        while (!bytes.empty() && std::chrono::steady_clock::now() < deadline) {
            const auto written = m_child->write_stdin(bytes);
            if (written.status == addon::process::Write_status::TRANSFERRED) {
                bytes.remove_prefix(written.bytes);
            }
            else
            if (written.status != addon::process::Write_status::WOULD_BLOCK) {
                return false;
            }
            if (!pump(!bytes.empty())) {
                return false;
            }
        }
        return bytes.empty();
    }

    std::optional<std::string> exchange(const addon::Json_value& request)
    {
        m_request_id = request.find("id")->as_integer();
        trace("exchange-enter");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        if (!send(addon::serialize_json(request) + "\n", deadline)) {
            trace("send-failed");
            return std::nullopt;
        }
        trace("send-complete");
        while (std::chrono::steady_clock::now() < deadline) {
            const std::size_t newline = m_stdout.find('\n');
            if (newline != std::string::npos) {
                std::string response = m_stdout.substr(0, newline);
                m_stdout.erase(0, newline + 1);
                addon::Json_parse_error error;
                const auto decoded = addon::parse_json(response, addon::Json_parse_limits{}, &error);
                const auto* id = decoded ? decoded->find("id") : nullptr;
                const auto* rpc = decoded ? decoded->find("jsonrpc") : nullptr;
                trace("response-line", response.size(), id && id->is_integer() ? id->as_integer() : -1);
                if (!id || *id != *request.find("id") || !rpc || *rpc != addon::Json_value("2.0") ||
                    (decoded->find("result") == nullptr) == (decoded->find("error") == nullptr))
                {
                    return std::nullopt;
                }
                trace("response-matched");
                return response;
            }
            if (!pump(false)) {
                trace("receive-failed");
                return std::nullopt;
            }
        }
        trace("exchange-expired");
        return std::nullopt;
    }

    void start_trace(fs::path path)
    {
        m_trace_path = std::move(path);
        trace("client-ready");
    }

    void trace(const char* phase, std::size_t bytes = 0, std::int64_t response_id = -1)
    {
        if (m_trace_path.empty() || ++m_trace_count > 512) {
            return;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - m_trace_started).count();
        std::ofstream output(m_trace_path, std::ios::app);
        output << "request=" << m_request_id << " phase=" << phase << " bytes=" << bytes
            << " response_id=" << response_id << " elapsed_ms=" << elapsed
            << " relay_pid=" << m_child->pid() << " relay_exited=" << m_child->exited() << '\n';
    }

    bool finish()
    {
        m_child->close_stdin();
        return drain_exit() && m_exit && !m_exit->signaled && m_exit->code == 0;
    }

private:
    bool pump(bool writing)
    {
        const auto activity = m_child->wait_activity(20, writing);
        if (!activity.stdout_bytes.empty()) { trace("stdout", activity.stdout_bytes.size()); }
        if (!activity.stderr_bytes.empty()) { trace("stderr", activity.stderr_bytes.size()); }
        if (activity.stdout_eof) { trace("stdout-eof"); }
        if (activity.exit) { trace("relay-exit", static_cast<std::size_t>(activity.exit->code)); }
        m_stderr_bytes += activity.stderr_bytes.size();
        if (activity.exit) {
            m_exit = activity.exit;
        }
        if (m_stdout.size() + activity.stdout_bytes.size() > 512u * 1024u || m_stderr_bytes > 16u * 1024u) {
            return false;
        }
        m_stdout += activity.stdout_bytes;
        return !activity.stdout_eof && !m_child->exited();
    }

    bool drain_exit()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!m_child->exited() && std::chrono::steady_clock::now() < deadline) {
            (void)pump(false);
        }
        return m_child->exited();
    }

    std::unique_ptr<addon::process::Child_process> m_child;
    std::optional<addon::process::Exit_status>     m_exit;
    std::string                                  m_stdout;
    std::size_t                                  m_stderr_bytes = 0;
    fs::path                                    m_trace_path;
    std::int64_t                                 m_request_id = 0;
    unsigned                                    m_trace_count = 0;
    std::chrono::steady_clock::time_point         m_trace_started = std::chrono::steady_clock::now();
};

int run_scoped_tools(const fs::path& artifact_root, const std::string& relay_program, const std::string& descriptor_path)
{
    addon::Json_parse_error error;
    if (!addon::path_from_utf8(relay_program).is_absolute() ||
        !addon::path_from_utf8(descriptor_path).is_absolute())
    {
        return 9;
    }
#if !defined(_WIN32)
    // A closed relay pipe is a transport failure, not a signal termination.
    ::signal(SIGPIPE, SIG_IGN);
#endif
    addon::process::Spawn_request spawn;
    spawn.executable = relay_program;
    spawn.argv = {spawn.executable, "--descriptor", descriptor_path, "--server", "coordination"};
    spawn.working_directory = addon::path_to_utf8(artifact_root);
    spawn.isolate = false;
    for (const auto& [name, value] : addon::current_process_environment()) {
        spawn.environment.push_back(name + "=" + value);
    }
    std::string spawn_error;
    auto child = addon::process::Child_process::spawn(spawn, &spawn_error);
    if (!child) {
        return 9;
    }
    Fixture_tool_client client(std::move(child));
    const auto initialize = addon::parse_json(
        R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"unknown-fixture","version":"1.0.0"}}})",
        addon::Json_parse_limits{}, &error);
    const auto initialized = client.exchange(*initialize);
    const auto reply = initialized ? addon::parse_json(*initialized, addon::Json_parse_limits{}, &error) : std::nullopt;
    if (!reply || !reply->find("result") ||
        !client.send("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"))
    {
        return 9;
    }
    const fs::path controls = artifact_root / "unknown-fixture-tools";
    addon::Json_value ready = addon::Json_value::object();
    ready.set("ready", addon::Json_value(true));
    if (!publish_tool_control(controls / "ready.json", ready)) {
        return 9;
    }
    // Readiness is published once. Reuse its declared, now-consumed temporary
    // file for diagnostics without changing the immutable readiness receipt.
    client.start_trace(controls / "ready.json.tmp");
    std::int64_t previous_id = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto bytes = read_tool_request(controls / "request.json");
        if (!bytes) {
            return 9;
        }
        if (bytes->empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        const auto request = addon::parse_json(*bytes, addon::Json_parse_limits{}, &error);
        const auto* id = request ? request->find("request_id") : nullptr;
        const auto* action = request ? request->find("action") : nullptr;
        const auto* arguments = request ? request->find("arguments") : nullptr;
        if (!request || !request->is_object() || request->as_object().size() != 3 || !id || !id->is_integer() ||
            !action || !action->is_string() || !arguments || !arguments->is_object())
        {
            return 9;
        }
        if (id->as_integer() == previous_id) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        if (id->as_integer() != previous_id + 1 || id->as_integer() > 8) {
            return 9;
        }
        if (action->as_string() == "finish") {
            return arguments->as_object().empty() && client.finish() ? 0 : 9;
        }
        if ((action->as_string() != "send" && action->as_string() != "receive") || arguments->find("session_capability")) {
            return 9;
        }
        addon::Json_value params = addon::Json_value::object();
        params.set("name", *action);
        params.set("arguments", *arguments);
        addon::Json_value call = addon::Json_value::object();
        call.set("jsonrpc", addon::Json_value("2.0"));
        call.set("id", *id);
        call.set("method", addon::Json_value("tools/call"));
        call.set("params", std::move(params));
        const auto response = client.exchange(call);
        if (!response) {
            return 9;
        }
        addon::Json_value receipt = addon::Json_value::object();
        receipt.set("request_id", *id);
        receipt.set("response_base64", addon::Json_value(addon::encode_base64(*response)));
        client.trace("receipt-publish-enter");
        if (!publish_tool_control(controls / "response.json", receipt)) {
            client.trace("receipt-publish-failed");
            return 9;
        }
        client.trace("receipt-published");
        previous_id = id->as_integer();
    }
    return 9;
}

std::optional<std::string> write_orchestration_result(std::string_view prompt, const fs::path& directory)
{
    std::string bytes;
    if (prompt == "orchestration-large-result") {
        bytes = std::string(80u * 1024u, 'x') + std::string("\xE2\x82\xAC", 3) + "-complete";
    }
    else
    if (prompt == "orchestration-invalid-utf8") {
        bytes = std::string("\xC3\x28", 2);
    }
    else {
        return std::nullopt;
    }
    const fs::path path = fs::absolute(directory / ".unknown-fixture-result.bin");
    if (!write_file(path, bytes)) {
        return std::nullopt;
    }
    return addon::path_to_utf8(path);
}

/// The JSON object in a file, or nullopt when the file is unreadable or not
/// an object.
std::optional<addon::Json_value> read_json_object(const fs::path& path)
{
    const std::optional<std::string> text = read_file(path);
    if (!text.has_value()) {
        return std::nullopt;
    }
    addon::Json_parse_error error;
    std::optional<addon::Json_value> value = addon::parse_json(*text, addon::Json_parse_limits{}, &error);
    if (!value.has_value() || !value->is_object()) {
        return std::nullopt;
    }
    return value;
}

int write_stdout(std::string_view bytes)
{
    if (std::fwrite(bytes.data(), 1, bytes.size(), stdout) != bytes.size() || std::fflush(stdout) != 0) {
        std::fprintf(stderr, "unknown_fixture_helper: cannot write to stdout\n");
        return addon::k_helper_exit_transport_failure;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// The host calls: Helper_client over the wrapper

std::optional<addon::Helper_client> bound_client()
{
    addon::Contract_failure failure;
    const std::optional<addon::Helper_environment> environment = addon::read_helper_environment(&failure);
    if (!environment.has_value()) {
        std::fprintf(stderr, "unknown_fixture_helper: %s: %s\n", failure.reason_code.c_str(), failure.detail.c_str());
        return std::nullopt;
    }
    return addon::Helper_client(*environment);
}

// The committed outcome in the wrapper's documented shape: {"accepted",
// "reason"?, "epoch"?, "helper_response"?}. A vendor hook runner reads the
// response, a test reads the disposition; both come from this one line.
std::string outcome_text(const addon::Provider_event_outcome& outcome)
{
    addon::Json_value object = addon::Json_value::object();
    object.set("accepted", addon::Json_value(outcome.accepted));
    if (!outcome.accepted && !outcome.reason.empty()) {
        object.set("reason", addon::Json_value(outcome.reason));
    }
    if (outcome.epoch.has_value()) {
        object.set("epoch", addon::Json_value(*outcome.epoch));
    }
    if (outcome.helper_response.has_value()) {
        object.set("helper_response", *outcome.helper_response);
    }
    return addon::serialize_json(object) + "\n";
}

int run_event(const std::vector<std::string>& arguments)
{
    if (arguments.empty()) {
        return usage("event needs the vendor event name");
    }
    const std::string& name = arguments.front();
    addon::Json_value  body = addon::Json_value::object();
    std::string        key;
    std::string        nonce;
    for (std::size_t i = 1; i < arguments.size(); i += 2) {
        if (i + 1 >= arguments.size()) {
            return usage("event options take one value each");
        }
        const std::string& option = arguments[i];
        const std::string& value  = arguments[i + 1];
        if (option == "--body-file") {
            const std::optional<addon::Json_value> object = read_json_object(addon::path_from_utf8(value));
            if (!object.has_value()) {
                return usage("--body-file names no readable JSON object");
            }
            body = *object;
        }
        else
        if (option == "--key") {
            key = value;
        }
        else
        if (option == "--nonce") {
            nonce = value;
        }
        else {
            return usage("unknown event option");
        }
    }

    std::optional<addon::Helper_client> client = bound_client();
    if (!client.has_value()) {
        return addon::k_helper_exit_invalid_arguments;
    }
    const addon::Provider_event_outcome outcome = client->deliver_provider_event(name, body, key, nonce);
    if (!outcome.committed) {
        std::fprintf(stderr, "unknown_fixture_helper: %s not committed (exit %d%s%s)\n",
            name.c_str(), outcome.exit_code, outcome.reason.empty() ? "" : ": ", outcome.reason.c_str());
        return outcome.exit_code;
    }
    const int written = write_stdout(outcome_text(outcome));
    return written != 0 ? written : outcome.exit_code;
}

int run_set_title(const std::vector<std::string>& arguments)
{
    if (arguments.size() != 1) {
        return usage("set-title takes exactly the title");
    }
    std::optional<addon::Helper_client> client = bound_client();
    return client.has_value() ? client->set_title(arguments.front()) : addon::k_helper_exit_invalid_arguments;
}

int run_changed_files(const std::vector<std::string>& arguments)
{
    if (arguments.empty()) {
        return usage("changed-files needs at least one path");
    }
    std::optional<addon::Helper_client> client = bound_client();
    return client.has_value() ? client->show_changed_files(arguments) : addon::k_helper_exit_invalid_arguments;
}

int run_show_diff(const std::vector<std::string>& arguments)
{
    if (arguments.size() != 2 || arguments.front() != "--manifest-file") {
        return usage("show-diff takes --manifest-file F");
    }
    const std::optional<std::string> manifest = read_file(addon::path_from_utf8(arguments.back()));
    if (!manifest.has_value()) {
        return usage("--manifest-file names no readable file");
    }
    std::optional<addon::Helper_client> client = bound_client();
    return client.has_value() ? client->show_diff(*manifest) : addon::k_helper_exit_invalid_arguments;
}

// ---------------------------------------------------------------------------
// The MCP server command: start the server on this process's own streams

#if defined(_WIN32)

std::wstring widen(const std::string& utf8)
{
    const int length = ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    std::wstring wide(length > 0 ? (std::size_t)length : 1, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(), length);
    wide.resize(wide.size() - 1);
    return wide;
}

// The quoting CommandLineToArgvW reverses: a backslash run before a quote
// is doubled and the quote escaped; a run before the closing quote is doubled.
std::wstring quote_argument(const std::wstring& argument)
{
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return argument;
    }
    std::wstring quoted = L"\"";
    std::size_t  backslashes = 0;
    for (const wchar_t c : argument) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
        }
        else {
            quoted.append(backslashes, L'\\');
        }
        backslashes = 0;
        quoted += c;
    }
    quoted.append(backslashes * 2, L'\\');
    quoted += L'"';
    return quoted;
}

int run_mcp_server(const std::vector<std::string>& command)
{
    std::wstring command_line;
    for (const std::string& argument : command) {
        if (!command_line.empty()) {
            command_line += L' ';
        }
        command_line += quote_argument(widen(argument));
    }
    STARTUPINFOW startup{};
    startup.cb         = sizeof(startup);
    startup.dwFlags    = STARTF_USESTDHANDLES;
    startup.hStdInput  = ::GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = ::GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError  = ::GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION information{};
    const BOOL created = ::CreateProcessW(
        nullptr, command_line.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &information);
    if (!created) {
        std::fprintf(stderr, "unknown_fixture_helper: cannot start %s (error %lu)\n",
            command.front().c_str(), (unsigned long)::GetLastError());
        return addon::k_helper_exit_transport_failure;
    }
    ::CloseHandle(information.hThread);
    ::WaitForSingleObject(information.hProcess, INFINITE);
    DWORD exit_code = (DWORD)addon::k_helper_exit_transport_failure;
    ::GetExitCodeProcess(information.hProcess, &exit_code);
    ::CloseHandle(information.hProcess);
    return (int)exit_code;
}

#else

int run_mcp_server(const std::vector<std::string>& command)
{
    std::vector<char*> argv;
    for (const std::string& argument : command) {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    pid_t     pid         = 0;
    const int spawn_error = ::posix_spawnp(&pid, command.front().c_str(), nullptr, nullptr, argv.data(), environ);
    if (spawn_error != 0) {
        std::fprintf(stderr, "unknown_fixture_helper: cannot start %s: %s\n",
            command.front().c_str(), std::strerror(spawn_error));
        return addon::k_helper_exit_transport_failure;
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return addon::k_helper_exit_transport_failure;
        }
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return addon::k_helper_exit_transport_failure;
}

#endif

int run_mcp(const std::vector<std::string>& arguments)
{
    if (arguments.empty()) {
        return usage("the MCP modes need the server program");
    }
    return run_mcp_server(arguments);
}

// Two real children of this native root, not simulated task-server or ACP
// readiness. Normal return cleans up; host authenticated reap must instead
// terminate this whole family while the root and both children are alive.
struct Native_family
{
    std::array<std::uint64_t, 2> ids{};
#if defined(_WIN32)
    std::array<HANDLE, 2> handles{};
#endif

    ~Native_family()
    {
        for (std::size_t index = 0; index < ids.size(); ++index) {
#if defined(_WIN32)
            if (handles[index] != nullptr) {
                ::TerminateProcess(handles[index], 9);
                ::WaitForSingleObject(handles[index], 5000);
                ::CloseHandle(handles[index]);
            }
#else
            if (ids[index] != 0) {
                ::kill(static_cast<pid_t>(ids[index]), SIGKILL);
                while (::waitpid(static_cast<pid_t>(ids[index]), nullptr, 0) < 0 && errno == EINTR) {}
            }
#endif
        }
    }

    bool start(std::size_t index, const std::string& executable, const fs::path& receipt)
    {
#if defined(_WIN32)
        std::wstring command = quote_argument(widen(executable)) + L" runtime-family-child " +
            quote_argument(receipt.wstring());
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
        {
            return false;
        }
        ids[index] = process.dwProcessId;
        handles[index] = process.hProcess;
        ::CloseHandle(process.hThread);
#else
        const std::string path = receipt.string();
        char* const arguments[] = {const_cast<char*>(executable.c_str()),
            const_cast<char*>("runtime-family-child"), const_cast<char*>(path.c_str()), nullptr};
        posix_spawn_file_actions_t actions;
        if (::posix_spawn_file_actions_init(&actions) != 0) {
            return false;
        }
        const bool streams = ::posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0) == 0 &&
            ::posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0) == 0 &&
            ::posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0) == 0;
        pid_t pid = 0;
        const int result = streams ? ::posix_spawn(&pid, executable.c_str(), &actions, nullptr, arguments, environ) : EIO;
        ::posix_spawn_file_actions_destroy(&actions);
        if (result != 0) {
            return false;
        }
        ids[index] = static_cast<std::uint64_t>(pid);
#endif
        return true;
    }
};

std::uint64_t native_process_id()
{
#if defined(_WIN32)
    return ::GetCurrentProcessId();
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

int run_native_family(const std::string& executable)
{
    Native_family children;
    for (std::size_t index = 0; index < children.ids.size(); ++index) {
        const fs::path receipt = fs::current_path() /
            (".runtime-family-child-" + std::to_string(index + 1) + ".ready");
        if (!children.start(index, executable, receipt)) {
            return addon::k_helper_exit_transport_failure;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (read_file(receipt) != std::optional<std::string>(std::to_string(children.ids[index]))) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return addon::k_helper_exit_transport_failure;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    addon::Json_value ids = addon::Json_value::array();
    for (const std::uint64_t id : children.ids) {
        ids.push_back(addon::Json_value(id));
    }
    addon::Json_value ready = addon::Json_value::object();
    ready.set("root_process_id", addon::Json_value(native_process_id()));
    ready.set("child_process_ids", std::move(ids));
    if (!write_file(fs::current_path() / ".runtime-family-ready.json", addon::serialize_json(ready))) {
        return addon::k_helper_exit_transport_failure;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    bool completion_sent = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!completion_sent && fs::exists(fs::current_path() / ".runtime-family-release")) {
            if (write_stdout("C\n") != 0) {
                return addon::k_helper_exit_transport_failure;
            }
            completion_sent = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return 9;
}

// ---------------------------------------------------------------------------
// The structured and delegated process: the prompt in, the line protocol out

struct Run_facts
{
    std::string              mode;
    addon::Structured_purpose purpose = addon::Structured_purpose::READ_ONLY_REVIEW;
    std::string              prompt;
    std::vector<std::string> server_ids;   // delegated: from the servers artifact
    std::string              artifact;     // delegated: the artifact this run wrote
    std::optional<std::string> result_file;
};

std::string join(const std::vector<std::string>& items, const char* separator)
{
    std::string joined;
    for (const std::string& item : items) {
        joined += (joined.empty() ? "" : separator) + item;
    }
    return joined;
}

std::string protocol_text(const Run_facts& facts)
{
    const char*       purpose = addon::structured_purpose_name(facts.purpose);
    const std::string digest  = addon::to_hex(addon::Sha256::digest(facts.prompt));
    const std::string bytes   = std::to_string(facts.prompt.size());
    std::string text;
    text += std::string(k_protocol_header) + " " + facts.mode + " " + purpose + "\n";
    text += "prompt-bytes " + bytes + "\n";
    text += "prompt-sha256 " + digest + "\n";
    if (facts.mode == "delegated") {
        text += "servers " + (facts.server_ids.empty() ? std::string("-") : join(facts.server_ids, ",")) + "\n";
        text += "artifact " + facts.artifact + "\n";
    }
    text += "presentation review_initialized\n";
    text += "presentation reasoning\n";
    text += "presentation analysis_update\n";
    if (facts.result_file.has_value()) {
        text += std::string(k_protocol_result_file_prefix) + *facts.result_file + "\n";
    }
    text += std::string(k_protocol_result_begin) + "\n";
    text += "Unknown fixture " + facts.mode + " " + purpose + " run over " + bytes +
            " prompt bytes (sha256 " + digest + ").\n";
    if (facts.mode == "delegated") {
        const std::string servers = facts.server_ids.empty() ? std::string("none") : join(facts.server_ids, ", ");
        text += "Task servers: " + servers + "\n";
    }
    text += std::string(k_protocol_result_end) + "\n";
    return text;
}

std::optional<addon::Structured_purpose> purpose_option(const std::vector<std::string>& arguments, const char* option)
{
    for (std::size_t i = 0; i + 1 < arguments.size(); i += 2) {
        if (arguments[i] == option) {
            return addon::parse_structured_purpose(arguments[i + 1]);
        }
    }
    return std::nullopt;
}

std::optional<std::string> string_option(const std::vector<std::string>& arguments, const char* option)
{
    for (std::size_t i = 0; i + 1 < arguments.size(); i += 2) {
        if (arguments[i] == option) {
            return arguments[i + 1];
        }
    }
    return std::nullopt;
}

int run_structured(const std::vector<std::string>& arguments)
{
    const std::optional<addon::Structured_purpose> purpose = purpose_option(arguments, "--purpose");
    if (arguments.size() != 2 || !purpose.has_value()) {
        return usage("structured takes --purpose <read_only_review|implementation>");
    }
    Run_facts facts;
    facts.mode    = "structured";
    facts.purpose = *purpose;
    facts.prompt  = read_stdin();
    if (facts.prompt == "orchestration-diagnostic-failure") {
        std::fputs("stderr-canary-secret-value\n"
            "https://diagnostic:secret@example.test/repository?token=secret\n", stderr);
        return 17;
    }
    if (is_orchestration_result_prompt(facts.prompt)) {
        facts.result_file = write_orchestration_result(facts.prompt, fs::current_path());
        if (!facts.result_file.has_value()) {
            return 9;
        }
    }
    if (const char* receipt = std::getenv("UNKNOWN_FIXTURE_PROMPT_RECEIPT")) {
        if (!write_file(fs::path(receipt), facts.prompt)) {
            return 1;
        }
        return write_stdout(protocol_text(facts) + "presentation result_authenticated\n");
    }
    return write_stdout(protocol_text(facts));
}

int run_stdin_eof(const std::vector<std::string>& arguments)
{
    if (arguments.size() != 1) {
        return usage("stdin-eof takes one scenario");
    }
    const bool queued_stream = arguments.front() == "queued-stream";
    if (arguments.front() == "queued-output" || queued_stream) {
        if (write_stdout(queued_stream ? ".\n" : "C\n") != 0) {
            return addon::k_helper_exit_transport_failure;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!fs::exists(fs::current_path() / ".emit-second")) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return 11;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (write_stdout(queued_stream ? std::string(2u * 1024u * 1024u, '.') : "BATCH2\n") != 0) {
            return addon::k_helper_exit_transport_failure;
        }
        if (!queued_stream) {
            std::this_thread::sleep_for(std::chrono::minutes(10));
            return 10;
        }
    }
    if (arguments.front() == "startup-closed") {
        if (read_stdin() != "initial\n" || std::ferror(stdin) != 0) {
            return 8;
        }
        if (write_stdout("L\n") != 0) {
            return addon::k_helper_exit_transport_failure;
        }
        // Keep the producer alive until the host refuses the attempted write
        // after EOF and reaps it; an exit must not mask that refusal.
        std::this_thread::sleep_for(std::chrono::minutes(10));
        return 10;
    }
    if (arguments.front() == "timeout") {
        // Closure arrives halfway through the host's operation budget so a
        // restarted deadline is observably different from the original one.
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    const bool history = arguments.front() == "presentation-history";
    if (history) {
        char input[16];
        if (std::fgets(input, sizeof(input), stdin) == nullptr || std::string_view(input) != "initial\n") {
            return 8;
        }
        constexpr int event_count = 641;
        for (int index = 0; index < event_count;) {
            // Retention needs ordered records, not hundreds of serial exchanges.
            // Keep the first-event checkpoint and bound every later inspection.
            const int batch_size = index == 0
                ? 1
                : std::min(event_count - index,
                    (int)addon::limits::k_max_presentation_records_per_inspection);
            std::string records;
            for (int offset = 0; offset < batch_size; ++offset) {
                records += (index + offset) % 2 == 0 ? 'P' : 'Q';
            }
            if (write_stdout(records) != 0) {
                return addon::k_helper_exit_transport_failure;
            }
            for (int acknowledged = 0; acknowledged < batch_size; ++acknowledged) {
                if (std::fgets(input, sizeof(input), stdin) == nullptr ||
                    std::string_view(input) != "next\n")
                {
                    return 8;
                }
            }
            if (index == 0) {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
                const fs::path release = fs::current_path() / ".runtime-presentation-release";
                while (!fs::exists(release)) {
                    if (std::chrono::steady_clock::now() >= deadline) {
                        return 11;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
            index += batch_size;
        }
    }
    // Without a newline the old host holds R forever while the native process
    // waits for its final input. The provider itself already accepts raw bytes.
    if (write_stdout(arguments.front() == "no-newline" ? "R" : "R\n") != 0) {
        return addon::k_helper_exit_transport_failure;
    }
    // A real pipe read cannot finish until the host has drained the entire
    // final input write and closed stdin. The receipt is independent evidence
    // even for scenarios whose later output or exit must fail inspection.
    const std::string expected = (history ? "" : "initial\n") + std::string(128u * 1024u, 'p') + "\n";
    if (read_stdin() != expected || std::ferror(stdin) != 0) {
        return 8;
    }
    if (!write_file(fs::current_path() / ".stdin-eof-receipt", "all input received before EOF")) {
        return 9;
    }
    const std::string& scenario = arguments.front();
    if (write_stdout(scenario == "partial" ? "DON" : "DONE\n") != 0) {
        return addon::k_helper_exit_transport_failure;
    }
    if (scenario == "nonzero") {
        return 7;
    }
    if (scenario == "close-nonzero") {
        return 17;
    }
    if (scenario == "partial") {
        return 0;
    }
    if (scenario == "late-input" || scenario == "early-complete") {
        if (write_stdout(scenario == "late-input" ? "L\n" : "C\n") != 0) {
            return addon::k_helper_exit_transport_failure;
        }
        std::this_thread::sleep_for(std::chrono::minutes(10));
    }
    return write_stdout(std::string(96u * 1024u, '.'));
}

std::vector<std::string> server_ids_of(const fs::path& artifact_root)
{
    std::vector<std::string> ids;
    const std::optional<addon::Json_value> servers_object = read_json_object(artifact_root / k_servers_artifact);
    const addon::Json_value* servers = servers_object ? servers_object->find("servers") : nullptr;
    if (servers == nullptr || !servers->is_array()) {
        return ids;
    }
    for (const addon::Json_value& server : servers->as_array()) {
        const addon::Json_value* id = server.find("server_id");
        if (id != nullptr && id->is_string()) {
            ids.push_back(id->as_string());
        }
    }
    return ids;
}

int run_delegated(const std::vector<std::string>& arguments)
{
    const std::optional<addon::Structured_purpose> purpose = purpose_option(arguments, "--purpose");
    const std::optional<std::string>               root    = string_option(arguments, "--artifact-root");
    const std::optional<std::string> control = string_option(arguments, "--fixture-control");
    const auto relay = string_option(arguments, "--relay-program");
    const auto descriptor = string_option(arguments, "--descriptor-path");
    const std::size_t expected_arguments = 4 + (control ? 2 : 0) + (relay ? 4 : 0);
    if (arguments.size() != expected_arguments || relay.has_value() != descriptor.has_value() ||
        (control && control->empty()) ||
        !purpose.has_value() || !root.has_value() || root->empty())
    {
        return usage("delegated takes --purpose <purpose> --artifact-root <dir> [--fixture-control <dir>]");
    }
    const fs::path artifact_root = addon::path_from_utf8(*root);
    Run_facts facts;
    facts.mode       = "delegated";
    facts.purpose    = *purpose;
    facts.prompt     = read_stdin();
    if (facts.prompt == "orchestration-diagnostic-failure") {
        std::fputs("stderr-canary-secret-value\n"
            "https://diagnostic:secret@example.test/repository?token=secret\n", stderr);
        return 17;
    }
    if (is_orchestration_result_prompt(facts.prompt)) {
        facts.result_file = write_orchestration_result(facts.prompt, artifact_root);
        if (!facts.result_file.has_value()) {
            return 9;
        }
    }
    facts.server_ids = server_ids_of(artifact_root);
    facts.artifact   = k_summary_artifact;

    // The run's own private artifact, below the root the host owns and
    // scrubs: what it saw, never the prompt itself.
    addon::Json_value servers = addon::Json_value::array();
    for (const std::string& id : facts.server_ids) {
        servers.push_back(addon::Json_value(id));
    }
    addon::Json_value summary = addon::Json_value::object();
    summary.set("purpose",       addon::Json_value(addon::structured_purpose_name(facts.purpose)));
    summary.set("prompt_bytes",  addon::Json_value((std::uint64_t)facts.prompt.size()));
    summary.set("prompt_sha256", addon::Json_value(addon::to_hex(addon::Sha256::digest(facts.prompt))));
    summary.set("servers",       std::move(servers));
    if (!write_file(artifact_root / k_summary_artifact, addon::serialize_json(summary) + "\n")) {
        std::fprintf(stderr, "unknown_fixture_helper: cannot write %s below %s\n", k_summary_artifact, root->c_str());
        return addon::k_helper_exit_transport_failure;
    }
    const std::string protocol = protocol_text(facts);
    if (facts.prompt == "orchestration-scoped-tools") {
        if (!relay || !descriptor) {
            return 9;
        }
        const int outcome = run_scoped_tools(artifact_root, *relay, *descriptor);
        return outcome == 0 ? write_stdout(protocol) : outcome;
    }
    if (control) {
        const fs::path control_root = addon::path_from_utf8(*control);
        addon::Json_value receipt = addon::Json_value::object();
        for (const char* name : {"LOGONOMIC_SESSION_CAPABILITY", "VNM_CONTROL_ENDPOINT",
                                "VNM_CONTROL_TOKEN", "LOGONOMIC_VNMCTL_PATH",
                                "VNM_OWNER_ENDPOINT", "VNM_OWNER_TOKEN",
                                "VNM_RELAY_ENDPOINT", "VNM_RELAY_TOKEN",
                                "VNM_BOOTSTRAP_ENDPOINT", "VNM_BOOTSTRAP_TOKEN",
                                "VNM_INVITATION_TOKEN", "VNM_AUTHORIZATION_TOKEN",
                                "VNM_WORKER_CONTROL_ENDPOINT", "VNM_WORKER_CONTROL_TOKEN"})
        {
#if defined(_WIN32)
            ::SetLastError(ERROR_SUCCESS);
            const DWORD length = ::GetEnvironmentVariableA(name, nullptr, 0);
            receipt.set(name, addon::Json_value(length != 0 || ::GetLastError() != ERROR_ENVVAR_NOT_FOUND));
#else
            receipt.set(name, addon::Json_value(std::getenv(name) != nullptr));
#endif
        }
#if defined(_WIN32)
        char canary[sizeof("fixture-canary")] = {};
        const DWORD canary_length = ::GetEnvironmentVariableA(
            "UNKNOWN_FIXTURE_DELEGATION_CANARY", canary, sizeof(canary));
        receipt.set("fixture_canary_matches", addon::Json_value(
            canary_length == sizeof(canary) - 1 && std::string_view(canary) == "fixture-canary"));
#else
        const char* canary = std::getenv("UNKNOWN_FIXTURE_DELEGATION_CANARY");
        receipt.set("fixture_canary_matches",
            addon::Json_value(canary != nullptr && std::string_view(canary) == "fixture-canary"));
#endif
        if (!write_file(control_root / ".unknown-fixture-delegation-environment.json",
                addon::serialize_json(receipt) + "\n"))
        {
            return addon::k_helper_exit_transport_failure;
        }
        const std::size_t result_begin = protocol.find(k_protocol_result_begin);
        if (write_stdout(std::string_view(protocol).substr(0, result_begin)) != 0) {
            return addon::k_helper_exit_transport_failure;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
        for (;;) {
            std::error_code error;
            const fs::path release = control_root / ".unknown-fixture-delegation-release";
            if (fs::exists(release, error) && fs::is_regular_file(release, error)) {
                return write_stdout(std::string_view(protocol).substr(result_begin));
            }
            if (error || std::chrono::steady_clock::now() >= deadline) {
                return addon::k_helper_exit_transport_failure;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    return write_stdout(protocol);
}

int run_account_refresh()
{
    const char* account = std::getenv("UNKNOWN_FIXTURE_ACCOUNT_KEY");
    const std::string source = account ? account : "";
    const std::string suffix = addon::to_hex(addon::Sha256::digest(source));
    const fs::path prefix = fs::current_path() / ("account-refresh-" + suffix);
    const auto path = [&prefix](const char* ending) {
        return fs::path(prefix).concat(ending);
    };
    const auto wait_gate = [&path](const char* ending) {
        while (fs::exists(path(ending))) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    };
    const auto now = []() {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    };
    const auto control_bytes = read_bounded_file(path(".json"), 4096);
    const auto control = control_bytes ? addon::parse_json(*control_bytes, {}, nullptr) : std::nullopt;
    const auto* mode_value = control ? control->find("mode") : nullptr;
    const std::string mode = mode_value && mode_value->is_string() ? mode_value->as_string() : "success";
    {
        std::ofstream starts(path("-starts"), std::ios::app);
#if defined(_WIN32)
        starts << ::GetCurrentProcessId() << '\n';
#else
        starts << ::getpid() << '\n';
#endif
    }
    std::fputs("private shared-refresh fixture diagnostic\n", stderr);
    std::fflush(stderr);
    wait_gate("-hold-quota");
    addon::Observation_scope scope{account ? std::optional<std::string>(source) : std::nullopt, now(), 60000};
    if (mode == "wrong-account") {
        scope.source_account_key = "other-profile";
    }
    if (mode == "stale-quota") {
        scope.observed_at_ms = 1;
        scope.fresh_for_ms = 0;
    }
    const auto record = [](const char* kind, addon::Json_value value) {
        addon::Json_value line = addon::Json_value::object();
        line.set("kind", addon::Json_value(kind));
        line.set("value", std::move(value));
        return addon::serialize_json(line) + "\n";
    };
    addon::Quota_observation quota;
    quota.scope = scope;
    quota.state = addon::Quota_status::READY;
    quota.primary = addon::Quota_window{37.5, 300, std::nullopt, "Primary"};
    quota.plan = "shared-refresh-plan";
    const std::string quota_record = record("quota",
        addon::encode_quota_inspect_result({true, {}, quota}, 2));
    if (mode != "missing-quota") {
        if (mode == "fragmented") {
            const std::size_t split = quota_record.size() / 2;
            if (write_stdout(std::string_view(quota_record).substr(0, split)) != 0 ||
                !write_file(path("-partial"), "quota fragment"))
            {
                return 8;
            }
            wait_gate("-hold-partial");
            if (write_stdout(std::string_view(quota_record).substr(split)) != 0) {
                return 8;
            }
        }
        else if (write_stdout(quota_record) != 0) {
            return 8;
        }
    }
    if (!write_file(path("-quota-written"), "quota stage reached")) {
        return 8;
    }
    wait_gate("-hold-auth");
    if (mode == "duplicate-quota" && write_stdout(quota_record) != 0) {
        return 8;
    }
    scope.observed_at_ms = now();
    scope.fresh_for_ms = 60000;
    addon::Auth_inspect_result auth{true, {}, addon::Auth_observation{
        scope, mode == "auth-error" ? addon::Auth_status::FAILED : addon::Auth_status::AUTHENTICATED}};
    if (mode != "auth-error") {
        const auto bytes = read_bounded_file(fs::current_path() / ("current-account-" + suffix + ".json"), 4096);
        auto identity = bytes ? addon::parse_json(*bytes, {}, nullptr) : std::nullopt;
        if (identity && identity->is_object()) {
            identity->set("version", addon::Json_value(1));
            identity->set("scope", addon::encode_observation_scope(scope));
            auth.current_account = addon::decode_current_account_observation(*identity);
        }
    }
    const auto auth_payload = mode == "malformed-auth"
        ? addon::Json_value("private invalid authentication bytes") : addon::encode_auth_inspect_result(auth);
    if (write_stdout(record("auth", auth_payload)) != 0 ||
        !write_file(path("-auth-written"), "authentication record written"))
    {
        return 8;
    }
    wait_gate("-hold-exit");
    return mode == "process-error" ? 7 : 0;
}

bool wait_for_auth_gate_removal(const fs::path& gate)
{
    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::seconds(120);
    auto access_deadline = deadline;
    do {
        std::error_code error;
        if (!fs::exists(gate, error) && !error) {
            return true;
        }
        const auto now = Clock::now();
        if (error) {
            // Windows can report access denied while the test removes its gate.
            // A persistent error must fail the probe, never release the gate.
            access_deadline = std::min(access_deadline, now + std::chrono::seconds(1));
            if (error != std::errc::permission_denied || now >= access_deadline) {
                return false;
            }
        }
        else {
            access_deadline = deadline;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (Clock::now() < deadline);
    return false;
}

int run_observation(const std::vector<std::string>& arguments)
{
    if (arguments.size() != 1) {
        return usage("observation takes one fixture mode");
    }
    if (arguments.front() == "timeout") {
        std::this_thread::sleep_for(std::chrono::minutes(10));
    }
    if (arguments.front() == "process-error") {
        std::fputs("private fixture process diagnostic\n", stderr);
        return 7;
    }
    addon::Observation_scope scope;
    if (const char* account = std::getenv("UNKNOWN_FIXTURE_ACCOUNT_KEY"); account != nullptr) {
        scope.source_account_key = account;
    }
    scope.observed_at_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    scope.fresh_for_ms = 60000;
    if (arguments.front() == "stale") {
        scope.observed_at_ms = 1;
        scope.fresh_for_ms   = 0;
    }
    if ((arguments.front() == "current-account" || arguments.front() == "current-account-auth") &&
        scope.source_account_key)
    {
        const std::string suffix = addon::to_hex(addon::Sha256::digest(*scope.source_account_key));
        const std::string kind = arguments.front() == "current-account-auth" ? "auth" : "quota";
        std::ofstream(kind + "-starts-" + suffix, std::ios::app) << native_process_id() << '\n';
    }
    if (arguments.front() == "current-account-auth" && scope.source_account_key) {
        const std::string suffix = addon::to_hex(addon::Sha256::digest(*scope.source_account_key));
        std::ofstream("auth-started-" + suffix).put('\n');
        if (!wait_for_auth_gate_removal("auth-hold-" + suffix)) {
            return 8;
        }
        if (fs::exists("auth-fail-" + suffix)) {
            return 7;
        }
        addon::Json_value result = addon::Json_value::object();
        std::ifstream input("current-account-" + suffix + ".json", std::ios::binary);
        const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        auto identity = addon::parse_json(bytes, addon::Json_parse_limits{}, nullptr);
        if (identity && identity->is_object()) {
            const auto* freshness = identity->find("fixture_fresh_for_ms");
            if (freshness && freshness->is_integer() && freshness->as_integer() > 0) {
                scope.fresh_for_ms = static_cast<std::uint64_t>(freshness->as_integer());
            }
            identity->erase("fixture_fresh_for_ms");
            identity->set("version", addon::Json_value(1));
            identity->set("scope", addon::encode_observation_scope(scope));
            result.set("current_account", std::move(*identity));
        }
        result.set("scope", addon::encode_observation_scope(scope));
        const int status = write_stdout(addon::serialize_json(result));
        if (status != 0 || !write_file("auth-written-" + suffix, "authentication output written")) {
            return status != 0 ? status : 8;
        }
        if (!wait_for_auth_gate_removal("auth-exit-hold-" + suffix)) {
            return 8;
        }
        return 0;
    }
    return write_stdout(addon::serialize_json(addon::encode_observation_scope(scope)));
}

} // namespace

int main(int argc, char** argv)
{
#if defined(_WIN32)
    (void)::_setmode(::_fileno(stdin),  _O_BINARY);
    (void)::_setmode(::_fileno(stdout), _O_BINARY);
#endif
    // Fixture-only argv recorder runs before mode dispatch and consumes no
    // argument. Only the explicit receipt path is read from the environment.
#if defined(_WIN32)
    std::wstring argv_receipt(32768, L'\0');
    const DWORD argv_receipt_length = ::GetEnvironmentVariableW(
        L"UNKNOWN_FIXTURE_ARGV_PROBE_RECORD", argv_receipt.data(), static_cast<DWORD>(argv_receipt.size()));
    const bool record_argv = argv_receipt_length != 0 && argv_receipt_length < argv_receipt.size();
    argv_receipt.resize(record_argv ? argv_receipt_length : 0);
#else
    const char* argv_receipt = std::getenv("UNKNOWN_FIXTURE_ARGV_PROBE_RECORD");
    const bool record_argv = argv_receipt != nullptr && *argv_receipt != '\0';
#endif
    if (record_argv) {
        addon::Json_value arguments = addon::Json_value::array();
        for (int index = 1; index < argc; ++index) {
            arguments.push_back(addon::Json_value(argv[index]));
        }
        return write_file(fs::path(argv_receipt), addon::serialize_json(arguments)) ? 0 : 9;
    }
    if (argc < 2) {
        return usage(nullptr);
    }
    const std::string_view         mode = argv[1];
    const std::vector<std::string> arguments(argv + 2, argv + argc);
    if (mode == "--resume" && arguments.size() == 1 && !arguments.front().empty()) {
        // Ownership tests install this native helper as unknown-fixture-cli.
        // Do not fabricate a provider observation or print the conversation ID.
        const auto root = fs::current_path();
        if (!write_file(root / ".unknown-fixture-resume-ready", "ready")) {
            return 9;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
        while (!fs::exists(root / ".unknown-fixture-resume-release")) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return 9;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        return 0;
    }
    if (mode == "event")            { return run_event(arguments);         }
    if (mode == "set-title")        { return run_set_title(arguments);     }
    if (mode == "changed-files")    { return run_changed_files(arguments); }
    if (mode == "show-diff")        { return run_show_diff(arguments);     }
    if (mode == "coordination-mcp") { return run_mcp(arguments);           }
    if (mode == "delegation-mcp")   { return run_mcp(arguments);           }
    if (mode == "structured")       { return run_structured(arguments);    }
    if (mode == "stdin-eof")        { return run_stdin_eof(arguments);     }
    if (mode == "delegated")        { return run_delegated(arguments);     }
    if (mode == "observation")      { return run_observation(arguments);   }
    if (mode == "account-refresh" && arguments.empty()) { return run_account_refresh(); }
    if (mode == "runtime-family" && arguments.empty()) {
        return run_native_family(argv[0]);
    }
    if (mode == "runtime-family-child" && arguments.size() == 1) {
        if (!write_file(addon::path_from_utf8(arguments.front()), std::to_string(native_process_id()))) {
            return 9;
        }
        std::this_thread::sleep_for(std::chrono::seconds(180));
        return 9;
    }
    if (mode == "incremental-quota" && arguments.empty()) {
        char input[16];
        for (const auto& exchange : {std::pair{"1", "first\n"}, std::pair{"2", "second\n"}}) {
            if (write_stdout(exchange.first) != 0 || std::fgets(input, sizeof(input), stdin) == nullptr ||
                std::string_view(input) != exchange.second)
            {
                return 8;
            }
        }
        if (!write_file(fs::current_path() / ".runtime-quota-receipt", "ordered input received")) {
            return 9;
        }
        return write_stdout("3");
    }
    if (mode == "runtime-descendant" && arguments.size() == 1) {
        (void)read_stdin();
        return run_mcp_server({argv[0], "runtime-detached-hold", arguments.front()});
    }
    if (mode == "runtime-detached-hold" && arguments.size() == 1) {
#if defined(_WIN32)
        const auto process_id = ::GetCurrentProcessId();
#else
        // Escape the parent's process group; native custody must still retain us.
        if (::setsid() < 0) {
            return addon::k_helper_exit_transport_failure;
        }
        const auto process_id = ::getpid();
#endif
        const auto& receipt_argument = arguments.front();
        const fs::path receipt(std::u8string(
            receipt_argument.begin(), receipt_argument.end()));
        if (!write_file(receipt, std::to_string(process_id))) {
            return addon::k_helper_exit_transport_failure;
        }
        std::this_thread::sleep_for(std::chrono::minutes(10));
        return 10;
    }
    if (mode == "runtime-private-output" && arguments.size() == 1) {
        (void)read_stdin();
        if (write_stdout("logonomic-private-version-marker\n") != 0) {
            return addon::k_helper_exit_transport_failure;
        }
        const auto& release_argument = arguments.front();
        const fs::path release(std::u8string(
            release_argument.begin(), release_argument.end()));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!fs::exists(release)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return 11;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return 7;
    }
    if (mode == "runtime-output-file" && arguments.size() == 1) {
        (void)read_stdin();
        const auto bytes = read_file(addon::path_from_utf8(arguments.front()));
        return bytes ? write_stdout(*bytes) : addon::k_helper_exit_transport_failure;
    }
    if ((mode == "runtime-hold" || mode == "runtime-stdout-bound") && arguments.empty()) {
        (void)read_stdin();
        if (mode == "runtime-stdout-bound" &&
            write_stdout(std::string(1024u * 1024u, 'x')) != 0)
        {
            return addon::k_helper_exit_transport_failure;
        }
        std::this_thread::sleep_for(std::chrono::minutes(10));
        return 10;
    }
    return usage("unknown mode");
}
