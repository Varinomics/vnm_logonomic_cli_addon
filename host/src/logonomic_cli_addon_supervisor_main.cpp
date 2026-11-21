// logonomic_cli_addon_supervisor: owns one adapter's bounded protocol I/O.
//
//   logonomic_cli_addon_supervisor --link <inherited link end>
//                                  --status <inherited status channel end>
//                                  --request <request file the custodian wrote>
//
// Spawns the adapter from the request (same session and group as this
// process, exactly stdin/stdout/stderr inherited), reports its pid to the
// custodian, then until the adapter exits: drains stdout through the SDK
// frame decoder and forwards frames to the desktop, drains stderr into the
// bounded ring, and feeds desktop frames to adapter stdin through a bounded
// queue. Termination is the custodian's; this process only ever closes
// stdin and exits.

#include "logonomic_cli_addon_native_file_path.h"
#include "logonomic_cli_addon_custodian_protocol.h"
#include <vnm_process_custody/process_channel.h>
#include <vnm_process_custody/process_clock.h>
#include "logonomic_cli_addon_process_drain.h"
#include <vnm_process_custody/process_pipe.h>
#include <vnm_process_custody/process_spawn.h>
#include "logonomic_cli_addon_supervisor_link.h"

#include <vnm_process_custody/process_tree.h>

#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/frame.h"
#include "logonomic_cli_addon/json.h"
#include "logonomic_cli_addon/limits.h"
#include "logonomic_cli_addon/package_source.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace fs      = std::filesystem;
namespace process = vnm::process_custody;
namespace link    = logonomic::cli_addon_host::link;
namespace sdk     = logonomic::cli_addon;
namespace limits  = logonomic::cli_addon::limits;
namespace reason  = logonomic::cli_addon::reason;

constexpr int k_exit_ok      = 0;
constexpr int k_exit_failure = 1;

constexpr int k_post_exit_drain_ms = 250;
constexpr int k_stream_turn_ms = 5;
constexpr int k_stderr_publication_cadence_ms = 50;

constexpr std::uint64_t k_max_outgoing_link_bytes =
    limits::k_max_stdin_queue_bytes + limits::k_max_link_frame_bytes;

void log_line(const std::string& in_text)
{
    std::fprintf(stderr, "logonomic_cli_addon_supervisor: %s\n", in_text.c_str());
}

struct Options
{
    process::Native_handle link    = process::k_invalid_handle;
    process::Native_handle status  = process::k_invalid_handle;
    std::string            request;
};

bool parse_options(int argc, char** argv, Options* out_options, std::string* out_error)
{
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        const bool has_value = i + 1 < argc;
        if (argument == "--link" && has_value) {
            if (!process::parse_native_handle_argument(argv[++i], &out_options->link)) {
                *out_error = "--link expects an inherited handle number";
                return false;
            }
        }
        else
        if (argument == "--status" && has_value) {
            if (!process::parse_native_handle_argument(argv[++i], &out_options->status)) {
                *out_error = "--status expects an inherited handle number";
                return false;
            }
        }
        else
        if (argument == "--request" && has_value) {
            out_options->request = argv[++i];
        }
        else {
            *out_error = "unexpected argument \"" + argument +
                "\"; usage: --link <handle> --status <handle> --request <path>";
            return false;
        }
    }
    if (out_options->link == process::k_invalid_handle || out_options->status == process::k_invalid_handle ||
        out_options->request.empty())
    {
        *out_error = "--link, --status and --request are all required";
        return false;
    }
    if (!process::native_handle_is_open(out_options->link) || !process::native_handle_is_open(out_options->status)) {
        *out_error = "the link or status end was not inherited";
        return false;
    }
    return true;
}

bool read_request(const std::string& in_path, link::Supervisor_request* out_request, std::string* out_error)
{
    std::ifstream file(logonomic::cli_addon_host::native_file_path(sdk::path_from_utf8(in_path)), std::ios::binary);
    if (!file) {
        *out_error = "cannot open request file " + in_path;
        return false;
    }
    std::string contents;
    contents.resize(limits::k_max_supervisor_request_bytes + 1);
    file.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    contents.resize(static_cast<std::size_t>(file.gcount()));
    if (contents.size() > limits::k_max_supervisor_request_bytes) {
        *out_error = "request file exceeds k_max_supervisor_request_bytes";
        return false;
    }
    return link::decode_supervisor_request(contents, out_request, out_error);
}

std::string exit_json(int in_exit_code, int in_signal)
{
    sdk::Json_value object = sdk::Json_value::object();
    object.set("exit_code", sdk::Json_value(in_exit_code));
    object.set("signal",    sdk::Json_value(in_signal));
    return sdk::serialize_json(object);
}

std::string violation_json(const std::string& in_reason)
{
    sdk::Json_value object = sdk::Json_value::object();
    object.set("reason", sdk::Json_value(in_reason));
    return sdk::serialize_json(object);
}

// ---------------------------------------------------------------------------

class Supervisor
{
public:
    Supervisor(Options in_options, link::Supervisor_request in_request)
    :
        m_options(std::move(in_options)),
        m_request(std::move(in_request)),
        m_link(m_options.link),
        m_status(m_options.status, limits::k_max_control_message_bytes),
        m_drain(m_request.max_frame_bytes),
        m_ring(m_request.stderr_ring_bytes),
        m_reader(m_request.max_link_frame_bytes)
    {}

    int run()
    {
        std::string error;
        if (!m_link.prepare_for_owner(&error)) {
            log_line("link: " + error);
            return k_exit_failure;
        }
#if defined(_WIN32) || defined(__APPLE__)
        if (!m_status.prepare_for_owner(&error)) {
            log_line("status receipt: " + error);
            return k_exit_failure;
        }
#endif
        if (!start_adapter()) {
            return k_exit_failure;
        }
        serve();
        return k_exit_ok;
    }

private:
    bool report_failure(const std::string& in_detail)
    {
        link::Status_report report;
        report.failure = in_detail;
        const std::string text = link::encode_status_report(report);
        (void)m_status.send(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
        log_line(in_detail);
        return false;
    }

    bool start_adapter()
    {
#if defined(__APPLE__)
        if (m_request.darwin_attachment_version != 1) {
            return report_failure("Darwin native custody attachment is unavailable");
        }
        m_stdin  = process::Writable_pipe(link::k_darwin_stdin_pipe);
        m_stdout = process::Readable_pipe(link::k_darwin_stdout_pipe);
        m_stderr = process::Readable_pipe(link::k_darwin_stderr_pipe);
        std::string error;
        if (!m_stdin.prepare_for_owner(&error) || !m_stdout.prepare_for_owner(&error) ||
            !m_stderr.prepare_for_owner(&error))
        {
            return report_failure("adapter attachment: " + error);
        }
        return m_status.send(
            reinterpret_cast<const std::uint8_t*>(link::k_darwin_attached.data()),
            link::k_darwin_attached.size()) == process::Io_status::TRANSFERRED;
#else
        std::string error;
        process::Readable_pipe stdin_child;
        process::Writable_pipe stdout_child;
        process::Writable_pipe stderr_child;
        if (!process::create_child_pipe_owner_writes(
                &m_stdin,
                &stdin_child,
                &error) ||
            !process::create_child_pipe_owner_reads(
                &m_stdout,
                &stdout_child,
                &error) ||
            !process::create_child_pipe_owner_reads(
                &m_stderr,
                &stderr_child,
                &error))
        {
            return report_failure("adapter pipes: " + error);
        }
        // This process's ends are prepared before the adapter exists, so a
        // failure here is reported without a running adapter to account for;
        // the child's ends are separate open descriptions and stay blocking.
        if (!m_stdin.prepare_for_owner(&error) || !m_stdout.prepare_for_owner(&error) ||
            !m_stderr.prepare_for_owner(&error))
        {
            return report_failure("adapter pipe preparation: " + error);
        }

        process::Spawn_request spawn;
        spawn.executable        = std::move(m_request.adapter_executable);
        auto arguments = std::move(m_request.adapter_argv);
        spawn.argv.reserve(arguments.size());
        for (std::string& argument : arguments) {
            spawn.argv.emplace_back(std::move(argument));
        }
        spawn.environment       = std::move(m_request.environment);
        spawn.working_directory = std::move(m_request.working_directory);
        spawn.inherited         = {
            process::Inherited_descriptor{stdin_child.native(),  0},
            process::Inherited_descriptor{stdout_child.native(), 1},
            process::Inherited_descriptor{stderr_child.native(), 2},
        };
        const process::Spawn_result spawned = process::spawn(spawn);
        stdin_child.close();
        stdout_child.close();
        stderr_child.close();
        if (!spawned.ok) {
#if defined(_WIN32)
            // The custodian put this supervisor in its non-breakaway Job
            // before creation. The failed adapter is already in that same
            // tree; reporting failure drives the custodian's retained Job
            // cleanup, which does not depend on this diagnostic handle.
            if (spawned.process_handle != process::k_invalid_handle) {
                process::close_native_handle(spawned.process_handle);
            }
#endif
            return report_failure(spawned.error);
        }
        m_adapter_pid    = spawned.process_id;
        m_adapter_handle = spawned.process_handle;

        link::Status_report report;
        report.adapter_started = true;
        report.adapter_pid     = m_adapter_pid;
#if defined(_WIN32)
        report.adapter_handle_number = reinterpret_cast<std::uintptr_t>(m_adapter_handle);
#endif
        const std::string text = link::encode_status_report(report);
        const auto* data = reinterpret_cast<const std::uint8_t*>(text.data());
        const process::Io_status sent = process::Duplex_channel::transfers_descriptors()
            ? m_status.send_descriptors(data, text.size(), {m_adapter_handle})
            : m_status.send(data, text.size());
        if (sent != process::Io_status::TRANSFERRED) {
            m_status.close();
            log_line("custodian did not take the adapter report; exiting");
            return false;
        }
#if defined(_WIN32)
        // The adapter may already be dead; this original handle still pins its
        // occurrence. Do not leave (and destroy the source handle table) until
        // the custodian has duplicated it from this exact supervisor instance.
        std::vector<std::uint8_t> receipt(limits::k_max_control_message_bytes);
        const auto received = m_status.receive(receipt.data(), receipt.size(),
            limits::k_custodian_start_deadline_ms);
        const std::string_view received_text(reinterpret_cast<const char*>(receipt.data()), received.size);
        if (received.status != process::Receive_status::MESSAGE ||
            received_text != link::encode_status_receipt(report))
        {
            m_status.close();
            log_line("custodian did not confirm exact adapter handle custody; exiting");
            return false;
        }
#endif
        m_status.close();
        return true;
#endif
    }

    // One pass of the readiness loop. Returns false when this process is done.
    void serve()
    {
        while (true) {
            if (!m_link.valid() || !write_link()) {
                m_stdin.close();
                return;
            }
            std::vector<process::Wait_entry> entries;
            const auto add = [&entries](process::Native_handle handle, bool read, bool write) {
                process::Wait_entry entry;
                entry.handle     = handle;
                entry.want_read  = read;
                entry.want_write = write;
                entries.push_back(entry);
                return entries.size() - 1;
            };
            const std::size_t link_index    = add(m_link.waitable(), true, false);
            const std::size_t link_write_index = !m_link_queue.empty()
                ? add(m_link.write_waitable(), false, true) : SIZE_MAX;
            const std::size_t stdout_index  = m_stdout.valid() ? add(m_stdout.waitable(), true, false) : SIZE_MAX;
            const std::size_t stderr_index  = m_stderr.valid() ? add(m_stderr.waitable(), true, false) : SIZE_MAX;
            const std::size_t stdin_index   = m_stdin.valid() && !m_stdin_queue.empty()
                ? add(m_stdin.waitable(), false, true) : SIZE_MAX;
#if defined(__APPLE__)
            const std::size_t adapter_index = !m_adapter_exited ? add(m_status.waitable(), true, false) : SIZE_MAX;
#else
            const std::size_t adapter_index = !m_adapter_exited ? add(m_adapter_handle, true, false) : SIZE_MAX;
#endif

            const int ready = process::wait_ready(
                entries.data(), entries.size(), stderr_publication_wait_ms());
            if (ready < 0) {
                log_line("wait failed; closing adapter stdin and exiting");
                m_stdin.close();
                return;
            }

            if (entries[link_index].readable || entries[link_index].closed || entries[link_index].failed) {
                if (!read_link()) {
                    // Desktop or custodian gone: nothing to forward to, and
                    // termination is the custodian's.
                    m_stdin.close();
                    return;
                }
            }
            if (stderr_index != SIZE_MAX && (entries[stderr_index].readable || entries[stderr_index].closed)) {
                drain_stderr();
            }
            publish_stderr(false);
            if (stdout_index != SIZE_MAX && (entries[stdout_index].readable || entries[stdout_index].closed)) {
                drain_stdout();
            }
            if (stdin_index != SIZE_MAX && (entries[stdin_index].writable || entries[stdin_index].closed ||
                entries[stdin_index].failed))
            {
                write_stdin();
            }
            if (link_write_index != SIZE_MAX &&
                (entries[link_write_index].writable || entries[link_write_index].closed ||
                    entries[link_write_index].failed))
            {
                if (!write_link()) {
                    m_stdin.close();
                    return;
                }
            }
            if (adapter_index != SIZE_MAX && (entries[adapter_index].readable || entries[adapter_index].closed)) {
                collect_adapter_exit();
            }

            if (m_adapter_exited) {
                finish_after_exit();
                return;
            }
        }
    }

    // Peeks the adapter's exit for the link report but does not reap it: the
    // custodian is the sole reaper of the tree, so the zombie is left to
    // reparent to the custodian when this process exits.
    void collect_adapter_exit()
    {
#if defined(__APPLE__)
        // Only the native owner can produce the adapter's exact exit status.
        // Losing its relay ends supervision without inventing an adapter exit.
        std::vector<std::uint8_t> bytes(limits::k_max_control_message_bytes);
        const auto received = m_status.receive(bytes.data(), bytes.size(), 0);
        for (const auto handle : received.descriptors) {
            process::close_native_handle(handle);
        }
        if (received.status == process::Receive_status::TIMEOUT) {
            return;
        }
        logonomic::cli_addon_host::custodian_protocol::Decode_failure failure;
        const auto message = received.status == process::Receive_status::MESSAGE
            ? logonomic::cli_addon_host::custodian_protocol::decode(
                  std::string_view(reinterpret_cast<const char*>(bytes.data()), received.size), &failure)
            : std::nullopt;
        const auto* exit = message
            ? std::get_if<logonomic::cli_addon_host::custodian_protocol::Exited>(&*message) : nullptr;
        if (!exit || exit->who != "adapter") {
            m_link.close();
            return;
        }
        m_adapter_exited    = true;
        m_adapter_exit_code = exit->exit_code;
        m_adapter_signal    = exit->signal;
        m_status.close();
#else
        process::Exit_status status;
        if (!process::peek_process_exit(m_adapter_pid, m_adapter_handle, &status)) {
            return;   // readable exit handle but the status is not yet available; the next pass peeks again
        }
        m_adapter_exited    = true;
        m_adapter_exit_code = status.exit_code;
        m_adapter_signal    = status.signal;
#endif
    }

    // Windows can signal process exit before an overlapped stdout read has
    // completed. Give both streams the same bounded post-exit drain used by
    // the SDK reference host, while continuing both directions of the link.
    // A grandchild holding a pipe open cannot extend this bound.
    void finish_after_exit()
    {
        const process::Deadline drain_deadline =
            process::Deadline::after(std::chrono::milliseconds(k_post_exit_drain_ms));
        while ((m_stdout.valid() || m_stderr.valid()) && !drain_deadline.expired() && m_link.valid()) {
            drain_stdout(drain_deadline);
            drain_stderr(drain_deadline);
            if (!m_stdout.valid() && !m_stderr.valid()) {
                break;
            }

            std::vector<process::Wait_entry> entries;
            const auto add = [&entries](process::Native_handle handle, bool read, bool write) {
                process::Wait_entry entry;
                entry.handle     = handle;
                entry.want_read  = read;
                entry.want_write = write;
                entries.push_back(entry);
                return entries.size() - 1;
            };
            const std::size_t link_index = add(m_link.waitable(), true, false);
            const std::size_t link_write_index = !m_link_queue.empty()
                ? add(m_link.write_waitable(), false, true) : SIZE_MAX;
            const std::size_t stdout_index = m_stdout.valid()
                ? add(m_stdout.waitable(), true, false) : SIZE_MAX;
            const std::size_t stderr_index = m_stderr.valid()
                ? add(m_stderr.waitable(), true, false) : SIZE_MAX;
            const int ready = process::wait_ready(
                entries.data(), entries.size(), drain_deadline.remaining_ms());
            if (ready <= 0) {
                break;
            }
            if (entries[link_index].readable || entries[link_index].closed || entries[link_index].failed) {
                if (!read_link(drain_deadline)) {
                    break;
                }
            }
            if (link_write_index != SIZE_MAX &&
                (entries[link_write_index].writable || entries[link_write_index].closed ||
                    entries[link_write_index].failed))
            {
                (void)write_link();
            }
            if (stdout_index != SIZE_MAX &&
                (entries[stdout_index].readable || entries[stdout_index].closed || entries[stdout_index].failed))
            {
                drain_stdout(drain_deadline);
            }
            if (stderr_index != SIZE_MAX &&
                (entries[stderr_index].readable || entries[stderr_index].closed || entries[stderr_index].failed))
            {
                drain_stderr(drain_deadline);
            }
        }
        if (m_stdout.valid()) {
            handle_drain_outcome(m_drain.finish());
            m_stdout.close();
        }
        m_stderr.close();
        m_stdin.close();
        flush_stderr_tail();
        send_link(link::Message_type::ADAPTER_EXITED, exit_json(m_adapter_exit_code, m_adapter_signal));

        const process::Deadline flush_deadline =
            process::Deadline::after(std::chrono::milliseconds(k_post_exit_drain_ms));
        while (m_link.valid() && !m_link_queue.empty() && !flush_deadline.expired()) {
            if (!write_link() || m_link_queue.empty()) {
                break;
            }
            process::Wait_entry entries[2];
            entries[0].handle     = m_link.waitable();
            entries[0].want_read  = true;
            entries[1].handle     = m_link.write_waitable();
            entries[1].want_write = true;
            const int ready = process::wait_ready(entries, 2, flush_deadline.remaining_ms());
            if (ready <= 0) {
                break;
            }
            if (entries[0].readable || entries[0].closed || entries[0].failed) {
                if (!read_link(flush_deadline)) {
                    break;
                }
            }
        }
        if (!m_link_queue.empty()) {
            m_link.close();
            m_link_queue.clear();
            m_link_queue_bytes = 0;
        }
    }

    void drain_stdout(const process::Deadline& in_deadline =
        process::Deadline::after(std::chrono::milliseconds(k_stream_turn_ms)))
    {
        logonomic::cli_addon_host::process::drain_stream_turn(m_stdout, in_deadline,
        [this](const process::Io_result& read, const std::uint8_t* chunk) {
            if (read.status == process::Io_status::TRANSFERRED) {
                handle_drain_outcome(m_drain.feed(chunk, read.bytes));
                return true;
            }
            if (read.status == process::Io_status::WOULD_BLOCK) {
                return false;
            }
            // CLOSED or FAILED: the stream is over either way.
            handle_drain_outcome(m_drain.finish());
            m_stdout.close();
            return false;
        });
    }

    void handle_drain_outcome(const logonomic::cli_addon_host::process::Stdout_frame_drain::Outcome& in_outcome)
    {
        for (const std::string& frame : in_outcome.frames) {
            if (!m_protocol_stopped) {
                send_link(link::Message_type::ADAPTER_FRAME, frame);
            }
        }
        if (in_outcome.violation) {
            stop_protocol_io();
            send_link(link::Message_type::VIOLATION, violation_json(in_outcome.reason));
        }
    }

    // Stderr is collected by the SDK's one canonical newest-tail producer.
    // Once it overflows, complete replacement snapshots are forwarded at the
    // publication cadence. That makes a live provider's retained diagnostic
    // observable without appending obsolete prefix bytes or queueing one link
    // frame per adapter write.
    void drain_stderr(const process::Deadline& in_deadline =
        process::Deadline::after(std::chrono::milliseconds(k_stream_turn_ms)))
    {
        logonomic::cli_addon_host::process::drain_stream_turn(m_stderr, in_deadline,
        [this](const process::Io_result& read, const std::uint8_t* chunk) {
            if (read.status == process::Io_status::WOULD_BLOCK) {
                return false;
            }
            if (read.status != process::Io_status::TRANSFERRED) {
                m_stderr.close();
                return false;
            }
            m_ring.append(std::string_view(reinterpret_cast<const char*>(chunk), read.bytes));
            return true;
        });
        publish_stderr(false);
    }

    void flush_stderr_tail()
    {
        publish_stderr(true);
    }

    int stderr_publication_wait_ms() const
    {
        if (m_ring.total_bytes() == m_reported_stderr_bytes) {
            return -1;
        }
        if (!m_has_stderr_publication) {
            return 0;
        }
        const auto next = m_last_stderr_publication +
            std::chrono::milliseconds(k_stderr_publication_cadence_ms);
        const auto now = std::chrono::steady_clock::now();
        if (now >= next) {
            return 0;
        }
        return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(next - now).count()) + 1;
    }

    void publish_stderr(bool in_terminal_edge)
    {
        const std::uint64_t total = m_ring.total_bytes();
        if (total == m_reported_stderr_bytes) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (!in_terminal_edge && m_has_stderr_publication &&
            now - m_last_stderr_publication < std::chrono::milliseconds(k_stderr_publication_cadence_ms))
        {
            return;
        }

        const std::string text = m_ring.text();
        if (m_ring.truncated()) {
            send_link(link::Message_type::STDERR_TRUNCATED, text);
        }
        else {
            const std::size_t offset = static_cast<std::size_t>(m_reported_stderr_bytes);
            send_link(link::Message_type::STDERR_CHUNK, std::string_view(text).substr(offset));
        }
        m_reported_stderr_bytes = total;
        m_last_stderr_publication = now;
        m_has_stderr_publication = true;
    }

    void write_stdin()
    {
        while (m_stdin.valid() && !m_stdin_queue.empty()) {
            const std::vector<std::uint8_t>& head = m_stdin_queue.front();
            process::Io_result written;
            if (m_stdin_ticket != 0) {
                written = m_stdin.complete_write(m_stdin_ticket);
                if (written.status != process::Io_status::WOULD_BLOCK) {
                    m_stdin_ticket = 0;
                }
            }
            else {
                const process::Write_submission_result submitted =
                    m_stdin.begin_write(head.data() + m_stdin_head_offset, head.size() - m_stdin_head_offset);
                if (submitted.status == process::Write_submission_status::PENDING) {
                    m_stdin_ticket = submitted.ticket;
                    written.status = process::Io_status::WOULD_BLOCK;
                }
                else
                if (submitted.status == process::Write_submission_status::BUSY) {
                    written.status = process::Io_status::WOULD_BLOCK;
                }
                else {
                    written.status = submitted.status == process::Write_submission_status::TRANSFERRED
                        ? process::Io_status::TRANSFERRED
                        : submitted.status == process::Write_submission_status::CLOSED
                            ? process::Io_status::CLOSED
                            : process::Io_status::FAILED;
                    written.bytes = submitted.bytes;
                    written.error = submitted.error;
                }
            }
            if (written.status == process::Io_status::WOULD_BLOCK) {
                return;
            }
            if (written.status != process::Io_status::TRANSFERRED) {
                // The adapter closed its stdin (or is gone): nothing more
                // can be delivered; the protocol layer decides what that means.
                m_stdin_queue.clear();
                m_stdin_queue_bytes = 0;
                m_stdin.close();
                return;
            }
            m_stdin_head_offset += written.bytes;
            m_stdin_queue_bytes -= written.bytes;
            if (m_stdin_head_offset == head.size()) {
                m_stdin_queue.pop_front();
                m_stdin_head_offset = 0;
            }
        }
    }

    // False once the link is gone.
    bool read_link(const process::Deadline& in_deadline =
        process::Deadline::after(std::chrono::milliseconds(k_stream_turn_ms)))
    {
        bool connected = true;
        logonomic::cli_addon_host::process::drain_stream_turn(m_link, in_deadline,
        [this, &connected](const process::Io_result& read, const std::uint8_t* chunk) {
            if (read.status == process::Io_status::WOULD_BLOCK) {
                return false;
            }
            if (read.status != process::Io_status::TRANSFERRED) {
                connected = false;
                return false;
            }
            const link::Frame_reader::Feed_status fed = m_reader.feed(chunk, read.bytes);
            if (fed != link::Frame_reader::Feed_status::OK) {
                // The desktop's stream is not decodable any more; protocol
                // I/O stops and the custodian ends the run.
                log_line(fed == link::Frame_reader::Feed_status::OVERSIZE_FRAME
                    ? "link frame exceeds max_link_frame_bytes; stopping protocol I/O"
                    : "link frame of unknown type; stopping protocol I/O");
                stop_protocol_io();
                return false;
            }
            while (const std::optional<link::Frame> frame = m_reader.next()) {
                dispatch_link_frame(*frame);
            }
            return true;
        });
        return connected;
    }

    void dispatch_link_frame(const link::Frame& in_frame)
    {
        switch (in_frame.type) {
            case link::Message_type::ADAPTER_FRAME:
                enqueue_stdin(in_frame.payload);
                return;
            default:
                log_line(std::string("desktop sent a supervisor-side message type ") +
                    link::message_type_name(in_frame.type) + "; ignored");
                return;
        }
    }

    void enqueue_stdin(const std::vector<std::uint8_t>& in_frame)
    {
        if (m_protocol_stopped || !m_stdin.valid()) {
            return;
        }
        const std::size_t bytes = in_frame.size() + 1;
        if (m_stdin_queue_bytes + bytes > m_request.max_stdin_queue_bytes) {
            stop_protocol_io();
            send_link(link::Message_type::VIOLATION, violation_json(reason::k_stdin_queue_overflow));
            return;
        }
        std::vector<std::uint8_t> line;
        line.reserve(bytes);
        line.insert(line.end(), in_frame.begin(), in_frame.end());
        line.push_back('\n');
        m_stdin_queue.push_back(std::move(line));
        m_stdin_queue_bytes += bytes;
        write_stdin();   // the pipe usually has room; otherwise the wait loop resumes it
    }

    // Protocol I/O stops: nothing more is written to the adapter and no
    // adapter frame is forwarded; stdout and stderr keep draining so the
    // adapter cannot block, and the custodian ends the run.
    void stop_protocol_io()
    {
        publish_stderr(true);
        m_protocol_stopped = true;
        m_stdin_queue.clear();
        m_stdin_queue_bytes = 0;
        m_stdin.close();
    }

    void send_link(link::Message_type in_type, std::string_view in_payload)
    {
        if (!m_link.valid()) {
            return;
        }
        std::vector<std::uint8_t> bytes = link::encode_frame(in_type, in_payload);
        if (m_link_queue_bytes + bytes.size() > k_max_outgoing_link_bytes) {
            log_line("bounded supervisor-link output queue overflow; closing link");
            m_link.close();
            m_link_queue.clear();
            m_link_queue_bytes = 0;
            return;
        }
        m_link_queue_bytes += bytes.size();
        m_link_queue.push_back(Pending_link{std::move(bytes), 0, 0});
        write_link();
    }

    bool write_link()
    {
        while (m_link.valid() && !m_link_queue.empty()) {
            Pending_link& head = m_link_queue.front();
            process::Io_result written;
            if (head.ticket != 0) {
                written = m_link.complete_write(head.ticket);
                if (written.status != process::Io_status::WOULD_BLOCK) {
                    head.ticket = 0;
                }
            }
            else {
                const process::Write_submission_result submitted =
                    m_link.begin_write(head.bytes.data() + head.offset, head.bytes.size() - head.offset);
                if (submitted.status == process::Write_submission_status::PENDING) {
                    head.ticket   = submitted.ticket;
                    written.status = process::Io_status::WOULD_BLOCK;
                }
                else
                if (submitted.status == process::Write_submission_status::BUSY) {
                    written.status = process::Io_status::WOULD_BLOCK;
                }
                else {
                    written.status = submitted.status == process::Write_submission_status::TRANSFERRED
                        ? process::Io_status::TRANSFERRED
                        : submitted.status == process::Write_submission_status::CLOSED
                            ? process::Io_status::CLOSED
                            : process::Io_status::FAILED;
                    written.bytes = submitted.bytes;
                    written.error = submitted.error;
                }
            }
            if (written.status == process::Io_status::WOULD_BLOCK) {
                return true;
            }
            if (written.status != process::Io_status::TRANSFERRED) {
                m_link.close();
                m_link_queue.clear();
                m_link_queue_bytes = 0;
                return false;
            }
            head.offset       += written.bytes;
            m_link_queue_bytes -= written.bytes;
            if (head.offset == head.bytes.size()) {
                m_link_queue.pop_front();
            }
        }
        return m_link.valid();
    }

    struct Pending_link
    {
        std::vector<std::uint8_t> bytes;
        std::size_t               offset = 0;
        process::Write_ticket     ticket = 0;
    };

    Options                              m_options;
    link::Supervisor_request             m_request;
    process::Stream_channel              m_link;
    process::Duplex_channel              m_status;
    process::Writable_pipe               m_stdin;
    process::Readable_pipe               m_stdout;
    process::Readable_pipe               m_stderr;
    logonomic::cli_addon_host::process::Stdout_frame_drain          m_drain;
    sdk::Stderr_ring                     m_ring;
    std::uint64_t                        m_reported_stderr_bytes = 0;
    std::chrono::steady_clock::time_point m_last_stderr_publication;
    bool                                 m_has_stderr_publication = false;
    link::Frame_reader                   m_reader;
    std::deque<Pending_link>             m_link_queue;
    std::uint64_t                        m_link_queue_bytes      = 0;
    std::deque<std::vector<std::uint8_t>> m_stdin_queue;
    std::size_t                          m_stdin_head_offset     = 0;
    process::Write_ticket                m_stdin_ticket          = 0;
    std::uint64_t                        m_stdin_queue_bytes     = 0;
    bool                                 m_protocol_stopped      = false;
    std::int64_t                         m_adapter_pid           = 0;
    process::Native_handle               m_adapter_handle        = process::k_invalid_handle;
    bool                                 m_adapter_exited        = false;
    int                                  m_adapter_exit_code     = -1;
    int                                  m_adapter_signal        = 0;
};

} // namespace

int run_supervisor(int argc, char** argv)
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
    link::Supervisor_request request;
    if (!read_request(options.request, &request, &error)) {
        // The custodian is waiting on the status channel; it learns why.
        process::Duplex_channel status(options.status, limits::k_max_control_message_bytes);
        link::Status_report report;
        report.failure = error;
        const std::string text = link::encode_status_report(report);
        (void)status.send(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
        log_line(error);
        return k_exit_failure;
    }
    Supervisor supervisor(std::move(options), std::move(request));
    return supervisor.run();
}

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv)
{
    std::vector<std::string> arguments;
    std::vector<char*>       pointers;
    arguments.reserve(static_cast<std::size_t>(argc));
    pointers.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        arguments.push_back(sdk::path_to_utf8(fs::path(argv[i])));
    }
    for (std::string& argument : arguments) {
        pointers.push_back(argument.data());
    }
    return run_supervisor(argc, pointers.data());
}
#else
int main(int argc, char** argv)
{
    return run_supervisor(argc, argv);
}
#endif
