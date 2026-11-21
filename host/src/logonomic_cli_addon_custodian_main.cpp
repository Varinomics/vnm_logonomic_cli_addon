// logonomic_cli_addon_custodian: owns one package generation's process tree.
//
//   logonomic_cli_addon_custodian --control <inherited control channel end>
//
// Reads exactly one `start` on the control channel, validates every field,
// becomes the tree's owner (Linux: subreaper; Windows: the kill-on-close
// Job), spawns the supervisor as the root of the tree (Linux: its own
// session and process group), reports `started` with the kill coordinates,
// reaps and reports statuses, and on `stop` or control-channel EOF
// terminates, kills and reaps the complete tree before exiting.
// Darwin keeps the native COMMAND owner as the adapter's direct parent and
// the supervisor as an I/O relay, with original-group settlement receipts.

#include "logonomic_cli_addon_custodian_protocol.h"
#include "logonomic_cli_addon_usage_writer_custody.h"
#include "logonomic_cli_addon_path_entry.h"
#include <vnm_process_custody/process_channel.h>
#include <vnm_process_custody/process_clock.h>
#include <vnm_process_custody/process_pipe.h>
#include <vnm_process_custody/process_spawn.h>
#include <vnm_process_custody/process_tree.h>
#if defined(__APPLE__)
#include <vnm_process_custody/owner.h>
#include <fcntl.h>
#endif
#include "logonomic_cli_addon_supervisor_link.h"
#include "logonomic_cli_addon_supervisor_request_file.h"

#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/limits.h"
#include "logonomic_cli_addon/package_source.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs       = std::filesystem;
namespace process  = vnm::process_custody;
namespace protocol = logonomic::cli_addon_host::custodian_protocol;
namespace link     = logonomic::cli_addon_host::link;
namespace sdk      = logonomic::cli_addon;
namespace limits   = logonomic::cli_addon::limits;
namespace reason   = logonomic::cli_addon::reason;
namespace writers  = logonomic::cli_addon_host::usage_writer_custody;

using Request_file = logonomic::cli_addon_host::Supervisor_request_file;

void close_usage_writers(const std::string& generation)
{
    if (generation.empty()) return;
    std::string error;
    while (!writers::close_generation(generation, &error)) {
        // Retain this same authority until its durable admission closure is
        // published. A failed file operation must not race a late dispatch.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

constexpr int k_exit_ok             = 0;
constexpr int k_exit_failure        = 1;   // could not run at all, or the run ended abnormally
constexpr int k_exit_start_rejected = 2;   // answered `error` before or while starting the tree

constexpr const char* k_host_product = "logonomic";

bool is_valid_start_environment_name(const std::string& in_name)
{
    if (sdk::is_valid_environment_name(in_name)) {
        return true;
    }
#ifdef _WIN32
    // The host-constructed Windows baseline includes this operating-system
    // variable. Adapter manifests and process proposals still use the strict
    // portable grammar and cannot introduce parenthesized names.
    return in_name == "ProgramFiles(x86)";
#else
    return false;
#endif
}

void log_line(const std::string& in_text)
{
    std::fprintf(stderr, "logonomic_cli_addon_custodian: %s\n", in_text.c_str());
}

// ---------------------------------------------------------------------------
// Command line

struct Options
{
    process::Native_handle control = process::k_invalid_handle;
};

bool parse_options(int argc, char** argv, Options* out_options, std::string* out_error)
{
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--control" && i + 1 < argc) {
            if (!process::parse_native_handle_argument(argv[++i], &out_options->control)) {
                *out_error = "--control expects an inherited handle number";
                return false;
            }
            continue;
        }
        *out_error = "unexpected argument \"" + argument + "\"; usage: --control <handle>";
        return false;
    }
    if (out_options->control == process::k_invalid_handle) {
        *out_error = "--control is required";
        return false;
    }
    if (!process::native_handle_is_open(out_options->control)) {
        *out_error = "the control channel end was not inherited";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Control channel

class Control
{
public:
    explicit Control(process::Native_handle in_end)
    :
        m_channel(in_end, limits::k_max_control_message_bytes),
        m_buffer(limits::k_max_control_message_bytes)
    {}

    process::Native_handle waitable() const { return m_channel.waitable(); }

    bool prepare(std::string* out_error) { return m_channel.prepare_for_owner(out_error); }

    bool send(const protocol::Message& in_message)
    {
        const std::string bytes = protocol::encode(in_message);
        return m_channel.send(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()) ==
            process::Io_status::TRANSFERRED;
    }

    bool send_started(const protocol::Started& in_started, const process::Kill_coordinates& in_coordinates)
    {
        const std::string bytes = protocol::encode(protocol::Message(in_started));
        const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
        const process::Io_status status = process::Duplex_channel::transfers_descriptors()
            ? m_channel.send_descriptors(data, bytes.size(), in_coordinates.process_handles)
            : m_channel.send(data, bytes.size());
        return status == process::Io_status::TRANSFERRED;
    }

    void send_error(const char* in_reason, const std::string& in_detail)
    {
        protocol::Error error;
        error.reason = in_reason;
        error.detail = in_detail;
        log_line(std::string("error ") + in_reason + ": " + in_detail);
        (void)send(protocol::Message(std::move(error)));
    }

    struct Incoming
    {
        process::Receive_status          status = process::Receive_status::FAILED;
        std::optional<protocol::Message> message;
        protocol::Decode_failure         failure;   // when status is MESSAGE and message is empty
    };

    Incoming receive(int in_timeout_ms)
    {
        Incoming incoming;
        const process::Receive_result received =
            m_channel.receive(m_buffer.data(), m_buffer.size(), in_timeout_ms);
        incoming.status = received.status;
        for (const process::Native_handle handle : received.descriptors) {
            process::close_native_handle(handle);   // the desktop sends none
        }
        if (received.status != process::Receive_status::MESSAGE) {
            return incoming;
        }
        incoming.message = protocol::decode(
            std::string_view(reinterpret_cast<const char*>(m_buffer.data()), received.size),
            &incoming.failure);
        return incoming;
    }

private:
    process::Duplex_channel   m_channel;
    std::vector<std::uint8_t> m_buffer;
};

// ---------------------------------------------------------------------------
// start validation

struct Validated_start
{
    fs::path                 generation_directory;
    fs::path                 adapter_executable;
    std::vector<std::string> environment;   // complete adapter environment, reserved variables included
};

bool is_confined_absolute_path(const std::string& in_text)
{
    if (in_text.empty() || in_text.size() > limits::k_max_start_absolute_path_bytes ||
        in_text.find('\0') != std::string::npos)
    {
        return false;
    }
    if (!sdk::is_valid_utf8(in_text)) {
        return false;
    }
    const fs::path path = sdk::path_from_utf8(in_text);
    if (!path.is_absolute() || path.lexically_normal() != path) {
        return false;
    }
    for (const fs::path& segment : path) {
        if (segment == ".." || segment == ".") {
            return false;
        }
    }
    return true;
}

bool has_control_character(const std::string& in_text)
{
    return std::any_of(in_text.begin(), in_text.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; });
}

std::string join_generations(const std::vector<std::int64_t>& in_generations)
{
    std::string joined;
    for (const std::int64_t generation : in_generations) {
        if (!joined.empty()) {
            joined.push_back(',');
        }
        joined += std::to_string(generation);
    }
    return joined;
}

bool validate_start(const protocol::Start_request& in_start, Validated_start* out_validated, protocol::Error* out_error)
{
    const auto reject = [out_error](const char* code, std::string detail) {
        out_error->reason = code;
        out_error->detail = std::move(detail);
        return false;
    };

    if (!sdk::is_valid_package_id(in_start.package_id)) {
        return reject(reason::k_frame_violation, "package_id is not a canonical package ID");
    }
    if (!sdk::is_valid_sha256_hex(in_start.generation_digest)) {
        return reject(reason::k_frame_violation, "generation_digest is not 64 lowercase hex characters");
    }
    if (!sdk::is_valid_generation_nonce_hex(in_start.generation_nonce)) {
        return reject(reason::k_frame_violation, "generation_nonce is not 32 lowercase hex characters");
    }
    if (!is_confined_absolute_path(in_start.custody_root)) {
        return reject(reason::k_path_escape, "custody_root is not a confined absolute path");
    }
    if (!is_confined_absolute_path(in_start.scratch_root)) {
        return reject(reason::k_path_escape, "scratch_root is not a confined absolute path");
    }
    if (!is_confined_absolute_path(in_start.supervisor_executable)) {
        return reject(reason::k_path_escape, "supervisor_executable is not a confined absolute path");
    }
    if (!sdk::is_valid_package_relative_path(in_start.adapter_relative_path) ||
        sdk::path_first_segment(in_start.adapter_relative_path) != sdk::k_segment_adapter)
    {
        return reject(reason::k_path_escape, "adapter_relative_path is not a canonical path below adapter/");
    }

    if (in_start.adapter_argv.size() > limits::k_max_argv_entries) {
        return reject(reason::k_frame_violation, "adapter_argv exceeds k_max_argv_entries");
    }
    std::size_t argv_bytes = 0;
    for (const std::string& argument : in_start.adapter_argv) {
        if (argument.empty() || argument.size() > limits::k_max_argv_entry_bytes || has_control_character(argument)) {
            return reject(reason::k_frame_violation, "an adapter_argv entry is empty, oversize or has a control character");
        }
        argv_bytes += argument.size();
    }
    if (argv_bytes > limits::k_max_argv_total_bytes) {
        return reject(reason::k_frame_violation, "adapter_argv exceeds k_max_argv_total_bytes");
    }

    if (in_start.environment.size() > limits::k_max_start_environment_entries) {
        return reject(reason::k_frame_violation, "environment exceeds k_max_start_environment_entries");
    }
    std::vector<std::string> environment_names;
    std::size_t environment_bytes = 0;
    for (const auto& [name, value] : in_start.environment) {
        if (sdk::is_reserved_environment_name(name)) {
            return reject(reason::k_reserved_environment_variable, "environment name \"" + name + "\" is reserved");
        }
        if (!is_valid_start_environment_name(name)) {
            return reject(reason::k_frame_violation, "environment name \"" + name + "\" is not a valid name");
        }
        const std::string comparison_name = sdk::ascii_lowercase(name);
        if (std::find(environment_names.begin(), environment_names.end(), comparison_name) != environment_names.end()) {
            return reject(reason::k_frame_violation,
                "environment repeats a case-insensitive name: " + name);
        }
        environment_names.push_back(comparison_name);
        if (value.size() > limits::k_max_environment_value_bytes || value.find('\0') != std::string::npos ||
            !sdk::is_valid_utf8(value))
        {
            return reject(reason::k_frame_violation, "environment value of \"" + name + "\" is oversize or has a NUL");
        }
        if (environment_bytes > limits::k_max_start_environment_bytes ||
            name.size() > limits::k_max_start_environment_bytes - environment_bytes)
        {
            return reject(reason::k_frame_violation, "environment exceeds k_max_start_environment_bytes");
        }
        environment_bytes += name.size();
        if (value.size() > limits::k_max_start_environment_bytes - environment_bytes) {
            return reject(reason::k_frame_violation, "environment exceeds k_max_start_environment_bytes");
        }
        environment_bytes += value.size();
    }

    if (in_start.core_protocol_generations.empty() ||
        in_start.core_protocol_generations.size() > limits::k_max_core_generations_per_package)
    {
        return reject(reason::k_frame_violation, "core_protocol_generations must hold 1-8 versions");
    }
    for (std::size_t i = 0; i < in_start.core_protocol_generations.size(); ++i) {
        const std::int64_t generation = in_start.core_protocol_generations[i];
        if (!sdk::is_valid_version(generation)) {
            return reject(reason::k_frame_violation, "core_protocol_generations holds an invalid version");
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (in_start.core_protocol_generations[j] == generation) {
                return reject(reason::k_frame_violation, "core_protocol_generations holds a duplicate");
            }
        }
    }

    const protocol::Start_limits& start_limits = in_start.limits;
    if (start_limits.max_frame_bytes > limits::k_max_frame_bytes ||
        start_limits.stderr_ring_bytes > limits::k_stderr_ring_bytes ||
        start_limits.max_link_frame_bytes > limits::k_max_link_frame_bytes ||
        start_limits.max_link_frame_bytes < start_limits.max_frame_bytes ||
        start_limits.max_stdin_queue_bytes > limits::k_max_stdin_queue_bytes ||
        start_limits.max_stdin_queue_bytes <= start_limits.max_frame_bytes)
    {
        return reject(reason::k_frame_violation, "limits are outside the contract bounds");
    }

    if (in_start.link_child_number < 3) {
        return reject(reason::k_frame_violation, "link_child_number must not name a standard stream");
    }
    process::Native_handle link_end = process::k_invalid_handle;
    if (!process::parse_native_handle_argument(std::to_string(in_start.link_child_number), &link_end) ||
        !process::native_handle_is_open(link_end))
    {
        return reject(reason::k_frame_violation, "link_child_number does not name an inherited handle");
    }

    // Paths below the custody root are resolved with the package identity the
    // desktop named; the supervisor runs the adapter from that directory only.
    out_validated->generation_directory =
        sdk::path_from_utf8(in_start.custody_root) /
        sdk::path_from_utf8(in_start.package_id) /
        sdk::path_from_utf8(in_start.generation_nonce);
    out_validated->adapter_executable =
        out_validated->generation_directory / sdk::path_from_utf8(in_start.adapter_relative_path);
    if (sdk::path_to_utf8(out_validated->generation_directory).size() > limits::k_max_generation_directory_bytes ||
        sdk::path_to_utf8(out_validated->adapter_executable).size() > limits::k_max_adapter_executable_bytes)
    {
        return reject(reason::k_path_escape, "the composed generation directory or adapter executable exceeds its path bound");
    }

    std::error_code error;
    if (!fs::is_directory(out_validated->generation_directory, error)) {
        return reject(reason::k_missing_file, "generation directory does not exist: " +
            sdk::path_to_utf8(out_validated->generation_directory));
    }
    const fs::file_status adapter_status = fs::symlink_status(out_validated->adapter_executable, error);
    if (logonomic::cli_addon_host::path_entry_is_link(
            out_validated->adapter_executable,
            adapter_status))
    {
        return reject(reason::k_link_entry, "adapter entry point is a link: " +
            sdk::path_to_utf8(out_validated->adapter_executable));
    }
    if (!fs::is_regular_file(adapter_status)) {
        return reject(reason::k_missing_file, "adapter entry point is not a regular file: " +
            sdk::path_to_utf8(out_validated->adapter_executable));
    }
    if (!fs::is_regular_file(fs::symlink_status(sdk::path_from_utf8(in_start.supervisor_executable), error))) {
        return reject(reason::k_missing_file, "supervisor executable is not a regular file: " +
            in_start.supervisor_executable);
    }
    if (!fs::is_directory(sdk::path_from_utf8(in_start.scratch_root), error)) {
        return reject(reason::k_missing_file, "scratch_root is not a directory: " + in_start.scratch_root);
    }

    for (const auto& [name, value] : in_start.environment) {
        out_validated->environment.push_back(name + "=" + value);
    }
    const std::vector<std::pair<std::string, std::string>> reserved = {
        {"LOGONOMIC_ADDON_PACKAGE_ID",                in_start.package_id},
        {"LOGONOMIC_ADDON_GENERATION_DIGEST",         in_start.generation_digest},
        {"LOGONOMIC_ADDON_GENERATION_NONCE",          in_start.generation_nonce},
        {"LOGONOMIC_ADDON_PACKAGE_ROOT",              sdk::path_to_utf8(out_validated->generation_directory)},
        {"LOGONOMIC_ADDON_SCRATCH_DIR",               in_start.scratch_root},
        {"LOGONOMIC_ADDON_CORE_PROTOCOL_GENERATIONS", join_generations(in_start.core_protocol_generations)},
        {"LOGONOMIC_ADDON_HOST_PRODUCT",              k_host_product},
    };
    std::size_t reserved_environment_bytes = 0;
    for (const auto& [name, value] : reserved) {
        if (reserved_environment_bytes > limits::k_max_reserved_environment_bytes ||
            name.size() >= limits::k_max_reserved_environment_bytes - reserved_environment_bytes)
        {
            return reject(reason::k_frame_violation,
                "reserved environment exceeds k_max_reserved_environment_bytes");
        }
        reserved_environment_bytes += name.size() + 1;
        if (value.size() > limits::k_max_reserved_environment_bytes - reserved_environment_bytes) {
            return reject(reason::k_frame_violation,
                "reserved environment exceeds k_max_reserved_environment_bytes");
        }
        reserved_environment_bytes += value.size();
        out_validated->environment.push_back(name + "=" + value);
    }
    if (out_validated->environment.size() > limits::k_max_start_environment_entries + reserved.size()) {
        return reject(reason::k_frame_violation, "complete environment exceeds its entry bound");
    }
    std::size_t complete_environment_bytes = 0;
    for (const std::string& entry : out_validated->environment) {
        if (complete_environment_bytes > limits::k_max_supervisor_environment_bytes ||
            entry.size() > limits::k_max_supervisor_environment_bytes - complete_environment_bytes)
        {
            return reject(reason::k_frame_violation, "complete environment exceeds k_max_supervisor_environment_bytes");
        }
        complete_environment_bytes += entry.size();
    }
    return true;
}

// ---------------------------------------------------------------------------
// The run

link::Supervisor_request supervisor_request(
    const protocol::Start_request& start, const Validated_start& validated)
{
    link::Supervisor_request request;
    request.adapter_executable = sdk::path_to_utf8(validated.adapter_executable);
    request.adapter_argv.push_back(request.adapter_executable);
    request.adapter_argv.insert(request.adapter_argv.end(), start.adapter_argv.begin(), start.adapter_argv.end());
    request.environment           = validated.environment;
    request.working_directory     = sdk::path_to_utf8(validated.generation_directory);
    request.max_frame_bytes       = start.limits.max_frame_bytes;
    request.stderr_ring_bytes     = start.limits.stderr_ring_bytes;
    request.max_link_frame_bytes  = start.limits.max_link_frame_bytes;
    request.max_stdin_queue_bytes = start.limits.max_stdin_queue_bytes;
    return request;
}

#if defined(__APPLE__)
// Each helper has one direct parent and one original group. The adapter is
// born only in the native owner; a supervisor can never orphan its wait duty.
class Darwin_custodian
{
public:
    Darwin_custodian(Control& control, std::string writer_generation)
    :
        m_control(control),
        m_writer_generation(std::move(writer_generation))
    {}

    int run(const protocol::Start_request& start, const Validated_start& validated)
    {
        const bool admitted = start.darwin_custody && start.darwin_custody->version == 1;
        if (!admitted) {
            m_control.send_error(reason::k_custodian_lost, "Darwin native custody capability is unavailable");
            close_usage_writers(m_writer_generation);
            return protocol::k_custody_clean_exit_code;
        }
        if (!start_helpers(start, validated)) {
            m_status.close();
            m_owner.close();
            finish_helpers();
            report_settlement(!m_start_attempted);
            // Before START, no adapter could exist. This fallback survives
            // the desktop closing control after the preceding error.
            return !m_start_attempted && m_helpers_confirmed ?
                protocol::k_custody_clean_exit_code : k_exit_failure;
        }
        return serve();
    }

private:
    bool fail(const std::string& detail)
    {
        m_control.send_error(reason::k_custodian_lost, detail);
        return false;
    }

    bool spawn_helper(process::Spawn_request& request, process::Process_tree_owner& tree)
    {
        std::string error;
        if (!tree.establish(&error)) {
            return fail(error);
        }
        tree.configure_root_spawn(request);
        const auto child = process::spawn(request);
        if (child.ok) {
            tree.record_root(child);
            return true;
        }
        if (child.failed_birth) {
            tree.record_failed_root(*child.failed_birth);
        }
        return fail(child.error);
    }

    bool start_helpers(const protocol::Start_request& start, const Validated_start& validated)
    {
        std::string error;
        process::Writable_pipe supervisor_stdin;
        process::Readable_pipe supervisor_stdout;
        process::Readable_pipe supervisor_stderr;
        process::Readable_pipe adapter_stdin;
        process::Writable_pipe adapter_stdout;
        process::Writable_pipe adapter_stderr;
        if (!process::create_child_pipe_owner_writes(&supervisor_stdin, &adapter_stdin, &error) ||
            !process::create_child_pipe_owner_reads(&supervisor_stdout, &adapter_stdout, &error) ||
            !process::create_child_pipe_owner_reads(&supervisor_stderr, &adapter_stderr, &error))
        {
            return fail(error);
        }
        process::Duplex_channel status_child;
        process::Duplex_channel owner_child;
        if (!process::Duplex_channel::create_pair(
                limits::k_max_control_message_bytes, &m_status, &status_child, &error) ||
            !process::Duplex_channel::create_pair(
                process::k_owner_message_bytes, &m_owner, &owner_child, &error) ||
            !m_status.prepare_for_owner(&error) || !m_owner.prepare_for_owner(&error))
        {
            return fail(error);
        }
        const int owner_flags = fcntl(m_owner.native(), F_GETFL);
        if (owner_flags < 0 || fcntl(m_owner.native(), F_SETFL, owner_flags | O_NONBLOCK) < 0) {
            return fail("native owner control channel could not become nonblocking");
        }
        auto request = supervisor_request(start, validated);
        request.darwin_attachment_version = 1;
        const auto request_path = sdk::path_from_utf8(start.scratch_root) /
            sdk::path_from_utf8("cli-addon-supervisor-request-" + start.generation_nonce + ".json");
        const auto encoded = link::encode_supervisor_request(request);
        if (encoded.size() > limits::k_max_supervisor_request_bytes ||
            !m_request_file.write(request_path, encoded, &error))
        {
            return fail("supervisor request: " + error);
        }
        process::Spawn_request supervisor;
        supervisor.executable        = start.supervisor_executable;
        supervisor.environment       = validated.environment;
        supervisor.working_directory = start.scratch_root;
        supervisor.inherited         = process::inherit_standard_streams();
        const auto link_end = (process::Native_handle)start.link_child_number;
        supervisor.inherited.push_back({link_end, 3});
        supervisor.inherited.push_back({status_child.native(), 4});
        supervisor.inherited.push_back({supervisor_stdin.native(), link::k_darwin_stdin_pipe});
        supervisor.inherited.push_back({supervisor_stdout.native(), link::k_darwin_stdout_pipe});
        supervisor.inherited.push_back({supervisor_stderr.native(), link::k_darwin_stderr_pipe});
        supervisor.argv = {supervisor.executable, "--link", "3", "--status", "4",
            "--request", sdk::path_to_utf8(request_path)};
        const bool supervisor_started = spawn_helper(supervisor, m_supervisor_process);
        process::close_native_handle(link_end);
        status_child.close();
        supervisor_stdin.close();
        supervisor_stdout.close();
        supervisor_stderr.close();
        if (!supervisor_started) {
            return false;
        }
        m_supervisor_pid = m_supervisor_process.coordinates().process_group;
        std::vector<std::uint8_t> bytes(limits::k_max_control_message_bytes);
        const auto attached = m_status.receive(bytes.data(), bytes.size(), limits::k_custodian_start_deadline_ms);
        for (const auto handle : attached.descriptors) {
            process::close_native_handle(handle);
        }
        if (attached.status != process::Receive_status::MESSAGE ||
            std::string_view(reinterpret_cast<const char*>(bytes.data()), attached.size) != link::k_darwin_attached)
        {
            return fail("supervisor did not attach the native adapter streams");
        }

        process::Spawn_request owner;
        owner.executable = sdk::path_to_utf8(
            sdk::path_from_utf8(start.supervisor_executable).parent_path() / "vnm_process_custody_owner");
        owner.environment       = validated.environment;
        owner.working_directory = start.scratch_root;
        owner.inherited         = process::inherit_standard_streams();
        owner.inherited.push_back({owner_child.native(), 3});
        owner.argv = {owner.executable, "--control", "3"};
        if (!spawn_helper(owner, m_owner_process)) {
            return false;
        }
        owner_child.close();
        process::Owner_start_request native;
        native.process.executable        = request.adapter_executable;
        native.process.environment       = request.environment;
        native.process.working_directory = request.working_directory;
        for (const auto& argument : request.adapter_argv) {
            native.process.argv.emplace_back(argument);
        }
        native.process.inherited = {
            {adapter_stdin.native(), 0}, {adapter_stdout.native(), 1}, {adapter_stderr.native(), 2}};
        m_start_attempted = true;
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(limits::k_custodian_start_deadline_ms);
        if (!process::send_owner_start_until(m_owner, native, deadline, &error)) {
            return fail("native adapter start: " + error);
        }
        return true;
    }

    void request_stop(int grace_ms)
    {
        if (m_stopping || m_root_exited || !m_owner.valid()) {
            return;
        }
        m_stopping = true;
        std::string error;
        if (!process::send_owner_stop(m_owner, grace_ms, &error)) {
            // Closing still asks the persistent owner to clean up. A lost
            // receipt remains unconfirmed; helper exit cannot replace it.
            m_owner.close();
        }
    }

    void receive_native()
    {
        while (m_owner.valid()) {
            process::Owner_event event;
            std::string error;
            const auto received = process::receive_owner_event(m_owner, &event, 0, &error);
            process::close_native_handle(event.root_reference);
            process::close_native_handle(event.pty_master);
            if (received == process::Receive_status::TIMEOUT) {
                return;
            }
            if (received != process::Receive_status::MESSAGE) {
                m_owner.close();
                return;
            }
            switch (event.kind) {
                case process::Owner_event_kind::STARTED: {
                    if (m_adapter_pid != 0 || event.root_birth_token == 0 || event.root_exit.process_id <= 0) {
                        fail("native owner did not report an original adapter occurrence");
                        request_stop(0);
                        break;
                    }
                    m_adapter_pid = event.root_exit.process_id;
                    protocol::Started started;
                    started.supervisor_pid = m_supervisor_pid;
                    started.adapter_pid    = m_adapter_pid;
                    started.process_group  = m_adapter_pid;
                    started.darwin_custody  = protocol::Darwin_custody{};
                    started.darwin_custody->root_birth_token = std::to_string(event.root_birth_token);
                    if (!m_control.send(protocol::Message(started))) {
                        request_stop(0);
                    }
                    break;
                }
                case process::Owner_event_kind::ROOT_EXITED: {
                    if (event.root_exit.process_id != m_adapter_pid || m_adapter_pid == 0) {
                        fail("native adapter exit does not match its birth");
                        request_stop(0);
                        break;
                    }
                    const protocol::Exited exited{"adapter", event.root_exit.exit_code, event.root_exit.signal};
                    const auto encoded = protocol::encode(protocol::Message(exited));
                    // The supervisor's ordered ADAPTER_EXITED follows its
                    // final bounded drain. Control must never overtake it.
                    (void)m_status.send(reinterpret_cast<const std::uint8_t*>(encoded.data()), encoded.size());
                    m_status.close();
                    m_root_exited = true;
                    m_relay_deadline = process::Deadline::after(std::chrono::milliseconds(1000));
                    break;
                }
                case process::Owner_event_kind::START_REJECTED:
                    m_control.send_error(reason::k_adapter_exited, event.detail);
                    m_status.close();
                    break;
                case process::Owner_event_kind::TREE_SETTLEMENT:
                    if (event.settlement != process::Tree_settlement::PENDING) {
                        m_native_settled = true;
                        m_native_confirmed = event.cleanup_scope == process::Cleanup_scope::OWNED_PROCESS_GROUPS &&
                            event.settlement == process::Tree_settlement::CONFIRMED;
                    }
                    break;
                default:
                    request_stop(0);
                    break;
            }
        }
    }

    int serve()
    {
        bool control_open = true;
        bool supervisor_exited = false;
        while (!m_owner_exited || !supervisor_exited) {
            std::vector<process::Wait_entry> entries;
            const auto add = [&entries](process::Native_handle handle) {
                process::Wait_entry entry;
                entry.handle = handle;
                entry.want_read = true;
                entries.push_back(entry);
            };
            if (control_open) { add(m_control.waitable()); }
            if (m_owner.valid()) { add(m_owner.waitable()); }
            if (!supervisor_exited) {
                for (const auto handle : m_supervisor_process.exit_wait_handles()) { add(handle); }
            }
            if (!m_owner_exited) {
                for (const auto handle : m_owner_process.exit_wait_handles()) { add(handle); }
            }
            const int timeout = m_root_exited && !supervisor_exited ? m_relay_deadline.remaining_ms() : -1;
            if (process::wait_ready(entries.data(), entries.size(), timeout) < 0) {
                request_stop(0);
            }
            receive_native();
            if (control_open) {
                const auto incoming = m_control.receive(0);
                if (incoming.status == process::Receive_status::MESSAGE && incoming.message) {
                    if (const auto* stop = std::get_if<protocol::Stop_request>(&*incoming.message)) {
                        request_stop(std::clamp(stop->grace_ms, 0, limits::k_terminate_to_kill_ms));
                    }
                    else {
                        m_control.send_error(reason::k_out_of_order_message, "expected stop after start");
                    }
                }
                else
                if (incoming.status != process::Receive_status::TIMEOUT) {
                    control_open = false;
                    request_stop(0);
                }
            }
            if (!m_supervisor_process.reap_available().empty()) {
                supervisor_exited = true;
                request_stop(0);
            }
            if (!m_owner_process.reap_available().empty()) {
                // Receive queued settlement before interpreting helper death.
                receive_native();
                m_owner_exited = true;
                m_status.close();
            }
            if (!supervisor_exited && ((m_owner_exited && !m_root_exited) ||
                    (m_root_exited && m_relay_deadline.expired())))
            {
                const auto outcome = m_supervisor_process.terminate_and_reap(
                    std::chrono::milliseconds(0), std::chrono::milliseconds(limits::k_reap_deadline_ms));
                supervisor_exited = outcome.complete || outcome.terminal_unconfirmed;
            }
        }
        finish_helpers();
        report_settlement(m_native_settled && m_native_confirmed);
        return k_exit_ok;
    }

    void finish_helpers()
    {
        // Native owner remains alive until its workload cleanup ends. On a
        // failed start, EOF cancels admission or requests that same cleanup.
        while (!m_owner_exited && m_owner_process.children_remain()) {
            if (!m_owner_process.reap_available().empty()) {
                m_owner_exited = true;
                break;
            }
            auto handles = m_owner_process.exit_wait_handles();
            std::vector<process::Wait_entry> entries;
            for (const auto handle : handles) {
                process::Wait_entry entry;
                entry.handle = handle;
                entry.want_read = true;
                entries.push_back(entry);
            }
            (void)process::wait_ready(entries.data(), entries.size(), -1);
        }
        const bool owner_confirmed = settle_helper(m_owner_process);
        const bool supervisor_confirmed = settle_helper(m_supervisor_process);
        m_helpers_confirmed = owner_confirmed && supervisor_confirmed;
    }

    static bool settle_helper(process::Process_tree_owner& helper)
    {
        while (true) {
            const auto outcome = helper.terminate_and_reap(
                std::chrono::milliseconds(0), std::chrono::milliseconds(limits::k_reap_deadline_ms));
            if (outcome.complete || outcome.terminal_unconfirmed) {
                return outcome.complete;
            }
            // A failed native observation cannot abandon the original direct
            // child's wait obligation. Retain this same owner between attempts.
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    void report_settlement(bool native_confirmed)
    {
        protocol::Reaped reaped;
        reaped.darwin_custody = protocol::Darwin_custody{};
        reaped.darwin_custody->owned_groups_confirmed = native_confirmed && m_helpers_confirmed;
        if (reaped.darwin_custody->owned_groups_confirmed) close_usage_writers(m_writer_generation);
        // complete deliberately remains false: detached descendants are
        // outside the Darwin original-group contract.
        (void)m_control.send(protocol::Message(reaped));
    }

    Control& m_control;
    std::string m_writer_generation;
    process::Duplex_channel m_owner;
    process::Duplex_channel m_status;
    process::Process_tree_owner m_owner_process;
    process::Process_tree_owner m_supervisor_process;
    Request_file m_request_file;
    std::int64_t m_supervisor_pid = 0;
    std::int64_t m_adapter_pid = 0;
    bool m_start_attempted = false;
    bool m_stopping = false;
    bool m_root_exited = false;
    bool m_native_settled = false;
    bool m_native_confirmed = false;
    bool m_helpers_confirmed = false;
    bool m_owner_exited = false;
    process::Deadline m_relay_deadline = process::Deadline::never();
};
#endif

class Custodian
{
public:
    explicit Custodian(process::Native_handle in_control)
    :
        m_control(in_control)
    {}

    int run()
    {
        const int result = run_owned();
#if defined(__APPLE__)
        return m_darwin_run ? result : protocol::k_custody_clean_exit_code;
#else
        if (m_tree_established && !m_tree_settled) {
            // Includes failed ResumeThread with a recorded, created root.
            // Closing the Job in the destructor is not a settlement receipt.
            finish_tree(std::chrono::milliseconds(0), false);
        }
        if (!m_tree_established) close_usage_writers(m_writer_generation);
        if (result != k_exit_ok) {
            log_line("run failed with code " + std::to_string(result) + "; custody is clean");
        }
        return protocol::k_custody_clean_exit_code;
#endif
    }

private:
    int run_owned()
    {
        std::string error;
        if (!m_control.prepare(&error)) {
            log_line("control channel preparation failed: " + error);
            return k_exit_failure;
        }
        protocol::Start_request start;
        if (!receive_start(&start)) {
            return k_exit_start_rejected;
        }

        Validated_start validated;
        protocol::Error rejection;
        if (!validate_start(start, &validated, &rejection)) {
            m_control.send_error(rejection.reason.c_str(), rejection.detail);
            return k_exit_start_rejected;
        }

        const std::string writer_generation = writers::registry_for_scratch(start.scratch_root);
        if (!writers::initialize_generation(writer_generation, &error)) {
            m_control.send_error(reason::k_custodian_lost, "could not establish usage writer custody: " + error);
            return k_exit_start_rejected;
        }
        m_writer_generation = writer_generation;

#if defined(__APPLE__)
        m_darwin_run = true;
        return Darwin_custodian(m_control, m_writer_generation).run(start, validated);
#else

        if (!m_tree.establish(&error)) {
            m_control.send_error(reason::k_custodian_lost, "could not establish process ownership: " + error);
            return k_exit_start_rejected;
        }
        m_tree_established = true;

        if (!start_supervisor(start, validated)) {
            return k_exit_start_rejected;
        }

        protocol::Started started;
        started.supervisor_pid = m_supervisor_pid;
        started.adapter_pid    = m_adapter_pid;
        started.process_group  = m_tree.coordinates().process_group;
        if (!m_control.send_started(started, m_tree.coordinates())) {
            log_line("desktop gone before started; tearing the tree down");
            teardown_quietly();
            return k_exit_failure;
        }

        return serve();
#endif
    }

private:
    bool receive_start(protocol::Start_request* out_start)
    {
        const Control::Incoming incoming = m_control.receive(limits::k_custodian_start_deadline_ms);
        switch (incoming.status) {
            case process::Receive_status::TIMEOUT:
                log_line("no start within k_custodian_start_deadline_ms");
                return false;
            case process::Receive_status::CLOSED:
                log_line("control channel closed before start");
                return false;
            case process::Receive_status::OVERSIZE:
                m_control.send_error(reason::k_oversize_frame, "control message exceeds k_max_control_message_bytes");
                return false;
            case process::Receive_status::FAILED:
                log_line("control channel receive failed");
                return false;
            case process::Receive_status::MESSAGE:
            default:
                break;
        }
        if (!incoming.message) {
            m_control.send_error(incoming.failure.reason.c_str(), incoming.failure.detail);
            return false;
        }
        if (const protocol::Start_request* start = std::get_if<protocol::Start_request>(&*incoming.message)) {
            *out_start = *start;
            return true;
        }
        if (std::holds_alternative<protocol::Stop_request>(*incoming.message)) {
            m_control.send_error(reason::k_out_of_order_message, "stop before start");
            return false;
        }
        m_control.send_error(reason::k_unknown_message_type, "the first control message must be start");
        return false;
    }

    bool start_supervisor(const protocol::Start_request& in_start, const Validated_start& in_validated)
    {
        const auto request = supervisor_request(in_start, in_validated);

        std::string error;
        const fs::path request_path =
            sdk::path_from_utf8(in_start.scratch_root) /
            sdk::path_from_utf8("cli-addon-supervisor-request-" + in_start.generation_nonce + ".json");
        const std::string encoded_request = link::encode_supervisor_request(request);
        if (encoded_request.size() > limits::k_max_supervisor_request_bytes) {
            m_control.send_error(reason::k_frame_violation,
                "generated supervisor request is " + std::to_string(encoded_request.size()) +
                    " bytes, above k_max_supervisor_request_bytes");
            return false;
        }
        if (!m_request_file.write(request_path, encoded_request, &error)) {
            m_control.send_error(reason::k_custody_write_failed, error);
            return false;
        }

        process::Duplex_channel status_child;
        if (!process::Duplex_channel::create_pair(
                limits::k_max_control_message_bytes, &m_status, &status_child, &error))
        {
            m_control.send_error(reason::k_supervisor_lost, "status channel: " + error);
            return false;
        }
        if (!m_status.prepare_for_owner(&error)) {
            m_control.send_error(reason::k_supervisor_lost, "status channel preparation: " + error);
            return false;
        }

        process::Native_handle link_end = process::k_invalid_handle;
        (void)process::parse_native_handle_argument(std::to_string(in_start.link_child_number), &link_end);

        process::Spawn_request spawn;
        spawn.executable        = in_start.supervisor_executable;
        spawn.environment       = in_validated.environment;
        spawn.working_directory = in_start.scratch_root;
        spawn.inherited         = process::inherit_standard_streams();
        const std::size_t link_index = spawn.inherited.size();
        const process::Inherited_descriptor link_descriptor{link_end, 3};
        spawn.inherited.push_back(link_descriptor);
        const std::size_t status_index = spawn.inherited.size();
        const process::Inherited_descriptor status_descriptor{status_child.native(), 4};
        spawn.inherited.push_back(status_descriptor);
        spawn.argv = {
            in_start.supervisor_executable,
            "--link",    process::inherited_argument(link_index),
            "--status",  process::inherited_argument(status_index),
            "--request", sdk::path_to_utf8(m_request_file.path()),
        };
        m_tree.configure_root_spawn(spawn);

        const process::Spawn_result spawned = process::spawn(spawn);
        // Only the supervisor and the desktop hold link ends from here on, so
        // link EOF means what the supervisor takes it to mean.
        process::close_native_handle(link_end);
        status_child.close();
        if (spawned.process_handle != process::k_invalid_handle) {
            m_tree.record_root(spawned);
            m_supervisor_pid = spawned.process_id;
            m_supervisor_handle = spawned.process_handle; // Borrowed from m_tree until teardown.
        }
        if (!spawned.ok) {
            m_control.send_error(reason::k_supervisor_lost, spawned.error);
            return false;
        }
        return await_status_report();
    }

    // The supervisor's report names the adapter (Linux: with its pidfd) or
    // says why the adapter could not start. A supervisor that exits first is
    // reported as lost.
    bool await_status_report()
    {
        const process::Deadline deadline =
            process::Deadline::after(std::chrono::milliseconds(limits::k_custodian_start_deadline_ms));
        std::vector<std::uint8_t> buffer(limits::k_max_control_message_bytes);
        while (true) {
            std::vector<process::Wait_entry> entries;
            process::Wait_entry status_entry;
            status_entry.handle    = m_status.waitable();
            status_entry.want_read = true;
            entries.push_back(status_entry);
            for (const process::Native_handle handle : m_tree.exit_wait_handles()) {
                process::Wait_entry entry;
                entry.handle    = handle;
                entry.want_read = true;
                entries.push_back(entry);
            }
            const int ready = process::wait_ready(entries.data(), entries.size(), deadline.remaining_ms());
            if (ready == 0) {
                m_control.send_error(reason::k_supervisor_lost,
                    "supervisor did not report within k_custodian_start_deadline_ms");
                teardown_quietly();
                return false;
            }
            if (ready < 0) {
                m_control.send_error(reason::k_supervisor_lost, "waiting for the supervisor report failed");
                teardown_quietly();
                return false;
            }

            // A short-lived adapter can make the supervisor report and exit
            // before this wait returns. Both handles are then ready; consume
            // the queued report before treating the process exit as a loss.
            if (entries[0].readable || entries[0].closed) {
                const process::Receive_result received = m_status.receive(buffer.data(), buffer.size(), 0);
                if (received.status != process::Receive_status::TIMEOUT) {
                    if (received.status != process::Receive_status::MESSAGE) {
                        for (const process::Native_handle handle : received.descriptors) {
                            process::close_native_handle(handle);
                        }
                        m_control.send_error(
                            reason::k_supervisor_lost,
                            "status channel closed before the adapter was reported");
                        teardown_quietly();
                        return false;
                    }

                    link::Status_report report;
                    const std::string_view text(reinterpret_cast<const char*>(buffer.data()), received.size);
                    if (!link::decode_status_report(text, &report)) {
                        for (const process::Native_handle handle : received.descriptors) {
                            process::close_native_handle(handle);
                        }
                        m_control.send_error(reason::k_supervisor_lost, "supervisor sent an unreadable status report");
                        teardown_quietly();
                        return false;
                    }
                    if (!report.adapter_started) {
                        m_control.send_error(reason::k_adapter_exited, "adapter could not be started: " + report.failure);
                        teardown_quietly();
                        return false;
                    }
#if !defined(_WIN32)
                    process::Native_handle adapter_handle = process::k_invalid_handle;
                    if (!received.descriptors.empty()) {
                        adapter_handle = received.descriptors[0];
                        for (std::size_t i = 1; i < received.descriptors.size(); ++i) {
                            process::close_native_handle(received.descriptors[i]);
                        }
                    }
#endif
#if defined(_WIN32)
                    process::Native_handle source_handle = process::k_invalid_handle;
                    const bool retained = report.adapter_handle_number != 0 &&
                        process::parse_native_handle_argument(std::to_string(report.adapter_handle_number),
                            &source_handle) &&
                        m_tree.record_descendant_from_process(report.adapter_pid, m_supervisor_handle, source_handle);
#else
                    const bool retained = m_tree.record_descendant(report.adapter_pid, adapter_handle);
#endif
                    if (!retained) {
                        m_control.send_error(reason::k_adapter_exited, "adapter " + std::to_string(report.adapter_pid) +
                            " exact native handle could not be retained");
                        teardown_quietly();
                        return false;
                    }
                    m_adapter_pid = report.adapter_pid;
#if defined(_WIN32)
                    const auto receipt = link::encode_status_receipt(report);
                    if (m_status.send(reinterpret_cast<const std::uint8_t*>(receipt.data()), receipt.size()) !=
                        process::Io_status::TRANSFERRED)
                    {
                        m_control.send_error(reason::k_supervisor_lost, "the adapter handle receipt could not be sent");
                        teardown_quietly();
                        return false;
                    }
#endif
                    m_status.close();
                    return true;
                }
            }

            for (const process::Exit_status& exit : m_tree.reap_available()) {
                if (exit.process_id == m_supervisor_pid) {
                    m_control.send_error(reason::k_supervisor_lost, "supervisor exited before reporting the adapter: " +
                        describe_exit(exit));
                    teardown_quietly();
                    return false;
                }
            }
        }
    }

    int serve()
    {
        while (true) {
            std::vector<process::Wait_entry> entries;
            process::Wait_entry control_entry;
            control_entry.handle    = m_control.waitable();
            control_entry.want_read = true;
            entries.push_back(control_entry);
            for (const process::Native_handle handle : m_tree.exit_wait_handles()) {
                process::Wait_entry entry;
                entry.handle    = handle;
                entry.want_read = true;
                entries.push_back(entry);
            }
            const int ready = process::wait_ready(entries.data(), entries.size(), -1);
            if (ready < 0) {
                log_line("wait failed; tearing the tree down");
                teardown_quietly();
                return k_exit_failure;
            }

            report_exits();

            if (!entries[0].readable && !entries[0].closed && !entries[0].failed) {
                continue;
            }
            const Control::Incoming incoming = m_control.receive(0);
            switch (incoming.status) {
                case process::Receive_status::TIMEOUT:
                    continue;
                case process::Receive_status::CLOSED:
                    log_line("desktop lost; terminating and reaping the tree");
                    teardown_quietly();
                    return k_exit_ok;
                case process::Receive_status::OVERSIZE:
                    m_control.send_error(reason::k_oversize_frame, "control message exceeds k_max_control_message_bytes");
                    continue;
                case process::Receive_status::FAILED:
                    log_line("control channel receive failed; tearing the tree down");
                    teardown_quietly();
                    return k_exit_failure;
                case process::Receive_status::MESSAGE:
                default:
                    break;
            }
            if (!incoming.message) {
                m_control.send_error(incoming.failure.reason.c_str(), incoming.failure.detail);
                continue;
            }
            if (const protocol::Stop_request* stop = std::get_if<protocol::Stop_request>(&*incoming.message)) {
                const int grace_ms = std::clamp(stop->grace_ms, 0, limits::k_terminate_to_kill_ms);
                finish_tree(std::chrono::milliseconds(grace_ms), true);
                return k_exit_ok;
            }
            if (std::holds_alternative<protocol::Start_request>(*incoming.message)) {
                m_control.send_error(reason::k_out_of_order_message, "a second start");
                continue;
            }
            m_control.send_error(reason::k_unknown_message_type, "the desktop sent a custodian-side message type");
        }
    }

    // Every status the tree produces passes through here, whether it was
    // reaped live or during a teardown: the supervisor's and the adapter's
    // are reported as `exited`, a reparented grandchild's is ours to reap and
    // nobody's to receive, so it only counts in the final report.
    void report_exit(const process::Exit_status& in_exit)
    {
        ++m_processes_reaped;
        protocol::Exited exited;
        exited.exit_code = in_exit.exit_code;
        exited.signal    = in_exit.signal;
        if (in_exit.process_id == m_supervisor_pid) {
            exited.who = "supervisor";
        }
        else
        if (in_exit.process_id == m_adapter_pid) {
            exited.who = "adapter";
        }
        else {
            return;
        }
        (void)m_control.send(protocol::Message(std::move(exited)));
    }

    void report_exits()
    {
        for (const process::Exit_status& exit : m_tree.reap_available()) {
            report_exit(exit);
        }
    }

    process::Reap_outcome terminate_tree(std::chrono::milliseconds in_terminate_to_kill)
    {
        const process::Reap_outcome outcome = m_tree.terminate_and_reap(
            in_terminate_to_kill,
            std::chrono::milliseconds(limits::k_reap_deadline_ms));
        if (!outcome.complete) {
            std::string remaining;
            for (const std::int64_t pid : outcome.remaining_children) {
                remaining += " " + std::to_string(pid);
            }
            log_line("tree not fully reaped after " + std::to_string(outcome.elapsed.count()) +
                " ms; children with unconfirmed cleanup:" + remaining +
                (outcome.error.empty() ? std::string{} : "; " + outcome.error));
        }
        return outcome;
    }

    // The teardown that ends a served run: the statuses the terminate/kill
    // produced are reported as `exited` before the caller sends the `reaped`
    // this returns, so the desktop learns how each process ended.
    protocol::Reaped teardown_and_report(std::chrono::milliseconds in_terminate_to_kill)
    {
        const process::Reap_outcome outcome = terminate_tree(in_terminate_to_kill);
        for (const process::Exit_status& exit : outcome.exits) {
            report_exit(exit);
        }
        protocol::Reaped reaped;
        reaped.complete          = outcome.complete;
        reaped.escalated_to_kill = outcome.escalated_to_kill;
        reaped.processes_reaped  = m_processes_reaped;
        reaped.remaining         = outcome.remaining_children;
        return reaped;
    }

    // The teardown of a run that never reached `started` or whose desktop is
    // gone: nothing is reported, the tree is only ended.
    void teardown_quietly()
    {
        finish_tree(std::chrono::milliseconds(limits::k_terminate_to_kill_ms), false);
    }

    void finish_tree(std::chrono::milliseconds in_grace, bool in_report)
    {
        while (true) {
            const protocol::Reaped reaped = teardown_and_report(in_grace);
            if (reaped.complete) close_usage_writers(m_writer_generation);
            if (in_report) {
                (void)m_control.send(protocol::Message(reaped));
            }
            if (reaped.complete) {
                m_tree_settled = true;
                return;
            }
            // Each native attempt is bounded; an expired attempt does not
            // authorize destruction of the sole tree owner. Retain it for
            // retry or the containing owner's independent native cleanup.
            in_grace = std::chrono::milliseconds(0);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    static std::string describe_exit(const process::Exit_status& in_exit)
    {
        if (in_exit.signal != 0) {
            return "signal " + std::to_string(in_exit.signal);
        }
        return "exit code " + std::to_string(in_exit.exit_code);
    }

    Control                     m_control;
    std::string                 m_writer_generation;
#if defined(__APPLE__)
    bool                        m_darwin_run = false;
#endif
    process::Process_tree_owner m_tree;
    bool                        m_tree_established = false;
    bool                        m_tree_settled = false;
    process::Duplex_channel     m_status;
    Request_file                m_request_file;
    std::int64_t                m_supervisor_pid   = 0;
    process::Native_handle      m_supervisor_handle = process::k_invalid_handle;
    std::int64_t                m_adapter_pid      = 0;
    int                         m_processes_reaped = 0;
};

} // namespace

int main(int argc, char** argv)
{
    Options options;
    std::string error;
    if (!parse_options(argc, argv, &options, &error)) {
        log_line(error);
        return k_exit_failure;
    }
    if (!process::prepare_parent_process(&error)) {
        log_line(error);
        return k_exit_failure;
    }
    Custodian custodian(options.control);
    return custodian.run();
}
