// Fixture packages select their adapter behaviour and handshake declarations
// through adapter.argv so the host can exercise them with one executable.
//
// Behaviours that need process control are implemented for POSIX and
// Windows where an equivalent exists; `detach-grandchild` on Windows asks
// for CREATE_BREAKAWAY_FROM_JOB and reports whether the system granted it.

#include "logonomic_cli_addon/adapter_runtime.h"
#include "logonomic_cli_addon/accounts.h"
#include "logonomic_cli_addon/auth_status.h"
#include "logonomic_cli_addon/account_refresh.h"
#include "logonomic_cli_addon/quota.h"
#include "logonomic_cli_addon/usage_scan.h"
#include "logonomic_cli_addon/usage_session.h"
#include "logonomic_cli_addon/usage_repository_attribution.h"
#include "logonomic_cli_addon/usage_owned_event_correction.h"
#include "logonomic_cli_addon/usage_lifetime_summary.h"
#include "logonomic_cli_addon/usage_saved_daily_tokens.h"
#include "logonomic_cli_addon/capability_ids.h"
#include "logonomic_cli_addon/frame.h"
#include "logonomic_cli_addon/helper_ingress.h"
#include "logonomic_cli_addon/host_simulator.h"
#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/json.h"
#include "logonomic_cli_addon/launch_preparation.h"
#include "logonomic_cli_addon/launcher_discovery.h"
#include "logonomic_cli_addon/launcher_approval.h"
#include "logonomic_cli_addon/launcher_utilities.h"
#include "logonomic_cli_addon/lifecycle_events.h"
#include "logonomic_cli_addon/lifecycle_correlation_lifetime.h"
#include "logonomic_cli_addon/limits.h"
#include "logonomic_cli_addon/managed_configuration.h"
#include "logonomic_cli_addon/package_source.h"
#include "logonomic_cli_addon/permission_snapshot.h"
#include "logonomic_cli_addon/process_proposal.h"
#include "logonomic_cli_addon/protocol.h"
#include "logonomic_cli_addon/session_contracts.h"
#include "logonomic_cli_addon/session_wake.h"
#include "logonomic_cli_addon/conversation_export.h"
#include "logonomic_cli_addon/sha256.h"
#include "logonomic_cli_addon/sidecar.h"
#include "logonomic_cli_addon/startup_observer.h"
#include "logonomic_cli_addon/version.h"

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <map>
#include <mutex>
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

bool native_preparation_fixture(const addon::Launcher_request& launcher)
{
    const std::string& script = launcher.script_body;
    if (script.size() < 3 || script.front() != '"' || script.back() != '"') {
        return false;
    }
    const fs::path program = addon::path_from_utf8(script.substr(1, script.size() - 2));
    return program.is_absolute() && program.filename() == "unknown-fixture-native.exe" &&
        addon::native_image(program);
}

enum class Behavior
{
    CONFORMANT,
    CRASH_ON_INVOKE,
    HANG_ON_INVOKE,
    HANG_ON_HELLO,
    DOUBLE_RESULT,
    OVERSIZE_FRAME,
    INVALID_UTF8_FRAME,
    STDOUT_GARBAGE,
    STDERR_FLOOD,
    BIDIRECTIONAL_FLOOD,
    STDIN_STALL,
    RESULT_THEN_EXIT,
    WRONG_NONCE_HELLO,
    WRONG_CORE_GENERATION,
    SIDECAR_WITHOUT_HANDSHAKE,
    HANDSHAKE_WITHOUT_SIDECAR,
    ORPHAN_GRANDCHILD,
    DETACH_GRANDCHILD,
    IGNORES_CANCEL,
    SLOW_SHUTDOWN,
    SLOW_RESULT,
    PROFILE_OMIT_ECHO,
    PROFILE_WRONG_ROOT,
    PROFILE_DEFAULT_DIR,
    PROFILE_WRONG_VERSION,
    PROVIDER_OBSERVATIONS,
    PROVIDER_ACCOUNTS,
    PROVIDER_SURFACES,
    PROVIDER_MANAGED_CONFIGURATION_V1,
    PROVIDER_MANAGED_CONFIGURATION_V2,
    PROVIDER_ACCOUNT_MANAGED_CONFIGURATION,
    PROVIDER_SESSIONS,
    PROVIDER_SESSIONS_ASSETS,
    PROVIDER_SESSIONS_CONTINUOUS,
    PROVIDER_SESSIONS_LIFETIME,
    PROVIDER_SESSIONS_STDIN_EOF,
    PROVIDER_INITIAL_PROMPT,
};

struct Behavior_name
{
    Behavior    behavior;
    const char* name;
};

constexpr Behavior_name k_behaviors[] = {
    {Behavior::CONFORMANT,                   "conformant"},
    {Behavior::CRASH_ON_INVOKE,              "crash-on-invoke"},
    {Behavior::HANG_ON_INVOKE,               "hang-on-invoke"},
    {Behavior::HANG_ON_HELLO,                "hang-on-hello"},
    {Behavior::DOUBLE_RESULT,                "double-result"},
    {Behavior::OVERSIZE_FRAME,               "oversize-frame"},
    {Behavior::INVALID_UTF8_FRAME,           "invalid-utf8-frame"},
    {Behavior::STDOUT_GARBAGE,               "stdout-garbage"},
    {Behavior::STDERR_FLOOD,                 "stderr-flood"},
    {Behavior::BIDIRECTIONAL_FLOOD,          "bidirectional-flood"},
    {Behavior::STDIN_STALL,                  "stdin-stall"},
    {Behavior::RESULT_THEN_EXIT,             "result-then-exit"},
    {Behavior::WRONG_NONCE_HELLO,            "wrong-nonce-hello"},
    {Behavior::WRONG_CORE_GENERATION,        "wrong-core-generation"},
    {Behavior::SIDECAR_WITHOUT_HANDSHAKE,    "sidecar-without-handshake"},
    {Behavior::HANDSHAKE_WITHOUT_SIDECAR,    "handshake-without-sidecar"},
    {Behavior::ORPHAN_GRANDCHILD,            "orphan-grandchild"},
    {Behavior::DETACH_GRANDCHILD,            "detach-grandchild"},
    {Behavior::IGNORES_CANCEL,               "ignores-cancel"},
    {Behavior::SLOW_SHUTDOWN,                "slow-shutdown"},
    {Behavior::SLOW_RESULT,                  "slow-result"},
    {Behavior::PROFILE_OMIT_ECHO,            "profile-omit-echo"},
    {Behavior::PROFILE_WRONG_ROOT,           "profile-wrong-root"},
    {Behavior::PROFILE_DEFAULT_DIR,          "profile-default-dir"},
    {Behavior::PROFILE_WRONG_VERSION,        "profile-wrong-version"},
    {Behavior::PROVIDER_OBSERVATIONS,        "provider-observations"},
    {Behavior::PROVIDER_ACCOUNTS,            "provider-accounts"},
    {Behavior::PROVIDER_SURFACES,            "provider-surfaces"},
    {Behavior::PROVIDER_MANAGED_CONFIGURATION_V1, "provider-managed-configuration-v1"},
    {Behavior::PROVIDER_MANAGED_CONFIGURATION_V2, "provider-managed-configuration-v2"},
    {Behavior::PROVIDER_ACCOUNT_MANAGED_CONFIGURATION, "provider-account-managed-configuration"},
    {Behavior::PROVIDER_SESSIONS,            "provider-sessions"},
    {Behavior::PROVIDER_SESSIONS_ASSETS,     "provider-sessions-assets"},
    {Behavior::PROVIDER_SESSIONS_CONTINUOUS, "provider-sessions-continuous"},
    {Behavior::PROVIDER_SESSIONS_LIFETIME,   "provider-sessions-lifetime"},
    {Behavior::PROVIDER_SESSIONS_STDIN_EOF,  "provider-sessions-stdin-eof"},
    {Behavior::PROVIDER_INITIAL_PROMPT,      "provider-initial-prompt"},
};

// slow-result answers every request after this delay, so a host test can
// place a deadline on either side of it.
constexpr int k_slow_result_delay_ms = 1500;

// Emulate a cold CLI probe that takes longer than an ordinary invocation.
constexpr int k_slow_resume_delay_ms = 6500;

// Long enough that the host's shutdown grace and its cancel grace both
// expire first.
constexpr int k_forever_ms = 600000;

const addon::Capability_pair k_probe{"conformance-probe", 1};
// Declared by sidecar and handshake alike but implemented by no operation:
// a host that does not adopt it never sees it, and one that does is
// answered unavailable/unknown_operation.
const addon::Capability_pair k_unadopted{"org.example.unadopted", 7};
const addon::Capability_pair k_accounts{addon::k_accounts_capability_id, addon::k_accounts_launcher_version};
const addon::Capability_pair k_profile_storage{addon::k_profile_storage_capability_id, addon::k_profile_storage_version};
const addon::Capability_pair k_presentation{
    addon::k_presentation_capability_id,
    addon::k_presentation_version,
};
const addon::Capability_pair k_launcher_discovery{
    addon::k_launcher_discovery_capability_id,
    addon::k_launcher_discovery_version,
};
const addon::Capability_pair k_launch_preparation{
    addon::k_launch_preparation_capability_id,
    addon::k_launch_preparation_execution_version,
};
const addon::Capability_pair k_managed_assets{
    addon::k_managed_assets_capability_id,
    addon::k_managed_assets_version,
};
const addon::Capability_pair k_structured_session{
    addon::k_structured_session_capability_id,
    addon::k_structured_session_version,
};
const addon::Capability_pair k_delegated_session{
    addon::k_delegated_session_capability_id,
    addon::k_delegated_session_version,
};
const addon::Capability_pair k_lifecycle_events{
    addon::k_lifecycle_events_capability_id,
    addon::k_lifecycle_events_version,
};
const addon::Capability_pair k_lifecycle_correlation_lifetime{
    addon::k_lifecycle_correlation_lifetime_capability_id,
    addon::k_lifecycle_correlation_lifetime_version,
};
// Static: the sidecar is its authority, and the handshake lists it so that
// the sidecar and the handshake agree on every pair, as presentation@1 does.
const addon::Capability_pair k_coordination_delivery{
    addon::k_coordination_delivery_capability_id,
    addon::k_coordination_delivery_version,
};

const addon::Capability_pair k_helper_ingress_degradation{
    addon::k_helper_ingress_capability_id,
    addon::k_helper_ingress_degradation_version,
};
const addon::Capability_pair k_helper_ingress_subagent_reconciliation{
    addon::k_helper_ingress_capability_id,
    addon::k_helper_ingress_subagent_reconciliation_version,
};

// The provider-session behaviours (docs/conformance.md, "Fixture roster").
// The packaged helper is the process every structured and delegated proposal
// launches, in the mode named by its first argument.
#if defined(_WIN32)
constexpr const char* k_helper_executable = "helpers/unknown_fixture_helper.exe";
#else
constexpr const char* k_helper_executable = "helpers/unknown_fixture_helper";
#endif
// The helper's line protocol, which `inspect` parses. Every other line the
// process writes - the helper's informational lines and any vendor noise -
// is tolerated and ignored.
constexpr const char* k_protocol_header_prefix       = "unknown-fixture-helper 1 ";
constexpr const char* k_protocol_presentation_prefix = "presentation ";
constexpr const char* k_protocol_result_begin        = "result-begin";
constexpr const char* k_protocol_result_end          = "result-end";
constexpr const char* k_protocol_result_file_prefix  = "result-file ";
// A protocol line is short; a longer one is noise whatever it says.
constexpr std::size_t k_max_protocol_line_bytes = 4096;
// The result travels inside protocol_state between inspections, base64
// encoded beside one pending line; the bound keeps the state inside
// k_max_protocol_state_bytes.
constexpr std::size_t k_max_helper_result_bytes = 32u * 1024u;
static_assert(
    (k_max_helper_result_bytes + k_max_protocol_line_bytes) * 4 / 3 + 256 < addon::limits::k_max_protocol_state_bytes,
    "the inspect state must fit the protocol_state bound");
// The private artifact a delegated preparation writes for its process.
constexpr const char* k_servers_artifact_path = "mcp/servers.json";
// Continuous observation reads this JSON-lines file in the session working
// directory: each complete line is one observation object.
constexpr const char* k_events_file_name = ".unknown-fixture-events.jsonl";
// How often an observe window re-reads the events file; cancellation is
// honoured within this interval.
constexpr int k_observe_poll_interval_ms = 100;
// An observation cursor is `<session token>:<line index>`; the token is the
// first 16 hex digits of SHA-256 over the session working directory.
constexpr std::size_t k_session_token_hex_chars = 16;

void sleep_ms(int ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

bool wait_for_fixture_gate(
    addon::Invocation_context& context,
    const fs::path& gate,
    const fs::path& ready,
    const char* cancellation_reason)
{
    std::ofstream entered(ready, std::ios::binary);
    entered << "ready";
    entered.close();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!fs::exists(gate)) {
        if (context.wait_for_cancel(10) || std::chrono::steady_clock::now() >= deadline) {
            context.fail(addon::Result_status::CANCELLED, cancellation_reason);
            return false;
        }
    }
    return true;
}

[[noreturn]] void hang_forever()
{
    while (true) {
        sleep_ms(k_forever_ms);
    }
}

[[noreturn]] void crash()
{
#if defined(_WIN32)
    ::TerminateProcess(::GetCurrentProcess(), 3);
    for (;;) {}
#else
    std::abort();
#endif
}

std::string self_executable(const char* argv0)
{
#if defined(_WIN32)
    wchar_t buffer[32768];
    const DWORD length = ::GetModuleFileNameW(nullptr, buffer, (DWORD)(sizeof(buffer) / sizeof(buffer[0])));
    if (length == 0) {
        return argv0;
    }
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, buffer, (int)length, nullptr, 0, nullptr, nullptr);
    std::string utf8((std::size_t)size, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, buffer, (int)length, utf8.data(), size, nullptr, nullptr);
    return utf8;
#else
    char buffer[4096];
    const ssize_t length = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (length <= 0) {
        return argv0;
    }
    return std::string(buffer, (std::size_t)length);
#endif
}

struct Spawned_child
{
    std::int64_t pid      = 0;
    bool         detached = false;   // the adapter tried to leave the host's group or Job
    std::string  error;
};

// Starts a copy of this executable that sleeps for `ms` with its standard
// streams closed off from the protocol pipes. `detach` reproduces the
// nonconforming daemonizing pattern: a new session behind a double fork on
// POSIX, breakaway from the Job on Windows.
Spawned_child spawn_sleeping_child(const std::string& executable, int ms, bool detach)
{
    Spawned_child child;
    child.detached = detach;
    const std::string sleep_argument = std::to_string(ms);
#if defined(_WIN32)
    std::wstring command = L"\"";
    const int wide_length = ::MultiByteToWideChar(CP_UTF8, 0, executable.c_str(), -1, nullptr, 0);
    std::wstring wide_executable((std::size_t)wide_length, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, executable.c_str(), -1, wide_executable.data(), wide_length);
    wide_executable.resize((std::size_t)wide_length - 1);
    command += wide_executable + L"\" --child-sleep " + std::wstring(sleep_argument.begin(), sleep_argument.end());

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION information{};
    DWORD flags = CREATE_NO_WINDOW;
    if (detach) {
        flags |= CREATE_BREAKAWAY_FROM_JOB | DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP;
    }
    BOOL created = ::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, flags, nullptr, nullptr, &startup, &information);
    if (!created && detach) {
        // The Job forbids breakaway; the attempt itself is the nonconformance.
        child.error = "breakaway refused";
        flags   = CREATE_NO_WINDOW | DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP;
        created = ::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, flags, nullptr, nullptr, &startup, &information);
    }
    if (!created) {
        child.error = "CreateProcessW failed";
        return child;
    }
    child.pid = information.dwProcessId;
    ::CloseHandle(information.hThread);
    ::CloseHandle(information.hProcess);
    return child;
#else
    int report[2] = {-1, -1};
#if defined(__APPLE__)
    if (detach) {
        if (::pipe(report) != 0) {
            child.error = "pipe failed";
            return child;
        }
        if (::fcntl(report[0], F_SETFD, FD_CLOEXEC) != 0 ||
            ::fcntl(report[1], F_SETFD, FD_CLOEXEC) != 0) {
            ::close(report[0]);
            ::close(report[1]);
            child.error = "setting close-on-exec failed";
            return child;
        }
    }
#else
    if (detach && ::pipe2(report, O_CLOEXEC) != 0) {
        child.error = "pipe2 failed";
        return child;
    }
#endif
    const pid_t first = ::fork();
    if (first < 0) {
#if defined(__APPLE__)
        if (detach) {
            ::close(report[0]);
            ::close(report[1]);
        }
#endif
        child.error = "fork failed";
        return child;
    }
    if (first == 0) {
        // Child: only async-signal-safe calls until exec.
        const auto silence = [] {
            const int null_fd = ::open("/dev/null", O_RDWR);
            if (null_fd >= 0) {
                ::dup2(null_fd, 0);
                ::dup2(null_fd, 1);
                ::dup2(null_fd, 2);
                if (null_fd > 2) {
                    ::close(null_fd);
                }
            }
        };
        const auto exec_sleeper = [&] {
            silence();
            char* const arguments[] = {
                const_cast<char*>(executable.c_str()),
                const_cast<char*>("--child-sleep"),
                const_cast<char*>(sleep_argument.c_str()),
                nullptr,
            };
            ::execv(executable.c_str(), arguments);
            ::_exit(127);
        };
        if (!detach) {
            exec_sleeper();
        }
        ::setsid();
        const pid_t second = ::fork();
        if (second == 0) {
            exec_sleeper();
        }
        if (second > 0) {
            const std::string text = std::to_string((long long)second);
            (void)!::write(report[1], text.data(), text.size());
        }
        ::_exit(second > 0 ? 0 : 1);
    }
    if (!detach) {
        child.pid = first;
        return child;
    }
    ::close(report[1]);
    int status = 0;
    ::waitpid(first, &status, 0);
    char buffer[32] = {};
    const ssize_t count = ::read(report[0], buffer, sizeof(buffer) - 1);
    ::close(report[0]);
    if (count <= 0) {
        child.error = "detached grandchild did not report its pid";
        return child;
    }
    child.pid = std::atoll(buffer);
    return child;
#endif
}

// ---------------------------------------------------------------------------
// The provider-session behaviours: shared pieces

void fail_with(addon::Invocation_context& context, const addon::Contract_failure& failure)
{
    context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
}

bool has_control_characters(std::string_view text)
{
    return std::any_of(text.begin(), text.end(), [](unsigned char c) { return c < 0x20 || c == 0x7F; });
}

// The process a structured or delegated preparation proposes: the packaged
// helper in the named mode, fed the prompt on stdin.
addon::Process_proposal helper_process(
    const char*                     mode,
    addon::Structured_purpose       purpose,
    std::string                     prompt,
    const std::vector<std::string>& extra_argv,
    addon::Cleanup_intent           cleanup)
{
    const char* purpose_name = addon::structured_purpose_name(purpose);
    addon::Process_proposal process;
    process.executable        = k_helper_executable;
    process.argv              = {mode, "--purpose", purpose_name};
    process.argv.insert(process.argv.end(), extra_argv.begin(), extra_argv.end());
    const bool raw_result = prompt == "orchestration-large-result" || prompt == "orchestration-invalid-utf8";
    process.working_directory = raw_result && std::strcmp(mode, "structured") == 0
        ? addon::Working_directory_intent::SCRATCH_DIRECTORY
        : addon::Working_directory_intent::SESSION_WORKSPACE;
    process.stdin_behavior    = addon::Stdin_behavior::PAYLOAD;
    process.stdin_payload     = std::move(prompt);
    process.start             = addon::Start_intent::ATTACHED;
    process.environment_names = {"PATH"};
    process.environment       = {{"UNKNOWN_FIXTURE_PURPOSE", purpose_name}};
    process.cleanup           = cleanup;
    return process;
}

// What `inspect` carries between inspections as protocol_state: the empty
// state is the one `prepare` issues, every later one is this object.
struct Inspect_state
{
    bool        header_seen      = false;
    bool        in_result        = false;
    bool        result_complete  = false;
    bool        result_truncated = false;
    bool        discarding_line  = false;   // inside a line longer than the protocol allows
    std::string result;                     // the lines between result-begin and result-end
    std::string partial;                    // the incomplete last line of the previous inspection
    std::string result_file;                // fixture-owned raw result file, when selected
    std::string observed_model;             // fixture native output, never launch input
};

std::string encode_inspect_state(const Inspect_state& state)
{
    addon::Json_value object = addon::Json_value::object();
    object.set("header_seen",      addon::Json_value(state.header_seen));
    object.set("in_result",        addon::Json_value(state.in_result));
    object.set("result_complete",  addon::Json_value(state.result_complete));
    object.set("result_truncated", addon::Json_value(state.result_truncated));
    object.set("discarding_line",  addon::Json_value(state.discarding_line));
    object.set("result_base64",    addon::Json_value(addon::encode_base64(state.result)));
    object.set("partial_base64",   addon::Json_value(addon::encode_base64(state.partial)));
    object.set("result_file_base64", addon::Json_value(addon::encode_base64(state.result_file)));
    object.set("observed_model_base64", addon::Json_value(addon::encode_base64(state.observed_model)));
    return addon::serialize_json(object);
}

std::optional<Inspect_state> decode_inspect_state(const std::string& text)
{
    Inspect_state state;
    if (text.empty()) {
        return state;
    }
    addon::Json_parse_error parse_error;
    const std::optional<addon::Json_value> object = addon::parse_json(text, addon::Json_parse_limits{}, &parse_error);
    if (!object.has_value() || !object->is_object()) {
        return std::nullopt;
    }
    const auto flag = [&object](const char* name, bool& out) {
        const addon::Json_value* value = object->find(name);
        if (value == nullptr || !value->is_boolean()) {
            return false;
        }
        out = value->as_boolean();
        return true;
    };
    const auto bytes = [&object](const char* name, std::string& out) {
        const addon::Json_value* value = object->find(name);
        const std::optional<std::string> decoded =
            value != nullptr && value->is_string() ? addon::decode_base64(value->as_string()) : std::nullopt;
        if (!decoded.has_value()) {
            return false;
        }
        out = *decoded;
        return true;
    };
    if (!flag("header_seen",      state.header_seen)      ||
        !flag("in_result",        state.in_result)        ||
        !flag("result_complete",  state.result_complete)  ||
        !flag("result_truncated", state.result_truncated) ||
        !flag("discarding_line",  state.discarding_line)  ||
        !bytes("result_base64",   state.result)           ||
        !bytes("partial_base64",  state.partial)          ||
        (object->find("result_file_base64") != nullptr &&
         !bytes("result_file_base64", state.result_file)) ||
        (object->find("observed_model_base64") != nullptr &&
         !bytes("observed_model_base64", state.observed_model)))
    {
        return std::nullopt;
    }
    return state;
}

std::optional<std::string> read_result_file(const Inspect_state& state)
{
    if (state.result_file.empty()) {
        return std::nullopt;
    }
    std::ifstream input(addon::path_from_utf8(state.result_file), std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    // One byte beyond the largest selected result detects excess without
    // allowing a native output path to cause an unbounded allocation.
    constexpr std::size_t max_result_bytes = 81920 + 3 + 9;
    std::string bytes(max_result_bytes + 1, '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    const auto count = static_cast<std::size_t>(input.gcount());
    if (input.bad() || count > max_result_bytes) {
        return std::nullopt;
    }
    bytes.resize(count);
    return bytes;
}

void note_presentation(std::vector<addon::Presentation_event>& presentation, addon::Presentation_event_kind kind)
{
    for (addon::Presentation_event& event : presentation) {
        if (event.kind == kind) {
            ++event.count;
            return;
        }
    }
    presentation.push_back({kind, 1});
}

void consume_protocol_line(
    Inspect_state&                          state,
    std::string_view                        line,
    std::vector<addon::Presentation_event>& presentation)
{
    if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
    }
    if (line.rfind(k_protocol_header_prefix, 0) == 0) {
        state.header_seen = true;
        return;
    }
    if (line == k_protocol_result_begin) {
        state.in_result        = true;
        state.result_complete  = false;
        state.result_truncated = false;
        state.result.clear();
        return;
    }
    if (line == k_protocol_result_end && state.in_result) {
        state.in_result       = false;
        state.result_complete = true;
        return;
    }
    if (!state.in_result && line.rfind(k_protocol_result_file_prefix, 0) == 0) {
        state.result_file = std::string(line.substr(std::strlen(k_protocol_result_file_prefix)));
        return;
    }
    if (!state.in_result && line.starts_with("model ")) {
        const auto model = line.substr(6);
        if (addon::is_valid_model_label(model)) {
            state.observed_model = model;
        }
        return;
    }
    if (state.in_result) {
        if (state.result.size() + line.size() + 1 > k_max_helper_result_bytes) {
            state.result_truncated = true;
        }
        else {
            state.result.append(line);
            state.result.push_back('\n');
        }
        return;
    }
    if (line.rfind(k_protocol_presentation_prefix, 0) == 0) {
        const std::optional<addon::Presentation_event_kind> kind =
            addon::parse_presentation_event_kind(line.substr(std::strlen(k_protocol_presentation_prefix)));
        if (kind.has_value()) {
            note_presentation(presentation, *kind);
        }
    }
}

// Feeds the stdout bytes accumulated since the previous inspection through
// the line protocol, collecting the presentation records of this increment.
void feed_inspection(
    Inspect_state&                          state,
    std::string_view                        bytes,
    std::vector<addon::Presentation_event>& presentation)
{
    std::string buffer = state.partial;
    buffer.append(bytes);
    state.partial.clear();

    std::size_t begin = 0;
    while (true) {
        const std::size_t newline = buffer.find('\n', begin);
        if (newline == std::string::npos) {
            break;
        }
        const std::string_view line(buffer.data() + begin, newline - begin);
        if (state.discarding_line) {
            state.discarding_line = false;   // the overlong line ends here
        }
        else
        if (line.size() <= k_max_protocol_line_bytes) {
            consume_protocol_line(state, line, presentation);
        }
        begin = newline + 1;
    }
    const std::string_view remainder(buffer.data() + begin, buffer.size() - begin);
    if (state.discarding_line || remainder.size() > k_max_protocol_line_bytes) {
        state.discarding_line = true;
    }
    else {
        state.partial = std::string(remainder);
    }
}

std::string exit_failure_reason(std::int32_t exit_code)
{
    return exit_code < 0
        ? "process_exited_minus_" + std::to_string(-(std::int64_t)exit_code)
        : "process_exited_" + std::to_string(exit_code);
}

// The failure a final inspection reports, or nullopt when the run produced
// its result.
std::optional<std::string> final_failure_reason(
    const Inspect_state&                state,
    const addon::Structured_inspection& inspection)
{
    if (inspection.timed_out)      { return "process_timed_out"; }
    if (inspection.exit_code != 0) { return exit_failure_reason(inspection.exit_code); }
    if (!state.header_seen)        { return "startup_failed"; }
    if (!state.result_complete)    { return "incomplete_output"; }
    if (state.result_truncated)    { return "result_too_large"; }
    return std::nullopt;
}

// The sessions this adapter has been asked to observe, by token. A poll
// names its session through the cursor; an empty cursor means the session
// observed most recently, from its first observation.
class Observed_sessions
{
public:
    struct Session
    {
        std::string token;
        std::string working_directory;
    };

    std::string observe(const std::string& working_directory)
    {
        const std::string token =
            addon::to_hex(addon::Sha256::digest(working_directory)).substr(0, k_session_token_hex_chars);
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_directories[token] = working_directory;
        m_latest             = token;
        return token;
    }

    std::optional<Session> resolve(const std::string& token) const
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_directories.find(token.empty() ? m_latest : token);
        if (found == m_directories.end()) {
            return std::nullopt;
        }
        return Session{found->first, found->second};
    }

private:
    mutable std::mutex                 m_mutex;
    std::map<std::string, std::string> m_directories;
    std::string                        m_latest;
};

struct Observation_cursor
{
    std::string   token;   // empty: the most recently observed session
    std::uint64_t index = 0;
};

std::string encode_cursor(const std::string& token, std::uint64_t index)
{
    return token + ":" + std::to_string(index);
}

std::optional<Observation_cursor> decode_cursor(const std::string& text)
{
    Observation_cursor cursor;
    if (text.empty()) {
        return cursor;
    }
    const std::size_t colon = text.find(':');
    if (colon != k_session_token_hex_chars || colon + 1 >= text.size() || text.size() - colon - 1 > 20) {
        return std::nullopt;
    }
    const std::string_view token(text.data(), colon);
    const bool token_is_hex = std::all_of(token.begin(), token.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
    if (!token_is_hex) {
        return std::nullopt;
    }
    std::uint64_t index = 0;
    for (std::size_t i = colon + 1; i < text.size(); ++i) {
        const char c = text[i];
        if (c < '0' || c > '9' || index > (UINT64_MAX - (std::uint64_t)(c - '0')) / 10) {
            return std::nullopt;
        }
        index = index * 10 + (std::uint64_t)(c - '0');
    }
    cursor.token = std::string(token);
    cursor.index = index;
    return cursor;
}

fs::path events_path(const std::string& working_directory)
{
    return addon::path_from_utf8(working_directory) / k_events_file_name;
}

// The complete lines of the events file, those terminated by '\n'; a partial
// last line is not yet an observation, and a missing file has none.
std::vector<std::string> read_complete_lines(const fs::path& path)
{
    std::vector<std::string> lines;
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return lines;
    }
    std::string line;
    while (std::getline(input, line)) {
        if (input.eof()) {
            break;
        }
        lines.push_back(line);
    }
    return lines;
}

std::optional<addon::Lifecycle_observation> decode_events_line(
    const std::string&       line,
    std::size_t              line_number,
    addon::Contract_failure* out_failure)
{
    const std::string where = "line " + std::to_string(line_number) + " of " + k_events_file_name;
    addon::Json_parse_error parse_error;
    const std::optional<addon::Json_value> value = addon::parse_json(line, addon::Json_parse_limits{}, &parse_error);
    if (!value.has_value()) {
        out_failure->reason_code = addon::reason::k_observation_invalid;
        out_failure->detail      = where + " is not JSON: " + parse_error.message;
        return std::nullopt;
    }
    std::optional<addon::Lifecycle_observation> observation = addon::decode_lifecycle_observation(*value, out_failure, addon::Lifecycle_observation_policy::CONTINUOUS);
    if (!observation.has_value()) {
        out_failure->detail = where + ": " + out_failure->detail;
    }
    return observation;
}

// A string member of a hook body, empty when absent or not a string; false
// when present but beyond the bound.
bool bounded_body_string(const addon::Json_value& body, const char* member, std::size_t bound, std::string& out)
{
    const addon::Json_value* value = body.find(member);
    if (value == nullptr || !value->is_string()) {
        out.clear();
        return true;
    }
    if (value->as_string().size() > bound || has_control_characters(value->as_string())) {
        return false;
    }
    out = value->as_string();
    return true;
}

// The observation every classification starts from: the event's identity is
// the evidence digest, so a retried delivery of the same event names the
// same observation.
addon::Lifecycle_observation base_observation(const addon::Provider_event& event)
{
    addon::Lifecycle_observation observation;
    observation.provider_event_name = event.name;
    observation.provider_event_id   = addon::provider_event_evidence_digest_hex(event);
    const addon::Json_value* sequence = event.body.find("sequence");
    if (sequence != nullptr && sequence->is_integer() && sequence->as_integer() >= 0) {
        observation.provider_sequence = std::to_string(sequence->as_integer());
    }
    return observation;
}

// The resume record this fixture keeps for a session, the host's default
// `{"provider_session_id": ...}`: what a started observation carries and
// what prepare_resume acknowledges.
addon::Json_value session_record(const std::string& provider_session_id)
{
    addon::Json_value record = addon::Json_value::object();
    record.set("provider_session_id", addon::Json_value(provider_session_id));
    return record;
}

// The quota modes that report a classified failure.
std::optional<addon::Quota_failure> quota_mode_failure(const std::string& mode)
{
    if (mode == "quota-invalid-response")    { return addon::Quota_failure::INVALID_RESPONSE;    }
    if (mode == "quota-request-failed")      { return addon::Quota_failure::REQUEST_FAILED;      }
    if (mode == "quota-timed-out")           { return addon::Quota_failure::TIMED_OUT;           }
    if (mode == "quota-credentials-expired") { return addon::Quota_failure::CREDENTIALS_EXPIRED; }
    return std::nullopt;
}

// An explanation a different package version could send. Each mode spoils one
// member, so the host must discard it whole.
addon::Json_value unusable_quota_explanation(const std::string& mode)
{
    auto explanation = addon::Json_value::object();
    explanation.set("version", addon::Json_value(mode == "quota-unusable-version" ? 17 : 1));
    explanation.set("reason", addon::Json_value(
        mode == "quota-unusable-reason" ? "private_unknown_reason" : "quota_request_failed"));
    auto diagnostic = addon::Json_value::object();
    diagnostic.set("stage", addon::Json_value(
        mode == "quota-unusable-stage" ? "private_unknown_stage" : "http_response"));
    diagnostic.set("code", mode == "quota-unusable-diagnostic"
        ? addon::Json_value("private_unknown_code") : addon::Json_value(503));
    explanation.set("diagnostic", std::move(diagnostic));
    return explanation;
}

struct Observation_options
{
    std::uint16_t auth_version  = 0;
    std::uint16_t quota_version = 0;
    std::uint16_t account_refresh_version = 0;
    std::uint16_t usage_version = 0;
    std::uint16_t resume_version = addon::k_interactive_resume_environment_version;
    std::uint16_t session_declaration_version = addon::k_structured_session_version;
    bool          accounts      = false;
    std::string   mode          = "success";
    fs::path      export_enabled_file;
};

enum class Lifetime_response
{
    NORMAL,
    HOLD,
    REJECT,
};

struct Lifetime_controls
{
    Lifetime_response reserve  = Lifetime_response::NORMAL;
    Lifetime_response retire   = Lifetime_response::NORMAL;
    Lifetime_response observe  = Lifetime_response::NORMAL;
    Lifetime_response classify = Lifetime_response::NORMAL;
    bool              terminal = false;
};

struct Lifetime_call
{
    std::string                operation;
    std::string                transition_nonce;
    std::optional<std::string> resume_cursor = std::nullopt;
    std::optional<std::string> disposition = std::nullopt;
    std::optional<std::string> consumed_cursor = std::nullopt;
};

class Fixture
{
public:
    Fixture(Behavior behavior, std::string executable, bool classify_hang, bool resume_foreign_digest,
        Observation_options observations)
    :
        m_behavior(behavior),
        m_executable(std::move(executable)),
        m_classify_hang(classify_hang),
        m_resume_foreign_digest(resume_foreign_digest),
        m_observations(std::move(observations))
    {}

    int run()
    {
        if (m_behavior == Behavior::STDIN_STALL) {
            return run_stdin_stall();
        }
        addon::Adapter_runtime_options options;
        options.adapter_name    = "unknown-fixture-adapter";
        options.adapter_version = std::to_string(addon::k_sdk_version_major) + "." +
                                  std::to_string(addon::k_sdk_version_minor) + "." +
                                  std::to_string(addon::k_sdk_version_patch);
        if (m_behavior != Behavior::SIDECAR_WITHOUT_HANDSHAKE) {
            options.capabilities.push_back(k_profile_storage);
        }
        options.capabilities.push_back(k_probe);
        options.capabilities.push_back(k_unadopted);
        if (m_behavior == Behavior::PROVIDER_SURFACES || m_behavior == Behavior::PROVIDER_OBSERVATIONS ||
            provider_managed_configuration() || provider_sessions() || initial_prompt())
        {
            options.capabilities.push_back(k_presentation);
            options.capabilities.push_back(k_launcher_discovery);
            options.capabilities.push_back({addon::k_launch_preparation_capability_id, addon::k_launch_preparation_execution_version});
        }
        if (initial_prompt()) {
            options.capabilities.push_back({addon::k_interactive_initial_prompt_capability_id,
                addon::k_interactive_initial_prompt_version});
        }
        if (m_behavior == Behavior::PROVIDER_SURFACES) {
            options.capabilities.push_back({addon::k_launcher_approval_capability_id,
                addon::k_launcher_approval_version});
        }
        if (provider_managed_configuration()) {
            if (m_behavior == Behavior::PROVIDER_ACCOUNT_MANAGED_CONFIGURATION) {
                options.capabilities.push_back({addon::k_managed_configuration_capability_id,
                    addon::k_managed_configuration_version});
                options.capabilities.push_back({addon::k_managed_configuration_capability_id,
                    addon::k_managed_configuration_selector_version});
            }
            else {
                options.capabilities.push_back({addon::k_managed_configuration_capability_id,
                    m_behavior == Behavior::PROVIDER_MANAGED_CONFIGURATION_V2
                        ? addon::k_managed_configuration_selector_version
                        : addon::k_managed_configuration_version});
            }
        }
        if (m_behavior == Behavior::PROVIDER_ACCOUNT_MANAGED_CONFIGURATION) {
            options.capabilities.push_back({addon::k_accounts_capability_id, 2});
        }
        else if (m_behavior == Behavior::PROVIDER_ACCOUNTS || m_observations.accounts) {
            options.capabilities.push_back(k_accounts);
        }
        if (m_observations.auth_version != 0) {
            options.capabilities.push_back({addon::k_auth_status_capability_id, m_observations.auth_version});
        }
        if (m_observations.mode == "current-account") {
            options.capabilities.push_back({addon::k_current_account_capability_id, addon::k_current_account_version});
        }
        if (m_observations.quota_version != 0) {
            options.capabilities.push_back({addon::k_quota_capability_id, m_observations.quota_version});
        }
        if (m_observations.account_refresh_version != 0) {
            options.capabilities.push_back({addon::k_account_refresh_capability_id, m_observations.account_refresh_version});
        }
        if (m_observations.usage_version != 0) {
            options.capabilities.push_back({addon::k_usage_scan_capability_id, m_observations.usage_version});
        }
        for (const char* capability : {addon::k_usage_lifetime_summary_capability_id,
                 addon::k_usage_saved_daily_tokens_capability_id, addon::k_usage_temporal_capability_id,
                 addon::k_usage_session_capability_id, addon::k_usage_repository_attribution_capability_id,
                 addon::k_usage_owned_event_correction_capability_id, addon::k_quota_failure_capability_id,
                 addon::k_quota_cost_capability_id, addon::k_account_pet_capability_id,
                 addon::k_structured_failure_diagnostics_capability_id,
                 addon::k_model_state_capability_id, addon::k_structured_model_observation_capability_id,
                 addon::k_session_observation_capability_id,
                 addon::k_delegated_model_selection_capability_id, "terminal-identity"})
        {
            const fs::path sidecar = addon::path_from_utf8(m_executable).parent_path().parent_path() /
                addon::sidecar_relative_path(capability, 1);
            std::error_code error;
            if (fs::is_regular_file(sidecar, error)) {
                options.capabilities.push_back({capability, 1});
            }
        }
        if (provider_sessions()) {
            options.capabilities.push_back({addon::k_structured_session_capability_id,
                m_observations.session_declaration_version});
            options.capabilities.push_back({addon::k_delegated_session_capability_id,
                m_observations.session_declaration_version});
            options.capabilities.push_back({addon::k_interactive_resume_capability_id, m_observations.resume_version});
            for (const auto version : {addon::k_interactive_resume_inspection_version,
                     addon::k_interactive_resume_content_version})
            {
                if (version == m_observations.resume_version) {
                    continue;
                }
                const fs::path inspection_sidecar = addon::path_from_utf8(m_executable)
                    .parent_path().parent_path() / addon::sidecar_relative_path(
                        addon::k_interactive_resume_capability_id, version);
                std::error_code inspection_error;
                if (fs::is_regular_file(inspection_sidecar, inspection_error)) {
                    options.capabilities.push_back({addon::k_interactive_resume_capability_id, version});
                }
            }
            options.capabilities.push_back(k_lifecycle_events);
            options.capabilities.push_back(k_coordination_delivery);
            options.capabilities.push_back(k_helper_ingress_degradation);
            const fs::path snapshot_sidecar = addon::path_from_utf8(m_executable)
                .parent_path().parent_path() / addon::sidecar_relative_path(
                    addon::k_permission_snapshot_capability_id, addon::k_permission_snapshot_version);
            std::error_code snapshot_error;
            if (fs::is_regular_file(snapshot_sidecar, snapshot_error)) {
                options.capabilities.push_back({addon::k_permission_snapshot_capability_id,
                    addon::k_permission_snapshot_version});
                options.capabilities.push_back({addon::k_managed_configuration_capability_id,
                    addon::k_managed_configuration_home_version});
            }
            // Reconciliation fixtures opt in through their installed sidecar;
            // the shared adapter must not advertise an undeclared pair.
            const fs::path reconciliation_sidecar = addon::path_from_utf8(m_executable)
                .parent_path().parent_path() / addon::sidecar_relative_path(
                    addon::k_helper_ingress_capability_id,
                    addon::k_helper_ingress_subagent_reconciliation_version);
            std::error_code reconciliation_error;
            if (fs::is_regular_file(reconciliation_sidecar, reconciliation_error)) {
                options.capabilities.push_back(k_helper_ingress_subagent_reconciliation);
            }
            if (lifetime_sessions()) {
                options.capabilities.push_back(k_lifecycle_correlation_lifetime);
                options.capabilities.push_back({addon::k_session_wake_capability_id, addon::k_session_wake_version});
            }
        }
        if (m_behavior == Behavior::PROVIDER_SESSIONS) {
            options.capabilities.push_back({addon::k_model_context_delivery_capability_id, addon::k_model_context_delivery_version});
            const fs::path activation_sidecar = addon::path_from_utf8(m_executable)
                .parent_path().parent_path() / addon::sidecar_relative_path(
                    addon::k_model_context_delivery_capability_id, addon::k_model_context_delivery_activation_version);
            std::error_code activation_error;
            if (fs::is_regular_file(activation_sidecar, activation_error)) {
                options.capabilities.push_back({addon::k_model_context_delivery_capability_id,
                    addon::k_model_context_delivery_activation_version});
            }
        }
        // Host export tests install this optional sidecar in their own copy.
        // Existing fixture packages keep their original capability surface.
        for (const auto version : {addon::k_conversation_export_version,
                 addon::k_conversation_export_prepared_source_version})
        {
            const fs::path conversation_sidecar = addon::path_from_utf8(m_executable)
                .parent_path().parent_path() / addon::sidecar_relative_path(
                    addon::k_conversation_export_capability_id, version);
            std::error_code conversation_error;
            if (fs::is_regular_file(conversation_sidecar, conversation_error) &&
                (m_observations.export_enabled_file.empty() ||
                    fs::is_regular_file(m_observations.export_enabled_file, conversation_error)))
            {
                options.capabilities.push_back({addon::k_conversation_export_capability_id, version});
            }
        }
        const fs::path context_sidecar = addon::path_from_utf8(m_executable)
            .parent_path().parent_path() / addon::sidecar_relative_path(
                addon::k_context_state_capability_id, addon::k_context_state_capability_version);
        std::error_code context_error;
        if (fs::is_regular_file(context_sidecar, context_error)) {
            options.capabilities.push_back({addon::k_context_state_capability_id,
                addon::k_context_state_capability_version});
        }
        const fs::path execution_sidecar = addon::path_from_utf8(m_executable)
            .parent_path().parent_path() / addon::sidecar_relative_path(
                addon::k_execution_context_capability_id, addon::k_execution_context_version);
        std::error_code execution_error;
        if (fs::is_regular_file(execution_sidecar, execution_error)) {
            options.capabilities.push_back({addon::k_execution_context_capability_id,
                addon::k_execution_context_version});
        }
        const fs::path resume_directory_sidecar = addon::path_from_utf8(m_executable)
            .parent_path().parent_path() / addon::sidecar_relative_path(
                addon::k_resume_directory_capability_id, addon::k_resume_directory_version);
        std::error_code resume_directory_error;
        if (fs::is_regular_file(resume_directory_sidecar, resume_directory_error)) {
            options.capabilities.push_back({addon::k_resume_directory_capability_id, addon::k_resume_directory_version});
        }
        if (initial_prompt() ||
            (provider_sessions() && m_observations.resume_version == addon::k_interactive_resume_environment_version))
        {
            const fs::path sidecar = addon::path_from_utf8(m_executable).parent_path().parent_path() /
                addon::sidecar_relative_path(addon::k_terminal_codex_invocation_capability_id,
                    addon::k_terminal_codex_invocation_version);
            std::error_code error;
            if (fs::is_regular_file(sidecar, error)) {
                options.capabilities.push_back({addon::k_terminal_codex_invocation_capability_id,
                    addon::k_terminal_codex_invocation_version});
            }
        }
        if (m_behavior == Behavior::PROVIDER_SESSIONS_ASSETS) {
            options.capabilities.push_back(k_managed_assets);
        }
        if (m_behavior == Behavior::HANDSHAKE_WITHOUT_SIDECAR) {
            options.capabilities.push_back(addon::Capability_pair{"org.example.extra", 1});
        }

        addon::Adapter_runtime runtime(std::move(options));
        m_runtime = &runtime;
        runtime.set_hello_hook([this](const addon::Host_hello& host, addon::Adapter_hello& hello) {
            m_package_id = host.package_id;
            switch (m_behavior) {
                case Behavior::HANG_ON_HELLO:
                    hang_forever();
                case Behavior::WRONG_NONCE_HELLO:
                    hello.generation_nonce[0] = hello.generation_nonce[0] == '0' ? '1' : '0';
                    break;
                case Behavior::WRONG_CORE_GENERATION:
                    hello.core_protocol_generation = (std::uint16_t)(host.core_protocol_generations.back() + 1);
                    break;
                default:
                    break;
            }
        });
        runtime.set_shutdown_hook([this](const addon::Shutdown_request& shutdown) {
            if (m_observations.mode == "crash-on-shutdown") {
                std::_Exit(23);
            }
            if (m_behavior == Behavior::SLOW_SHUTDOWN || m_observations.mode == "slow-shutdown") {
                sleep_ms((int)shutdown.grace_ms * 4);
            }
        });
        runtime.set_invoke_handler([this](addon::Invocation_context& context) { handle(context); });
        return runtime.run();
    }

private:
    bool provider_managed_configuration() const
    {
        return m_behavior == Behavior::PROVIDER_MANAGED_CONFIGURATION_V1 ||
            m_behavior == Behavior::PROVIDER_MANAGED_CONFIGURATION_V2 ||
            m_behavior == Behavior::PROVIDER_ACCOUNT_MANAGED_CONFIGURATION;
    }

    bool provider_sessions() const
    {
        return m_behavior == Behavior::PROVIDER_ACCOUNTS        ||
            m_behavior == Behavior::PROVIDER_SESSIONS        ||
            m_behavior == Behavior::PROVIDER_SESSIONS_ASSETS    ||
            m_behavior == Behavior::PROVIDER_SESSIONS_CONTINUOUS ||
            m_behavior == Behavior::PROVIDER_SESSIONS_LIFETIME   ||
            m_behavior == Behavior::PROVIDER_SESSIONS_STDIN_EOF;
    }

    bool lifetime_sessions() const
    {
        return m_behavior == Behavior::PROVIDER_SESSIONS_LIFETIME;
    }

    bool account_managed_configuration() const
    {
        return m_behavior == Behavior::PROVIDER_ACCOUNT_MANAGED_CONFIGURATION;
    }

    bool initial_prompt() const
    {
        return m_behavior == Behavior::PROVIDER_INITIAL_PROMPT;
    }

    void add_session_context(
        addon::Process_proposal&             process,
        const addon::Session_launch_context& context) const
    {
        if (m_behavior == Behavior::PROVIDER_ACCOUNTS) {
            if (context.profile_storage) {
                process.environment.emplace_back("UNKNOWN_FIXTURE_PROFILE_STORAGE", context.profile_storage->root);
            }
            if (context.account_context) {
                process.environment.emplace_back("UNKNOWN_FIXTURE_ACCOUNT_KEY", context.account_context->adapter_account_key);
            }
            return;
        }
        if (m_behavior != Behavior::PROVIDER_SESSIONS_ASSETS) {
            return;
        }
        process.assets.push_back("assets/mark.svg");
        process.environment.emplace_back(
            "UNKNOWN_FIXTURE_ASSET",
            "{{asset:assets/mark.svg}}");
        if (context.profile_storage) {
            process.environment.emplace_back(
                "UNKNOWN_FIXTURE_PROFILE_STORAGE",
                context.profile_storage->root);
        }
    }

    void add_session_patch_context(
        addon::Launch_patch&                              patch,
        const std::optional<addon::Profile_storage_context>& profile_storage) const
    {
        if (m_behavior == Behavior::PROVIDER_ACCOUNTS) {
            if (profile_storage) {
                patch.set.emplace_back("UNKNOWN_FIXTURE_PROFILE_STORAGE", profile_storage->root);
            }
            if (patch.account_context) {
                patch.set.emplace_back("UNKNOWN_FIXTURE_ACCOUNT_KEY", patch.account_context->adapter_account_key);
            }
            return;
        }
        if (m_behavior != Behavior::PROVIDER_SESSIONS_ASSETS) {
            return;
        }
        patch.set.emplace_back(
            "UNKNOWN_FIXTURE_ASSET",
            "{{asset:assets/mark.svg}}");
        if (profile_storage) {
            patch.set.emplace_back(
                "UNKNOWN_FIXTURE_PROFILE_STORAGE",
                profile_storage->root);
        }
    }

    int run_stdin_stall()
    {
#if defined(_WIN32)
        (void)::_setmode(::_fileno(stdin), _O_BINARY);
        (void)::_setmode(::_fileno(stdout), _O_BINARY);
#endif
        std::string frame;
        while (frame.size() <= addon::limits::k_max_frame_bytes) {
            const int byte = std::fgetc(stdin);
            if (byte == EOF) {
                return 51;
            }
            if (byte == '\n') {
                break;
            }
            frame.push_back(static_cast<char>(byte));
        }
        addon::Contract_failure failure;
        const std::optional<addon::Host_message> message =
            addon::decode_host_message(frame, addon::Json_parse_limits{}, &failure);
        const addon::Host_hello* host = message ? std::get_if<addon::Host_hello>(&*message) : nullptr;
        if (host == nullptr || host->core_protocol_generations.empty()) {
            return 52;
        }

        addon::Adapter_hello hello;
        hello.core_protocol_generation = host->core_protocol_generations.front();
        hello.package_id       = host->package_id;
        hello.generation_digest = host->generation_digest;
        hello.generation_nonce = host->generation_nonce;
        hello.adapter_name     = "unknown-fixture-adapter";
        hello.adapter_version  = "1.0.0";
        addon::Capabilities_message capabilities;
        capabilities.capabilities = {k_profile_storage, k_probe, k_unadopted};
        const std::string output =
            addon::encode_frame(addon::encode_adapter_message(addon::Adapter_message(hello))) +
            addon::encode_frame(addon::encode_adapter_message(addon::Adapter_message(capabilities)));
        if (std::fwrite(output.data(), 1, output.size(), stdout) != output.size() || std::fflush(stdout) != 0) {
            return 53;
        }
        hang_forever();
    }

    void handle(addon::Invocation_context& context)
    {
        switch (m_behavior) {
            case Behavior::CRASH_ON_INVOKE:
                crash();
            case Behavior::HANG_ON_INVOKE:
                hang_forever();
            case Behavior::SLOW_RESULT:
                sleep_ms(k_slow_result_delay_ms);
                break;
            case Behavior::STDERR_FLOOD: {
                constexpr char k_live_stderr[] = "live stderr before flood\n";
                (void)std::fwrite(k_live_stderr, 1, sizeof(k_live_stderr) - 1, stderr);
                std::fflush(stderr);
                sleep_ms(250);
                const std::string line(1024, 'e');
                for (int i = 0; i < 1024; ++i) {
                    std::fprintf(stderr, "%s\n", line.c_str());
                    if (i == 511) {
                        // Make the host publish at least two overflow
                        // snapshots. Its retained value must replace the
                        // earlier snapshot instead of appending it.
                        std::fflush(stderr);
                        sleep_ms(100);
                    }
                }
                std::fflush(stderr);
                sleep_ms(100);
                break;
            }
            case Behavior::STDOUT_GARBAGE:
                m_runtime->write_raw("this is not a frame\n");
                break;
            case Behavior::OVERSIZE_FRAME: {
                std::string frame = "{\"type\":\"progress\",\"request_id\":\"" + context.request().request_id + "\",\"completed\":1,\"note\":\"";
                frame.append((std::size_t)m_runtime->effective_limits().max_frame_bytes, 'o');
                frame += "\"}\n";
                m_runtime->write_raw(frame);
                break;
            }
            case Behavior::INVALID_UTF8_FRAME:
                m_runtime->write_raw("{\"type\":\"progress\",\"request_id\":\"" + context.request().request_id +
                    "\",\"completed\":1,\"note\":\"\xFF\xFE\"}\n");
                break;
            case Behavior::BIDIRECTIONAL_FLOOD:
                if (context.request().operation == "inbound_flood") {
                    addon::Progress_message progress;
                    progress.request_id = context.request().request_id;
                    progress.note = std::string(addon::limits::k_max_progress_note_bytes, 'p');
                    const std::string frame = addon::encode_frame(
                        addon::encode_adapter_message(addon::Adapter_message(progress)));
                    std::string batch;
                    for (int i = 0; i < 128; ++i) {
                        batch += frame;
                    }
                    // Exceed the host's transport budget even when it keeps
                    // draining the pipe. Pausing between small batches keeps
                    // this a receiver-backlog test, not a supervisor pipe test.
                    const std::size_t total_bytes = 8 * addon::limits::k_max_frame_bytes;
                    for (std::size_t emitted = 0; emitted < total_bytes; emitted += batch.size()) {
                        m_runtime->write_raw(batch);
                        sleep_ms(2);
                    }
                    hang_forever();
                }
                if (context.request().operation == "flood") {
                    const std::size_t bound = std::min<std::size_t>(
                        static_cast<std::size_t>(m_runtime->effective_limits().max_progress_frames_per_request),
                        4096);
                    const std::string note(1024, 'p');
                    for (std::size_t i = 0; i < bound; ++i) {
                        context.report_progress(i, bound, note);
                    }
                    hang_forever();
                }
                break;
            case Behavior::RESULT_THEN_EXIT: {
                const addon::Json_value* queued_progress =
                    context.request().payload.find("progress_before_result");
                if (queued_progress && queued_progress->is_boolean() && queued_progress->as_boolean()) {
                    const std::string note(addon::limits::k_max_progress_note_bytes, 'p');
                    for (std::size_t i = 0; i < 512; ++i) {
                        context.report_progress(i, 512, note);
                    }
                }
                break;
            }
            default:
                break;
        }

        const addon::Invoke_request& request = context.request();
        if (request.capability == addon::Capability_pair{
                addon::k_account_refresh_capability_id, addon::k_account_refresh_version})
        {
            handle_account_refresh(context);
            return;
        }
        if (request.capability == k_profile_storage) {
            handle_profile_storage(context);
        }
        else
        if (request.capability == k_probe) {
            handle_probe(context);
        }
        else
        if (request.capability == addon::Capability_pair{addon::k_auth_status_capability_id, addon::k_auth_status_environment_version} ||
            request.capability == addon::Capability_pair{addon::k_quota_capability_id, addon::k_quota_exchange_version} ||
            request.capability == addon::Capability_pair{addon::k_quota_capability_id, addon::k_quota_collection_version})
        {
            handle_observation(context);
            return;
        }
        else
        if (request.capability == addon::Capability_pair{addon::k_usage_scan_capability_id, addon::k_usage_scan_acceptance_version} ||
            request.capability == addon::Capability_pair{addon::k_usage_repository_attribution_capability_id,
                addon::k_usage_repository_attribution_version} ||
            request.capability == addon::Capability_pair{addon::k_usage_owned_event_correction_capability_id,
                addon::k_usage_owned_event_correction_version})
        {
            handle_usage_scan(context);
            return;
        }
        else
        if (request.capability == addon::Capability_pair{addon::k_usage_session_capability_id, addon::k_usage_session_version}) {
            handle_usage_session(context);
            return;
        }
        else
        if (request.capability.capability_id == addon::k_usage_lifetime_summary_capability_id ||
            request.capability.capability_id == addon::k_usage_saved_daily_tokens_capability_id)
        {
            handle_usage_summary(context);
            return;
        }
        else
        if (request.capability == k_accounts) {
            handle_accounts(context);
            return;
        }
        else
        if (request.capability == k_launcher_discovery) {
            handle_launcher_discovery(context);
        }
        else
        if (request.capability.capability_id == addon::k_launcher_approval_capability_id) {
            handle_launcher_approval(context);
        }
        else
        if (request.capability == k_launch_preparation) {
            handle_launch_preparation(context);
        }
        else
        if (request.capability.capability_id == addon::k_managed_configuration_capability_id) {
            handle_managed_configuration(context);
        }
        else
        if (request.capability == k_structured_session) {
            handle_structured_session(context);
        }
        else
        if (request.capability == k_delegated_session) {
            handle_delegated_session(context);
        }
        else
        if (request.capability == addon::Capability_pair{addon::k_interactive_resume_capability_id,
                addon::k_interactive_resume_environment_version} ||
            request.capability == addon::Capability_pair{addon::k_interactive_resume_capability_id,
                addon::k_interactive_resume_inspection_version} ||
            request.capability == addon::Capability_pair{addon::k_interactive_resume_capability_id,
                addon::k_interactive_resume_content_version})
        {
            handle_interactive_resume(context);
        }
        else
        if (request.capability == addon::Capability_pair{"terminal-identity", 1}) {
            handle_terminal_identity(context);
        }
        else
        if (request.capability == k_lifecycle_events) {
            handle_lifecycle_events(context);
        }
        else
        if (request.capability == k_lifecycle_correlation_lifetime) {
            handle_lifecycle_correlation_lifetime(context);
        }
        else
        if (request.capability == addon::Capability_pair{addon::k_session_wake_capability_id,
                addon::k_session_wake_version})
        {
            handle_session_wake(context);
        }
        else
        if (request.capability == addon::Capability_pair{addon::k_conversation_export_capability_id,
                addon::k_conversation_export_version} ||
            request.capability == addon::Capability_pair{addon::k_conversation_export_capability_id,
                addon::k_conversation_export_prepared_source_version})
        {
            addon::handle_conversation_export(context, [](const addon::Conversation_export_request& source,
                    const std::function<bool()>& cancelled, addon::Contract_failure* failure)
                    -> std::optional<addon::Conversation_export_result>
                {
                    addon::Conversation_writer writer(source, cancelled);
                    if (!writer.open(failure) || !writer.append({"user", {{addon::Conversation_block_kind::TEXT,
                            addon::serialize_json(addon::encode_conversation_export_request(source)), {}}}}, failure))
                    {
                        return std::nullopt;
                    }
                    if (source.provider_session_id == "cancel-export") {
                        std::ofstream started(addon::path_from_utf8(source.working_directory) /
                            "conversation-export-started.txt", std::ios::binary);
                        started << source.output_directory;
                        started.close();
                        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
                        while (!cancelled() && std::chrono::steady_clock::now() < deadline) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        }
                    }
                    auto result = writer.finish(false, failure);
                    if (result && source.provider_session_id == "invalid-export-descriptor") {
                        result->file_name = "../foreign.txt";
                    }
                    return result;
                });
        }
        else
        if (request.capability.capability_id == addon::k_permission_snapshot_capability_id) {
            handle_permission_snapshot(context);
        }
        else
        if (request.capability == k_helper_ingress_degradation ||
            request.capability == k_helper_ingress_subagent_reconciliation)
        {
            handle_helper_ingress(context);
        }
        else {
            context.reject_unknown_operation();
        }

        if (m_behavior == Behavior::DOUBLE_RESULT) {
            addon::Result_message duplicate;
            duplicate.request_id = request.request_id;
            duplicate.status     = addon::Result_status::SUCCESS;
            duplicate.payload    = addon::Json_value::object();
            m_runtime->write_raw(addon::encode_frame(addon::encode_adapter_message(addon::Adapter_message(duplicate))));
            // Make the fixture's post-violation process status deterministic:
            // both hosts must terminate it instead of racing a clean EOF.
            hang_forever();
        }
        if (m_behavior == Behavior::RESULT_THEN_EXIT || m_observations.mode == "result-then-exit") {
            constexpr char k_final_stderr[] = "result-then-exit stderr\n";
            (void)std::fwrite(k_final_stderr, 1, sizeof(k_final_stderr) - 1, stderr);
            std::fflush(stderr);
            std::_Exit(0);
        }
    }

    void handle_account_refresh(addon::Invocation_context& context)
    {
        const auto& request = context.request();
        addon::Contract_failure failure;
        if (request.operation == addon::k_prepare_operation) {
            const auto decoded = addon::decode_account_refresh_prepare_request(request.payload, &failure);
            if (!decoded) {
                context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
                return;
            }
            if (!decoded->observe_current_account) {
                context.fail(addon::Result_status::UNAVAILABLE, "fixture_refresh_requires_current_account");
                return;
            }
            addon::Structured_prepare_result result;
            result.process.executable = k_helper_executable;
            result.process.argv = {"account-refresh"};
            result.process.working_directory = addon::Working_directory_intent::SESSION_WORKSPACE;
            result.process.stdin_behavior = addon::Stdin_behavior::PAYLOAD;
            result.process.start = addon::Start_intent::ATTACHED;
            result.process.environment_names = {"PATH"};
            result.process.cleanup = addon::Cleanup_intent::REMOVE_SCRATCH;
            result.profile_storage = decoded->context.profile_storage;
            result.account_context = decoded->context.account_context;
            if (result.account_context) {
                result.process.environment.emplace_back(
                    "UNKNOWN_FIXTURE_ACCOUNT_KEY", result.account_context->adapter_account_key);
            }
            result.inspection.incremental = true;
            result.inspection.protocol_state = "{}";
            context.succeed(addon::encode_structured_prepare_result(result));
            return;
        }
        if (request.operation != addon::k_inspect_operation) {
            context.reject_unknown_operation();
            return;
        }
        const auto decoded = addon::decode_account_refresh_inspect_request(request.payload, &failure);
        if (!decoded) {
            context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
            return;
        }
        const auto& facts = decoded->inspection;
        auto state = addon::parse_json(facts.protocol_state, {}, nullptr);
        if (!state || !state->is_object()) {
            context.fail(addon::Result_status::FAILED, "fixture_refresh_state_invalid");
            return;
        }
        const auto* pending_value = state->find("pending");
        std::string pending = pending_value && pending_value->is_string() ? pending_value->as_string() : "";
        pending += facts.stdout_bytes;
        if (pending.size() > k_max_helper_result_bytes) {
            context.fail(addon::Result_status::FAILED, "fixture_refresh_output_bound");
            return;
        }
        addon::Account_refresh_inspect_result result;
        result.complete = facts.final;
        for (std::size_t newline; (newline = pending.find('\n')) != std::string::npos;) {
            const auto line = addon::parse_json(pending.substr(0, newline), {}, nullptr);
            pending.erase(0, newline + 1);
            const auto* kind = line ? line->find("kind") : nullptr;
            const auto* value = line ? line->find("value") : nullptr;
            if (!kind || !kind->is_string() || !value) {
                context.fail(addon::Result_status::FAILED, "fixture_refresh_record_invalid");
                return;
            }
            if (kind->as_string() == "quota") {
                if (result.quota) {
                    context.fail(addon::Result_status::FAILED, "fixture_refresh_duplicate_quota");
                    return;
                }
                result.quota = addon::decode_quota_inspect_result(*value, true, &failure, 2);
                if (!result.quota) {
                    context.fail(addon::Result_status::FAILED, "fixture_refresh_quota_invalid");
                    return;
                }
            }
            else if (kind->as_string() == "auth") {
                state->set("auth", *value);
            }
            else {
                context.fail(addon::Result_status::FAILED, "fixture_refresh_record_invalid");
                return;
            }
        }
        if (facts.final) {
            const auto* auth = state->find("auth");
            if (!pending.empty() || !auth) {
                context.fail(addon::Result_status::FAILED, "fixture_refresh_auth_missing");
                return;
            }
            result.auth = addon::decode_auth_inspect_result(*auth, true, &failure);
            if (!result.auth) {
                context.fail(addon::Result_status::FAILED, "fixture_refresh_auth_invalid");
                return;
            }
            if (!decoded->observe_current_account) {
                result.auth->current_account.reset();
            }
        }
        else {
            state->set("pending", addon::Json_value(std::move(pending)));
            result.protocol_state = addon::serialize_json(*state);
        }
        context.succeed(addon::encode_account_refresh_inspect_result(result));
    }

    void handle_observation(addon::Invocation_context& context)
    {
        const auto& request = context.request();
        const auto version = request.capability.version;
        const auto request_version = request.capability.capability_id == addon::k_quota_capability_id ? 2 : version;
        const bool incremental_quota = request.capability.capability_id == addon::k_quota_capability_id &&
            version == 2 && m_observations.mode == "incremental-quota";
        addon::Contract_failure failure;
        if (request.operation == addon::k_prepare_operation) {
            const auto decoded = addon::decode_probe_prepare_request(request.payload, &failure, request_version);
            if (!decoded) {
                context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
                return;
            }
            if (request.capability.capability_id == addon::k_quota_capability_id &&
                m_observations.mode == "quota-prepare-failed")
            {
                // The reason code is the package's disposition; the message is
                // adapter diagnostic text the host keeps out of its projection.
                context.fail(addon::Result_status::FAILED, "fixture_quota_prepare_failed",
                    "private fixture diagnostic");
                return;
            }
            addon::Structured_prepare_result result;
            result.process.executable        = k_helper_executable;
            result.process.argv              = {"observation", m_observations.mode};
            if (decoded->observe_current_account &&
                request.capability.capability_id == addon::k_auth_status_capability_id &&
                m_observations.mode == "current-account")
            {
                result.process.argv = {"observation", "current-account-auth"};
            }
            result.process.working_directory = addon::Working_directory_intent::SESSION_WORKSPACE;
            result.process.stdin_behavior    = addon::Stdin_behavior::PAYLOAD;
            result.process.start             = addon::Start_intent::ATTACHED;
            result.process.environment_names = {"PATH"};
            result.process.cleanup           = addon::Cleanup_intent::REMOVE_SCRATCH;
            result.profile_storage           = decoded->context.profile_storage;
            result.account_context           = decoded->context.account_context;
            if (result.account_context) {
                result.process.environment.emplace_back(
                    "UNKNOWN_FIXTURE_ACCOUNT_KEY", result.account_context->adapter_account_key);
            }
            if (incremental_quota) {
                result.process.argv = {"incremental-quota"};
                result.inspection.incremental = true;
                result.inspection.keep_stdin_open = true;
                result.inspection.protocol_state = "0";
            }
            context.succeed(addon::encode_structured_prepare_result(result));
            return;
        }
        if (request.operation != addon::k_inspect_operation) {
            context.reject_unknown_operation();
            return;
        }
        const auto decoded = addon::decode_probe_inspect_request(request.payload, &failure, request_version);
        if (!decoded) {
            context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
            return;
        }
        const auto& facts = decoded->inspection;
        if (incremental_quota) {
            addon::Quota_inspect_result result;
            std::string state = facts.protocol_state;
            bool invalid = state.size() != 1 || state[0] < '0' || state[0] > '3';
            for (char record : facts.stdout_bytes) {
                if (invalid || state[0] == '3' || record != state[0] + 1) {
                    invalid = true;
                    break;
                }
                state[0] = record;
                if (record == '1') result.stdin_bytes = "first\n";
                if (record == '2') result.stdin_bytes = "second\n";
            }
            result.complete = facts.final || invalid;
            if (!result.complete) {
                result.protocol_state = std::move(state);
            }
            else {
                result.stdin_bytes.reset();
                if (invalid || state != "3" || facts.timed_out || facts.exit_code != 0) {
                    context.fail(addon::Result_status::FAILED, "fixture_quota_exchange_failed");
                    return;
                }
                else {
                    addon::Quota_observation observation;
                    observation.state = addon::Quota_status::READY;
                    if (decoded->account_context) {
                        observation.scope.source_account_key = decoded->account_context->adapter_account_key;
                    }
                    observation.scope.observed_at_ms = static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count());
                    observation.scope.fresh_for_ms = 60000;
                    observation.primary = addon::Quota_window{37.5, 300, std::nullopt, "Primary"};
                    observation.secondary = addon::Quota_window{12.0, 10080, std::nullopt, "Secondary"};
                    observation.plan = "runtime-test-plan";
                    result.observation = std::move(observation);
                }
            }
            context.succeed(addon::encode_quota_inspect_result(result, version));
            return;
        }
        const bool auth = request.capability.capability_id == addon::k_auth_status_capability_id;
        if (!facts.final) {
            if (auth) {
                context.succeed(addon::encode_auth_inspect_result({false, {}, std::nullopt}));
            }
            else {
                context.succeed(addon::encode_quota_inspect_result({false, {}, std::nullopt}, version));
            }
            return;
        }
        if (facts.timed_out || facts.exit_code != 0) {
            context.fail(addon::Result_status::FAILED, "fixture_probe_failed", "private fixture diagnostic");
            return;
        }
        const auto payload = addon::parse_json(facts.stdout_bytes, addon::Json_parse_limits{}, nullptr);
        const auto* scope_payload = payload && auth && decoded->observe_current_account &&
            m_observations.mode == "current-account" ? payload->find("scope") : payload ? &*payload : nullptr;
        auto scope = scope_payload ? addon::decode_observation_scope(*scope_payload, &failure) : std::nullopt;
        if (!scope) {
            context.fail(addon::Result_status::FAILED, "fixture_probe_failed", "invalid helper observation");
            return;
        }
        if (m_observations.mode == "wrong-account") {
            scope->source_account_key = "wrong-account";
        }
        if (auth) {
            addon::Auth_observation observation;
            observation.scope = *scope;
            observation.state = m_observations.mode == "auth-error" ? addon::Auth_status::FAILED :
                m_observations.mode == "not-logged-in" ? addon::Auth_status::NOT_LOGGED_IN :
                addon::Auth_status::AUTHENTICATED;
            addon::Auth_inspect_result result{true, {}, observation};
            if (decoded->observe_current_account && m_observations.mode == "current-account") {
                if (const auto* current = payload->find("current_account")) {
                    result.current_account = addon::decode_current_account_observation(*current);
                }
            }
            context.succeed(addon::encode_auth_inspect_result(result));
        }
        else {
            addon::Quota_observation observation;
            observation.scope = *scope;
            if (m_observations.mode == "expiring") {
                observation.scope.fresh_for_ms = 1000;
            }
            const bool cost_mode = m_observations.mode.starts_with("quota-cost-");
            const bool cost_windows = m_observations.mode == "quota-cost-windows" ||
                m_observations.mode == "quota-cost-windows-failed";
            const bool changing_live_failure = m_observations.mode == "quota-cost-changing-live-failure";
            const auto cost_read = m_observations.mode == "quota-cost-changing" || changing_live_failure
                ? m_quota_observation_count.fetch_add(1) : 0;
            const auto failure = m_observations.mode == "quota-cost-live-failure" ||
                (changing_live_failure && cost_read > 0)
                ? std::optional{addon::Quota_failure::REQUEST_FAILED} : quota_mode_failure(m_observations.mode);
            observation.state =
                m_observations.mode == "quota-cost-not-logged-in"
                                                          ? addon::Quota_status::NOT_LOGGED_IN :
                (m_observations.mode == "quota-error" || failure || m_observations.mode.starts_with("quota-unusable-"))
                                                          ? addon::Quota_status::FAILED      :
                (m_observations.mode == "quota-unavailable" || (cost_mode && !cost_windows))
                                                          ? addon::Quota_status::UNAVAILABLE :
                                                             addon::Quota_status::READY;
            if (m_observations.mode == "quota-cost-wrong-account") {
                observation.scope.source_account_key = "another-cost-account";
            }
            if (observation.state == addon::Quota_status::READY) {
                if (version == addon::k_quota_collection_version) {
                    observation.windows = std::vector<addon::Quota_window>{
                        {25.5, std::nullopt, scope->observed_at_ms + 60000, "Week (all models)", std::nullopt, false},
                        {80, std::nullopt, std::nullopt, "Week (Fable)", std::nullopt, true},
                        {123.5, 90, std::nullopt, "New scope", "Later", false},
                    };
                    if (m_observations.mode == "quota-changing") {
                        const auto count = m_quota_observation_count.fetch_add(1);
                        if (count == 1) {
                            observation.windows->erase(observation.windows->begin());
                            observation.windows->resize(1);
                        }
                        else if (count > 1) {
                            observation.windows->clear();
                        }
                    }
                }
                else {
                    observation.primary = addon::Quota_window{25.5, 300, scope->observed_at_ms + 60000, "Five hours"};
                    observation.secondary = addon::Quota_window{80, 10080, std::nullopt, "Week"};
                }
                observation.plan            = "Fixture plan";
                observation.reset_credits   = 15;
                observation.credits_balance = "12.3400";
            }
            addon::Quota_inspect_result result{true, {}, observation};
            if (cost_mode) {
                if (m_observations.mode == "quota-cost-failed" ||
                    m_observations.mode == "quota-cost-windows-failed" ||
                    (!changing_live_failure && cost_read == 1))
                {
                    result.cost_failed = true;
                }
                else
                if (cost_read == 0 || (changing_live_failure && cost_read == 1)) {
                    result.cost = addon::Quota_cost{
                        m_observations.mode == "quota-cost-zero" ? 0.0 : cost_read == 1 ? 23.4567 : 12.3456,
                        "USD", m_observations.mode != "quota-cost-available", "Recorded sessions", std::nullopt};
                    if (m_observations.mode == "quota-cost-available") {
                        result.cost->available = 87.6544;
                    }
                }
            }
            if (failure) {
                result.failure_reason = addon::quota_failure_reason(*failure);
                if (*failure == addon::Quota_failure::INVALID_RESPONSE) {
                    result.diagnostic = addon::Quota_diagnostic{addon::Quota_failure_stage::HTTP_RESPONSE, 503};
                }
            }
            auto encoded = addon::encode_quota_inspect_result(
                result, version, decoded->report_quota_failure, decoded->report_quota_cost);
            if (decoded->report_quota_cost && m_observations.mode == "quota-cost-unknown") {
                auto cost = addon::encode_quota_cost(*result.cost);
                cost.set("version", addon::Json_value(19));
                encoded.set("quota_cost", std::move(cost));
            }
            if (decoded->report_quota_cost && m_observations.mode == "quota-cost-invalid") {
                auto cost = addon::encode_quota_cost(*result.cost);
                cost.set("amount", addon::Json_value(-1.0));
                encoded.set("quota_cost", std::move(cost));
            }
            if (decoded->report_quota_failure && m_observations.mode.starts_with("quota-unusable-")) {
                encoded.set("quota_failure", unusable_quota_explanation(m_observations.mode));
            }
            context.succeed(std::move(encoded));
        }
    }

    void handle_usage_session(addon::Invocation_context& context)
    {
        if (context.request().operation != addon::k_usage_session_describe_operation) {
            context.reject_unknown_operation();
            return;
        }
        if (m_observations.mode == "usage-session-unavailable") {
            context.fail(addon::Result_status::UNAVAILABLE, "fixture_session_unavailable");
            return;
        }
        const auto request = addon::decode_usage_session_request(context.request().payload, nullptr);
        if (!request) {
            context.fail(addon::Result_status::FAILED, "fixture_session_request_invalid");
            return;
        }
        std::vector<addon::Usage_session_description> descriptions;
        for (const auto& identity : *request) {
            addon::Usage_session_description description{identity, std::nullopt};
            if (identity.session_key == "fixture-session" && m_observations.mode != "usage-session-null" &&
                !(m_observations.mode == "usage-session-mixed" && identity.record_id == "record-02"))
            {
                description.session = addon::Usage_session_fact{"native-fixture-session", true};
            }
            if (m_observations.mode == "usage-session-wrong-key") {
                description.identity.session_key = "foreign-session";
            }
            descriptions.push_back(std::move(description));
        }
        context.succeed(addon::encode_usage_session_result(descriptions));
    }

    void handle_usage_scan(addon::Invocation_context& context)
    {
        if (context.request().operation != addon::k_usage_scan_operation) {
            context.reject_unknown_operation();
            return;
        }
        addon::Contract_failure failure;
        std::optional<addon::Usage_owned_event_correction_request> owned_request;
        std::optional<addon::Usage_scan_request> request;
        if (context.request().capability.capability_id == addon::k_usage_owned_event_correction_capability_id) {
            owned_request = addon::decode_usage_owned_event_correction_request(context.request().payload, &failure);
            if (owned_request) request = owned_request->scan;
        }
        else request = addon::decode_usage_scan_request(context.request().payload, &failure, m_observations.usage_version);
        if (!request) {
            context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
            return;
        }
        if (owned_request && !request->cursor &&
            (m_observations.mode == "usage-predecessor-held" ||
                (m_observations.mode == "usage-predecessor-held-after-acceptance" && request->cache_selection->accepted)))
        {
            const fs::path profile = addon::usage_cache_io_path(
                addon::path_from_utf8(request->context.profile_storage->root));
            addon::Json_value pending = addon::Json_value::object();
            pending.set("path", addon::Json_value(owned_request->predecessors.path));
            pending.set("sha256", addon::Json_value(owned_request->predecessors.sha256_hex));
            pending.set("candidate_root", addon::Json_value(request->cache_selection->candidate_root));
            {
                std::ofstream file(profile / ".fixture-usage-predecessor-pending.json", std::ios::binary);
                file << addon::serialize_json(pending);
                file.close();
                if (!file) {
                    context.fail(addon::Result_status::FAILED, "fixture_predecessor_receipt_failed");
                    return;
                }
            }
            context.report_progress(0, 2);
            // The test releases the read after the consumer has departed.
            // Ignoring cancellation keeps input use under native custody.
            std::error_code error;
            while (!fs::exists(profile / ".fixture-usage-predecessor-read", error)) {
                error.clear();
                sleep_ms(10);
            }
            std::ifstream input(addon::usage_cache_io_path(
                addon::path_from_utf8(owned_request->predecessors.path)), std::ios::binary);
            const bool opened = input.is_open();
            const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
            addon::Json_value consumed = addon::Json_value::object();
            consumed.set("opened", addon::Json_value(opened));
            consumed.set("sha256", addon::Json_value(addon::to_hex(addon::Sha256::digest(bytes))));
            {
                std::ofstream file(profile / ".fixture-usage-predecessor-read.json", std::ios::binary);
                file << addon::serialize_json(consumed);
            }
            hang_forever();
        }
        if (m_observations.mode == "usage-cancel") {
            context.report_progress(0, 2);
            context.wait_for_cancel(k_forever_ms);
            context.fail(addon::Result_status::CANCELLED, "cancelled");
            return;
        }
        if (m_observations.mode == "usage-hang") {
            // Reports once, then neither answers nor honours a cancel.
            context.report_progress(0, 2);
            hang_forever();
        }
        const bool second = request->cursor.has_value();
        // An opening request that stands for a package building a large
        // snapshot: it works for two seconds and reports every 100 ms. Like
        // the real packages it scans one account at a time, so another
        // account's opening request waits, silent, until this one is built.
        if (!second && m_observations.mode == "usage-slow-open") {
            static std::timed_mutex scan_mutex;
            std::unique_lock serial(scan_mutex, std::defer_lock);
            while (!serial.try_lock_for(std::chrono::milliseconds(20))) {
                if (context.cancel_requested()) {
                    context.fail(addon::Result_status::CANCELLED, "cancelled");
                    return;
                }
            }
            constexpr std::uint64_t k_steps = 20;
            for (std::uint64_t step = 1; step <= k_steps; ++step) {
                if (context.wait_for_cancel(100)) {
                    context.fail(addon::Result_status::CANCELLED, "cancelled");
                    return;
                }
                context.report_progress(step, k_steps);
            }
        }
        if (second && (request->cursor != "page-2" || request->snapshot_id != "fixture-snapshot-1")) {
            context.fail(addon::Result_status::FAILED, "fixture_snapshot_invalid");
            return;
        }
        if (second && m_observations.mode == "usage-partial") {
            context.fail(addon::Result_status::FAILED, "fixture_history_incomplete");
            return;
        }
        std::optional<addon::Usage_cache_reference> cache_candidate;
        const auto& selection = *request->cache_selection;
        const auto& profile_root = request->context.profile_storage->root;
        const auto cancelled = [&context] { return context.cancel_requested(); };
        const auto root = addon::usage_cache_io_path(addon::path_from_utf8(selection.candidate_root));
        if (selection.accepted && m_observations.mode.starts_with("usage-cache-recovery-")) {
            const auto scenario = m_observations.mode.substr(std::string("usage-cache-recovery-").size());
            if (second && scenario == "later-page") {
                context.fail(addon::Result_status::FAILED, "usage_cache_invalid");
                return;
            }
            if (!second && scenario != "later-page") {
                if (!addon::validate_usage_cache_allocation(selection.candidate_root, profile_root, &failure)) {
                    fail_with(context, failure);
                    return;
                }
                if (request->mode == "repair" && scenario == "cancel") {
                    context.report_progress(0, 2);
                    context.wait_for_cancel(k_forever_ms);
                    context.fail(addon::Result_status::CANCELLED, "cancelled");
                    return;
                }
                if (request->mode != "repair" || scenario == "failure") {
                    std::ofstream rejected(root / "fixture-rejected", std::ios::binary);
                    rejected << "unfinished candidate\n";
                    rejected.close();
                    const auto status = scenario == "backpressured" || scenario == "observation-unsettled" ||
                        scenario == "ownership-unavailable"
                        ? addon::Result_status::UNAVAILABLE : addon::Result_status::FAILED;
                    const auto reason = scenario == "backpressured" ? addon::reason::k_backpressured
                        : scenario == "observation-unsettled" || scenario == "observation-failed"
                            ? "usage_observation_unsettled"
                        : scenario == "ownership-unavailable" ? "usage_ownership_unavailable"
                        : scenario == "other-reason" ? "usage_sessions_unavailable" : "usage_cache_invalid";
                    context.fail(status, reason, "usage_cache_invalid");
                    return;
                }
            }
        }
        auto frozen = *request;
        frozen.snapshot_id.reset();
        frozen.cursor.reset();
        const auto request_hash = addon::to_hex(addon::Sha256::digest(
            addon::serialize_json_canonical(addon::encode_usage_scan_request(frozen, 3))));
        if (!second) {
            if (!addon::validate_usage_cache_allocation(selection.candidate_root, profile_root, &failure) ||
                (selection.accepted && !addon::validate_usage_cache_candidate(
                    *selection.accepted, profile_root, request->context.account_context, cancelled, &failure)))
            {
                fail_with(context, failure);
                return;
            }
            std::ofstream output(root / "fixture-records", std::ios::binary);
            output << "fixture-session 100\n";
            output.close();
            std::ofstream binding(root / "fixture-request", std::ios::binary);
            binding << request_hash;
            binding.close();
            if (!output || !binding) {
                context.fail(addon::Result_status::FAILED, "fixture_cache_write_failed");
                return;
            }
            cache_candidate = addon::seal_usage_cache_candidate(
                selection.candidate_root, profile_root, request->context.account_context,
                "fixture-snapshot-1", "fixture-usage@1", cancelled, &failure);
            if (!cache_candidate) {
                fail_with(context, failure);
                return;
            }
        }
        else {
            if (!addon::validate_usage_cache_root(selection.candidate_root, profile_root, &failure)) {
                context.fail(addon::Result_status::FAILED, "fixture_cache_unavailable");
                return;
            }
            std::ifstream input(root / "cache-manifest.json", std::ios::binary);
            addon::Sha256 hash;
            char buffer[65536];
            while (input.read(buffer, sizeof(buffer)) || input.gcount() > 0) {
                if (cancelled()) {
                    context.fail(addon::Result_status::CANCELLED, "cancelled");
                    return;
                }
                hash.update(buffer, (std::size_t)input.gcount());
            }
            cache_candidate = addon::Usage_cache_reference{selection.candidate_root, "fixture-snapshot-1",
                addon::to_hex(hash.finish())};
            if (!input.is_open() || input.bad() || !input.eof() || !addon::validate_usage_cache_candidate(
                    *cache_candidate, profile_root, request->context.account_context, cancelled, &failure))
            {
                context.fail(addon::Result_status::FAILED, "fixture_cache_unavailable");
                return;
            }
            std::ifstream binding(root / "fixture-request", std::ios::binary);
            std::string recorded(request_hash.size(), '\0');
            binding.read(recorded.data(), (std::streamsize)recorded.size());
            if (!binding || recorded != request_hash || binding.peek() != std::char_traits<char>::eof()) {
                context.fail(addon::Result_status::FAILED, "fixture_selection_changed");
                return;
            }
        }

        addon::Usage_scan_page page;
        page.cache_candidate = std::move(cache_candidate);
        page.account_context = request->context.account_context;
        page.profile_storage = request->context.profile_storage;

        if (request->context.account_context) {
            page.scope.source_account_key = request->context.account_context->adapter_account_key;
        }
        page.scope.observed_at_ms = m_usage_observed_at_ms;
        page.scope.fresh_for_ms   = 60000;
        if (second && m_observations.mode == "usage-observation-change") {
            ++page.scope.observed_at_ms;
        }
        page.snapshot_id         = second && m_observations.mode == "usage-snapshot-change" ?
            "fixture-snapshot-2" : "fixture-snapshot-1";
        page.complete = second;
        if (!second || m_observations.mode == "usage-cursor-repeat") {
            page.next_cursor = "page-2";
            page.complete    = false;
        }
        page.incomplete_historical_coverage = m_observations.mode == "usage-incomplete-history" ||
            m_observations.mode == "usage-incomplete-mixed-evidence";
        addon::Usage_record record;
        record.record_id      = second && m_observations.mode != "usage-duplicate" ? "record-02" : "record-01";
        record.session_key    = "fixture-session";
        record.completed_date = second ? "2026-09-08" : "2026-09-07";
        record.total_tokens   = second ? 40 : 60;
        record.semantics      = second ? addon::Usage_counter_semantics::DELTA_EVENT :
            addon::Usage_counter_semantics::CUMULATIVE_FINAL;
        if (m_observations.mode == "usage-incomplete-mixed-evidence" && request->context.profile_storage) {
            record.source_path = request->context.profile_storage->root +
                (second ? "/retained-rollout.jsonl" : "/corrected-rollout.jsonl");
        }
        record.input          = {true, second ? 30U : 40U};
        record.output         = {true, second ? 10U : 20U};
        record.cache_read     = {true, 0};
        record.cache_creation = {false, std::nullopt};
        for (auto* amount : {&record.input, &record.output, &record.cache_read, &record.cache_creation}) {
            amount->coverage = addon::Usage_field_coverage{
                amount->applicable ? 1U : 0U, 1, false,
                amount->applicable ? addon::Usage_support::SUPPORTED : addon::Usage_support::UNSUPPORTED};
        }

        record.aggregate_evidence = m_observations.mode == "usage-partial-evidence" ||
            m_observations.mode == "usage-partial-source-transition" ||
            m_observations.mode == "usage-partial-git-init" ||
            m_observations.mode.starts_with("usage-attribution-") ||
            m_observations.mode.starts_with("usage-owned-") ||
            m_observations.mode.starts_with("usage-partial-zero-coverage") ||
            m_observations.mode.starts_with("usage-partial-growth") ||
            (m_observations.mode == "usage-incomplete-mixed-evidence" && second) ? "partial" : "complete";

        if (!second &&
            m_observations.mode.starts_with("usage-partial-zero-coverage"))
        {
            if (m_observations.mode.ends_with("-growth")) {
                record.total_tokens = 80;
                record.input.value = 60;
            }
            for (auto* amount : {&record.input, &record.output, &record.cache_read, &record.cache_creation}) {
                amount->coverage = addon::Usage_field_coverage{0, 0, false,
                    amount->applicable ? addon::Usage_support::UNKNOWN : addon::Usage_support::UNSUPPORTED};
            }
        }
        if (!second && m_observations.mode == "usage-partial-source-transition") {
            record.counter_field = "usage_record.models.*.totalTokens";
            record.source_counter_semantics = "cumulative_final";
        }
        if (!second && m_observations.mode.starts_with("usage-attribution-")) {
            const bool correcting = request->cache_selection->accepted.has_value();
            const auto& profile = request->context.profile_storage->root;
            record.source_path = profile + "/source.jsonl";
            record.model = "accepted-model";
            if (m_observations.mode == "usage-attribution-delta") {
                record.semantics = addon::Usage_counter_semantics::DELTA_EVENT;
            }
            if (correcting) {
                if (m_observations.mode == "usage-attribution-total") ++record.total_tokens;
                if (m_observations.mode == "usage-attribution-date") record.completed_date = "2026-09-09";
                if (m_observations.mode == "usage-attribution-dimension") {
                    ++*record.input.value;
                    --*record.output.value;
                }
                if (m_observations.mode == "usage-attribution-coverage") {
                    ++record.input.coverage->covered_records;
                    ++record.input.coverage->total_records;
                }
                if (m_observations.mode == "usage-attribution-provenance") record.model = "changed-model";
                if (m_observations.mode != "usage-attribution-no-tag") {
                    auto tag = addon::encode_usage_repository_attribution_tag();
                    if (m_observations.mode == "usage-attribution-unknown") tag.set("version", addon::Json_value(2));
                    if (m_observations.mode == "usage-attribution-malformed") tag.set("amount", addon::Json_value(0));
                    record.capabilities.set(addon::k_usage_repository_attribution_capability_id, std::move(tag));
                }
            }
            record.repository_usage = addon::Usage_repository{
                {{profile + (correcting ? "/corrected" : "/original"), record.total_tokens}}, 0};
            if (correcting && (m_observations.mode == "usage-attribution-missing-partition" ||
                    m_observations.mode == "usage-attribution-legacy-path"))
            {
                record.repository_path = profile + "/unresolved-legacy-path";
                if (m_observations.mode == "usage-attribution-missing-partition") record.repository_usage.reset();
            }
        }
        if (!second && m_observations.mode.starts_with("usage-owned-")) {
            const std::string scenario = m_observations.mode.substr(std::string("usage-owned-").size());
            const bool correcting = request->cache_selection->accepted.has_value();
            const auto& profile = request->context.profile_storage->root;
            record.source_path = profile + "/source.jsonl";
            record.model = "accepted-model";
            record.repository_usage = addon::Usage_repository{{{profile + "/original", 60}}, 0};
            for (auto* amount : {&record.input, &record.output, &record.cache_read, &record.cache_creation}) {
                amount->applicable = true;
                if (!amount->value) amount->value = 0;
                const std::uint64_t count = scenario == "zero-event" ? 3 : 2;
                amount->coverage = addon::Usage_field_coverage{count, count, false, addon::Usage_support::SUPPORTED};
            }
            addon::Usage_temporal temporal;
            temporal.parent = {record.record_id, 60, page.snapshot_id, page.scope.source_account_key};
            temporal.portions = {{"instant", "2026-09-07T10:00:00Z", {}, {}, 40},
                {"instant", "2026-09-07T11:00:00Z", {}, {}, 20}};
            record.temporal = temporal;
            if (scenario == "unknown-sessions") {
                record.coverage_unit = addon::Usage_coverage_unit::SESSIONS;
                for (auto* amount : {&record.input, &record.output, &record.cache_read, &record.cache_creation}) {
                    amount->value = 0;
                    amount->coverage = addon::Usage_field_coverage{0, 2, false, addon::Usage_support::UNKNOWN};
                }
            }
            if (correcting) {
                addon::Usage_owned_event_correction correction;
                correction.disposition = "correct";
                correction.predecessor_sha256_hex = std::string(64, 'a');
                correction.coverage_policy = scenario == "unknown-sessions" ? "retained_unknown_sessions" : "records";
                correction.original_record_ids = {"record-02"};
                if (owned_request) {
                    std::ifstream input(addon::usage_cache_io_path(
                        addon::path_from_utf8(owned_request->predecessors.path)), std::ios::binary);
                    std::string line;
                    std::getline(input, line);
                    std::getline(input, line);
                    const auto json = addon::parse_json(line, addon::Json_parse_limits{}, nullptr);
                    const auto previous = json ? addon::decode_usage_owned_event_predecessor(*json, nullptr) : std::nullopt;
                    if (!previous || previous->record.record_id != record.record_id) {
                        context.fail(addon::Result_status::FAILED, "fixture_predecessor_invalid");
                        return;
                    }
                    correction.predecessor_sha256_hex = previous->predecessor_sha256_hex;
                }
                if (scenario == "retain" || scenario == "retain-opaque") {
                    correction.disposition = "retain";
                    correction.coverage_policy.clear();
                    correction.original_record_ids.clear();
                    record.temporal.reset();
                }
                else {
                    addon::Usage_owned_event_removal removed;
                    removed.total_tokens = 40;
                    removed.total_records = 1;
                    removed.input = {30, 1};
                    removed.output = {10, 1};
                    removed.cache_read = {0, 1};
                    removed.cache_creation = {0, 1};
                    removed.repository_usage = addon::Usage_repository{{{profile + "/original", 40}}, 0};
                    removed.temporal = addon::Usage_owned_event_temporal_removal{{temporal.portions.front()}, 0};
                    record.total_tokens = 20;
                    record.input.value = scenario == "unknown-sessions" ? 0 : 10;
                    record.output.value = scenario == "unknown-sessions" ? 0 : 10;
                    record.repository_usage->allocations.front().total_tokens = 20;
                    record.temporal->parent.total_tokens = 20;
                    record.temporal->portions.erase(record.temporal->portions.begin());
                    if (scenario != "unknown-sessions") {
                        for (auto* amount : {&record.input, &record.output, &record.cache_read, &record.cache_creation}) {
                            amount->coverage = addon::Usage_field_coverage{1, 1, false, addon::Usage_support::SUPPORTED};
                        }
                    }
                    if (scenario == "growth") {
                        record.total_tokens = 30;
                        record.input.value = 16;
                        record.output.value = 14;
                        record.repository_usage->allocations.front().total_tokens = 30;
                        record.temporal->parent.total_tokens = 30;
                        record.temporal->portions.push_back({"instant", "2026-09-07T12:00:00Z", {}, {}, 10});
                        for (auto* amount : {&record.input, &record.output, &record.cache_read, &record.cache_creation}) {
                            amount->coverage = addon::Usage_field_coverage{2, 2, false, addon::Usage_support::SUPPORTED};
                        }
                    }
                    if (scenario == "zero") {
                        removed.total_tokens = 60;
                        removed.total_records = 2;
                        removed.input = {40, 2};
                        removed.output = {20, 2};
                        removed.cache_read = {0, 2};
                        removed.cache_creation = {0, 2};
                        removed.repository_usage.allocations.front().total_tokens = 60;
                        removed.temporal->portions = temporal.portions;
                        record.total_tokens = 0;
                        record.repository_usage->allocations.clear();
                        record.temporal->parent.total_tokens = 0;
                        record.temporal->portions.clear();
                        for (auto* amount : {&record.input, &record.output, &record.cache_read, &record.cache_creation}) {
                            amount->value = 0;
                            amount->coverage = addon::Usage_field_coverage{0, 0, false, addon::Usage_support::UNKNOWN};
                        }
                    }
                    if (scenario == "zero-event") {
                        removed.total_tokens = 0;
                        removed.input = {0, 1};
                        removed.output = {0, 1};
                        removed.repository_usage.allocations.clear();
                        removed.temporal->portions.clear();
                        record.total_tokens = 60;
                        record.input.value = 40;
                        record.output.value = 20;
                        record.repository_usage->allocations.front().total_tokens = 60;
                        record.temporal = temporal;
                        for (auto* amount : {&record.input, &record.output, &record.cache_read, &record.cache_creation}) {
                            amount->coverage = addon::Usage_field_coverage{2, 2, false, addon::Usage_support::SUPPORTED};
                        }
                    }
                    if (scenario == "category-underflow") removed.input.value = 50;
                    if (scenario == "coverage-underflow") removed.total_records = 3;
                    if (scenario == "repository-underflow") removed.repository_usage.allocations.front().repository_path = profile + "/foreign";
                    if (scenario == "temporal-underflow") removed.temporal->portions.front().instant = "2026-09-07T09:00:00Z";
                    if (scenario == "capacity") {
                        record.temporal->portions.clear();
                        record.temporal->unavailable_tokens = 20;
                        record.temporal->coverage_reason = "capacity";
                    }
                    correction.removed = std::move(removed);
                }
                if (scenario == "wrong-digest") correction.predecessor_sha256_hex.assign(64, '0');
                if (scenario == "missing-owner") correction.original_record_ids = {"record-03"};
                if (scenario == "self-owner") correction.original_record_ids = {record.record_id};
                if (scenario == "complete") record.aggregate_evidence = "complete";
                if (scenario == "provenance") record.model = "different-model";
                if (scenario == "date") record.completed_date = "2026-09-08";
                record.capabilities.set(addon::k_usage_owned_event_correction_capability_id,
                    addon::encode_usage_owned_event_correction(correction));
            }
        }
        if (m_observations.mode == "usage-repository") {
            record.repository_path = request->context.profile_storage ? request->context.profile_storage->root : std::string{};
        }
        if (!second && m_observations.mode == "usage-partial-git-init") {
            record.repository_path = request->context.profile_storage->root;
            record.source_path = *record.repository_path + "/unchanged.jsonl";
        }
        if (!second && m_observations.mode.starts_with("usage-partial-growth") && request->context.profile_storage) {
            record.source_path = request->context.profile_storage->root + "/current.jsonl";
            record.model = "multiple";
            record.repository_usage = m_observations.mode == "usage-partial-growth-unattributed"
                ? addon::Usage_repository{{}, 60}
                : addon::Usage_repository{{{request->context.profile_storage->root, 50}}, 10};
            if (m_observations.mode == "usage-partial-growth-wide-attribution") {
                record.total_tokens = 10'000;
                record.input.value = 9'980;
                record.repository_usage->unattributed_tokens = 9'950;
            }
        }
        if (m_observations.mode.starts_with("usage-temporal-large")) {
            record.repository_usage = addon::Usage_repository{{}, record.total_tokens};
            for (int index = 0; index < 4096; ++index) {
                const auto suffix = std::to_string(10000 + index);
#ifdef _WIN32
                const std::string path = "C:/usage-temporal/" + suffix;
#else
                const std::string path = "/usage-temporal/" + suffix;
#endif
                record.repository_usage->allocations.push_back({path, 0});
            }
        }
        if (!second && m_observations.mode == "usage-temporal-corrected-capacity") {
            record.total_tokens = 50;
            record.output.value = 10;
        }
        if (!second && m_observations.mode == "usage-temporal-metadata-capacity") {
            record.model = "corrected-model";
        }
        if (m_observations.mode.starts_with("usage-temporal") && m_observations.mode != "usage-temporal-absent" &&
            m_observations.mode != "usage-temporal-large-absent")
        {
            addon::Usage_temporal evidence;
            evidence.parent = {record.record_id, record.total_tokens, page.snapshot_id, page.scope.source_account_key};
            evidence.portions = {{"instant", second ? "2026-09-08T00:01:00+02:00" : "2026-09-06T23:59:00Z",
                "", "", record.total_tokens}};
            if (m_observations.mode == "usage-temporal-invalid") {
                evidence.portions.front().instant = "2026-09-06T23:59:00";
            }
            if (m_observations.mode == "usage-temporal-rfc3339") {
                evidence.portions.front().instant = second ? "2017-01-01t13:59:60.1234567891+14:00" :
                    "2026-10-08t23:59:30.12345678901234567890z";
            }
            if (m_observations.mode == "usage-temporal-foreign-parent") {
                evidence.parent.snapshot_id = "foreign-snapshot";
            }
            if (m_observations.mode == "usage-temporal-capacity-lost" ||
                m_observations.mode == "usage-temporal-corrected-capacity" ||
                m_observations.mode == "usage-temporal-metadata-capacity")
            {
                evidence.portions.clear();
                evidence.unavailable_tokens = record.total_tokens;
                evidence.coverage_reason = "capacity";
            }
            if (m_observations.mode == "usage-temporal-capacity-retained") {
                evidence.coverage_reason = "capacity";
                if (request->cache_selection->accepted) {
                    evidence.retained_from = addon::Usage_temporal_retained_from{
                        request->cache_selection->accepted->snapshot_id,
                        request->cache_selection->accepted->manifest_sha256_hex};
                }
            }
            if (m_observations.mode == "usage-temporal-false-continuity") {
                evidence.retained_from = addon::Usage_temporal_retained_from{"foreign-snapshot", std::string(64, 'a')};
            }
            if (m_observations.mode == "usage-temporal-unknown" || m_observations.mode == "usage-temporal-large-unknown") {
                auto opaque = addon::encode_usage_temporal(evidence);
                opaque.set("version", addon::Json_value(19));
                opaque.set("future-metadata", addon::Json_value("preserve-opaque"));
                if (m_observations.mode == "usage-temporal-large-unknown") {
                    opaque.set("future-body", addon::Json_value(std::string(375'000, '"')));
                }
                record.capabilities.set(addon::k_usage_temporal_capability_id, std::move(opaque));
            }
            else {
                record.temporal = std::move(evidence);
            }
            if (m_observations.mode == "usage-temporal-reserved-history") {
                record.capabilities.set("usage-temporal-history", addon::Json_value::object());
            }
        }
        page.records.push_back(std::move(record));
        const bool additional_record = m_observations.mode == "usage-partial-git-init" && request->cache_selection->accepted;
        if (second && additional_record) {
            addon::Usage_record added = page.records.back();
            added.record_id    = "record-03";
            added.total_tokens = 25;
            added.input        = {true, 20};
            added.output       = {true, 5};
            if (added.source_path) {
                added.source_path = added.source_path->substr(0, added.source_path->find_last_of("/\\") + 1) +
                    "record-three.jsonl";
            }
            added.repository_usage = addon::Usage_repository{{}, added.total_tokens};
            if (m_observations.mode == "usage-partial-git-init") {
                added.session_key = "unrelated-session";
            }
            for (auto* amount : {&added.input, &added.output}) {
                amount->coverage = addon::Usage_field_coverage{1, 1, false, addon::Usage_support::SUPPORTED};
            }
            page.records.push_back(std::move(added));
        }
        if (!second && m_observations.mode == "usage-empty-page") {
            page.records.clear();
        }
        const std::uint64_t total = additional_record ? 3 : 2;
        context.report_progress(second ? total : 1, total);
        const auto result = addon::encode_usage_scan_result(page, m_observations.usage_version);
        if (m_observations.mode == "usage-repository" && !addon::decode_usage_scan_result(result, &failure)) {
            fail_with(context, failure);
            return;
        }
        context.succeed(result);
    }

    void handle_usage_summary(addon::Invocation_context& context)
    {
        if (context.request().operation != "read") {
            context.reject_unknown_operation();
            return;
        }
        if (m_observations.mode == "summary-unavailable") {
            context.fail(addon::Result_status::UNAVAILABLE, "stats_cache_unavailable");
            return;
        }
        if (m_observations.mode == "summary-failed") {
            context.fail(addon::Result_status::FAILED, "stats_cache_invalid");
            return;
        }
        addon::Contract_failure failure;
        const bool lifetime = context.request().capability.capability_id ==
            addon::k_usage_lifetime_summary_capability_id;
        if (lifetime) {
            const auto request = addon::decode_usage_lifetime_summary_request(context.request().payload, &failure);
            if (!request) {
                fail_with(context, failure);
                return;
            }
            addon::Usage_lifetime_summary result;
            result.account_context = request->account_context;
            if (m_observations.mode == "summary-foreign-source") {
                result.account_context.adapter_account_key = "foreign";
            }
            result.source_kind = m_observations.mode == "summary-other-source"
                ? "fixture-usage-cache" : "claude-stats-cache";
            result.source_content_digest = std::string(64, 'a');
            result.cache_stated_date = "2026-09-08";
            result.models = {{"fixture-model", 10, 20, 3, 7}};
            context.succeed(addon::encode_usage_lifetime_summary_result(result));
            return;
        }
        const auto request = addon::decode_usage_saved_daily_tokens_request(context.request().payload, &failure);
        if (!request) {
            fail_with(context, failure);
            return;
        }
        addon::Usage_saved_daily_tokens result;
        result.account_context = request->account_context;
        if (m_observations.mode == "summary-foreign-source") {
            result.account_context.adapter_account_key = "foreign";
        }
        result.source_kind = m_observations.mode == "summary-other-source"
            ? "fixture-usage-cache" : "claude-stats-cache";
        result.source_content_digest = std::string(64, 'b');
        result.cache_stated_date = "2026-09-08";
        result.days = {{"2026-09-08", {{"fixture-model", 46}}}};
        context.succeed(addon::encode_usage_saved_daily_tokens_result(result));
    }

    void handle_accounts(addon::Invocation_context& context)
    {
        const auto& request = context.request();
        addon::Contract_failure failure;
        const auto decoded = addon::decode_accounts_launcher_request(request.payload, &failure);
        if (!decoded) {
            context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
            return;
        }
        if (account_managed_configuration()) {
            const fs::path root = addon::path_from_utf8(
                decoded->normalization_directory);
            const std::string script = decoded->launcher.script_body;
            if (request.operation == addon::k_accounts_discover_operation) {
                context.succeed(addon::encode_accounts({
                    {"A", "Fixture account A", addon::Account_status::AVAILABLE,
                        (root / "account-a").string()},
                    {"B", "Fixture account B", addon::Account_status::AVAILABLE,
                        (root / "account-b").string()},
                    {"C", "Fixture account without profile", addon::Account_status::AVAILABLE,
                        std::nullopt},
                }));
                return;
            }
            if (request.operation == addon::k_accounts_resolve_operation) {
                context.succeed(addon::encode_account_resolution({
                    script == "unknown-fixture-cli --account null-profile" ? "C" :
                    script == "unknown-fixture-cli --account unmanaged-same-profile" ? "A" : "B"}));
                return;
            }
            context.reject_unknown_operation();
            return;
        }
        if (request.operation == addon::k_accounts_discover_operation) {
            std::optional<std::string> path;
            if (!decoded->default_home_fallback.empty()) {
                path = decoded->default_home_fallback;
            }
            if (m_observations.mode == "current-account" && path) {
                const auto root = addon::path_from_utf8(*path);
                context.succeed(addon::encode_accounts({
                    {"Primary", "Default profile", addon::Account_status::AVAILABLE,
                        addon::path_to_utf8(root / ".fixture-a")},
                    {"primary", "Named profile", addon::Account_status::AVAILABLE,
                        addon::path_to_utf8(root / ".fixture-z")},
                }));
                return;
            }
            context.succeed(addon::encode_accounts({
                {"Primary", "Primary Account",    addon::Account_status::AVAILABLE, std::nullopt},
                {"primary", "Filesystem Account", addon::Account_status::AVAILABLE, path},
            }));
            return;
        }
        if (request.operation == addon::k_accounts_resolve_operation) {
            const std::string& script = decoded->launcher.script_body;
            if (script == "unknown-fixture-cli --account fail") {
                context.fail(addon::Result_status::FAILED, "account_resolution_failed", "fixture account failure");
                return;
            }
            if (script == "unknown-fixture-cli --account delayed") {
                sleep_ms(250);
            }
            addon::Account_resolution resolution;
            if (script == "unknown-fixture-cli --account primary") {
                resolution.adapter_account_key = "primary";
            }
            else
            if (script == "unknown-fixture-cli --account missing") {
                resolution.adapter_account_key = "missing";
            }
            else
            if (script != "unknown-fixture-cli --account none") {
                resolution.adapter_account_key = "Primary";
            }
            context.succeed(addon::encode_account_resolution(resolution));
            return;
        }
        context.reject_unknown_operation();
    }

    static addon::Launcher_approval_rules launcher_approval_rules()
    {
        addon::Launcher_approval_rules rules;
        rules.program_names      = {"unknown-fixture-cli"};
        rules.canonical_argument = "--skip-approvals";
        rules.bypass_arguments   = {"--skip-approvals"};
        rules.value_arguments    = {"--settings"};
        rules.flag_arguments     = {"--bad-changed", "--bad-unchanged", "--slow-approval"};
        return rules;
    }

    void handle_launcher_approval(addon::Invocation_context& context)
    {
        const auto& request = context.request();
        addon::Contract_failure failure;
        if (request.operation == addon::k_launcher_approval_transform_operation) {
            const auto decoded = addon::decode_launcher_approval_transform_request(request.payload, &failure);
            if (!decoded) {
                context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
                return;
            }
            if (decoded->launcher.script_body.find(" --slow-approval") != std::string::npos &&
                context.wait_for_cancel(6000))
            {
                context.fail(addon::Result_status::CANCELLED, "cancelled");
                return;
            }
            auto result = addon::transform_launcher_approval(*decoded, launcher_approval_rules());
            if (decoded->launcher.script_body.ends_with(" --bad-changed")) {
                result.disposition = addon::Launcher_approval_disposition::CHANGED;
                result.script_body = decoded->launcher.script_body;
            }
            else
            if (decoded->launcher.script_body.ends_with(" --bad-unchanged")) {
                result.disposition = addon::Launcher_approval_disposition::UNCHANGED;
                result.script_body = decoded->launcher.script_body + " --skip-approvals";
            }
            context.succeed(addon::encode_launcher_approval_transform_result(result));
            return;
        }
        if (request.operation == addon::k_launcher_approval_comparison_operation) {
            const auto decoded = addon::decode_launcher_approval_comparison_request(request.payload, &failure);
            if (!decoded) {
                context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
                return;
            }
            context.succeed(addon::encode_launcher_approval_comparison_result(
                addon::launcher_approval_comparison_key(*decoded, launcher_approval_rules())));
            return;
        }
        context.reject_unknown_operation();
    }

    void handle_launcher_discovery(addon::Invocation_context& context)
    {
        const addon::Invoke_request& request = context.request();
        if (request.operation == addon::k_discover_operation) {
            addon::Contract_failure failure;
            const auto decoded = addon::decode_discover_request(request.payload, &failure);
            if (!decoded) {
                context.fail(
                    addon::Result_status::FAILED,
                    failure.reason_code,
                    failure.detail);
                return;
            }
            if (m_observations.mode == "discover-on-path" &&
                !addon::resolve_program("unknown-fixture-cli", addon::environment_pairs(decoded->ambient_environment),
                    decoded->default_home_fallback))
            {
                context.succeed(addon::encode_candidates({}));
                return;
            }
            addon::Json_value evidence = addon::Json_value::object();
            evidence.set("source", addon::Json_value("provider-unknown-fixture"));
            context.succeed(addon::encode_candidates({{
                "unknown_fixture",
                "Unknown Fixture",
                "unknown-fixture-cli",
                addon::k_runner_direct,
                {},
                true,
                std::move(evidence),
            }}));
            return;
        }
        if (request.operation == addon::k_recognize_operation) {
            addon::Contract_failure failure;
            const std::optional<addon::Recognize_request> decoded =
                addon::decode_recognize_request(request.payload, &failure);
            if (!decoded) {
                context.fail(
                    addon::Result_status::FAILED,
                    failure.reason_code,
                    failure.detail);
                return;
            }
            addon::Launcher_recognition recognition;
            constexpr std::string_view native_codex_suffix = "\" --codex native";
            const std::string& script = decoded->launcher.script_body;
            if (script.find(" --slow-approval") != std::string::npos &&
                request.deadline_ms == addon::limits::k_no_invoke_deadline_ms &&
                context.wait_for_cancel(6000))
            {
                context.fail(addon::Result_status::CANCELLED, "cancelled");
                return;
            }
            const bool native_codex_script = initial_prompt() && script.size() > native_codex_suffix.size() + 1 &&
                script.front() == '"' && script.ends_with(native_codex_suffix);
            const fs::path native_codex_program = native_codex_script
                ? addon::path_from_utf8(script.substr(1, script.size() - native_codex_suffix.size() - 1)) : fs::path{};
            std::error_code native_codex_error;
            const bool explicit_native_codex = native_codex_script && native_codex_program.is_absolute() &&
                fs::is_regular_file(native_codex_program, native_codex_error);
            const bool approval_script = m_behavior == Behavior::PROVIDER_SURFACES &&
                addon::launcher_approval_comparison_key(
                    {decoded->launcher, decoded->platform_id}, launcher_approval_rules()).supported;
            recognition.recognized =
                (decoded->launcher.script_body == "unknown-fixture-cli" ||
                    approval_script ||
                    decoded->launcher.script_body == "unknown-fixture-cli --slow-resume" ||
                    decoded->launcher.script_body == "unknown-fixture-cli --wait-for-resume-cancel" ||
                    ((m_behavior == Behavior::PROVIDER_ACCOUNTS ||
                        account_managed_configuration() || m_observations.accounts) &&
                        decoded->launcher.script_body.starts_with("unknown-fixture-cli --account ")) ||
                    (initial_prompt() &&
                        decoded->launcher.script_body.starts_with("unknown-fixture-cli --prompt ")) ||
                    explicit_native_codex) &&
                decoded->launcher.runner_key == addon::k_runner_direct &&
                (m_behavior == Behavior::PROVIDER_ACCOUNTS ||
                    account_managed_configuration() || m_observations.accounts ||
                    decoded->launcher.environment_text.empty());
            if (m_behavior == Behavior::PROVIDER_ACCOUNTS &&
                native_preparation_fixture(decoded->launcher))
            {
                recognition.recognized = true;
            }
            if (recognition.recognized) {
                recognition.display_name = "Unknown Fixture";
            }
            if (recognition.recognized && m_behavior == Behavior::PROVIDER_ACCOUNTS &&
                decoded->launcher.script_body == "unknown-fixture-cli --account warning")
            {
                recognition.compatibility_warning = std::make_pair(
                    "fixture_compatibility_warning", "Fixture compatibility warning.");
            }
            bool held = false;
            {
                const std::lock_guard<std::mutex> lock(m_recognition_mutex);
                if (m_recognition_armed && decoded->launcher.script_body == m_recognition_hold_script) {
                    m_recognition_armed = false;
                    m_recognition_entered = true;
                    held = true;
                }
            }
            if (held) {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
                for (;;) {
                    bool released;
                    {
                        const std::lock_guard<std::mutex> lock(m_recognition_mutex);
                        m_recognition_cancel_observed =
                            m_recognition_cancel_observed || context.cancel_requested();
                        released = m_recognition_released;
                    }
                    if (released) {
                        break;
                    }
                    if (std::chrono::steady_clock::now() >= deadline) {
                        context.fail(addon::Result_status::FAILED, "fixture_recognition_hold_timeout");
                        const std::lock_guard<std::mutex> lock(m_recognition_mutex);
                        m_recognition_finished = true;
                        return;
                    }
                    // Deliberately finish late after cancellation: the caller
                    // must prove a stale successful response cannot publish.
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
            context.succeed(addon::encode_recognition(recognition));
            if (held) {
                const std::lock_guard<std::mutex> lock(m_recognition_mutex);
                m_recognition_result_attempted = true;
                m_recognition_finished = true;
            }
            return;
        }
        context.reject_unknown_operation();
    }

    void handle_launch_preparation(addon::Invocation_context& context)
    {
        if (context.request().operation != addon::k_prepare_operation) {
            context.reject_unknown_operation();
            return;
        }
        addon::Contract_failure failure;
        const std::optional<addon::Prepare_request> request =
            addon::decode_prepare_request(context.request().payload, &failure, addon::k_launch_preparation_execution_version);
        if (!request) {
            context.fail(
                addon::Result_status::FAILED,
                failure.reason_code,
                failure.detail);
            return;
        }
        const auto& argv = request->launcher_argv;
        if (m_behavior == Behavior::PROVIDER_SURFACES && argv.size() == 4) {
            if (argv[1] == "--refuse-launch") {
                context.fail(addon::Result_status::UNAVAILABLE, argv[2], argv[3]);
                return;
            }
            if (argv[1] == "--wait-for-cancel" || argv[1] == "--cancel-launch") {
                if (argv[1] == "--wait-for-cancel") {
                    context.report_progress(0, 1);
                    context.wait_for_cancel(k_forever_ms);
                }
                context.fail(addon::Result_status::CANCELLED, argv[2]);
                return;
            }
        }
        addon::Launch_patch patch;
        patch.profile_storage = request->profile_storage;
        patch.account_context = request->account_context;
        const auto declared = [this](const char* capability, std::uint16_t version) {
            const auto sidecar = addon::path_from_utf8(m_executable).parent_path().parent_path() /
                addon::sidecar_relative_path(capability, version);
            std::error_code error;
            return fs::is_regular_file(sidecar, error);
        };
        patch.disposition = addon::Launch_disposition{true,
            declared(addon::k_lifecycle_events_capability_id, addon::k_lifecycle_events_version),
            declared(addon::k_coordination_delivery_capability_id, addon::k_coordination_delivery_version)};
        if (initial_prompt()) {
            // The launcher's own invocation, prepared. A request that starts
            // with a first user message gets it as the CLI's trailing
            // positional argument, the way a CLI that takes an initial
            // interactive prompt is given one, and the digest of the text
            // the invocation submits. The launcher's script names the edge
            // it reproduces instead.
            patch.disposition = addon::Launch_disposition{true, false, false};
            patch.argv        = request->launcher_argv;
            if (request->initial_prompt) {
                const std::string& script = request->launcher.script_body;
                if (script == "unknown-fixture-cli --prompt refuse") {
                    context.fail(addon::Result_status::UNAVAILABLE, addon::reason::k_initial_prompt_unavailable,
                        "this launcher cannot be prepared to carry an initial prompt");
                    return;
                }
                const std::string& text = request->initial_prompt->text;
                patch.argv->push_back(text);
                if (script != "unknown-fixture-cli --prompt unacknowledged") {
                    patch.initial_prompt = addon::Initial_prompt_acknowledgment{
                        addon::initial_prompt_text_digest_hex(
                            script == "unknown-fixture-cli --prompt mismatched" ? text + " " : text)};
                }
            }
            const std::string& script = request->launcher.script_body;
            if ((request->codex_invocation && script != "unknown-fixture-cli --codex missing") ||
                script == "unknown-fixture-cli --codex unsolicited")
            {
                const bool native_prefix = script == "unknown-fixture-cli --codex native" ||
                    (request->launcher_argv.size() == 3 &&
                        addon::path_from_utf8(request->launcher_argv.front()).is_absolute() &&
                        request->launcher_argv[1] == "--codex" && request->launcher_argv[2] == "native");
                const std::size_t prefix_size = native_prefix ? 1 : 2;
                patch.codex_invocation = addon::Codex_invocation{prefix_size};
                if (script == "unknown-fixture-cli --codex oversized") {
                    patch.codex_invocation->argument_prefix_size = patch.argv->size() + 1;
                }
                if (script == "unknown-fixture-cli --codex unprepared") {
                    patch.disposition->prepared = false;
                }
            }
        }
        if (account_managed_configuration()) {
            const bool managed = request->launcher.script_body !=
                "unknown-fixture-cli --account unmanaged-switch" &&
                request->launcher.script_body !=
                "unknown-fixture-cli --account unmanaged-same-profile";
            patch.apply_managed_configuration = managed;
            patch.disposition = addon::Launch_disposition{managed, false, false};
            if (request->launcher.script_body ==
                "unknown-fixture-cli --account patch-conflict")
            {
                patch.set.emplace_back("FIXTURE_HOME", "/fixture-conflict");
            }
            if (request->launcher.script_body ==
                "unknown-fixture-cli --account unrequested-initial-prompt")
            {
                // A replacement invocation that submits an initial prompt and
                // echoes it, answering a request that carried no prompt.
                const std::string prompt = "Review the staged changes before committing.";
                patch.argv = request->launcher_argv;
                patch.argv->push_back(prompt);
                patch.initial_prompt = addon::Initial_prompt_acknowledgment{
                    addon::initial_prompt_text_digest_hex(prompt)};
            }
        }
        add_session_patch_context(patch, request->profile_storage);
        addon::Json_value payload = addon::encode_launch_patch(patch, addon::k_launch_preparation_execution_version);
        if (initial_prompt() && request->launcher.script_body == "unknown-fixture-cli --codex unknown") {
            addon::Json_value marker = addon::Json_value::object();
            marker.set("version", addon::Json_value(std::int64_t{2}));
            marker.set("argument_prefix_size", addon::Json_value(std::int64_t{2}));
            payload.set(addon::k_codex_invocation_member, std::move(marker));
        }
        if (m_behavior == Behavior::PROVIDER_ACCOUNTS) {
            if (request->launcher.script_body == "unknown-fixture-cli --account echo-missing") {
                payload.erase(addon::k_account_context_member);
            }
            else
            if (request->launcher.script_body == "unknown-fixture-cli --account echo-mismatch") {
                payload.set(addon::k_account_context_member,
                    addon::encode_account_context(addon::Account_context{1, "primary"}));
            }
        }
        context.succeed(std::move(payload));
    }

    void handle_managed_configuration(addon::Invocation_context& context)
    {
        if (context.request().operation !=
            addon::k_managed_configuration_transform_operation)
        {
            context.reject_unknown_operation();
            return;
        }
        addon::Contract_failure failure;
        const auto request = addon::decode_managed_configuration_transform_request(
            context.request().payload, &failure);
        if (!request) {
            context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
            return;
        }
        if (request->content == "refuse-managed-configuration\n") {
            context.fail(addon::Result_status::FAILED,
                "fixture_settings_invalid", "Fixture settings are invalid.");
            return;
        }
        if (request->content == "refuse-managed-configuration-reason-only\n") {
            context.fail(addon::Result_status::UNAVAILABLE, "fixture_settings_size_limit");
            return;
        }
        context.succeed(addon::encode_managed_configuration_transform_result({
            request->grant, "managed-by-account-binding\n"}));
    }

    void handle_profile_storage(addon::Invocation_context& context)
    {
        const addon::Invoke_request& request = context.request();
        if (request.operation != addon::k_profile_storage_acknowledge) {
            context.reject_unknown_operation();
            return;
        }
        const addon::Json_value* member = request.payload.find(addon::k_profile_storage_member);
        if (member == nullptr || !member->is_object() || member->find("root") == nullptr || !member->find("root")->is_string() ||
            member->find("version") == nullptr || !member->find("version")->is_integer())
        {
            context.fail(addon::Result_status::FAILED, "invalid_payload", "profile_storage member is missing or malformed");
            return;
        }

        addon::Profile_storage_context echo;
        echo.version = (std::uint16_t)member->find("version")->as_integer();
        echo.root    = member->find("root")->as_string();
        switch (m_behavior) {
            case Behavior::PROFILE_OMIT_ECHO:
                context.succeed(addon::Json_value::object());
                return;
            case Behavior::PROFILE_WRONG_ROOT:
                echo.root += "/other";
                break;
            case Behavior::PROFILE_DEFAULT_DIR: {
                // What a vendor CLI does when it ignores the host: its own
                // dot-directory below the user's home.
                const char* home = std::getenv("HOME");
                if (home == nullptr) {
                    home = std::getenv("USERPROFILE");
                }
                echo.root = std::string(home != nullptr ? home : "") + "/.unknown-fixture";
                break;
            }
            case Behavior::PROFILE_WRONG_VERSION:
                echo.version = (std::uint16_t)(echo.version + 1);
                break;
            default:
                break;
        }
        addon::Json_value payload = addon::Json_value::object();
        payload.set(addon::k_profile_storage_member, addon::profile_storage_member(echo));
        context.succeed(std::move(payload));
    }

    bool decode_lifetime_response(
        const addon::Json_value* value,
        Lifetime_response*        out_response,
        addon::Invocation_context& context,
        const char*                name)
    {
        if (value == nullptr) return true;
        if (!value->is_string()) {
            context.fail(addon::Result_status::FAILED, "fixture_lifetime_invalid_mode",
                std::string(name) + " must be normal, hold or reject");
            return false;
        }
        if (value->as_string() == "normal") *out_response = Lifetime_response::NORMAL;
        else if (value->as_string() == "hold") *out_response = Lifetime_response::HOLD;
        else if (value->as_string() == "reject") *out_response = Lifetime_response::REJECT;
        else {
            context.fail(addon::Result_status::FAILED, "fixture_lifetime_invalid_mode",
                std::string(name) + " must be normal, hold or reject");
            return false;
        }
        return true;
    }

    bool lifetime_probe_payload_is_closed(
        const addon::Json_value& payload,
        std::initializer_list<std::string_view> names,
        addon::Invocation_context& context)
    {
        if (!payload.is_object()) {
            context.fail(addon::Result_status::FAILED, "fixture_lifetime_invalid_payload");
            return false;
        }
        for (const addon::Json_member& member : payload.as_object()) {
            if (std::find(names.begin(), names.end(), member.name) == names.end()) {
                context.fail(addon::Result_status::FAILED, "fixture_lifetime_invalid_payload");
                return false;
            }
        }
        return true;
    }

    void handle_lifetime_probe(addon::Invocation_context& context)
    {
        const std::string& operation = context.request().operation;
        const addon::Json_value& payload = context.request().payload;
        if (operation == "lifetime_mode") {
            if (!lifetime_probe_payload_is_closed(payload,
                    {"reserve", "retire", "observe", "classify", "terminal"}, context)) return;
            Lifetime_controls updated;
            {
                const std::lock_guard<std::mutex> lock(m_lifetime_mutex);
                updated = m_lifetime_controls;
            }
            if (!decode_lifetime_response(payload.find("reserve"), &updated.reserve, context, "reserve") ||
                !decode_lifetime_response(payload.find("retire"), &updated.retire, context, "retire") ||
                !decode_lifetime_response(payload.find("observe"), &updated.observe, context, "observe") ||
                !decode_lifetime_response(payload.find("classify"), &updated.classify, context, "classify"))
            {
                return;
            }
            if (const addon::Json_value* terminal = payload.find("terminal")) {
                if (!terminal->is_boolean()) {
                    context.fail(addon::Result_status::FAILED, "fixture_lifetime_invalid_mode",
                        "terminal must be a boolean");
                    return;
                }
                updated.terminal = terminal->as_boolean();
            }
            {
                const std::lock_guard<std::mutex> lock(m_lifetime_mutex);
                m_lifetime_controls = updated;
            }
            context.succeed(addon::Json_value::object());
            return;
        }
        if (operation == "lifetime_release") {
            if (!lifetime_probe_payload_is_closed(payload, {"operation"}, context)) return;
            const addon::Json_value* requested = payload.find("operation");
            if (requested != nullptr && (!requested->is_string() ||
                    (requested->as_string() != "reserve" && requested->as_string() != "retire" &&
                        requested->as_string() != "observe")))
            {
                context.fail(addon::Result_status::FAILED, "fixture_lifetime_invalid_payload");
                return;
            }
            const std::string operation_name = requested == nullptr ? std::string{} : requested->as_string();
            {
                const std::lock_guard<std::mutex> lock(m_lifetime_mutex);
                if (operation_name.empty() || operation_name == "reserve") m_lifetime_reserve_released = true;
                if (operation_name.empty() || operation_name == "retire") m_lifetime_retire_released = true;
                if (operation_name.empty() || operation_name == "observe") m_lifetime_observe_released = true;
            }
            context.succeed(addon::Json_value::object());
            return;
        }
        if (operation == "lifetime_state") {
            if (!lifetime_probe_payload_is_closed(payload, {}, context)) return;
            addon::Json_value calls = addon::Json_value::array();
            bool reserve_held = false;
            bool retire_held = false;
            bool observe_held = false;
            {
                const std::lock_guard<std::mutex> lock(m_lifetime_mutex);
                reserve_held = m_lifetime_reserve_held;
                retire_held = m_lifetime_retire_held;
                observe_held = m_lifetime_observe_held;
                for (const Lifetime_call& call : m_lifetime_calls) {
                    addon::Json_value record = addon::Json_value::object();
                    record.set("operation", addon::Json_value(call.operation));
                    record.set("transition_nonce", addon::Json_value(call.transition_nonce));
                    if (call.resume_cursor) record.set("resume_cursor", addon::Json_value(*call.resume_cursor));
                    if (call.disposition) record.set("disposition", addon::Json_value(*call.disposition));
                    if (call.consumed_cursor) record.set("consumed_cursor", addon::Json_value(*call.consumed_cursor));
                    calls.push_back(std::move(record));
                }
            }
            addon::Json_value state = addon::Json_value::object();
            state.set("calls", std::move(calls));
            state.set("reserve_held", addon::Json_value(reserve_held));
            state.set("retire_held", addon::Json_value(retire_held));
            state.set("observe_held", addon::Json_value(observe_held));
            context.succeed(std::move(state));
            return;
        }
        context.reject_unknown_operation();
    }

    void handle_wake_probe(addon::Invocation_context& context)
    {
        const auto& payload = context.request().payload;
        const auto& operation = context.request().operation;
        const std::lock_guard<std::mutex> lock(m_wake_mutex);
        if (operation == "wake_mode") {
            for (const char* phase : {"inspect", "notify"}) {
                if (const auto* value = payload.find(phase)) {
                    if (!value->is_string()) {
                        context.fail(addon::Result_status::FAILED, "fixture_wake_invalid_mode");
                        return;
                    }
                    m_wake_modes[phase] = value->as_string();
                }
            }
            context.succeed(addon::Json_value::object());
            return;
        }
        if (operation == "wake_release") {
            const auto* phase = payload.find("operation");
            if (!phase || !phase->is_string() ||
                (phase->as_string() != "inspect" && phase->as_string() != "notify"))
            {
                context.fail(addon::Result_status::FAILED, "fixture_wake_invalid_phase");
                return;
            }
            m_wake_released[phase->as_string()] = true;
            context.succeed(addon::Json_value::object());
            return;
        }
        addon::Json_value state = addon::Json_value::object();
        state.set("calls", m_wake_calls);
        state.set("inspect_held", addon::Json_value(m_wake_held["inspect"]));
        state.set("notify_held", addon::Json_value(m_wake_held["notify"]));
        context.succeed(std::move(state));
    }

    void handle_session_wake(addon::Invocation_context& context)
    {
        const std::string operation = context.request().operation;
        addon::Contract_failure failure;
        const bool inspect = operation == addon::k_session_wake_inspect_operation;
        bool valid = false;
        if (inspect) {
            valid = addon::decode_session_wake_inspect_request(context.request().payload, &failure).has_value();
        }
        else
        if (operation == addon::k_session_wake_notify_operation) {
            valid = addon::decode_session_wake_notify_request(context.request().payload, &failure).has_value();
        }
        if (!valid) {
            context.fail(addon::Result_status::FAILED, "fixture_wake_invalid_request", failure.detail);
            return;
        }
        std::string mode;
        {
            const std::lock_guard<std::mutex> lock(m_wake_mutex);
            addon::Json_value record = addon::Json_value::object();
            record.set("operation", addon::Json_value(operation));
            record.set("payload", context.request().payload);
            m_wake_calls.push_back(std::move(record));
            mode = m_wake_modes[operation];
            if (mode == "hold" || mode == "hold_unavailable") {
                m_wake_held[operation] = true;
                m_wake_released[operation] = false;
            }
        }
        while (mode == "hold" || mode == "hold_unavailable") {
            {
                const std::lock_guard<std::mutex> lock(m_wake_mutex);
                if (m_wake_released[operation]) {
                    m_wake_held[operation] = false;
                    mode = mode == "hold_unavailable" ? "unavailable" : m_wake_modes[operation];
                    if (mode == "hold") {
                        mode = inspect ? "available" : "accepted";
                    }
                    break;
                }
            }
            if (context.wait_for_cancel(10)) {
                const std::lock_guard<std::mutex> lock(m_wake_mutex);
                m_wake_held[operation] = false;
                return;
            }
        }
        if (mode == "failed") {
            context.fail(addon::Result_status::FAILED, "fixture_wake_transport_failed");
            return;
        }
        if (inspect) {
            const bool available = mode == "available";
            context.succeed(addon::encode_session_wake_readiness({available,
                available ? std::string{} : "native_session_unavailable"}));
            return;
        }
        addon::Session_wake_notify_result result;
        if (mode == "accepted") {
            result.disposition = addon::Session_wake_disposition::ACCEPTED;
            result.receipt = "native-notification-receipt";
        }
        else {
            if (mode == "uncertain") {
                result.disposition = addon::Session_wake_disposition::UNCERTAIN;
            }
            else
            if (mode == "deferred") {
                result.disposition = addon::Session_wake_disposition::DEFERRED;
            }
            result.reason = "native_session_waiting";
        }
        context.succeed(addon::encode_session_wake_notify_result(result));
    }

    void handle_probe(addon::Invocation_context& context)
    {
        const addon::Invoke_request& request = context.request();
        const std::string& operation = request.operation;

        if (m_behavior == Behavior::PROVIDER_ACCOUNTS &&
            (operation == "configure_recognition" || operation == "recognition_state" ||
                operation == "release_recognition"))
        {
            const auto& payload = request.payload;
            const auto* script = payload.find("hold_script");
            const bool configure = operation == "configure_recognition";
            if (!payload.is_object() ||
                (configure ? payload.as_object().size() != 1 || !script || !script->is_string() ||
                    (script->as_string() != "unknown-fixture-cli --account warning" &&
                        script->as_string() != "unknown-fixture-cli --account none") :
                    !payload.as_object().empty()))
            {
                context.fail(addon::Result_status::FAILED, "fixture_recognition_invalid_payload");
                return;
            }
            const std::lock_guard<std::mutex> lock(m_recognition_mutex);
            if (configure) {
                if (m_recognition_armed || (m_recognition_entered && !m_recognition_finished)) {
                    context.fail(addon::Result_status::FAILED, "fixture_recognition_busy");
                    return;
                }
                m_recognition_hold_script = script->as_string();
                m_recognition_armed = true;
                m_recognition_entered = false;
                m_recognition_released = false;
                m_recognition_cancel_observed = false;
                m_recognition_result_attempted = false;
                m_recognition_finished = false;
            }
            if (operation == "release_recognition") {
                m_recognition_released = true;
            }
            addon::Json_value state = addon::Json_value::object();
            state.set("armed", addon::Json_value(m_recognition_armed));
            state.set("entered", addon::Json_value(m_recognition_entered));
            state.set("released", addon::Json_value(m_recognition_released));
            state.set("cancel_observed", addon::Json_value(m_recognition_cancel_observed));
            state.set("result_attempted", addon::Json_value(m_recognition_result_attempted));
            state.set("finished", addon::Json_value(m_recognition_finished));
            context.succeed(std::move(state));
            return;
        }

        if (m_behavior == Behavior::PROVIDER_SESSIONS_STDIN_EOF &&
            (operation == "hold_inspection" || operation == "release_inspection" ||
                operation == "inspection_state"))
        {
            if (operation == "hold_inspection") {
                m_hold_inspection.store(true);
            }
            if (operation == "release_inspection") {
                m_hold_inspection.store(false);
            }
            addon::Json_value state = addon::Json_value::object();
            state.set("entered", addon::Json_value(m_inspection_entered.load()));
            state.set("count", addon::Json_value((std::int64_t)m_inspection_count.load()));
            context.succeed(std::move(state));
            return;
        }

        if (lifetime_sessions() && (operation == "lifetime_mode" || operation == "lifetime_release" ||
                operation == "lifetime_state"))
        {
            handle_lifetime_probe(context);
            return;
        }

        if (lifetime_sessions() && (operation == "wake_mode" || operation == "wake_release" ||
                operation == "wake_state"))
        {
            handle_wake_probe(context);
            return;
        }

        if (operation == "echo") {
            context.succeed(request.payload);
            return;
        }
        if (operation == "payload_lifetime") {
            const auto* mode = request.payload.find("mode");
            const auto* blob = request.payload.find("blob");
            if (!mode || !mode->is_string() || !blob || !blob->is_string()) {
                context.fail(addon::Result_status::FAILED, "invalid_payload");
                return;
            }
            const std::string terminal_mode = mode->as_string();
            const std::size_t input_size    = blob->as_string().size();
            struct Payload_observer
            {
                const addon::Invoke_request* m_request = nullptr;
                std::string m_request_id;
                addon::Capability_pair m_capability;
                std::int64_t m_deadline_ms = 0;

                ~Payload_observer()
                {
                    // This is an SDK ownership probe, not an adapter callback.
                    // Worker_completion cannot become ready before TLS cleanup,
                    // so the runtime still owns this context while we inspect it.
                    const bool released = m_request->payload.is_null();
                    const bool identity_retained =
                        m_request->request_id  == m_request_id  &&
                        m_request->capability  == m_capability  &&
                        m_request->deadline_ms == m_deadline_ms &&
                        m_request->operation   == "payload_lifetime";
                    std::fputs(released && identity_retained
                        ? "fixture payload released with identity retained\n"
                        : "fixture payload lifetime mismatch\n", stderr);
                    std::fflush(stderr);
                }
            };
            thread_local Payload_observer observer;
            observer.m_request     = &request;
            observer.m_request_id  = request.request_id;
            observer.m_capability  = request.capability;
            observer.m_deadline_ms = request.deadline_ms;
            if (terminal_mode == "success") {
                context.succeed(addon::Json_value::object());
            }
            else
            if (terminal_mode == "failure") {
                context.fail(addon::Result_status::FAILED, "payload_probe_failure");
            }
            const auto* remaining = request.payload.find("blob");
            const bool available = remaining && remaining->is_string() &&
                remaining->as_string().size() == input_size;
            std::fputs(available
                ? "fixture payload available through handler cleanup\n"
                : "fixture payload missing during handler cleanup\n", stderr);
            std::fflush(stderr);
            return;
        }
        if (operation == "result_then_cleanup") {
            const addon::Json_value* kind_value = request.payload.find("kind");
            const addon::Json_value* failure    = request.payload.find("failure");
            if (kind_value == nullptr || !kind_value->is_string() ||
                failure == nullptr || !failure->is_boolean())
            {
                context.fail(addon::Result_status::FAILED, "invalid_payload", "kind and failure are required");
                return;
            }
            const std::string& kind = kind_value->as_string();
            if (kind != "handler" && kind != "thread" && kind != "cancel") {
                context.fail(addon::Result_status::FAILED, "invalid_payload", "unknown cleanup kind");
                return;
            }
            if (kind == "thread") {
                struct Cleanup
                {
                    ~Cleanup()
                    {
                        std::fputs("fixture thread cleanup entered\n", stderr);
                        std::fflush(stderr);
                        sleep_ms(k_forever_ms);
                    }
                };
                thread_local Cleanup cleanup;
                (void)cleanup;
            }
            if (failure->as_boolean()) {
                context.fail(addon::Result_status::FAILED, "cleanup_failure");
            }
            else {
                context.succeed(addon::Json_value::object());
            }
            if (kind == "handler") {
                std::fputs("fixture handler cleanup entered\n", stderr);
                std::fflush(stderr);
                sleep_ms(k_forever_ms);
            }
            else
            if (kind == "cancel" && context.wait_for_cancel(k_forever_ms)) {
                std::fputs("fixture cleanup cancelled\n", stderr);
                std::fflush(stderr);
            }
            return;
        }
        if (operation == "oversize_success") {
            addon::Json_value payload = addon::Json_value::object();
            payload.set("blob", addon::Json_value(
                std::string(addon::limits::k_max_json_string_bytes + 1, 'x')));
            context.succeed(std::move(payload));
            return;
        }
        if (operation == "unicode_failure") {
            context.fail(addon::Result_status::FAILED, "diagnostic",
                std::string(addon::limits::k_max_result_message_bytes - 1, 'x') + "\xe2\x82\xac");
            return;
        }
        if (operation == "invalid_diagnostic") {
            context.fail(addon::Result_status::FAILED, "diagnostic", std::string("\xc3\x28", 2));
            return;
        }
        if (operation == "unicode_progress") {
            context.report_progress(1, 1,
                std::string(addon::limits::k_max_progress_note_bytes - 1, 'x') + "\xe2\x82\xac");
            context.succeed(addon::Json_value::object());
            return;
        }
        if (operation == "progress") {
            const addon::Json_value* count = request.payload.find("count");
            if (count == nullptr || !count->is_integer() || count->as_integer() < 0) {
                context.fail(addon::Result_status::FAILED, "invalid_payload", "count must be a non-negative integer");
                return;
            }
            const std::uint64_t total = (std::uint64_t)count->as_integer();
            for (std::uint64_t i = 1; i <= total; ++i) {
                context.report_progress(i, total, "step");
            }
            addon::Json_value payload = addon::Json_value::object();
            payload.set("reported", addon::Json_value((std::int64_t)total));
            context.succeed(std::move(payload));
            return;
        }
        if (operation == "raw_progress_then_wait") {
            addon::Progress_message progress;
            progress.request_id = request.request_id;
            progress.total = 8192;
            for (std::uint64_t step = 1; step <= *progress.total; ++step) {
                progress.completed = step;
                m_runtime->write_raw(addon::encode_frame(addon::encode_adapter_message(addon::Adapter_message(progress))));
                if (step % 64 == 0 && context.wait_for_cancel(2)) {
                    context.fail(addon::Result_status::CANCELLED, "cancelled");
                    return;
                }
            }
            context.wait_for_cancel(k_forever_ms);
            context.fail(addon::Result_status::CANCELLED, "cancelled");
            return;
        }
        if (operation == "progress_then_wait") {
            // Reports `count` times, `interval_ms` apart, then waits for its
            // cancel without reporting again.
            const addon::Json_value* count    = request.payload.find("count");
            const addon::Json_value* interval = request.payload.find("interval_ms");
            if (count == nullptr || !count->is_integer() || count->as_integer() < 0 ||
                interval == nullptr || !interval->is_integer() || interval->as_integer() < 0)
            {
                context.fail(addon::Result_status::FAILED, "invalid_payload",
                    "count and interval_ms must be non-negative integers");
                return;
            }
            const std::uint64_t total = (std::uint64_t)count->as_integer();
            const int pause = (int)std::min<std::int64_t>(interval->as_integer(), k_forever_ms);
            for (std::uint64_t i = 1; i <= total; ++i) {
                context.report_progress(i, total);
                if (context.wait_for_cancel(pause)) {
                    context.fail(addon::Result_status::CANCELLED, "cancelled");
                    return;
                }
            }
            context.wait_for_cancel(k_forever_ms);
            context.fail(addon::Result_status::CANCELLED, "cancelled");
            return;
        }
        if (operation == "sleep") {
            const addon::Json_value* ms = request.payload.find("ms");
            if (ms == nullptr || !ms->is_integer() || ms->as_integer() < 0) {
                context.fail(addon::Result_status::FAILED, "invalid_payload", "ms must be a non-negative integer");
                return;
            }
            const int duration = (int)std::min<std::int64_t>(ms->as_integer(), k_forever_ms);
            bool cancelled = false;
            if (m_behavior == Behavior::IGNORES_CANCEL) {
                sleep_ms(duration);
            }
            else {
                cancelled = context.wait_for_cancel(duration);
            }
            if (cancelled) {
                context.fail(addon::Result_status::CANCELLED, "cancelled");
                return;
            }
            addon::Json_value payload = addon::Json_value::object();
            payload.set("slept_ms", addon::Json_value((std::int64_t)duration));
            context.succeed(std::move(payload));
            return;
        }
        if (operation == "spawn_child") {
            const addon::Json_value* ms = request.payload.find("ms");
            const int duration = ms != nullptr && ms->is_integer() ? (int)std::min<std::int64_t>(ms->as_integer(), k_forever_ms) : 3000;
            const bool detach = m_behavior == Behavior::DETACH_GRANDCHILD;
            const Spawned_child child = spawn_sleeping_child(m_executable, duration, detach);
            if (child.pid == 0) {
                context.fail(addon::Result_status::FAILED, "spawn_failed", child.error);
                return;
            }
            addon::Json_value payload = addon::Json_value::object();
            payload.set("child_pid", addon::Json_value((std::int64_t)child.pid));
            payload.set("detached",  addon::Json_value(child.detached));
            if (!child.error.empty()) {
                payload.set("note", addon::Json_value(child.error));
            }
            context.succeed(std::move(payload));
            return;
        }
        if (operation == "fail") {
            const addon::Json_value* reason  = request.payload.find("reason");
            const addon::Json_value* message = request.payload.find("message");
            context.fail(addon::Result_status::FAILED,
                reason != nullptr && reason->is_string() ? reason->as_string() : std::string("probe_failure"),
                message != nullptr && message->is_string() ? message->as_string() : std::string());
            return;
        }
        if (operation == "unavailable") {
            context.fail(addon::Result_status::UNAVAILABLE, "probe_unavailable", "requested unavailability");
            return;
        }
        if (operation == "propose_detached") {
            addon::Process_proposal proposal;
            proposal.executable = "helpers/background-sync";
            proposal.argv       = {"--daemon"};
            proposal.start      = addon::Start_intent::DETACHED;
            addon::Json_value payload = addon::Json_value::object();
            payload.set("process_proposal", addon::encode_process_proposal(proposal));
            context.succeed(std::move(payload));
            return;
        }
        context.reject_unknown_operation();
    }

    // -----------------------------------------------------------------------
    // structured-session@3

    void handle_structured_session(addon::Invocation_context& context)
    {
        const std::string& operation = context.request().operation;
        if (operation == addon::k_prepare_operation) {
            prepare_structured(context);
            return;
        }
        if (operation == addon::k_inspect_operation) {
            inspect_structured(context);
            return;
        }
        context.reject_unknown_operation();
    }

    // The prompt reaches the helper as the proposal's stdin payload, a JSON
    // string, so this fixture takes text prompts only.
    bool prompt_is_text(addon::Invocation_context& context, const std::string& prompt)
    {
        if (addon::is_valid_utf8(prompt)) {
            return true;
        }
        context.fail(addon::Result_status::FAILED, addon::reason::k_invalid_utf8,
            "the prompt is not UTF-8 text; this fixture hands it to its helper as the stdin payload");
        return false;
    }

    bool native_preparation_uses_direct(
        addon::Invocation_context& context, const addon::Launcher_request& launcher)
    {
        if (native_preparation_fixture(launcher) && launcher.runner_key != addon::k_runner_direct) {
            context.fail(addon::Result_status::FAILED, "fixture_native_runner_required",
                "the native preparation fixture requires the Direct runner");
            return false;
        }
        return true;
    }

    void prepare_structured(addon::Invocation_context& context)
    {
        addon::Contract_failure failure;
        const std::optional<addon::Structured_prepare_request> request =
            addon::decode_structured_prepare_request(context.request().payload, &failure);
        if (!request) {
            fail_with(context, failure);
            return;
        }
        if (!prompt_is_text(context, request->prompt)) {
            return;
        }
        if (!native_preparation_uses_direct(context, request->context.launcher)) {
            return;
        }
        addon::Structured_prepare_result result;
        result.process = helper_process(
            "structured", request->purpose, request->prompt, {}, addon::Cleanup_intent::REMOVE_SCRATCH);
        result.process.environment.emplace_back("UNKNOWN_FIXTURE_PREPARE_DEADLINE_MS",
            std::to_string(context.request().deadline_ms));
        add_session_context(result.process, request->context);
        if (m_behavior == Behavior::PROVIDER_SESSIONS_ASSETS) {
            result.process.working_directory = addon::Working_directory_intent::SCRATCH_DIRECTORY;
        }
        result.inspection.incremental = true;
        result.profile_storage        = request->context.profile_storage;
        result.account_context = request->context.account_context;
        if (request->prompt == "split-model-observation") {
            result.inspection.protocol_state = encode_inspect_state({});
            result.inspection.protocol_state.resize(
                addon::limits::k_max_exchange_protocol_state_code_units, ' ');
        }
        if (m_behavior == Behavior::PROVIDER_SESSIONS_STDIN_EOF) {
            configure_stdin_eof(result.process, result.inspection, request->prompt);
        }
        context.succeed(addon::encode_structured_prepare_result(result));
    }

    // R requests the last input, DONE\n proves native EOF, and dots are
    // advisory output. An unfinished receipt must fail final inspection.
    // X/L/C deliberately exercise repeated closure, forbidden input and early
    // completion. A no-newline native scenario proves that add-on record
    // framing, including unfinished suffixes, is owned by this inspector.
    // Large continuation padding forces the real proxy frame split.
    void configure_stdin_eof(
        addon::Process_proposal& process, addon::Inspection_setup& inspection,
        const std::string& scenario)
    {
        process.argv = {"stdin-eof", scenario};
        process.stdin_payload = "initial\n";
        inspection.keep_stdin_open = true;
        inspection.protocol_state = "00";
        if (scenario == "native-family") {
            process.argv = {"runtime-family"};
            process.stdin_payload.clear();
            inspection.keep_stdin_open = false;
        }
        if (scenario == "presentation-history") {
            inspection.protocol_state = "history:00";
        }
        if (scenario == "oversized-result") {
            inspection.protocol_state = "oversized:00";
        }
        if (scenario == "close-only") {
            process.stdin_payload += std::string(128u * 1024u, 'p') + "\n";
            inspection.protocol_state[0] = '2';
        }
        if (scenario == "split" || scenario == "split-after-input") {
            inspection.protocol_state.resize(2u * 1024u * 1024u, '.');
        }
        if (scenario == "split-after-input") {
            inspection.protocol_state[0] = '1';
        }
    }

    void inspect_stdin_eof(addon::Invocation_context& context)
    {
        addon::Contract_failure failure;
        const auto request = addon::decode_structured_inspect_request(context.request().payload, &failure);
        if (!request) {
            fail_with(context, failure);
            return;
        }
        const auto& facts = request->inspection;
        std::string state = facts.protocol_state;
        const bool history = state.starts_with("history:");
        const bool oversized = state.starts_with("oversized:");
        const std::string prefix = history ? "history:" : oversized ? "oversized:" : "";
        state.erase(0, prefix.size());
        if (state.size() < 2 || state[0] < '0' || state[0] > '2' ||
            state[1] < '0' || state[1] > '5' ||
            state.find_first_not_of('.', 2) != std::string::npos)
        {
            context.fail(addon::Result_status::FAILED, addon::reason::k_inspection_invalid,
                "invalid EOF fixture continuation");
            return;
        }
        addon::Structured_inspect_result result;
        bool invalid = false;
        for (char record : facts.stdout_bytes) {
            if (state[1] > '0' && state[1] < '5') {
                constexpr std::string_view k_receipt = "DONE\n";
                invalid = invalid || record != k_receipt[(std::size_t)(state[1] - '0')];
                ++state[1];
                continue;
            }
            switch (record) {
                case 'P':
                case 'Q':
                    if (!history || state[0] != '0') {
                        invalid = true;
                        break;
                    }
                    result.presentation_state = addon::Presentation_state::AVAILABLE;
                    result.presentation.push_back({record == 'P' ?
                        addon::Presentation_event_kind::REASONING :
                        addon::Presentation_event_kind::ANALYSIS_UPDATE, 1});
                    if (!result.stdin_bytes) {
                        result.stdin_bytes.emplace();
                    }
                    result.stdin_bytes->append("next\n");
                    break;
                case 'R':
                    if (state[0] == '1') {
                        invalid = true;
                        break;
                    }
                    result.close_stdin = true;
                    if (state[0] == '0') {
                        result.stdin_bytes = std::string(128u * 1024u, 'p') + "\n";
                    }
                    state[0] = '1';
                    break;
                case 'D':
                    invalid = invalid || state[0] != '1' || state[1] != '0';
                    state[1] = '1';
                    break;
                case '.':
                case '\n': break;
                case 'X': result.close_stdin = true; break;
                case 'L': result.stdin_bytes = "forbidden-after-eof\n"; break;
                case 'C': result.complete = true; break;
                default:  invalid = true; break;
            }
        }
        result.complete = result.complete || facts.final || invalid;
        if (result.complete) {
            result.close_stdin = false;
            result.stdin_bytes.reset();
            if (invalid) {
                result.failure_reason = "invalid_native_record";
            }
            else
            if (facts.timed_out) {
                result.failure_reason = "process_timed_out";
            }
            else
            if (facts.exit_code != 0) {
                result.failure_reason = "native_exit_failed";
            }
            else
            if (facts.final && state[1] != '5') {
                result.failure_reason = "missing_native_eof_receipt";
            }
            else {
                result.result_bytes = oversized ?
                    std::string(addon::limits::k_max_structured_stream_bytes + 1, 'x') :
                    "all input received before EOF";
            }
        }
        else {
            result.protocol_state = prefix + state;
        }
        context.succeed(addon::encode_structured_inspect_result(result));
    }

    void inspect_structured(addon::Invocation_context& context)
    {
        ++m_inspection_count;
        if (m_hold_inspection.load()) {
            m_inspection_entered.store(true);
            while (m_hold_inspection.load()) {
                if (context.wait_for_cancel(10)) {
                    context.fail(addon::Result_status::CANCELLED, "cancelled");
                    return;
                }
            }
        }
        if (m_behavior == Behavior::PROVIDER_SESSIONS_STDIN_EOF) {
            inspect_stdin_eof(context);
            return;
        }
        // This adapter models the strict inspect decoder shipped with interactive-only model reporting.
        if (m_behavior == Behavior::PROVIDER_SESSIONS_ASSETS && context.request().payload.find("model_state")) {
            context.fail(addon::Result_status::FAILED, addon::reason::k_unexpected_member, "inspect.model_state");
            return;
        }
        addon::Contract_failure failure;
        const std::optional<addon::Structured_inspect_request> request =
            addon::decode_structured_inspect_request(context.request().payload, &failure);
        if (!request) {
            fail_with(context, failure);
            return;
        }
        std::optional<Inspect_state> state = decode_inspect_state(request->inspection.protocol_state);
        if (!state) {
            context.fail(addon::Result_status::FAILED, addon::reason::k_inspection_invalid,
                "protocol_state is not one this fixture issued");
            return;
        }
        const std::string previous_model = state->observed_model;
        std::vector<addon::Presentation_event> presentation;
        feed_inspection(*state, request->inspection.stdout_bytes, presentation);

        addon::Structured_inspect_result result;
        result.complete = request->inspection.final;
        if (request->model_state_version == addon::k_model_state_version &&
            !state->observed_model.empty() && state->observed_model != previous_model)
        {
            result.model_observation = addon::Structured_model_observation{
                {}, state->observed_model, "fixture.native_output"};
        }
        if (result.complete) {
            const std::optional<std::string> failure_reason = final_failure_reason(*state, request->inspection);
            if (failure_reason.has_value()) {
                result.failure_reason = failure_reason;
                if (request->inspection.exit_code == 17) {
                    const std::string prefix = request->report_failure_diagnostics
                        ? "Requested native failure: " : "Unrequested native failure: ";
                    result.failure_diagnostics = addon::Structured_failure_diagnostics{
                        "process_exit", prefix + std::string(2300, 'd') + " END", false};
                }
            }
            else {
                if (state->result_file.empty()) {
                    result.result_bytes = state->result;
                }
                else {
                    const std::optional<std::string> file_bytes = read_result_file(*state);
                    if (file_bytes.has_value()) {
                        result.result_bytes = *file_bytes;
                    }
                    else {
                        result.failure_reason = "fixture_result_file_unreadable";
                    }
                }
            }
        }
        if (!result.complete) {
            result.protocol_state = encode_inspect_state(*state);
        }
        if (!presentation.empty()) {
            result.presentation_state = addon::Presentation_state::AVAILABLE;
            result.presentation       = std::move(presentation);
        }
        context.succeed(addon::encode_structured_inspect_result(result,
            request->report_failure_diagnostics ||
                request->inspection.stderr_bytes == "fixture-force-failure-diagnostics"));
    }

    // -----------------------------------------------------------------------
    // delegated-session@3

    void handle_delegated_session(addon::Invocation_context& context)
    {
        if (context.request().operation != addon::k_prepare_operation) {
            context.reject_unknown_operation();
            return;
        }
        addon::Contract_failure failure;
        const std::optional<addon::Delegated_prepare_request> request =
            addon::decode_delegated_prepare_request(context.request().payload, &failure);
        if (!request) {
            fail_with(context, failure);
            return;
        }
        if (!prompt_is_text(context, request->prompt)) {
            return;
        }
        if (!native_preparation_uses_direct(context, request->context.launcher)) {
            return;
        }
        const std::string& task_root = request->control.private_artifact_root;
        if (request->prompt == "orchestration-scoped-tools" && !request->control.connection) {
            failure.reason_code = addon::reason::k_unsupported_version;
            failure.detail = "scoped tools require control.connection v1";
            fail_with(context, failure);
            return;
        }
        addon::Delegated_prepare_result result;
        result.process = helper_process(
            "delegated",
            request->purpose,
            request->prompt,
            {"--artifact-root", task_root},
            addon::Cleanup_intent::RETAIN_SCRATCH);
        add_session_context(result.process, request->context);

        if (request->control.connection) {
            result.process.argv.push_back("--relay-program");
            result.process.argv.push_back(request->control.connection->relay_program);
            result.process.argv.push_back("--descriptor-path");
            result.process.argv.push_back(request->control.connection->descriptor_path);
            result.acknowledgment.connection_version = addon::k_delegated_connection_version;
        }

        // A workspace-local opt-in holds the real child until the test releases
        // it. No vendor protocol or control credential enters this fixture.
        std::error_code hold_error;
        if (fs::is_regular_file(addon::path_from_utf8(request->context.working_directory) /
                ".unknown-fixture-delegation-hold", hold_error))
        {
            result.process.argv.push_back("--fixture-control");
            result.process.argv.push_back(request->context.working_directory);
            result.process.environment.emplace_back(
                "UNKNOWN_FIXTURE_DELEGATION_CANARY", "fixture-canary");
        }

        // The servers artifact names the servers, their programs and actions
        // for the helper; the control environment carries the host's endpoint
        // and token and is request input, never part of a result.
        addon::Json_value servers = addon::Json_value::array();
        for (const addon::Delegated_task_server& server : request->control.servers) {
            result.acknowledgment.server_ids.push_back(server.server_id);
            addon::Json_value actions = addon::Json_value::array();
            for (const std::string& action : server.actions) {
                actions.push_back(addon::Json_value(action));
            }
            addon::Json_value entry = addon::Json_value::object();
            entry.set("server_id", addon::Json_value(server.server_id));
            entry.set("program",   addon::Json_value(server.program));
            entry.set("actions",   std::move(actions));
            servers.push_back(std::move(entry));
        }
        addon::Json_value artifact = addon::Json_value::object();
        artifact.set("version", addon::Json_value(1));
        artifact.set("task_id", addon::Json_value(request->control.task_id));
        artifact.set("servers", std::move(servers));
        result.private_artifacts.push_back({k_servers_artifact_path, addon::serialize_json(artifact) + "\n"});
        // Declare files the helper will populate so the host tracks their
        // exact cleanup paths, not just the servers file or its parent.
        result.private_artifacts.push_back({"unknown-fixture-helper/summary.json", ""});
        if (request->prompt == "orchestration-diagnostic-failure") {
            // Only .logonomic is reserved: this is a valid provider filename.
            result.private_artifacts.push_back({"diagnostics.json", "provider-owned evidence"});
        }
        if (request->prompt == "orchestration-large-result" || request->prompt == "orchestration-invalid-utf8") {
            result.private_artifacts.push_back({".unknown-fixture-result.bin", ""});
        }
        if (request->prompt == "orchestration-scoped-tools") {
            for (const char* name : {"request.json", "response.json", "ready.json"}) {
                const std::string path = std::string("unknown-fixture-tools/") + name;
                result.private_artifacts.push_back({path, ""});
                result.private_artifacts.push_back({path + ".tmp", ""});
            }
        }
        result.inspection.incremental = true;
        result.profile_storage        = request->context.profile_storage;
        result.account_context = request->context.account_context;
        if (m_behavior == Behavior::PROVIDER_SESSIONS_STDIN_EOF) {
            configure_stdin_eof(result.process, result.inspection, request->prompt);
        }
        result.model_selection = request->model_selection;
        if (request->prompt == "delegation-model-selection-context") {
            addon::Json_value observed = addon::Json_value::object();
            for (const char* field : {"launcher", "launcher_argv", "account_context", "model_selection", "purpose"}) {
                if (const addon::Json_value* value = context.request().payload.find(field)) {
                    observed.set(field, *value);
                }
            }
            result.private_artifacts.push_back({"model-selection-context.json", addon::serialize_json(observed)});
        }
        if (request->prompt == "delegation-model-selection-omit-echo") {
            result.model_selection.reset();
        }
        if (request->prompt == "delegation-model-selection-change-model" && result.model_selection) {
            result.model_selection->model = "different-native-model";
        }
        if (request->prompt == "delegation-model-selection-omit-effort" && result.model_selection) {
            result.model_selection->reasoning_effort.reset();
        }
        if (request->prompt == "delegation-model-selection-unsolicited") {
            result.model_selection = addon::Delegated_model_selection{"unsolicited-model", std::nullopt};
        }
        context.succeed(addon::encode_delegated_prepare_result(result));
    }

    // -----------------------------------------------------------------------
    // interactive-resume

    void handle_interactive_resume(addon::Invocation_context& context)
    {
        const auto version = context.request().capability.version;
        if (version == addon::k_interactive_resume_inspection_version ||
            version == addon::k_interactive_resume_content_version)
        {
            if (context.request().operation != addon::k_inspect_resume_operation) {
                context.reject_unknown_operation();
                return;
            }
            addon::Contract_failure failure;
            const auto request = addon::decode_resume_inspect_request(context.request().payload, &failure);
            if (!request) {
                fail_with(context, failure);
                return;
            }
            const auto* state_path = request->resume_record.find("fixture_state_path");
            if (!state_path || !state_path->is_string()) {
                context.fail(addon::Result_status::FAILED, "fixture_state_path_missing");
                return;
            }
            // A disposable control file lets the broker test release held
            // observations and change native availability without a CLI launch.
            const auto path = addon::path_from_utf8(state_path->as_string());
            std::error_code error;
            while (!fs::exists(path, error) && !error) {
                if (context.wait_for_cancel(10)) {
                    context.fail(addon::Result_status::CANCELLED, "cancelled");
                    return;
                }
            }
            std::ifstream input(path, std::ios::binary);
            std::string state;
            std::string content;
            input >> state >> content;
            if (state == "error") {
                context.fail(addon::Result_status::FAILED, "fixture_inspection_failed");
                return;
            }
            if (state == "malformed") {
                context.succeed(addon::Json_value::object());
                return;
            }
            addon::Resume_inspect_result result;
            if (state == "available") {
                result.availability = addon::Resume_availability::AVAILABLE;
            }
            else
            if (state == "absent" || state == "wrong-digest") {
                result.availability = addon::Resume_availability::ABSENT;
            }
            if (content == "empty") {
                result.content = addon::Resume_content::EMPTY;
            }
            else
            if (content == "nonempty") {
                result.content = addon::Resume_content::NONEMPTY;
            }
            result.request_digest_hex = state == "wrong-digest"
                ? std::string(64, '0') : addon::resume_inspect_request_digest_hex(*request);
            context.succeed(addon::encode_resume_inspect_result(result, version));
            return;
        }
        if (context.request().capability.version == addon::k_interactive_resume_environment_version &&
            context.request().operation == addon::k_classify_startup_operation)
        {
            addon::Contract_failure failure;
            const auto request = addon::decode_startup_observation_request(context.request().payload, &failure);
            if (!request) {
                fail_with(context, failure);
                return;
            }
            // Fixture evidence uses the normalized identity grammar as its
            // provider format. The host must still classify these opaque bytes
            // through the negotiated package before completing a checkpoint.
            const auto evidence = addon::parse_json(request->body, {}, nullptr);
            const auto observation = evidence
                ? addon::decode_startup_observation_result(*evidence, &failure) : std::nullopt;
            if (!observation) {
                context.fail(addon::Result_status::FAILED, "fixture_startup_observation_invalid");
                return;
            }
            context.succeed(addon::encode_startup_observation_result(*observation));
            return;
        }
        if (context.request().operation != addon::k_prepare_resume_operation) {
            context.reject_unknown_operation();
            return;
        }
        addon::Contract_failure failure;
        const std::optional<addon::Resume_prepare_request> request =
            addon::decode_resume_prepare_request(context.request().payload, &failure, m_observations.resume_version);
        if (!request) {
            fail_with(context, failure);
            return;
        }
        if (const auto* reason = request->resume_record.find("fixture_failure_reason");
            reason && reason->is_string())
        {
            const auto* description = request->resume_record.find("fixture_failure_description");
            const auto* unavailable = request->resume_record.find("fixture_failure_unavailable");
            const auto status = unavailable && unavailable->is_boolean() && unavailable->as_boolean()
                ? addon::Result_status::UNAVAILABLE : addon::Result_status::FAILED;
            context.fail(status, reason->as_string(),
                description && description->is_string() ? description->as_string() : std::string{});
            return;
        }
        const addon::Json_value* session_id = request->resume_record.find("provider_session_id");
        if (session_id == nullptr || !session_id->is_string() || session_id->as_string().empty() ||
            session_id->as_string().size() > addon::limits::k_max_lifecycle_identity_bytes ||
            has_control_characters(session_id->as_string()))
        {
            context.fail(addon::Result_status::FAILED, addon::reason::k_resume_record_invalid,
                "record.provider_session_id is missing, empty, oversize or not plain text");
            return;
        }
        if (request->context.launcher.script_body == "unknown-fixture-cli --slow-resume") {
            if (context.wait_for_cancel(k_slow_resume_delay_ms)) {
                context.fail(addon::Result_status::CANCELLED, "cancelled");
                return;
            }
        }
        if (request->context.launcher.script_body == "unknown-fixture-cli --wait-for-resume-cancel") {
            context.wait_for_cancel(k_forever_ms);
            context.fail(addon::Result_status::CANCELLED, "cancelled");
            return;
        }
        addon::Resume_prepare_result result;
        result.process.executable        = addon::Process_executable::launcher_program();
        result.process.argv              = {"--resume", session_id->as_string()};
        result.process.working_directory = addon::Working_directory_intent::SESSION_WORKSPACE;
        result.process.stdin_behavior    = addon::Stdin_behavior::INHERIT;
        result.process.start             = addon::Start_intent::ATTACHED;
        result.process.cleanup           = addon::Cleanup_intent::RETAIN_SCRATCH;
        result.patch.set                 = {
            {"UNKNOWN_FIXTURE_RESUME", "1"},
            {"UNKNOWN_FIXTURE_RESUME_RECORD_DIGEST", addon::resume_record_digest_hex(request->resume_record)},
        };
        result.patch.profile_storage     = request->context.profile_storage;
        result.patch.account_context = request->context.account_context;
        if (request->context.codex_invocation) {
            const addon::Json_value* revision = request->resume_record.find("revision");
            const std::string mode = revision && revision->is_string() ? revision->as_string() : "native";
            result.process.argv.assign(request->context.launcher_argv.begin() + 1,
                request->context.launcher_argv.end());
            result.process.argv.push_back("--resume");
            result.process.argv.push_back(session_id->as_string());
            if (mode != "missing") {
                const std::size_t prefix_size = mode == "native" ? 1 : 2;
                result.patch.codex_invocation = addon::Codex_invocation{prefix_size};
                if (mode == "oversized") {
                    result.patch.codex_invocation->argument_prefix_size = result.process.argv.size() + 2;
                }
            }
        }
        add_session_patch_context(
            result.patch,
            request->context.profile_storage);
        // The foreign-digest variant acknowledges another session's record,
        // which a host must refuse as acknowledgment_mismatch without a launch.
        const addon::Json_value acknowledged = m_resume_foreign_digest
            ? session_record(session_id->as_string() + "-other")
            : request->resume_record;
        result.acknowledgment.record_digest_hex = addon::resume_record_digest_hex(acknowledged);
        result.startup_observer = request->startup_observer;
        context.succeed(addon::encode_resume_prepare_result(result, m_observations.resume_version));
    }

    // -----------------------------------------------------------------------
    // lifecycle-events@2: continuous observation. The lifetime fixture uses
    // its reserved helper correlation.

    void handle_lifecycle_events(addon::Invocation_context& context)
    {
        const std::string& operation = context.request().operation;
        if (m_behavior != Behavior::PROVIDER_SESSIONS_CONTINUOUS && !lifetime_sessions()) {
            context.reject_unknown_operation();
            return;
        }
        if (operation == addon::k_observe_operation) {
            observe(context);
            return;
        }
        if (operation == addon::k_poll_observations_operation) {
            poll_observations(context);
            return;
        }
        context.reject_unknown_operation();
    }

    bool lifetime_active(const std::string& nonce) const
    {
        const std::lock_guard<std::mutex> lock(m_lifetime_mutex);
        return m_lifetime_active.contains(nonce);
    }

    void record_lifetime_call(Lifetime_call call)
    {
        const std::lock_guard<std::mutex> lock(m_lifetime_mutex);
        m_lifetime_calls.push_back(std::move(call));
    }

    Lifetime_response lifetime_response(Lifetime_response Lifetime_controls::* member) const
    {
        const std::lock_guard<std::mutex> lock(m_lifetime_mutex);
        return m_lifetime_controls.*member;
    }

    bool wait_for_lifetime_release(addon::Invocation_context& context, const char* operation)
    {
        bool* held = nullptr;
        bool* released = nullptr;
        {
            const std::lock_guard<std::mutex> lock(m_lifetime_mutex);
            if (std::string_view(operation) == "reserve") {
                held = &m_lifetime_reserve_held;
                released = &m_lifetime_reserve_released;
            }
            else if (std::string_view(operation) == "retire") {
                held = &m_lifetime_retire_held;
                released = &m_lifetime_retire_released;
            }
            else {
                held = &m_lifetime_observe_held;
                released = &m_lifetime_observe_released;
            }
            *held = true;
            *released = false;
        }
        while (true) {
            {
                const std::lock_guard<std::mutex> lock(m_lifetime_mutex);
                if (*released) {
                    *held = false;
                    *released = false;
                    return true;
                }
            }
            if (context.wait_for_cancel(10)) {
                const std::lock_guard<std::mutex> lock(m_lifetime_mutex);
                *held = false;
                return false;
            }
        }
    }

    void handle_lifecycle_correlation_lifetime(addon::Invocation_context& context)
    {
        if (!lifetime_sessions()) {
            context.reject_unknown_operation();
            return;
        }
        addon::Contract_failure failure;
        if (context.request().operation == addon::k_lifecycle_correlation_lifetime_reserve_operation) {
            const auto request = addon::decode_lifecycle_correlation_reserve_request(
                context.request().payload, &failure);
            if (!request) {
                fail_with(context, failure);
                return;
            }
            const std::string& nonce = request->correlation.transition_nonce;
            record_lifetime_call({"reserve", nonce});
            const Lifetime_response response = lifetime_response(&Lifetime_controls::reserve);
            if (response == Lifetime_response::REJECT) {
                context.fail(addon::Result_status::FAILED, "fixture_lifetime_reserve_rejected");
                return;
            }
            if (response == Lifetime_response::HOLD && !wait_for_lifetime_release(context, "reserve")) {
                context.fail(addon::Result_status::CANCELLED, "cancelled");
                return;
            }
            {
                const std::lock_guard<std::mutex> lock(m_lifetime_mutex);
                // Idempotent reserve preserves any already-frozen session context.
                m_lifetime_active.try_emplace(nonce);
            }
            context.succeed(addon::encode_lifecycle_correlation_lifetime_ack());
            return;
        }
        if (context.request().operation == addon::k_lifecycle_correlation_lifetime_retire_operation) {
            const auto request = addon::decode_lifecycle_correlation_retire_request(
                context.request().payload, &failure);
            if (!request) {
                fail_with(context, failure);
                return;
            }
            const std::string disposition = request->disposition ==
                    addon::Lifecycle_correlation_retire_disposition::TERMINAL_CONSUMED
                ? "terminal-consumed" : "abandoned";
            record_lifetime_call({"retire", request->correlation.transition_nonce, std::nullopt,
                disposition, request->consumed_cursor});
            const Lifetime_response response = lifetime_response(&Lifetime_controls::retire);
            if (response == Lifetime_response::REJECT) {
                context.fail(addon::Result_status::FAILED, "fixture_lifetime_retire_rejected");
                return;
            }
            if (response == Lifetime_response::HOLD && !wait_for_lifetime_release(context, "retire")) {
                context.fail(addon::Result_status::CANCELLED, "cancelled");
                return;
            }
            {
                const std::lock_guard<std::mutex> lock(m_lifetime_mutex);
                const std::string& nonce = request->correlation.transition_nonce;
                const auto final = m_lifetime_terminal_cursors.find(nonce);
                if (m_lifetime_active.contains(nonce) &&
                    request->disposition == addon::Lifecycle_correlation_retire_disposition::TERMINAL_CONSUMED &&
                    (final == m_lifetime_terminal_cursors.end() ||
                        !request->consumed_cursor || *request->consumed_cursor != final->second))
                {
                    context.fail(addon::Result_status::FAILED, "fixture_lifetime_terminal_cursor_invalid");
                    return;
                }
                m_lifetime_active.erase(nonce);
                m_lifetime_terminal_cursors.erase(nonce);
            }
            context.succeed(addon::encode_lifecycle_correlation_lifetime_ack());
            return;
        }
        context.reject_unknown_operation();
    }

    // One window: re-read the events file until the window elapses or the
    // host cancels, announcing every growth as progress; observations
    // themselves travel only through poll_observations.
    void observe(addon::Invocation_context& context)
    {
        using Clock = std::chrono::steady_clock;
        addon::Contract_failure failure;
        const std::optional<addon::Observe_request> request =
            addon::decode_observe_request(context.request().payload, &failure);
        if (!request) {
            fail_with(context, failure);
            return;
        }
        std::string correlation;
        bool lifetime_observe_was_held = false;
        if (lifetime_sessions()) {
            if (!request->correlation || !lifetime_active(request->correlation->transition_nonce)) {
                context.fail(addon::Result_status::FAILED, "fixture_lifetime_unreserved");
                return;
            }
            correlation = request->correlation->transition_nonce;
            record_lifetime_call({"observe", correlation, request->resume_cursor});
            {
                const std::lock_guard<std::mutex> lock(m_lifetime_mutex);
                m_lifetime_active[correlation] = request->session.provider_session_id;
            }
            if (lifetime_response(&Lifetime_controls::observe) == Lifetime_response::HOLD) {
                lifetime_observe_was_held = true;
                if (!wait_for_lifetime_release(context, "observe")) {
                    context.fail(addon::Result_status::CANCELLED, "cancelled");
                    return;
                }
            }
        }
        const std::optional<Observation_cursor> cursor = decode_cursor(request->resume_cursor);
        if (!cursor) {
            context.fail(addon::Result_status::FAILED, addon::reason::k_frame_violation,
                "resume_cursor is not one this fixture issued");
            return;
        }
        const std::string token = m_sessions.observe(request->session.working_directory);
        if (!cursor->token.empty() && cursor->token != token) {
            context.fail(addon::Result_status::FAILED, addon::reason::k_frame_violation,
                "resume_cursor names another session than the request");
            return;
        }
        const fs::path          events   = events_path(request->session.working_directory);
        const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(
            lifetime_observe_was_held ? 0 : request->window_ms);
        std::uint64_t announced = 0;
        while (true) {
            const std::uint64_t complete = read_complete_lines(events).size();
            if (complete > cursor->index + announced) {
                announced = complete - cursor->index;
                context.report_progress(announced, std::nullopt, addon::observe_progress_note(announced));
            }
            const Clock::time_point now = Clock::now();
            if (now >= deadline) {
                break;
            }
            const auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            const int  wait_ms      =
                (int)std::max<long long>(1, std::min<long long>(remaining_ms, k_observe_poll_interval_ms));
            if (context.wait_for_cancel(wait_ms)) {
                context.fail(addon::Result_status::CANCELLED, "cancelled");
                return;
            }
        }
        const addon::Observe_result result{encode_cursor(token, cursor->index + announced)};
        context.succeed(addon::encode_observe_result(result));
    }

    void poll_observations(addon::Invocation_context& context)
    {
        addon::Contract_failure failure;
        const std::optional<addon::Poll_observations_request> request =
            addon::decode_poll_observations_request(context.request().payload, &failure);
        if (!request) {
            fail_with(context, failure);
            return;
        }
        std::string correlation;
        if (lifetime_sessions()) {
            if (!request->correlation || !lifetime_active(request->correlation->transition_nonce)) {
                context.fail(addon::Result_status::FAILED, "fixture_lifetime_unreserved");
                return;
            }
            correlation = request->correlation->transition_nonce;
            record_lifetime_call({"poll_observations", correlation, request->resume_cursor});
            bool terminal = false;
            std::string provider_session_id;
            std::optional<std::string> final_cursor;
            {
                const std::lock_guard<std::mutex> lock(m_lifetime_mutex);
                terminal = m_lifetime_controls.terminal;
                const auto active = m_lifetime_active.find(correlation);
                if (active != m_lifetime_active.end()) provider_session_id = active->second;
                const auto final = m_lifetime_terminal_cursors.find(correlation);
                if (final != m_lifetime_terminal_cursors.end()) final_cursor = final->second;
            }
            if (terminal || final_cursor) {
                addon::Poll_observations_result result;
                addon::Lifecycle_observation end;
                end.provider_event_name = "FixtureLifetimeEnd";
                end.provider_event_id = "fixture-lifetime-end-" + correlation;
                end.provider_session_id = provider_session_id;
                end.session_transition = addon::Session_transition::ENDED;
                result.observations.push_back(std::move(end));
                result.resume_cursor = final_cursor.value_or("terminal-consumed");
                result.more = false;
                if (!final_cursor) {
                    const std::lock_guard<std::mutex> lock(m_lifetime_mutex);
                    m_lifetime_terminal_cursors[correlation] = result.resume_cursor;
                    m_lifetime_controls.terminal = false;
                }
                context.succeed(addon::encode_poll_observations_result(result));
                return;
            }
        }
        const std::optional<Observation_cursor> cursor = decode_cursor(request->resume_cursor);
        if (!cursor) {
            context.fail(addon::Result_status::FAILED, addon::reason::k_frame_violation,
                "resume_cursor is not one this fixture issued");
            return;
        }
        const std::optional<Observed_sessions::Session> session = m_sessions.resolve(cursor->token);
        if (!session) {
            context.fail(addon::Result_status::FAILED, addon::reason::k_frame_violation,
                "resume_cursor names no session this adapter has observed");
            return;
        }
        const std::vector<std::string> lines = read_complete_lines(events_path(session->working_directory));
        addon::Poll_observations_result result;
        std::uint64_t index = cursor->index;
        while (index < lines.size() && result.observations.size() < request->limit) {
            std::optional<addon::Lifecycle_observation> observation =
                decode_events_line(lines[(std::size_t)index], (std::size_t)index + 1, &failure);
            if (!observation) {
                fail_with(context, failure);
                return;
            }
            result.observations.push_back(std::move(*observation));
            ++index;
        }
        result.resume_cursor = encode_cursor(session->token, index);
        result.more          = index < lines.size();
        context.succeed(addon::encode_poll_observations_result(result));
    }

    // -----------------------------------------------------------------------
    // permission-snapshot@1

    void handle_permission_snapshot(addon::Invocation_context& context)
    {
        if (context.request().operation != addon::k_classify_event_operation) {
            context.reject_unknown_operation();
            return;
        }
        addon::Contract_failure failure;
        const auto request = addon::decode_classify_event_request(context.request().payload, &failure);
        if (!request) {
            fail_with(context, failure);
            return;
        }
        const auto* pending = request->event.body.find("pending");
        std::string session;
        if (request->event.name != "StatusLine" || !pending || !pending->is_boolean() ||
            !bounded_body_string(request->event.body, "session_id",
                addon::limits::k_max_lifecycle_identity_bytes, session) || session.empty())
        {
            context.succeed(addon::encode_permission_snapshot_classification_result({}));
            return;
        }
        const auto* gate = request->event.body.find("fixture_gate_path");
        const auto* ready = request->event.body.find("fixture_ready_path");
        if (gate && gate->is_string() && ready && ready->is_string()) {
            if (!wait_for_fixture_gate(context, addon::path_from_utf8(gate->as_string()),
                addon::path_from_utf8(ready->as_string()), "fixture_snapshot_cancelled"))
            {
                return;
            }
        }
        // Return the claimed identity even when stale so the integration test
        // exercises the host's independent epoch and session checks.
        addon::Lifecycle_observation observation;
        observation.provider_event_name = request->event.name;
        observation.provider_session_id = std::move(session);
        addon::Permission_snapshot_classification_result result;
        result.base.classification = addon::Classification::EVENT;
        result.base.observations.push_back(std::move(observation));
        result.permission_snapshot = addon::Permission_snapshot_update{1, 0, pending->as_boolean()};
        context.succeed(addon::encode_permission_snapshot_classification_result(result));
    }

    // -----------------------------------------------------------------------
    // helper-ingress@2/@3


    void handle_terminal_identity(addon::Invocation_context& context)
    {
        if (context.request().operation != addon::k_classify_event_operation) {
            context.reject_unknown_operation();
            return;
        }
        const auto& payload = context.request().payload;
        const auto* supplied = payload.find("terminal_identity");
        const auto* binding = supplied ? supplied->find("context") : nullptr;
        const auto* frozen = binding ? binding->find("fixture_context") : nullptr;
        const auto* native = supplied ? supplied->find("terminal_event") : nullptr;
        const auto* terminal_payload = native ? native->find("payload") : nullptr;
        const auto* title = terminal_payload ? terminal_payload->find("title") : nullptr;
        if (!frozen || !frozen->is_string() || frozen->as_string() != "frozen-native-home") {
            context.fail(addon::Result_status::FAILED, "fixture_identity_context_missing");
            return;
        }
        addon::Classify_event_result result;
        addon::Json_value identity;
        if (title && title->is_string()) {
            const auto separator = title->as_string().rfind(" :: ");
            if (separator != std::string::npos) {
                const std::string session_id = title->as_string().substr(separator + 4);
                const auto* gated = binding->find("fixture_gate_session_id");
                const auto* gate = binding->find("fixture_gate_path");
                const auto* ready = binding->find("fixture_ready_path");
                if (gated && gated->is_string() && gated->as_string() == session_id &&
                    gate && gate->is_string() && ready && ready->is_string())
                {
                    if (!wait_for_fixture_gate(context, addon::path_from_utf8(gate->as_string()),
                        addon::path_from_utf8(ready->as_string()), "fixture_identity_cancelled"))
                    {
                        return;
                    }
                    // Longer than the host test's animation interval: raw
                    // spinner frames must not invalidate a stable selection.
                    if (context.wait_for_cancel(80)) {
                        context.fail(addon::Result_status::CANCELLED, "fixture_identity_cancelled");
                        return;
                    }
                }
                const auto* ledger = payload.find("ledger");
                const auto* current = ledger ? ledger->find("current_transition") : nullptr;
                const auto* previous = current ? current->find("provider_session_id") : nullptr;
                if (!previous || !previous->is_string() || previous->as_string() != session_id) {
                    addon::Lifecycle_observation observation;
                    observation.provider_event_name = "NativeSignal";
                    observation.provider_session_id = session_id;
                    observation.session_transition = addon::Session_transition::STARTED;
                    observation.session_source = "terminal_title";
                    observation.resume_record = session_record(session_id);
                    result.classification = addon::Classification::TRANSITION;
                    result.transition = addon::Transition_kind::RESUME;
                    result.observations.push_back(std::move(observation));
                }
                identity = addon::Json_value::object();
                identity.set("provider_session_id", addon::Json_value(session_id));
                identity.set("title", addon::Json_value(title->as_string().substr(0, separator)));
            }
        }
        auto response = addon::encode_classify_event_result(result);
        response.set("terminal_identity", std::move(identity));
        context.succeed(std::move(response));
    }

    void handle_helper_ingress(addon::Invocation_context& context)
    {
        if (context.request().operation != addon::k_classify_event_operation) {
            context.reject_unknown_operation();
            return;
        }
        addon::Contract_failure failure;
        const std::optional<addon::Classify_event_request> request =
            addon::decode_classify_event_request(context.request().payload, &failure);
        if (!request) {
            fail_with(context, failure);
            return;
        }
        if (m_classify_hang) {
            // The classification-timeout variant: no answer until the host
            // cancels, which it does at its deadline.
            while (!context.wait_for_cancel(k_forever_ms)) {}
            context.fail(addon::Result_status::CANCELLED, "cancelled");
            return;
        }
        if (lifetime_sessions()) {
            record_lifetime_call({"classify", request->transition_nonce});
            if (!lifetime_active(request->transition_nonce)) {
                context.fail(addon::Result_status::FAILED, "fixture_lifetime_unreserved");
                return;
            }
            if (lifetime_response(&Lifetime_controls::classify) == Lifetime_response::REJECT) {
                context.fail(addon::Result_status::FAILED, "fixture_lifetime_classify_rejected");
                return;
            }
        }

        const std::string&       name = request->event.name;
        const addon::Json_value& body = request->event.body;
        const auto* gate = body.find("fixture_gate_path");
        const auto* ready = body.find("fixture_ready_path");
        if (gate && gate->is_string() && ready && ready->is_string()) {
            if (!wait_for_fixture_gate(context, addon::path_from_utf8(gate->as_string()),
                addon::path_from_utf8(ready->as_string()), "fixture_classification_cancelled"))
            {
                return;
            }
        }
        // A result's classification defaults to `ignore`, which is the answer
        // for every event name below other than the three the fixture maps.
        addon::Classify_event_result result;
        addon::Json_value response = addon::Json_value::object();
        response.set("seen", addon::Json_value(name));
        if (const addon::Json_value* supplied = body.find("helper_response")) {
            response = *supplied;
        }
        result.helper_response = std::move(response);

        // The host test changes this source while a correlation reservation is
        // held. Reading here proves the sample belongs to actual dispatch,
        // rather than helper admission or a replayed payload.
        if (const auto* path = body.find("fixture_context_path"); path && path->is_string() &&
            request->context_state_version == addon::k_context_state_capability_version &&
            request->context_snapshot_revision)
        {
            std::ifstream input(addon::path_from_utf8(path->as_string()), std::ios::binary);
            char bytes[4097];
            input.read(bytes, sizeof(bytes));
            const auto size = static_cast<std::size_t>(input.gcount());
            const auto sample = size <= 4096
                ? addon::parse_json(std::string_view(bytes, size), {}, nullptr) : std::nullopt;
            if (sample) {
                addon::decode_context_updates(*sample, result.context_updates, result.context_failure);
                for (auto& update : result.context_updates) {
                    if (update.compaction_count) {
                        update.compaction_count_revision = request->context_snapshot_revision;
                    }
                }
            }
            else {
                std::string source;
                (void)bounded_body_string(body, "session_id", addon::limits::k_max_lifecycle_identity_bytes, source);
                result.context_failure = addon::Context_failure{std::move(source),
                    {addon::reason::k_observation_invalid, "fixture context source is unreadable"}};
            }
            result.helper_response->set("context_snapshot_revision", addon::Json_value(*request->context_snapshot_revision));
        }

        if (name == "SessionStart") {
            std::string session_id;
            std::string source;
            if (!bounded_body_string(body, "session_id", addon::limits::k_max_lifecycle_identity_bytes, session_id) ||
                !bounded_body_string(body, "source", addon::limits::k_max_lifecycle_label_bytes, source) ||
                session_id.empty())
            {
                context.fail(addon::Result_status::FAILED, addon::reason::k_observation_invalid,
                    "SessionStart needs a session_id within k_max_lifecycle_identity_bytes "
                    "and a source within k_max_lifecycle_label_bytes");
                return;
            }
            addon::Lifecycle_observation observation = base_observation(request->event);
            observation.session_transition  = addon::Session_transition::STARTED;
            observation.provider_session_id = session_id;
            observation.session_source      = source;
            observation.resume_record       = session_record(session_id);
            if (const addon::Json_value* revision = body.find("resume_revision")) {
                if (!revision->is_string() ||
                    revision->as_string().size() >
                        addon::limits::k_max_lifecycle_label_bytes ||
                    has_control_characters(revision->as_string()))
                {
                    context.fail(addon::Result_status::FAILED,
                        addon::reason::k_observation_invalid,
                        "SessionStart resume_revision must be a string within "
                        "k_max_lifecycle_label_bytes and free of control "
                        "characters");
                    return;
                }
                observation.resume_record->set("revision", *revision);
            }
            if (const auto* path = body.find("fixture_state_path"); path && path->is_string()) {
                observation.resume_record->set("fixture_state_path", *path);
            }
            // Explicit fixture-only control for exercising the host default
            // resume-record producer, rather than prebuilding its result.
            if (const auto* omit = body.find("resume_omit_record"); omit && omit->is_boolean() && omit->as_boolean()) {
                observation.resume_record.reset();
            }
            result.classification = addon::Classification::TRANSITION;
            result.transition     = source == "resume" ? addon::Transition_kind::RESUME : addon::Transition_kind::START;
            if (lifetime_sessions()) {
                const std::lock_guard<std::mutex> lock(m_lifetime_mutex);
                m_lifetime_active[request->transition_nonce] = session_id;
            }
            result.observations.push_back(std::move(observation));
            const auto* confirm = body.find("fixture_confirm_title");
            const auto* cwd = body.find("fixture_cwd");
            if (confirm && confirm->is_boolean() && confirm->as_boolean() &&
                request->terminal_identity && request->ledger.current_transition &&
                request->ledger.current_transition->provider_session_id == session_id)
            {
                // The queued-hook oracle requires the actual committed title
                // epoch. It confirms the current activation without opening
                // another epoch, and its execution only when asked.
                result.classification = addon::Classification::IGNORED;
                result.transition.reset();
                result.observations.clear();
                if (cwd && cwd->is_string() &&
                    request->execution_context_version == addon::k_execution_context_version)
                {
                    result.execution_updates.push_back({session_id, request->ledger.current_transition->nonce, 1,
                        addon::Execution_context_status::CONFIRMED, "local", cwd->as_string(), "fixture_native_hook"});
                }
            }
            else if (const auto* execution = body.find("fixture_confirm_execution");
                execution && execution->is_boolean() && execution->as_boolean() && cwd && cwd->is_string() &&
                request->execution_context_version == addon::k_execution_context_version)
            {
                result.execution_updates.push_back({session_id, request->transition_nonce, 1,
                    addon::Execution_context_status::CONFIRMED, "local", cwd->as_string(), "fixture_native_hook"});
            }
            else if (const auto* unavailable = body.find("fixture_execution_unavailable");
                unavailable && unavailable->is_boolean() && unavailable->as_boolean() &&
                request->execution_context_version == addon::k_execution_context_version)
            {
                // A provider that reports its execution namespace as one the
                // host cannot use, attributed to this activation.
                result.execution_updates.push_back({session_id, request->transition_nonce, 1,
                    addon::Execution_context_status::UNAVAILABLE, "unsupported", {}, "fixture_native_hook"});
            }
        }
        else
        if (name == "SessionEnd") {
            std::string session_id;
            if (!bounded_body_string(body, "session_id",
                    addon::limits::k_max_lifecycle_identity_bytes,
                    session_id) || session_id.empty())
            {
                context.fail(addon::Result_status::FAILED,
                    addon::reason::k_observation_invalid,
                    "SessionEnd needs a session_id within "
                    "k_max_lifecycle_identity_bytes");
                return;
            }
            // A session boundary remains usable when root correlation is lost.
            addon::Lifecycle_observation observation;
            observation.provider_event_name = name;
            observation.session_transition = addon::Session_transition::ENDED;
            observation.provider_session_id = session_id;
            result.classification = addon::Classification::EVENT;
            result.observations.push_back(std::move(observation));
        }
        else
        if (name == "Stop" || name == "UserPromptSubmit") {
            addon::Lifecycle_observation observation = base_observation(request->event);
            if (!bounded_body_string(body, "session_id", addon::limits::k_max_lifecycle_identity_bytes,
                    observation.provider_session_id) ||
                !bounded_body_string(body, "turn_id", addon::limits::k_max_lifecycle_identity_bytes,
                    observation.provider_turn_id))
            {
                context.fail(addon::Result_status::FAILED, addon::reason::k_observation_invalid,
                    name + " carries a session_id or turn_id beyond k_max_lifecycle_identity_bytes");
                return;
            }
            observation.phase_transition = name == "Stop"
                ? addon::Phase_transition::AWAITING_INPUT
                : addon::Phase_transition::WORKING;
            observation.phase_evidence   = addon::Evidence::CONFIRMED;
            result.classification = addon::Classification::EVENT;
            result.observations.push_back(std::move(observation));
        }
        else
        if (name == "AtomicReject") {
            addon::Lifecycle_observation first = base_observation(request->event);
            first.provider_session_id = "native-A";
            first.provider_turn_id = "turn-before-rejection";
            first.phase_transition = addon::Phase_transition::WORKING;
            first.phase_evidence = addon::Evidence::CONFIRMED;
            addon::Lifecycle_observation conflicting = first;
            conflicting.provider_session_id = "foreign-session";
            conflicting.provider_event_name = "AtomicRejectConflict";
            result.classification = addon::Classification::EVENT;
            result.observations.push_back(std::move(first));
            result.observations.push_back(std::move(conflicting));
        }
        if (name == "LifecycleDegraded" && context.request().capability.version ==
            addon::k_helper_ingress_degradation_version)
        {
            addon::Lifecycle_degradation degradation;
            if (const addon::Json_value* session = body.find("session_id")) {
                degradation.provider_session_id = session->as_string();
            }
            result.lifecycle_degradation = std::move(degradation);
        }
        context.succeed(addon::encode_classify_event_result(result));
    }

    std::string             m_package_id;
    Behavior                m_behavior;
    std::string             m_executable;
    bool                    m_classify_hang;
    bool                    m_resume_foreign_digest;
    std::atomic_bool        m_hold_inspection = false;
    std::atomic_bool        m_inspection_entered = false;
    std::atomic_uint64_t    m_inspection_count = 0;
    std::mutex             m_recognition_mutex;
    std::string            m_recognition_hold_script;
    bool                   m_recognition_armed = false;
    bool                   m_recognition_entered = false;
    bool                   m_recognition_released = false;
    bool                   m_recognition_cancel_observed = false;
    bool                   m_recognition_result_attempted = false;
    bool                   m_recognition_finished = false;
    addon::Adapter_runtime* m_runtime = nullptr;
    Observed_sessions       m_sessions;
    Observation_options     m_observations;
    mutable std::mutex      m_lifetime_mutex;
    Lifetime_controls       m_lifetime_controls;
    std::vector<Lifetime_call> m_lifetime_calls;
    std::map<std::string, std::string> m_lifetime_active;
    std::map<std::string, std::string> m_lifetime_terminal_cursors;
    std::atomic<std::size_t> m_quota_observation_count = 0;
    bool m_lifetime_reserve_held = false;
    bool m_lifetime_retire_held = false;
    bool m_lifetime_observe_held = false;
    bool m_lifetime_reserve_released = false;
    bool m_lifetime_retire_released = false;
    bool m_lifetime_observe_released = false;
    std::mutex m_wake_mutex;
    std::map<std::string, std::string> m_wake_modes{{"inspect", "available"}, {"notify", "accepted"}};
    std::map<std::string, bool> m_wake_held;
    std::map<std::string, bool> m_wake_released;
    addon::Json_value m_wake_calls = addon::Json_value::array();
    const std::uint64_t      m_usage_observed_at_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
};

int usage()
{
    std::fprintf(stderr, "usage: unknown-fixture-adapter [--behavior <name>] [--classify-hang] "
        "[--resume-foreign-digest] [--session-declaration-version <version>] | --child-sleep <ms>\n");
    return 2;
}

} // namespace

int main(int argc, char** argv)
{
    Behavior behavior              = Behavior::CONFORMANT;
    bool     classify_hang         = false;
    bool     resume_foreign_digest = false;
    Observation_options observations;
    const std::map<std::string_view, std::uint16_t*> version_options{
        {"--auth-version",                &observations.auth_version},
        {"--quota-version",               &observations.quota_version},
        {"--usage-version",               &observations.usage_version},
        {"--resume-version",              &observations.resume_version},
        {"--account-refresh-version",     &observations.account_refresh_version},
        {"--session-declaration-version", &observations.session_declaration_version},
    };
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--child-sleep" && i + 1 < argc) {
            sleep_ms(std::atoi(argv[i + 1]));
            return 0;
        }
        if (argument == "--behavior" && i + 1 < argc) {
            const std::string name = argv[++i];
            bool known = false;
            for (const Behavior_name& entry : k_behaviors) {
                if (name == entry.name) {
                    behavior = entry.behavior;
                    known    = true;
                }
            }
            if (!known) {
                std::fprintf(stderr, "unknown-fixture-adapter: unknown behavior %s\n", name.c_str());
                return usage();
            }
            continue;
        }
        if (argument == "--classify-hang") {
            classify_hang = true;
            continue;
        }
        if (argument == "--resume-foreign-digest") {
            resume_foreign_digest = true;
            continue;
        }
        // Declaration-only fixtures need matching handshakes, including an opaque future version.
        if (const auto option = version_options.find(argument); option != version_options.end() && i + 1 < argc) {
            const std::string_view version = argv[++i];
            std::uint16_t number = 0;
            const auto end = version.data() + version.size();
            const auto parsed = std::from_chars(version.data(), end, number);
            if (parsed.ec != std::errc{} || parsed.ptr != end || number == 0) {
                return usage();
            }
            *option->second = number;
            continue;
        }
        if (argument == "--with-accounts") {
            observations.accounts = true;
            continue;
        }
        if (argument == "--probe-mode" && i + 1 < argc) {
            observations.mode = argv[++i];
            continue;
        }
        if (argument == "--export-when-file-exists" && i + 1 < argc) {
            observations.export_enabled_file = addon::path_from_utf8(argv[++i]);
            continue;
        }
        return usage();
    }

    Fixture fixture(behavior, self_executable(argv[0]), classify_hang, resume_foreign_digest, observations);
    return fixture.run();
}
