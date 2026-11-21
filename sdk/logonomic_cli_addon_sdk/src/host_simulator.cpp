#include "logonomic_cli_addon/host_simulator.h"
#include "logonomic_cli_addon/frame.h"
#include "logonomic_cli_addon/limits.h"
#include "logonomic_cli_addon/package_source.h"
#include "logonomic_cli_addon/sha256.h"
#include "logonomic_cli_addon/version.h"
#include "simulator_process.h"
#include "strict_json.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <map>
#include <limits>

namespace logonomic::cli_addon {

namespace fs = std::filesystem;
using Clock  = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// Environment

namespace {

constexpr const char* k_linux_baseline[] = {
    "HOME", "USER", "LOGNAME", "LANG", "LC_ALL", "LC_CTYPE", "TZ", "TMPDIR",
};

constexpr const char* k_windows_baseline[] = {
    "PATHEXT", "SystemRoot", "SystemDrive", "windir", "TEMP", "TMP", "USERPROFILE", "USERNAME",
    "APPDATA", "LOCALAPPDATA", "ProgramData", "ProgramFiles", "ProgramFiles(x86)", "ProgramW6432",
    "NUMBER_OF_PROCESSORS", "PROCESSOR_ARCHITECTURE", "OS",
};

bool names_equal(std::string_view a, std::string_view b, bool case_insensitive)
{
    if (!case_insensitive) {
        return a == b;
    }
    return ascii_lowercase(a) == ascii_lowercase(b);
}

const std::string* ambient_value(
    const std::vector<std::pair<std::string, std::string>>& ambient,
    std::string_view                                        name,
    bool                                                    case_insensitive)
{
    for (const auto& [ambient_name, value] : ambient) {
        if (names_equal(ambient_name, name, case_insensitive)) {
            return &value;
        }
    }
    return nullptr;
}

std::string join_generations(const std::vector<std::uint16_t>& generations)
{
    std::string joined;
    for (const std::uint16_t generation : generations) {
        if (!joined.empty()) {
            joined.push_back(',');
        }
        joined += std::to_string(generation);
    }
    return joined;
}

} // namespace

std::vector<std::pair<std::string, std::string>> build_adapter_environment(
    const Adapter_environment_inputs&                       inputs,
    const std::vector<std::pair<std::string, std::string>>& ambient)
{
    const bool windows = inputs.platform_id == k_platform_windows_x86_64;
    std::vector<std::pair<std::string, std::string>> environment;

    // Baseline: allowlisted names copied from the ambient environment.
    if (windows) {
        for (const char* name : k_windows_baseline) {
            if (const std::string* value = ambient_value(ambient, name, true)) {
                environment.emplace_back(name, *value);
            }
        }
        const std::string* system_root = ambient_value(ambient, "SystemRoot", true);
        const std::string root = system_root != nullptr ? *system_root : std::string("C:\\Windows");
        environment.emplace_back("PATH",
            inputs.package_root + "\\helpers;" + root + "\\System32;" + root + ";" + root + "\\System32\\Wbem");
    }
    else {
        for (const char* name : k_linux_baseline) {
            if (const std::string* value = ambient_value(ambient, name, false)) {
                environment.emplace_back(name, *value);
            }
        }
        environment.emplace_back("PATH", inputs.package_root + "/helpers:/usr/local/bin:/usr/bin:/bin");
    }

    // Manifest variables; reserved names were rejected by the validator, and
    // a manifest variable replaces a baseline value of the same name.
    for (const auto& [name, value] : inputs.manifest_environment) {
        if (is_reserved_environment_name(name)) {
            continue;
        }
        bool replaced = false;
        for (auto& [existing_name, existing_value] : environment) {
            if (names_equal(existing_name, name, windows)) {
                existing_value = value;
                replaced       = true;
                break;
            }
        }
        if (!replaced) {
            environment.emplace_back(name, value);
        }
    }

    // Reserved, host-set.
    environment.emplace_back(k_env_package_id,                inputs.package_id);
    environment.emplace_back(k_env_generation_digest,         inputs.generation_digest);
    environment.emplace_back(k_env_generation_nonce,          inputs.generation_nonce);
    environment.emplace_back(k_env_package_root,              inputs.package_root);
    environment.emplace_back(k_env_scratch_dir,               inputs.scratch_directory);
    environment.emplace_back(k_env_core_protocol_generations, join_generations(inputs.core_protocol_generations));
    environment.emplace_back(k_env_host_product,              inputs.host_product);

    std::sort(environment.begin(), environment.end());
    return environment;
}

bool adapter_start_environment_within_budget(
    const std::vector<std::pair<std::string, std::string>>& environment)
{
    if (environment.size() > limits::k_max_start_environment_entries) {
        return false;
    }
    std::size_t bytes = 0;
    for (const auto& [name, value] : environment) {
        if (bytes > limits::k_max_start_environment_bytes ||
            name.size() > limits::k_max_start_environment_bytes - bytes)
        {
            return false;
        }
        bytes += name.size();
        if (value.size() > limits::k_max_start_environment_bytes - bytes) {
            return false;
        }
        bytes += value.size();
    }
    return true;
}

bool adapter_supervisor_environment_within_budget(
    const std::vector<std::pair<std::string, std::string>>& environment)
{
    if (environment.size() > limits::k_max_start_environment_entries + 7) {
        return false;
    }
    std::size_t bytes = 0;
    for (const auto& [name, value] : environment) {
        if (bytes > limits::k_max_supervisor_environment_bytes ||
            name.size() >= limits::k_max_supervisor_environment_bytes - bytes)
        {
            return false;
        }
        bytes += name.size() + 1;
        if (value.size() > limits::k_max_supervisor_environment_bytes - bytes) {
            return false;
        }
        bytes += value.size();
    }
    return true;
}

bool adapter_reserved_environment_within_budget(
    const std::vector<std::pair<std::string, std::string>>& environment)
{
    std::size_t bytes = 0;
    std::size_t entries = 0;
    for (const auto& [name, value] : environment) {
        if (!is_reserved_environment_name(name)) {
            continue;
        }
        ++entries;
        if (bytes > limits::k_max_reserved_environment_bytes ||
            name.size() >= limits::k_max_reserved_environment_bytes - bytes)
        {
            return false;
        }
        bytes += name.size() + 1;
        if (value.size() > limits::k_max_reserved_environment_bytes - bytes) {
            return false;
        }
        bytes += value.size();
    }
    return entries == 7;
}

// ---------------------------------------------------------------------------
// Host_simulator

const char* host_status_name(Host_status status)
{
    switch (status) {
        case Host_status::SUCCESS:             return "success";
        case Host_status::UNAVAILABLE:         return "unavailable";
        case Host_status::CANCELLED:           return "cancelled";
        case Host_status::FAILED:               return "error";
        case Host_status::UNSUPPORTED_VERSION: return "unsupported_version";
        case Host_status::UNKNOWN_CAPABILITY:  return "unknown_capability";
        case Host_status::PACKAGE_UNAVAILABLE: return "package_unavailable";
        case Host_status::TIMED_OUT:           return "timed_out";
        case Host_status::GENERATION_RETIRED:
        default:                               return "generation_retired";
    }
}

namespace {

constexpr int k_post_exit_drain_ms = 250;

Host_status status_from_result(Result_status status)
{
    switch (status) {
        case Result_status::SUCCESS:     return Host_status::SUCCESS;
        case Result_status::UNAVAILABLE: return Host_status::UNAVAILABLE;
        case Result_status::CANCELLED:   return Host_status::CANCELLED;
        case Result_status::FAILED:
        default:                         return Host_status::FAILED;
    }
}

struct Pending_request
{
    Invoke_request                         request;
    Clock::time_point                      deadline;
    bool                                   cancel_sent = false;
    Clock::time_point                      cancel_grace_deadline;
    std::optional<Profile_storage_context> profile_storage;
};

} // namespace

struct Host_simulator::Impl
{
    struct Progress_observation
    {
        std::deque<Progress_message> recent;
        std::uint64_t count = 0;
    };

    Simulator_options                          options;
    Transcript_recorder*                       recorder = nullptr;
    Simulator_state                            state    = Simulator_state::NOT_STARTED;
    Contract_failure                           retirement;
    std::optional<Validated_package>           package;
    std::string                                nonce;
    std::unique_ptr<process::Child_process>    child;
    Frame_decoder                              decoder{limits::k_max_frame_bytes};
    std::optional<Session_tracker>             tracker;
    Stderr_ring                                stderr_ring;
    Json_parse_limits                          parse_limits;
    std::map<std::string, Pending_request>     pending;
    std::map<std::string, Host_outcome>        outcomes;
    std::map<std::string, Progress_observation> progress;
    std::deque<std::string>                    stdin_queue;
    std::size_t                                stdin_head_offset = 0;
    std::uint64_t                              stdin_queue_bytes = 0;
    unsigned                                   next_request = 1;
    unsigned                                   next_local   = 1;
    std::optional<process::Exit_status>        exit;
    bool                                       stdout_eof         = false;
    bool                                       stderr_eof         = false;
    bool                                       shutdown_requested = false;
    std::vector<std::string>                   environment;
    fs::path                                   scratch;
    fs::path                                   profile_root;
    bool                                       owns_scratch = false;
    std::vector<Capability_pair>               live;

    void event(std::string_view kind, std::string_view detail = {})
    {
        if (recorder != nullptr) {
            recorder->record_event(kind, detail);
        }
    }

    bool send(const Host_message& message)
    {
        const Json_value object = encode_host_message(message);
        return send(object, encode_frame(object));
    }

    bool send(const Json_value& object, std::string frame)
    {
        if (recorder != nullptr) {
            recorder->record_host_frame(object);
        }
        if (stdin_queue_bytes + frame.size() > limits::k_max_stdin_queue_bytes) {
            event("violation", reason::k_stdin_queue_overflow);
            retire(reason::k_stdin_queue_overflow, "the bounded adapter-stdin queue is full");
            return false;
        }
        stdin_queue_bytes += frame.size();
        stdin_queue.push_back(std::move(frame));
        flush_stdin();
        return true;
    }

    void flush_stdin()
    {
        while (child && !stdin_queue.empty()) {
            const std::string& head = stdin_queue.front();
            const process::Write_result written =
                child->write_stdin(std::string_view(head).substr(stdin_head_offset));
            if (written.status == process::Write_status::WOULD_BLOCK) {
                return;
            }
            if (written.status != process::Write_status::TRANSFERRED) {
                child->close_stdin();
                stdin_queue.clear();
                stdin_head_offset = 0;
                stdin_queue_bytes = 0;
                retire(reason::k_adapter_exited, "adapter stdin closed before accepting queued frames");
                return;
            }
            stdin_head_offset += written.bytes;
            stdin_queue_bytes -= written.bytes;
            if (stdin_head_offset == head.size()) {
                stdin_queue.pop_front();
                stdin_head_offset = 0;
            }
        }
    }

    void set_outcome(const std::string& request_id, Host_outcome outcome)
    {
        outcomes[request_id] = std::move(outcome);
        pending.erase(request_id);
    }

    void fail_pending(Host_status status, const std::string& reason_code, const std::string& except)
    {
        std::vector<std::string> ids;
        for (const auto& [request_id, request] : pending) {
            if (request_id != except) {
                ids.push_back(request_id);
            }
        }
        for (const std::string& request_id : ids) {
            Host_outcome outcome;
            outcome.status      = status;
            outcome.reason_code = reason_code;
            set_outcome(request_id, std::move(outcome));
        }
    }

    // The generation is gone: live requests fail, the tree is killed, and no
    // later frame is interpreted.
    void retire(const std::string& reason_code, const std::string& detail, const std::string& culprit = {})
    {
        if (state == Simulator_state::RETIRED) {
            return;
        }
        state                  = Simulator_state::RETIRED;
        retirement.reason_code = reason_code;
        retirement.detail      = detail;
        event("retired", reason_code);
        fail_pending(Host_status::GENERATION_RETIRED, reason_code, culprit);
        if (child) {
            child->close_stdin();
        }
        stdin_queue.clear();
        stdin_head_offset = 0;
        stdin_queue_bytes = 0;
        if (child && !exit.has_value()) {
            child->kill();
            event("killed");
        }
    }

    void on_exit(const process::Exit_status& status)
    {
        exit = status;
        event("adapter_exited", (status.signaled ? "signal=" : "exit_code=") + std::to_string(status.code));
        if (state == Simulator_state::RETIRED) {
            return;
        }
        state                  = Simulator_state::RETIRED;
        retirement.reason_code = shutdown_requested ? reason::k_shutdown : reason::k_adapter_exited;
        retirement.detail      = "adapter exited";
        if (!shutdown_requested) {
            event("retired", reason::k_adapter_exited);
        }
        fail_pending(Host_status::GENERATION_RETIRED, reason::k_adapter_exited, {});
    }

    void handle_result(const Result_message& result)
    {
        const auto it = pending.find(result.request_id);
        if (it == pending.end()) {
            return;   // the tracker already validated correlation
        }
        Host_outcome outcome;
        outcome.status      = status_from_result(result.status);
        outcome.reason_code = result.reason;
        outcome.message     = result.message;
        outcome.payload     = result.payload;
        if (result.status == Result_status::SUCCESS && it->second.profile_storage.has_value()) {
            const std::optional<std::string> failure =
                profile_storage_acknowledgment_failure(*it->second.profile_storage, result.payload);
            if (failure.has_value()) {
                outcome.status      = Host_status::FAILED;
                outcome.reason_code = *failure;
                event("profile_storage", *failure);
            }
            else {
                event("profile_storage", "acknowledged");
            }
        }
        set_outcome(result.request_id, std::move(outcome));
    }

    void handle_frame(const std::string& frame)
    {
        Contract_failure failure;
        const std::optional<Adapter_message> message = decode_adapter_message(frame, parse_limits, &failure);
        if (!message.has_value()) {
            event("violation", failure.reason_code);
            retire(failure.reason_code, failure.detail);
            return;
        }
        if (recorder != nullptr) {
            recorder->record_adapter_frame(encode_adapter_message(*message));
        }
        if (!tracker->accept(*message, &failure)) {
            event("violation", failure.reason_code);
            retire(failure.reason_code, failure.detail);
            return;
        }
        if (const Progress_message* progress_message = std::get_if<Progress_message>(&*message)) {
            auto& observation = progress[progress_message->request_id];
            if (observation.recent.size() == Host_simulator::k_retained_progress_frames) {
                observation.recent.pop_front();
            }
            observation.recent.push_back(*progress_message);
            if (observation.count != std::numeric_limits<std::uint64_t>::max()) {
                ++observation.count;
            }
        }
        else
        if (const Result_message* result = std::get_if<Result_message>(&*message)) {
            handle_result(*result);
        }
        else
        if (std::get_if<Adapter_hello>(&*message) != nullptr) {
            const Protocol_limits effective = tracker->effective_limits();
            decoder.set_max_frame_bytes(effective.max_frame_bytes);
            parse_limits.max_bytes = effective.max_frame_bytes;
            parse_limits.max_depth = (std::size_t)effective.max_json_depth;
        }
    }

    void check_deadlines(Clock::time_point now)
    {
        std::vector<std::string> expired;
        std::vector<std::string> grace_expired;
        for (auto& [request_id, request] : pending) {
            if (!request.cancel_sent && now >= request.deadline) {
                expired.push_back(request_id);
            }
            else
            if (request.cancel_sent && now >= request.cancel_grace_deadline) {
                grace_expired.push_back(request_id);
            }
        }
        for (const std::string& request_id : expired) {
            event("deadline_expired", request_id);
            send_cancel(request_id);
        }
        for (const std::string& request_id : grace_expired) {
            event("cancel_grace_expired", request_id);
            Host_outcome outcome;
            outcome.status      = Host_status::TIMED_OUT;
            outcome.reason_code = reason::k_cancel_grace_expired;
            set_outcome(request_id, std::move(outcome));
            retire(reason::k_cancel_grace_expired, "no terminal result within the cancel grace for " + request_id);
            return;
        }
    }

    void send_cancel(const std::string& request_id)
    {
        const auto it = pending.find(request_id);
        if (it == pending.end() || it->second.cancel_sent || state != Simulator_state::READY) {
            return;
        }
        it->second.cancel_sent           = true;
        it->second.cancel_grace_deadline = Clock::now() + std::chrono::milliseconds(options.cancel_grace_ms);
        send(Host_message(Cancel_request{request_id}));
    }

    std::optional<Clock::time_point> next_wakeup() const
    {
        std::optional<Clock::time_point> next;
        for (const auto& [request_id, request] : pending) {
            const Clock::time_point when = request.cancel_sent ? request.cancel_grace_deadline : request.deadline;
            if (!next.has_value() || when < *next) {
                next = when;
            }
        }
        return next;
    }

    // One round of I/O: waits until the deadline or the next request
    // deadline, then processes whatever arrived.
    void pump_once(Clock::time_point until)
    {
        if (!child) {
            return;
        }
        Clock::time_point wake = until;
        if (const std::optional<Clock::time_point> next = next_wakeup(); next.has_value() && *next < wake) {
            wake = *next;
        }
        const Clock::time_point now = Clock::now();
        const int timeout = wake <= now ? 0 : (int)std::chrono::duration_cast<std::chrono::milliseconds>(wake - now).count();

        flush_stdin();
        process::Activity activity = child->wait_activity(timeout, !stdin_queue.empty());
        if (activity.stdin_writable) {
            flush_stdin();
        }
        if (!activity.stderr_bytes.empty()) {
            const bool was_truncated = stderr_ring.truncated();
            stderr_ring.append(activity.stderr_bytes);
            if (!was_truncated && stderr_ring.truncated()) {
                event("stderr_truncated");
            }
        }
        if (activity.stderr_eof) {
            stderr_eof = true;
        }
        if (!activity.stdout_bytes.empty() && state != Simulator_state::RETIRED) {
            if (!decoder.feed(activity.stdout_bytes)) {
                const char* reason_code = frame_violation_reason_code(decoder.violation());
                event("violation", reason_code);
                retire(reason_code, "stdout framing violation");
            }
            std::string frame;
            while (state != Simulator_state::RETIRED && decoder.next_frame(frame)) {
                handle_frame(frame);
            }
        }
        if (activity.stdout_eof) {
            stdout_eof = true;
            if (state != Simulator_state::RETIRED && !decoder.finish()) {
                const char* reason_code = frame_violation_reason_code(decoder.violation());
                event("violation", reason_code);
                retire(reason_code, "stdout ended inside a frame");
            }
        }
        if (activity.exit.has_value()) {
            on_exit(*activity.exit);
        }
        if (state == Simulator_state::READY) {
            check_deadlines(Clock::now());
        }
    }

    template <typename Predicate>
    void pump_until(Clock::time_point until, Predicate done)
    {
        while (!done()) {
            const Clock::time_point now = Clock::now();
            if (now >= until) {
                return;
            }
            pump_once(until);
        }
    }

    // After the process is gone the pipes drain to end of file; a bounded
    // wait keeps a descendant holding them from stalling the host.
    void drain_after_exit()
    {
        const Clock::time_point until = Clock::now() + std::chrono::milliseconds(k_post_exit_drain_ms);
        pump_until(until, [this] { return stdout_eof && stderr_eof; });
    }

    bool wait_exit(int timeout_ms)
    {
        const Clock::time_point until = Clock::now() + std::chrono::milliseconds(timeout_ms);
        pump_until(until, [this] { return exit.has_value(); });
        return exit.has_value();
    }

    void kill_tree()
    {
        if (!child) {
            return;
        }
        if (!exit.has_value()) {
            child->kill();
            event("killed");
            wait_exit(options.terminate_to_kill_ms);
        }
        else {
            child->kill();   // orphaned descendants of an exited adapter
        }
        drain_after_exit();
    }

    std::vector<Capability_pair> compute_live() const
    {
        std::vector<Capability_pair> result;
        if (!package.has_value() || !tracker.has_value()) {
            return result;
        }
        for (const Capability_pair& pair : package->declared_capabilities()) {
            if (!tracker->handshake_declares(pair)) {
                continue;
            }
            if (options.adopted_capabilities.has_value()) {
                const auto& adopted = *options.adopted_capabilities;
                if (std::find(adopted.begin(), adopted.end(), pair) == adopted.end()) {
                    continue;
                }
            }
            result.push_back(pair);
        }
        return result;
    }

    std::string local_outcome(Host_status status, std::string reason_code, std::string message = {})
    {
        const std::string id = "l" + std::to_string(next_local++);
        Host_outcome outcome;
        outcome.status      = status;
        outcome.reason_code = std::move(reason_code);
        outcome.message     = std::move(message);
        outcomes[id]        = std::move(outcome);
        return id;
    }
};

Host_simulator::Host_simulator(Simulator_options options, Transcript_recorder* recorder)
:
    m_impl(std::make_unique<Impl>())
{
    m_impl->options  = std::move(options);
    m_impl->recorder = recorder;
}

Host_simulator::~Host_simulator()
{
    Impl& impl = *m_impl;
    if (impl.child) {
        impl.kill_tree();
        impl.child.reset();
    }
    if (impl.owns_scratch) {
        std::error_code error;
        fs::remove_all(impl.scratch, error);
    }
}

bool Host_simulator::start(Contract_failure* out_failure)
{
    Impl& impl = *m_impl;
    const auto fail = [&impl, out_failure](const std::string& reason_code, const std::string& detail) {
        impl.state                  = Simulator_state::RETIRED;
        impl.retirement.reason_code = reason_code;
        impl.retirement.detail      = detail;
        if (out_failure != nullptr) {
            *out_failure = impl.retirement;
        }
        return false;
    };
    if (impl.state != Simulator_state::NOT_STARTED) {
        return fail(impl.retirement.reason_code.empty() ? std::string("already_started") : impl.retirement.reason_code,
            "start() was already called");
    }

    // 1. Validate.
    Directory_package_source source(impl.options.package_directory);
    Package_validator validator(impl.options.validation);
    Contract_failure validation_failure;
    std::optional<Validated_package> package = validator.validate(source, &validation_failure);
    if (!package.has_value()) {
        impl.event("package_invalid", validation_failure.reason_code);
        return fail(validation_failure.reason_code, validation_failure.detail);
    }
    impl.package = std::move(*package);
    impl.nonce   = impl.options.generation_nonce.value_or(random_nonce_hex());
    if (impl.recorder != nullptr) {
        Transcript_placeholders placeholders;
        placeholders.generation_digest = impl.package->generation_digest_hex;
        placeholders.generation_nonce  = impl.nonce;
        impl.recorder->set_placeholders(std::move(placeholders));
    }

    // 2. Scratch and profile-storage roots.
    std::error_code error;
    if (impl.options.scratch_directory.empty()) {
        impl.scratch      = fs::temp_directory_path(error) / ("logonomic-cli-addon-sim-" + impl.nonce.substr(0, 16));
        impl.owns_scratch = true;
    }
    else {
        impl.scratch = impl.options.scratch_directory;
    }
    fs::create_directories(impl.scratch, error);
    if (error) {
        return fail(reason::k_custody_write_failed, "cannot create scratch directory: " + error.message());
    }
    impl.profile_root = impl.options.profile_storage_root.empty()
        ? impl.scratch / "profile-storage"
        : impl.options.profile_storage_root;
    fs::create_directories(impl.profile_root, error);

    // 3. Environment and spawn.
    const fs::path package_root = fs::absolute(impl.options.package_directory, error).lexically_normal();
    Adapter_environment_inputs inputs;
    inputs.platform_id               = impl.options.validation.platform_id;
    inputs.package_id                = impl.package->bootstrap.package_id;
    inputs.generation_digest         = impl.package->generation_digest_hex;
    inputs.generation_nonce          = impl.nonce;
    inputs.package_root              = path_to_utf8(package_root);
    inputs.scratch_directory         = path_to_utf8(fs::absolute(impl.scratch, error));
    inputs.core_protocol_generations = impl.options.validation.host_core_protocol_generations;
    inputs.host_product              = impl.options.host_product;
    inputs.manifest_environment      = impl.package->bootstrap.adapter_environment;
    impl.environment.clear();
    const std::vector<std::pair<std::string, std::string>> environment =
        build_adapter_environment(inputs, current_process_environment());
    std::vector<std::pair<std::string, std::string>> start_environment;
    start_environment.reserve(environment.size());
    for (const auto& pair : environment) {
        if (!is_reserved_environment_name(pair.first)) {
            start_environment.push_back(pair);
        }
    }
    if (!adapter_start_environment_within_budget(start_environment) ||
        !adapter_reserved_environment_within_budget(environment) ||
        !adapter_supervisor_environment_within_budget(environment))
    {
        return fail(reason::k_payload_too_large,
            "generated adapter environment exceeds its start, reserved or supervisor environment budget");
    }
    for (const auto& [name, value] : environment) {
        impl.environment.push_back(name + "=" + value);
    }

    process::Spawn_request spawn;
    spawn.executable        = path_to_utf8(package_root / path_from_utf8(impl.package->adapter_relative_path));
    spawn.argv.push_back(spawn.executable);
    for (const std::string& argument : impl.package->bootstrap.adapter_argv) {
        spawn.argv.push_back(argument);
    }
    spawn.environment       = impl.environment;
    spawn.working_directory = inputs.package_root;
    spawn.isolate           = impl.options.isolate_process_group;
    std::string spawn_error;
    impl.child = process::Child_process::spawn(spawn, &spawn_error);
    if (!impl.child) {
        impl.event("spawn_failed");
        return fail(reason::k_adapter_exited, "cannot start the adapter: " + spawn_error);
    }

    // 4. Handshake.
    Host_hello hello;
    hello.core_protocol_generations = impl.options.validation.host_core_protocol_generations;
    hello.package_id                = impl.package->bootstrap.package_id;
    hello.generation_digest         = impl.package->generation_digest_hex;
    hello.generation_nonce          = impl.nonce;
    hello.limits                    = impl.options.limits;
    hello.host_product              = impl.options.host_product;
    hello.host_version              = impl.options.host_version;
    impl.tracker.emplace(hello);
    impl.decoder      = Frame_decoder(impl.options.limits.max_frame_bytes);
    impl.parse_limits.max_bytes = impl.options.limits.max_frame_bytes;
    impl.parse_limits.max_depth = (std::size_t)impl.options.limits.max_json_depth;
    impl.state = Simulator_state::READY;   // protocol state; ready() below decides the public state
    if (!impl.send(Host_message(hello))) {
        impl.retire(reason::k_adapter_exited, "cannot write hello");
        return fail(impl.retirement.reason_code, impl.retirement.detail);
    }

    const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(impl.options.hello_deadline_ms);
    impl.pump_until(deadline, [&impl] {
        return impl.state == Simulator_state::RETIRED || impl.tracker->ready();
    });
    if (impl.state == Simulator_state::RETIRED) {
        return fail(impl.retirement.reason_code, impl.retirement.detail);
    }
    if (!impl.tracker->ready()) {
        impl.event("hello_timeout");
        impl.retire(reason::k_hello_timeout, "no hello and capabilities within the hello deadline");
        return fail(impl.retirement.reason_code, impl.retirement.detail);
    }
    impl.live = impl.compute_live();
    return true;
}

Simulator_state Host_simulator::state() const                  { return m_impl->state; }
const Contract_failure& Host_simulator::retirement() const     { return m_impl->retirement; }
const Validated_package* Host_simulator::package() const       { return m_impl->package.has_value() ? &*m_impl->package : nullptr; }
const Session_tracker* Host_simulator::session() const         { return m_impl->tracker.has_value() ? &*m_impl->tracker : nullptr; }
std::string Host_simulator::generation_nonce() const           { return m_impl->nonce; }
std::vector<Capability_pair> Host_simulator::live_capabilities() const { return m_impl->live; }
std::filesystem::path Host_simulator::scratch_directory() const    { return m_impl->scratch; }
std::filesystem::path Host_simulator::profile_storage_root() const { return m_impl->profile_root; }
const std::vector<std::string>& Host_simulator::adapter_environment() const { return m_impl->environment; }

std::string Host_simulator::next_request_id() const
{
    return "r" + std::to_string(m_impl->next_request);
}

std::string Host_simulator::generation_digest() const
{
    return m_impl->package.has_value() ? m_impl->package->generation_digest_hex : std::string();
}

std::string Host_simulator::invoke(
    const Capability_pair&              capability,
    std::string                         operation,
    Json_value                          payload,
    int                                 deadline_ms,
    const Simulator_invocation_options& invocation)
{
    Impl& impl = *m_impl;
    if (impl.state != Simulator_state::READY) {
        return impl.local_outcome(Host_status::PACKAGE_UNAVAILABLE,
            impl.retirement.reason_code.empty() ? std::string(reason::k_registry_not_settled) : impl.retirement.reason_code);
    }

    if (!invocation.bypass_liveness) {
        const bool is_live = std::find(impl.live.begin(), impl.live.end(), capability) != impl.live.end();
        if (!is_live) {
            const std::vector<Capability_pair> declared = impl.package->declared_capabilities();
            const bool sidecar_declares_pair = std::find(declared.begin(), declared.end(), capability) != declared.end();
            const bool live_at_other_version = std::any_of(impl.live.begin(), impl.live.end(), [&capability](const Capability_pair& pair) {
                return pair.capability_id == capability.capability_id;
            });
            if (live_at_other_version) {
                impl.event("unsupported_version", capability.capability_id + "@" + std::to_string(capability.version));
                return impl.local_outcome(Host_status::UNSUPPORTED_VERSION, reason::k_unsupported_version);
            }
            const bool handshake_declares_pair = impl.tracker->handshake_declares(capability);
            const char* reason_code = sidecar_declares_pair && !handshake_declares_pair
                ? reason::k_sidecar_handshake_disagreement
                : reason::k_unknown_capability;
            impl.event("unknown_capability", capability.capability_id + "@" + std::to_string(capability.version));
            return impl.local_outcome(Host_status::UNKNOWN_CAPABILITY, reason_code);
        }
    }

    const Protocol_limits effective = impl.tracker->effective_limits();
    if (impl.pending.size() >= effective.max_in_flight_requests) {
        impl.event("backpressured");
        return impl.local_outcome(Host_status::UNAVAILABLE, reason::k_backpressured);
    }

    Invoke_request request;
    request.request_id  = "r" + std::to_string(impl.next_request);
    request.capability  = capability;
    request.operation   = std::move(operation);
    request.payload     = payload.is_object() ? std::move(payload) : Json_value::object();
    request.deadline_ms = deadline_ms;
    if (invocation.profile_storage.has_value()) {
        request.payload.set(k_profile_storage_member, profile_storage_member(*invocation.profile_storage));
        if (impl.recorder != nullptr) {
            Transcript_placeholders placeholders;
            placeholders.generation_digest = impl.package->generation_digest_hex;
            placeholders.generation_nonce  = impl.nonce;
            placeholders.roots.push_back(invocation.profile_storage->root);
            impl.recorder->set_placeholders(std::move(placeholders));
        }
    }

    const Json_value object = encode_host_message(Host_message(request));
    std::string frame;
    if (auto refusal = encode_request_frame(object, impl.parse_limits, frame)) {
        impl.event("payload_too_large", refusal->bound);
        return impl.local_outcome(Host_status::FAILED, reason::k_payload_too_large, std::move(refusal->detail));
    }

    ++impl.next_request;
    Pending_request pending;
    pending.request         = request;
    pending.deadline        = deadline_ms == limits::k_no_invoke_deadline_ms
        ? Clock::time_point::max()
        : Clock::now() + std::chrono::milliseconds(deadline_ms);
    pending.profile_storage = invocation.profile_storage;
    impl.pending.emplace(request.request_id, std::move(pending));
    impl.tracker->note_invoke_sent(request.request_id);
    (void)impl.send(object, std::move(frame));
    return request.request_id;
}

void Host_simulator::cancel(const std::string& request_id)
{
    m_impl->send_cancel(request_id);
}

std::optional<Host_outcome> Host_simulator::outcome(const std::string& request_id) const
{
    const auto it = m_impl->outcomes.find(request_id);
    if (it == m_impl->outcomes.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<Host_outcome> Host_simulator::wait_outcome(const std::string& request_id, int timeout_ms)
{
    Impl& impl = *m_impl;
    const Clock::time_point until = Clock::now() + std::chrono::milliseconds(timeout_ms);
    impl.pump_until(until, [&impl, &request_id] { return impl.outcomes.count(request_id) != 0; });
    return outcome(request_id);
}

std::vector<Progress_message> Host_simulator::progress(const std::string& request_id) const
{
    const auto it = m_impl->progress.find(request_id);
    return it == m_impl->progress.end() ? std::vector<Progress_message>()
        : std::vector<Progress_message>(it->second.recent.begin(), it->second.recent.end());
}

std::uint64_t Host_simulator::progress_count(const std::string& request_id) const
{
    const auto it = m_impl->progress.find(request_id);
    return it == m_impl->progress.end() ? 0 : it->second.count;
}

void Host_simulator::pump(int ms)
{
    Impl& impl = *m_impl;
    const Clock::time_point until = Clock::now() + std::chrono::milliseconds(ms);
    impl.pump_until(until, [] { return false; });
}

bool Host_simulator::shutdown(int grace_ms, int* out_exit_code)
{
    Impl& impl = *m_impl;
    if (!impl.child) {
        return false;
    }
    if (!impl.exit.has_value() && impl.state == Simulator_state::READY) {
        impl.shutdown_requested = true;
        impl.send(Host_message(Shutdown_request{grace_ms}));
    }
    const bool exited_in_grace = impl.wait_exit(grace_ms);
    if (!exited_in_grace) {
        impl.child->terminate();
        impl.event("terminated");
        if (!impl.wait_exit(impl.options.terminate_to_kill_ms)) {
            impl.kill_tree();
        }
        if (impl.state != Simulator_state::RETIRED) {
            impl.retire(reason::k_shutdown, "shutdown grace expired");
        }
    }
    else {
        impl.child->kill();   // descendants that outlived the adapter
    }
    impl.drain_after_exit();
    if (out_exit_code != nullptr && impl.exit.has_value()) {
        *out_exit_code = impl.exit->code;
    }
    return exited_in_grace;
}

void Host_simulator::terminate()
{
    Impl& impl = *m_impl;
    if (!impl.child) {
        return;
    }
    impl.retire(reason::k_shutdown, "terminated by the host");
    impl.kill_tree();
}

bool Host_simulator::adapter_exited() const      { return m_impl->exit.has_value(); }
bool Host_simulator::exited_by_signal() const    { return m_impl->exit.has_value() && m_impl->exit->signaled; }
std::int64_t Host_simulator::adapter_pid() const { return m_impl->child ? m_impl->child->pid() : 0; }
std::string Host_simulator::stderr_text() const  { return m_impl->stderr_ring.text(); }
bool Host_simulator::stderr_truncated() const    { return m_impl->stderr_ring.truncated(); }

std::optional<int> Host_simulator::exit_code() const
{
    if (!m_impl->exit.has_value()) {
        return std::nullopt;
    }
    return m_impl->exit->code;
}

bool Host_simulator::owns_process(std::int64_t pid) const
{
    return m_impl->child && m_impl->child->owns(pid);
}

// ---------------------------------------------------------------------------
// Scripts

bool Host_simulator::run_script(const Json_value& script, std::string* out_error)
{
    const auto fail = [out_error](const std::string& text) {
        if (out_error != nullptr) {
            *out_error = text;
        }
        return false;
    };
    if (!script.is_object()) {
        return fail("script is not an object");
    }
    const std::optional<std::int64_t> version = strict::integer_member(script, "script_version", 1, 1);
    if (!version.has_value()) {
        return fail("script_version must be 1");
    }
    const Json_value* steps = strict::member_of_type(script, "steps", Json_type::ARRAY);
    if (steps == nullptr) {
        return fail("steps is missing or not an array");
    }

    std::map<std::string, std::string> request_ids;   // script id -> wire or local id
    for (const Json_value& step : steps->as_array()) {
        if (!step.is_object()) {
            return fail("a step is not an object");
        }
        const std::string* op = strict::string_member(step, "op");
        if (op == nullptr) {
            return fail("a step has no op");
        }

        if (*op == "invoke") {
            const std::string* id            = strict::string_member(step, "id");
            const std::string* capability_id = strict::string_member(step, "capability_id");
            const std::optional<std::int64_t> capability_version = strict::integer_member(step, "version", 0, limits::k_max_version);
            const std::string* operation     = strict::string_member(step, "operation");
            const Json_value*  payload       = step.find("payload");
            const auto deadline = strict::integer_member(step, "deadline_ms", 1, limits::k_no_invoke_deadline_ms);
            if (step.find("deadline_ms") && !deadline) {
                return fail("invoke deadline_ms must be a positive 32-bit integer");
            }
            if (id == nullptr || capability_id == nullptr || !capability_version.has_value() || operation == nullptr) {
                return fail("invoke step needs id, capability_id, version and operation");
            }
            Simulator_invocation_options invocation;
            if (const Json_value* profile = step.find("profile_storage"); profile != nullptr && profile->is_boolean() && profile->as_boolean()) {
                if (!m_impl->package.has_value()) {
                    return fail("profile_storage requested before start");
                }
                std::string allocation_error;
                const std::optional<std::string> root = allocate_profile_storage_root(
                    m_impl->profile_root, m_impl->package->bootstrap.package_id, &allocation_error);
                if (!root.has_value()) {
                    return fail(allocation_error);
                }
                invocation.profile_storage = Profile_storage_context{k_profile_storage_version, *root};
            }
            if (const Json_value* bypass = step.find("bypass_liveness"); bypass != nullptr && bypass->is_boolean()) {
                invocation.bypass_liveness = bypass->as_boolean();
            }
            request_ids[*id] = invoke(
                Capability_pair{*capability_id, (std::uint16_t)*capability_version},
                *operation,
                payload != nullptr ? *payload : Json_value::object(),
                (int)deadline.value_or(limits::k_default_invoke_deadline_ms),
                invocation);
        }
        else
        if (*op == "cancel") {
            const std::string* id = strict::string_member(step, "id");
            if (id == nullptr || request_ids.count(*id) == 0) {
                return fail("cancel step names an unknown id");
            }
            cancel(request_ids[*id]);
        }
        else
        if (*op == "wait_result") {
            const std::string* id = strict::string_member(step, "id");
            const std::optional<std::int64_t> timeout = strict::integer_member(step, "timeout_ms", 0, 3600000);
            if (id == nullptr || request_ids.count(*id) == 0) {
                return fail("wait_result step names an unknown id");
            }
            wait_outcome(request_ids[*id], (int)timeout.value_or(limits::k_default_invoke_deadline_ms + limits::k_cancel_grace_ms));
        }
        else
        if (*op == "sleep_ms" || *op == "pump_ms") {
            const std::optional<std::int64_t> ms = strict::integer_member(step, "ms", 0, 3600000);
            if (!ms.has_value()) {
                return fail(*op + " step needs ms");
            }
            pump((int)*ms);
        }
        else
        if (*op == "shutdown") {
            const std::optional<std::int64_t> grace = strict::integer_member(step, "grace_ms", 0, 3600000);
            shutdown((int)grace.value_or(limits::k_shutdown_grace_ms), nullptr);
        }
        else
        if (*op == "wait_exit") {
            const std::optional<std::int64_t> timeout = strict::integer_member(step, "timeout_ms", 0, 3600000);
            m_impl->wait_exit((int)timeout.value_or(limits::k_shutdown_grace_ms));
        }
        else {
            return fail("unknown step op " + *op);
        }
    }
    return true;
}

} // namespace logonomic::cli_addon
