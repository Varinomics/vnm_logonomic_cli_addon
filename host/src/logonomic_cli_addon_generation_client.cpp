#include "logonomic_cli_addon_generation_client.h"

#include "logonomic_cli_addon_custodian_protocol.h"
#include "logonomic_cli_addon_generation_custody.h"
#include "logonomic_cli_addon_usage_writer_custody.h"
#include "logonomic_cli_addon_json_bridge.h"
#include "logonomic_cli_addon_native_file_path.h"
#include "logonomic_cli_addon_path_entry.h"
#include <vnm_process_custody/process_channel.h>
#include <vnm_process_custody/process_clock.h>
#include <vnm_process_custody/process_pipe.h>
#include <vnm_process_custody/process_spawn.h>
#include <vnm_process_custody/process_tree.h>
#include "logonomic_cli_addon_supervisor_link.h"

#include "logonomic_cli_addon/frame.h"
#include "logonomic_cli_addon/host_simulator.h"
#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/json.h"
#include "logonomic_cli_addon/limits.h"
#include "logonomic_cli_addon/protocol.h"
#include "logonomic_cli_addon/version.h"

#include <vnm_qt_dispatch/vnm_qt_dispatch.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QPointer>
#include <QSet>
#include <QThread>
#include <QTimer>

#if defined(_WIN32)
#include <windows.h>
#else
#include <signal.h>
#include <sys/wait.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace logonomic::cli_addon_host {

namespace {

namespace fs       = std::filesystem;
namespace sdk      = logonomic::cli_addon;
namespace limits   = logonomic::cli_addon::limits;
namespace reason   = logonomic::cli_addon::reason;
namespace protocol = logonomic::cli_addon_host::custodian_protocol;
namespace link     = logonomic::cli_addon_host::link;
namespace process  = vnm::process_custody;

using State = Logonomic_cli_addon_activation_state;
using std::chrono::milliseconds;

#ifdef LOGONOMIC_ENABLE_CLI_PROVIDER_TEST_HOOKS
std::mutex            g_custody_start_barrier_mutex;
std::function<void()> g_custody_start_barrier;
std::mutex            g_scratch_creation_barrier_mutex;
std::function<void()> g_scratch_creation_barrier;
#endif

// Where the custodian finds its two inherited ends (descriptor numbers on
// Linux; Windows keeps the handle values and reads them from argv).
constexpr int k_control_child_number = 3;
constexpr int k_link_child_number    = 4;

constexpr std::size_t k_link_read_chunk_bytes = 64 * 1024;

// The supervisor owns the normative adapter-stdin queue and must be able to
// receive one complete frame past that bound so it can report the canonical
// stdin_queue_overflow violation. Each direction uses this retained-byte
// budget independently; the inbound mailbox also charges frame bookkeeping.
constexpr std::uint64_t k_max_link_queue_bytes =
    limits::k_max_stdin_queue_bytes + limits::k_max_link_frame_bytes;

// A failed worker-side start gives a custodian enough time to observe control
// EOF and clean up before the worker kills and reaps that direct child.
constexpr milliseconds k_failed_start_custodian_wait{
    limits::k_terminate_to_kill_ms + limits::k_reap_deadline_ms + 1000};

constexpr const char* k_host_product = "logonomic";

std::string to_std(const QString& text)
{
    return text.toStdString();
}

std::string to_std(const QByteArray& bytes)
{
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

std::string native_path(const QString& path)
{
    return to_std(QDir::toNativeSeparators(path));
}

fs::path filesystem_path(const QString& path)
{
#if defined(_WIN32)
    return fs::path(path.toStdWString());
#else
    const QByteArray encoded = QFile::encodeName(path);
    return fs::path(encoded.constData());
#endif
}

bool is_strict_descendant(const fs::path& in_root, const fs::path& in_path)
{
    const fs::path relative = in_path.lexically_normal().lexically_relative(
        in_root.lexically_normal());
    if (relative.empty() || relative.is_absolute()) {
        return false;
    }
    return std::none_of(relative.begin(), relative.end(), [](const fs::path& in_component) {
        return in_component == "..";
    });
}

bool real_directory_chain(
    const fs::path& in_root,
    const fs::path& in_path,
    QString*        out_error)
{
    const fs::path relative = in_path.lexically_normal().lexically_relative(
        in_root.lexically_normal());
    fs::path current = in_root.lexically_normal();
    const auto inspect = [out_error](const fs::path& in_entry) {
        std::error_code status_error;
        const auto io_path      = native_file_path(in_entry);
        const auto entry_status = fs::symlink_status(io_path, status_error);
        if (status_error) {
            *out_error = QStringLiteral("cannot inspect the created directory: %1")
                .arg(QString::fromStdString(status_error.message()));
            return false;
        }
        if (!fs::is_directory(entry_status) || path_entry_is_link(io_path, entry_status)) {
            *out_error = QStringLiteral("the created path is not a real directory");
            return false;
        }
        return true;
    };
    if (!inspect(current)) {
        return false;
    }
    for (const fs::path& component : relative) {
        current /= component;
        if (!inspect(current)) {
            return false;
        }
    }
    return true;
}

bool create_real_directory_below_root(
    const QString& in_root,
    const QString& in_directory,
    QString*       out_error)
{
    const fs::path root_path      = filesystem_path(in_root);
    const fs::path directory_path = filesystem_path(in_directory);
    if (!is_strict_descendant(root_path, directory_path)) {
        *out_error = QStringLiteral("the intended directory is outside its validated root");
        return false;
    }

    constexpr int k_create_attempts = 3;
    std::error_code create_error;
    QString         inspection_error;
    for (int attempt = 0; attempt < k_create_attempts; ++attempt) {
        create_error.clear();
        (void)fs::create_directories(native_file_path(directory_path), create_error);
        inspection_error.clear();
        if (real_directory_chain(root_path, directory_path, &inspection_error)) {
            return true;
        }
        if (!create_error) {
            *out_error = std::move(inspection_error);
            return false;
        }
        std::this_thread::yield();
    }
    *out_error = QStringLiteral("filesystem creation failed: %1")
        .arg(QString::fromStdString(create_error.message()));
    return false;
}

const char* default_reason_code(Retirement_reason_kind kind)
{
    switch (kind) {
        case Retirement_reason_kind::ADAPTER_EXITED:       return reason::k_adapter_exited;
        case Retirement_reason_kind::SUPERVISOR_LOST:      return reason::k_supervisor_lost;
        case Retirement_reason_kind::CUSTODIAN_LOST:       return reason::k_custodian_lost;
        case Retirement_reason_kind::PROTOCOL_VIOLATION:   return reason::k_frame_violation;
        case Retirement_reason_kind::ACTIVATION_TIMEOUT:   return reason::k_activation_timeout;
        case Retirement_reason_kind::CANCEL_GRACE_EXPIRED: return reason::k_cancel_grace_expired;
        case Retirement_reason_kind::SHUTDOWN:
        default:                                           return reason::k_shutdown;
    }
}

// A custodian error before `started` names what could not be started.
Retirement_reason_kind kind_for_custodian_error(const std::string& reason_code)
{
    if (reason_code == reason::k_supervisor_lost) {
        return Retirement_reason_kind::SUPERVISOR_LOST;
    }
    if (reason_code == reason::k_adapter_exited) {
        return Retirement_reason_kind::ADAPTER_EXITED;
    }
    return Retirement_reason_kind::CUSTODIAN_LOST;
}

std::string exit_description(int exit_code, int signal)
{
    return signal != 0
        ? "signal=" + std::to_string(signal)
        : "exit_code=" + std::to_string(exit_code);
}

struct Custodian_exit
{
    int exit_code = -1;
    int signal    = 0;
};

std::uintptr_t native_reference(process::Native_handle handle)
{
#if defined(_WIN32)
    return reinterpret_cast<std::uintptr_t>(handle);
#else
    return static_cast<std::uintptr_t>(handle);
#endif
}

// Reaps the desktop's direct custodian child if it has ended. Only this
// process's own child is ever waited; provider descendants belong to the
// custodian and, in the runtime owner after its loss, to the native family's
// adopted-descendant reaper. This waiter never consumes descendant results.
bool try_reap_custodian(std::int64_t pid, process::Native_handle handle, bool block, Custodian_exit* out_exit)
{
#if defined(_WIN32)
    if (::WaitForSingleObject(handle, block ? INFINITE : 0) != WAIT_OBJECT_0) {
        return false;
    }
    DWORD code = 0;
    if (!::GetExitCodeProcess(handle, &code)) {
        return false;
    }
    out_exit->exit_code = static_cast<int>(code);
    out_exit->signal    = 0;
    (void)pid;
    return true;
#else
    (void)handle;
    int status = 0;
    const pid_t waited = ::waitpid(static_cast<pid_t>(pid), &status, block ? 0 : WNOHANG);
    if (waited != static_cast<pid_t>(pid)) {
        return false;
    }
    if (WIFEXITED(status)) {
        out_exit->exit_code = WEXITSTATUS(status);
        out_exit->signal    = 0;
    }
    else {
        out_exit->exit_code = -1;
        out_exit->signal    = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
    }
    return true;
#endif
}

void kill_custodian(std::int64_t pid, process::Native_handle handle)
{
#if defined(_WIN32)
    (void)pid;
    ::TerminateProcess(handle, 1);
#else
    (void)handle;
    (void)::kill(static_cast<pid_t>(pid), SIGKILL);
#endif
}

// Bytes the broker queues for the reader thread, which is the only user of
// the channels once it runs.
struct Outgoing
{
    enum class Kind
    {
        LINK,
        CONTROL,
        CLOSE_CONTROL,
    };

    Kind                      kind = Kind::LINK;
    std::vector<std::uint8_t> bytes;
    std::size_t               offset = 0;
    process::Write_ticket     ticket = 0;
};

std::optional<sdk::Json_value> parse_link_payload(const link::Frame& frame)
{
    return sdk::parse_json(
        std::string_view(reinterpret_cast<const char*>(frame.payload.data()), frame.payload.size()),
        sdk::Json_parse_limits{},
        nullptr);
}

std::optional<std::int64_t> integer_member(const std::optional<sdk::Json_value>& object, const char* name)
{
    if (!object || !object->is_object()) {
        return std::nullopt;
    }
    const sdk::Json_value* value = object->find(name);
    if (!value || !value->is_integer()) {
        return std::nullopt;
    }
    return value->as_integer();
}

std::string string_member(const std::optional<sdk::Json_value>& object, const char* name)
{
    if (!object || !object->is_object()) {
        return {};
    }
    const sdk::Json_value* value = object->find(name);
    return value && value->is_string() ? value->as_string() : std::string();
}

} // namespace

// ---------------------------------------------------------------------------

struct Generation_client::Impl
{
    Impl(Generation_client* in_owner, Activation_context in_context);

    // -- broker-thread state --------------------------------------------------
    Generation_client* owner;
    Activation_context context;
    State              state = State::NOT_STARTED;
    QString            retirement_reason;
    // The generation's scratch directory and the run lock that keeps a
    // concurrent discovery's stale recovery away from it while it lives.
    QString                   scratch_directory;
    QString                   scratch_lock_path;
    std::unique_ptr<Run_lock> scratch_lock;
    bool                      scratch_path_owned = false;
#ifdef LOGONOMIC_ENABLE_CLI_PROVIDER_TEST_HOOKS
    std::optional<bool> observed_native_cleanup_result;
#if defined(__APPLE__)
    std::function<void(int, std::optional<bool>)> control_write_probe;
#endif
#endif
    std::string        package_id;
    std::string        digest;
    std::string        nonce;

    // Process ends. Created and owned by the worker thread; the broker only
    // appends logical output and observes the published pid.
    process::Duplex_channel control;
    process::Stream_channel link;
    process::Readable_pipe  wake_read;
    process::Writable_pipe  wake_write;
    process::Write_ticket   wake_ticket = 0;
    std::mutex              wake_mutex;
    bool                    wake_ready = false;
    std::atomic<std::int64_t> custodian_pid{0};
    process::Native_handle  custodian_handle = process::k_invalid_handle;
    std::unique_ptr<Native_child_custody> custodian_membership;
    bool custodian_membership_published = false;

    // The reader thread and what the two threads share.
    std::thread          reader;
    std::atomic_bool     custodian_reaped{false};
    Custodian_exit       observed_custodian_exit;
    bool                start_request_attempted = false;
    std::atomic_bool     worker_finished{false};
    std::atomic_bool     custody_stop_requested{false};
    std::mutex           outgoing_mutex;
    std::deque<Outgoing> outgoing;
    std::uint64_t        outgoing_link_bytes = 0;
    std::atomic_bool     discard_link_output{false};
    // One bounded mailbox, not one owning Qt event per link frame. An active
    // GUI drain keeps its frame charged until processing has completed.
    std::mutex              inbound_mutex;
    std::deque<link::Frame> inbound;
    // The worker produces only a finite set of custody facts. Deliver them
    // after retained frames so exit/finished cannot overtake a queued result.
    std::deque<std::function<void()>> inbound_custody_events;
    std::uint64_t           inbound_bytes = 0;
    bool                    inbound_notification_scheduled = false;
    bool                    inbound_overflowed = false;
    bool                    inbound_overflow_pending = false;
    sdk::Stderr_ring         inbound_stderr;
    std::uint64_t           inbound_stderr_published_bytes = 0;
    bool                    inbound_stderr_pending = false;
#ifdef LOGONOMIC_ENABLE_CLI_PROVIDER_TEST_HOOKS
    Generation_client::Inbound_mailbox_snapshot inbound_snapshot;
    bool reject_next_link_enqueue = false; // broker-thread test seam
    std::atomic_bool native_cleanup_observation_withheld{false};
    std::atomic<int> native_containment_at_birth{-1};
    std::size_t inbound_pending_notifications = 0;
#endif
    // Recorded coordinates with their handles; written by the reader thread
    // and read only after join.
    process::Kill_coordinates coordinates;
    process::Process_tree_containment generation_containment;

    // Protocol (broker thread).
    std::optional<sdk::Session_tracker> tracker;
    sdk::Json_parse_limits              parse_limits;
    sdk::Protocol_limits                effective_limits;
    bool                                started_received = false;
    std::int64_t                        supervisor_pid   = 0;
    std::int64_t                        adapter_pid      = 0;
    std::int64_t                        group            = 0;
    QTimer                              activation_deadline;
    QTimer                              hello_deadline;
    QTimer                              shutdown_grace;
    bool                                shutdown_requested      = false;
    bool                                idle_shutdown_delivered = false;
    bool                                idle_force_requested    = false;
    bool                                idle_force_confirmed    = false;
    bool                                orderly_adapter_exit    = false;
    bool                                orderly_supervisor_exit = false;
    Idle_stop_outcome                   idle_outcome            = Idle_stop_outcome::NONE;
    bool                                adapter_exited        = false;
    bool                                adapter_exit_recorded = false;
    bool                                stop_sent             = false;
    bool                                finished_emitted      = false;
    unsigned                            next_request          = 1;
    QHash<QByteArray, std::string>      wire_ids;       // correlation id -> wire request id
    QHash<QByteArray, QByteArray>       correlations;   // wire request id -> correlation id
    QSet<QByteArray>                    cancelled;      // correlation ids a cancel was sent for
    QList<Declared_capability>          live;
    sdk::Transcript_recorder*           recorder = nullptr;
    sdk::Transcript_placeholders        placeholders;

    bool is_live() const { return state != State::NOT_STARTED && state != State::TERMINALLY_UNAVAILABLE; }

    // -- recording ------------------------------------------------------------
    void record(const char* kind, const std::string& detail = {});
    void record_host_frame(const sdk::Json_value& message);

    // -- sending (broker thread -> reader thread) -----------------------------
    bool enqueue(Outgoing::Kind kind, std::vector<std::uint8_t> bytes);
    void wake();
    bool send_link_frame(const sdk::Json_value& message);
    bool send_link_frame(const sdk::Json_value& message, std::string_view body,
        link::Message_type type = link::Message_type::ADAPTER_FRAME);
    void send_control(const protocol::Message& message);
    void end_process_tree(bool kill_now);

    // -- transitions ----------------------------------------------------------
    void become_ready();
    void retire_internal(Retirement_reason_kind kind, std::string code, std::string detail, bool record_retired_line);
    void send_shutdown();
    void note_adapter_exit(int exit_code, int signal);
    QList<Declared_capability> compute_live() const;

    // -- events from the reader thread ------------------------------------------
    void on_control_message(protocol::Message message);
    void on_control_closed(bool clean);
    void on_custodian_exited(Custodian_exit exit, bool scope_confirmed, bool cleanup_complete);
    void on_link_frame(link::Frame frame);
    void on_link_closed();
    bool admit_inbound(link::Frame frame);
    void post_inbound_notification();
    void drain_inbound();
    void drain_inbound_stderr();
    void report_inbound_overflow();

    // -- the reader thread -------------------------------------------------------
    bool initialize_custody(std::string* out_reason, std::string* out_detail);
    bool cleanup_worker_state(bool cleanup_complete);
    void run_reader();
    template <typename Handler>
    void post(Handler&& handler);
};

Generation_client::Impl::Impl(Generation_client* in_owner, Activation_context in_context)
:
    owner(in_owner),
    context(std::move(in_context)),
    package_id(to_std(context.record.package_id)),
    digest(to_std(context.record.generation_digest_hex)),
    nonce(to_std(context.activation_nonce.isEmpty() ? context.record.generation_nonce_hex : context.activation_nonce)),
    activation_deadline(in_owner),
    hello_deadline(in_owner),
    shutdown_grace(in_owner)
{
    for (QTimer* timer : {&activation_deadline, &hello_deadline, &shutdown_grace}) {
        timer->setSingleShot(true);
        timer->setTimerType(Qt::PreciseTimer);
    }
}

template <typename Handler>
void Generation_client::Impl::post(Handler&& handler)
{
    bool notify = false;
    {
        const std::lock_guard lock(inbound_mutex);
        inbound_custody_events.emplace_back(std::forward<Handler>(handler));
        if (!inbound_notification_scheduled) {
            inbound_notification_scheduled = true;
            notify = true;
        }
    }
    if (notify) {
        post_inbound_notification();
    }
}

bool Generation_client::Impl::admit_inbound(link::Frame frame)
{
    bool notify = false;
    bool admitted = true;
    {
        const std::lock_guard lock(inbound_mutex);
        if (frame.type == link::Message_type::STDERR_CHUNK ||
            frame.type == link::Message_type::STDERR_TRUNCATED)
        {
            if (frame.type == link::Message_type::STDERR_TRUNCATED) {
                inbound_stderr = sdk::Stderr_ring();
            }
            inbound_stderr.append(std::string_view(
                reinterpret_cast<const char*>(frame.payload.data()), frame.payload.size()));
            inbound_stderr_pending = true;
        }
        else {
            const std::uint64_t charge = sizeof(link::Frame) + frame.payload.capacity();
            if (inbound_overflowed) {
                return false;
            }
            if (charge > k_max_link_queue_bytes - inbound_bytes) {
                inbound_overflowed = true;
                inbound_overflow_pending = true;
                admitted = false;
                // A frame currently being processed by the GUI remains
                // charged; only queued frames are released here.
                for (const link::Frame& queued : inbound) {
                    inbound_bytes -= sizeof(link::Frame) + queued.payload.capacity();
                }
                inbound.clear();
            }
            else {
                inbound_bytes += charge;
                inbound.push_back(std::move(frame));
#ifdef LOGONOMIC_ENABLE_CLI_PROVIDER_TEST_HOOKS
                inbound_snapshot.peak_retained_bytes =
                    std::max(inbound_snapshot.peak_retained_bytes, inbound_bytes);
#endif
            }
        }
        if (!inbound_notification_scheduled) {
            inbound_notification_scheduled = true;
            notify = true;
        }
    }
    if (notify) {
        post_inbound_notification();
    }
    return admitted;
}

void Generation_client::Impl::post_inbound_notification()
{
#ifdef LOGONOMIC_ENABLE_CLI_PROVIDER_TEST_HOOKS
    {
        const std::lock_guard lock(inbound_mutex);
        ++inbound_pending_notifications;
        inbound_snapshot.peak_pending_notifications = std::max(
            inbound_snapshot.peak_pending_notifications, inbound_pending_notifications);
    }
#endif
    // The proxy retains this client until finished(), and finished follows
    // every queued frame and custody event. Qt drops calls on destruction.
    (void)vnm::qt::post(owner, [this]() {
        const QPointer<Generation_client> guard(owner);
        report_inbound_overflow();
        if (guard) {
            drain_inbound();
        }
    });
}

void Generation_client::Impl::report_inbound_overflow()
{
    {
        const std::lock_guard lock(inbound_mutex);
        if (!inbound_overflow_pending) {
            return;
        }
        inbound_overflow_pending = false;
    }
    // Physical stop belongs to the worker; the GUI publishes its outcome
    // without sending a second stop or depending on a GUI deadline.
    stop_sent = true;
    if (is_live()) {
        record("killed");
        retire_internal(
            Retirement_reason_kind::PROTOCOL_VIOLATION,
            reason::k_inbound_queue_overflow,
            "the bounded host input mailbox could not retain another adapter frame",
            true);
    }
}

void Generation_client::Impl::drain_inbound_stderr()
{
    std::optional<link::Frame> diagnostic;
    {
        const std::lock_guard lock(inbound_mutex);
        if (inbound_stderr_pending) {
            const std::string text = inbound_stderr.text();
            const bool truncated = inbound_stderr.truncated();
            const std::size_t offset = truncated
                ? 0 : static_cast<std::size_t>(inbound_stderr_published_bytes);
            diagnostic.emplace();
            diagnostic->type = truncated
                ? link::Message_type::STDERR_TRUNCATED : link::Message_type::STDERR_CHUNK;
            diagnostic->payload.assign(text.begin() + static_cast<std::ptrdiff_t>(offset), text.end());
            inbound_stderr_published_bytes = inbound_stderr.total_bytes();
            inbound_stderr_pending = false;
        }
    }
    if (diagnostic) {
        on_link_frame(std::move(*diagnostic));
    }
}

void Generation_client::Impl::drain_inbound()
{
    const QPointer<Generation_client> guard(owner);
#ifdef LOGONOMIC_ENABLE_CLI_PROVIDER_TEST_HOOKS
    {
        const std::lock_guard lock(inbound_mutex);
        --inbound_pending_notifications;
    }
#endif
    drain_inbound_stderr();
    if (!guard) {
        return;
    }
    std::uint64_t processed_bytes = 0;
    while (processed_bytes < k_link_read_chunk_bytes) {
        std::optional<link::Frame> frame;
        std::uint64_t charge = 0;
        {
            const std::lock_guard lock(inbound_mutex);
            if (inbound.empty()) {
                break;
            }
            frame.emplace(std::move(inbound.front()));
            inbound.pop_front();
            charge = sizeof(link::Frame) + frame->payload.capacity();
        }
        drain_inbound_stderr();
        if (!guard) {
            return;
        }
        on_link_frame(std::move(*frame));
        if (!guard) {
            return;
        }
        processed_bytes += charge;
        {
            const std::lock_guard lock(inbound_mutex);
            inbound_bytes -= charge;
        }
        report_inbound_overflow();
        if (!guard) {
            return;
        }
    }
    while (true) {
        std::function<void()> event;
        {
            const std::lock_guard lock(inbound_mutex);
            if (!inbound.empty() || inbound_stderr_pending || inbound_custody_events.empty()) {
                break;
            }
            event = std::move(inbound_custody_events.front());
            inbound_custody_events.pop_front();
        }
        report_inbound_overflow();
        if (!guard) {
            return;
        }
        event();
        if (!guard) {
            return;
        }
    }
    bool notify;
    {
        const std::lock_guard lock(inbound_mutex);
        notify = !inbound.empty() || !inbound_custody_events.empty() ||
            inbound_stderr_pending || inbound_overflow_pending;
        if (!notify) {
            inbound_notification_scheduled = false;
        }
    }
    if (notify) {
        post_inbound_notification();
    }
}

// ---------------------------------------------------------------------------
// Recording

void Generation_client::Impl::record(const char* kind, const std::string& detail)
{
    if (recorder) {
        recorder->record_event(kind, detail);
    }
}

void Generation_client::Impl::record_host_frame(const sdk::Json_value& message)
{
    if (recorder) {
        recorder->record_host_frame(message);
    }
}

// ---------------------------------------------------------------------------
// Sending

bool Generation_client::Impl::enqueue(Outgoing::Kind kind, std::vector<std::uint8_t> bytes)
{
#ifdef LOGONOMIC_ENABLE_CLI_PROVIDER_TEST_HOOKS
    if (kind == Outgoing::Kind::LINK && std::exchange(reject_next_link_enqueue, false)) {
        return false;
    }
#endif
    {
        const std::lock_guard lock(outgoing_mutex);
        if (kind == Outgoing::Kind::LINK &&
            outgoing_link_bytes + bytes.size() > k_max_link_queue_bytes)
        {
            return false;
        }
        if (kind == Outgoing::Kind::LINK) {
            outgoing_link_bytes += bytes.size();
        }
        outgoing.push_back(Outgoing{kind, std::move(bytes), 0, 0});
    }
    wake();
    return true;
}

void Generation_client::Impl::wake()
{
    const std::lock_guard lock(wake_mutex);
    if (!wake_ready) {
        return;
    }
    if (wake_ticket != 0) {
        const process::Io_result completed = wake_write.complete_write(wake_ticket);
        if (completed.status == process::Io_status::WOULD_BLOCK) {
            return;
        }
        wake_ticket = 0;
    }
    const std::uint8_t byte = 1;
    const process::Write_submission_result submitted = wake_write.begin_write(&byte, 1);
    if (submitted.status == process::Write_submission_status::PENDING) {
        wake_ticket = submitted.ticket;
    }
}

bool Generation_client::Impl::send_link_frame(const sdk::Json_value& message)
{
    return send_link_frame(message, sdk::serialize_json(message));
}

bool Generation_client::Impl::send_link_frame(
    const sdk::Json_value& message,
    std::string_view       body,
    link::Message_type     type)
{
    record_host_frame(message);
    return enqueue(
        Outgoing::Kind::LINK,
        link::encode_frame(type, body));
}

void Generation_client::Impl::send_control(const protocol::Message& message)
{
    const std::string encoded = protocol::encode(message);
    (void)enqueue(Outgoing::Kind::CONTROL, std::vector<std::uint8_t>(encoded.begin(), encoded.end()));
}

// Ends the generation's process tree once: an immediate kill for a live
// adapter that is being retired, a terminate-then-kill when nothing live is
// expected, and control-channel EOF for a custodian that never reported
// `started` (it is not reading control yet; EOF is what it acts on).
void Generation_client::Impl::end_process_tree(bool kill_now)
{
    if (stop_sent) {
        return;
    }
    stop_sent = true;
#if defined(__APPLE__)
    if (adapter_exited) {
        // The Darwin native owner already began settlement at root exit. Its
        // custodian drains the relay and sends Reaped without waiting for Stop.
        return;
    }
#endif
    custody_stop_requested.store(true);
    discard_link_output.store(true);
    wake();
    if (custodian_pid.load() == 0) {
        return;
    }
    if (!started_received) {
        (void)enqueue(Outgoing::Kind::CLOSE_CONTROL, {});
        return;
    }
    protocol::Stop_request stop;
    stop.grace_ms = kill_now ? 0 : limits::k_terminate_to_kill_ms;
    send_control(protocol::Message(stop));
    if (kill_now) {
        record("killed");
    }
}

// ---------------------------------------------------------------------------
// Transitions

QList<Declared_capability> Generation_client::Impl::compute_live() const
{
    QList<Declared_capability> result;
    if (!tracker) {
        return result;
    }
    for (const Declared_capability& declared : context.record.declared_capabilities) {
        const sdk::Capability_pair pair{to_std(declared.capability_id), static_cast<std::uint16_t>(declared.version)};
        if (!tracker->handshake_declares(pair)) {
            continue;
        }
        const bool adopted = std::any_of(
            context.adopted_capabilities.cbegin(),
            context.adopted_capabilities.cend(),
            [&declared](const Declared_capability& candidate) {
                return candidate.version == declared.version && candidate.capability_id == declared.capability_id;
            });
        if (adopted) {
            result.push_back(declared);
        }
    }
    return result;
}

void Generation_client::Impl::become_ready()
{
    live  = compute_live();
    state = State::READY;
    activation_deadline.stop();
    hello_deadline.stop();
    emit owner->ready();
}

void Generation_client::Impl::retire_internal(
    Retirement_reason_kind kind,
    std::string            code,
    std::string            detail,
    bool                   record_retired_line)
{
    if (!is_live()) {
        return;
    }
    state             = State::TERMINALLY_UNAVAILABLE;
    retirement_reason = QString::fromStdString(code);
    activation_deadline.stop();
    hello_deadline.stop();
    shutdown_grace.stop();
    if (record_retired_line) {
        record("retired", code);
    }
    end_process_tree(!adapter_exited);
    emit owner->retired(kind, retirement_reason, QString::fromStdString(detail));
}

void Generation_client::Impl::note_adapter_exit(int exit_code, int signal)
{
    if (!adapter_exit_recorded) {
        adapter_exit_recorded = true;
        record("adapter_exited", exit_description(exit_code, signal));
    }
    if (adapter_exited) {
        return;
    }
    adapter_exited = true;
    if (!is_live()) {
        return;
    }
    if (shutdown_requested) {
        retire_internal(Retirement_reason_kind::SHUTDOWN, reason::k_shutdown,
            "the adapter exited after shutdown", false);
        return;
    }
    if (state == State::IDLE_STOPPING) {
        orderly_adapter_exit = exit_code == 0 && signal == 0;
        if (idle_shutdown_delivered && (orderly_adapter_exit || idle_force_requested)) {
            // The ordered exit follows the supervisor's final adapter frames.
            // A nonzero status still needs the native owner's force receipt.
            shutdown_grace.stop();
            if (orderly_supervisor_exit) {
                end_process_tree(false);
            }
            return;
        }
    }
    retire_internal(
        Retirement_reason_kind::ADAPTER_EXITED,
        reason::k_adapter_exited,
        "the adapter exited (" + exit_description(exit_code, signal) + ")",
        true);
}

// ---------------------------------------------------------------------------
// Events from the reader thread (running on the broker thread)

void Generation_client::Impl::on_control_message(protocol::Message message)
{
    if (const protocol::Started* started = std::get_if<protocol::Started>(&message)) {
        if (state != State::STARTING || started_received) {
            return;
        }
#if defined(__APPLE__)
        if (!started->darwin_custody || started->darwin_custody->version != 1 ||
            started->darwin_custody->root_birth_token.empty())
        {
            retire_internal(Retirement_reason_kind::CUSTODIAN_LOST, reason::k_custodian_lost,
                "Darwin native custody capability is unavailable", true);
            return;
        }
#endif
        started_received = true;
        supervisor_pid   = started->supervisor_pid;
        adapter_pid      = started->adapter_pid;
        group            = started->process_group;

        sdk::Host_hello hello;
        hello.core_protocol_generations.assign(
            sdk::k_supported_core_protocol_generations.begin(),
            sdk::k_supported_core_protocol_generations.end());
        hello.package_id        = package_id;
        hello.generation_digest = digest;
        hello.generation_nonce  = nonce;
        hello.limits            = sdk::Protocol_limits{};
        hello.host_product      = k_host_product;
        const QString version   = QCoreApplication::applicationVersion();
        hello.host_version      = version.isEmpty() ? std::string("unversioned") : to_std(version);
        tracker.emplace(hello);
        effective_limits       = hello.limits;
        parse_limits.max_bytes = effective_limits.max_frame_bytes;
        parse_limits.max_depth = static_cast<std::size_t>(effective_limits.max_json_depth);
        if (!send_link_frame(sdk::encode_host_message(sdk::Host_message(hello)))) {
            retire_internal(
                Retirement_reason_kind::PROTOCOL_VIOLATION,
                reason::k_stdin_queue_overflow,
                "the bounded supervisor-link queue could not admit host hello",
                true);
            return;
        }
        hello_deadline.start(limits::k_hello_deadline_ms);
        return;
    }
    if (const protocol::Exited* exited = std::get_if<protocol::Exited>(&message)) {
        if (state == State::IDLE_STOPPING) {
            // Reaping races relay delivery. The supervisor must exit on its
            // own after draining the adapter, before we terminate descendants.
            if (exited->who == "supervisor") {
                if (exited->exit_code != 0 || exited->signal != 0) {
                    retire_internal(Retirement_reason_kind::SUPERVISOR_LOST, reason::k_supervisor_lost,
                        "the supervisor ended (" + exit_description(exited->exit_code, exited->signal) + ")", true);
                }
                else {
                    orderly_supervisor_exit = true;
                    if (adapter_exited) {
                        end_process_tree(false);
                    }
                }
            }
            return;
        }
        if (exited->who == "adapter") {
            note_adapter_exit(exited->exit_code, exited->signal);
        }
        // Darwin's ordered relay can carry the final adapter status after
        // the native supervisor reap reached the control channel.
#if !defined(__APPLE__)
        else
        if (is_live() && !adapter_exited) {
            retire_internal(
                Retirement_reason_kind::SUPERVISOR_LOST,
                reason::k_supervisor_lost,
                "the supervisor ended (" + exit_description(exited->exit_code, exited->signal) + ")",
                true);
        }
#endif
        return;
    }
    if (const protocol::Error* error = std::get_if<protocol::Error>(&message)) {
        if ((!started_received && is_live()) || state == State::IDLE_STOPPING) {
            retire_internal(kind_for_custodian_error(error->reason), error->reason, error->detail, true);
            return;
        }
        emit owner->custodian_error(QString::fromStdString(error->reason), QString::fromStdString(error->detail));
        return;
    }
    if (const auto* reaped = std::get_if<protocol::Reaped>(&message)) {
        idle_force_confirmed = reaped->idle_adapter_forced;
    }
}

void Generation_client::Impl::on_control_closed(bool clean)
{
    if (clean || !is_live()) {
        return;
    }
    // The reader thread signals the recorded processes; there is no
    // custodian left to send a stop to.
    stop_sent = true;
    retire_internal(
        Retirement_reason_kind::CUSTODIAN_LOST,
        reason::k_custodian_lost,
        "the custodian ended without reaping its tree",
        true);
}

void Generation_client::Impl::on_custodian_exited(Custodian_exit exit, bool scope_confirmed, bool cleanup_complete)
{
    const QPointer<Generation_client> guard(owner);
    stop_sent = true;
#if defined(__APPLE__)
    constexpr int normal_exit_code = 0;
#else
    constexpr int normal_exit_code = protocol::k_custody_clean_exit_code;
#endif
#ifdef LOGONOMIC_ENABLE_CLI_PROVIDER_TEST_HOOKS
    observed_native_cleanup_result = cleanup_complete;
#endif
    if (state == State::IDLE_STOPPING && exit.signal == 0 && exit.exit_code == normal_exit_code &&
        adapter_exited && orderly_supervisor_exit)
    {
        // The reader has finished its native observations. A final denial
        // retires admission while the missing proof keeps custody retained.
        if (!scope_confirmed || !cleanup_complete) {
            retire_internal(Retirement_reason_kind::CUSTODIAN_LOST, reason::k_custodian_lost,
                "the custodian ended without confirming idle cleanup", true);
        }
        else
        if (orderly_adapter_exit || (idle_force_requested && idle_force_confirmed)) {
            idle_outcome = orderly_adapter_exit ? Idle_stop_outcome::ORDERLY : Idle_stop_outcome::FORCED;
            retire_internal(Retirement_reason_kind::IDLE_STOP, reason::k_shutdown,
                "the idle adapter and its native scope settled", false);
        }
        else {
            retire_internal(Retirement_reason_kind::ADAPTER_EXITED, reason::k_adapter_exited,
                "the adapter exited abnormally during idle shutdown", true);
        }
    }
    else
    if (is_live()) {
        retire_internal(
            Retirement_reason_kind::CUSTODIAN_LOST,
            reason::k_custodian_lost,
            "the custodian ended (" + exit_description(exit.exit_code, exit.signal) + ")",
            true);
    }
    if (guard && cleanup_complete && !finished_emitted) {
        finished_emitted = true;
        emit owner->finished();
    }
}

void Generation_client::Impl::on_link_frame(link::Frame frame)
{
    switch (frame.type) {
        case link::Message_type::IDLE_SHUTDOWN:
            if (state == State::IDLE_STOPPING && !idle_shutdown_delivered && frame.payload.empty()) {
                // Start the adapter's full grace after its stdin accepted the
                // shutdown. An unsolicited exit already in transit is a fault.
                idle_shutdown_delivered = true;
                shutdown_grace.start(limits::k_shutdown_grace_ms);
            }
            return;
        case link::Message_type::ADAPTER_FRAME: {
            if (!is_live()) {
                return;   // a frame that raced the retirement; nothing after it is interpreted
            }
            const std::string_view text(reinterpret_cast<const char*>(frame.payload.data()), frame.payload.size());
            sdk::Contract_failure failure;
            const std::optional<sdk::Adapter_message> message = sdk::decode_adapter_message(text, parse_limits, &failure);
            if (!message) {
                record("violation", failure.reason_code);
                retire_internal(Retirement_reason_kind::PROTOCOL_VIOLATION, failure.reason_code, failure.detail, true);
                return;
            }
            if (recorder) {
                recorder->record_adapter_frame(sdk::encode_adapter_message(*message));
            }
            if (!tracker) {
                failure.reason_code = reason::k_out_of_order_message;
                failure.detail      = "adapter frame before the host hello";
            }
            if (!tracker || !tracker->accept(*message, &failure)) {
                record("violation", failure.reason_code);
                retire_internal(Retirement_reason_kind::PROTOCOL_VIOLATION, failure.reason_code, failure.detail, true);
                return;
            }
            if (std::get_if<sdk::Adapter_hello>(&*message)) {
                effective_limits       = tracker->effective_limits();
                parse_limits.max_bytes = effective_limits.max_frame_bytes;
                parse_limits.max_depth = static_cast<std::size_t>(effective_limits.max_json_depth);
                return;
            }
            if (std::get_if<sdk::Capabilities_message>(&*message)) {
                if (tracker->ready() && state == State::STARTING) {
                    become_ready();
                }
                return;
            }
            if (const sdk::Progress_message* progress = std::get_if<sdk::Progress_message>(&*message)) {
                const QByteArray correlation = correlations.value(QByteArray::fromStdString(progress->request_id));
                if (correlation.isEmpty()) {
                    return;
                }
                Logonomic_cli_capability_progress report;
                report.completed = progress->completed;
                if (progress->total) {
                    report.total = *progress->total;
                }
                report.note = QString::fromStdString(progress->note);
                emit owner->progress_received(correlation, report);
                return;
            }
            if (const sdk::Result_message* result = std::get_if<sdk::Result_message>(&*message)) {
                const QByteArray wire_id     = QByteArray::fromStdString(result->request_id);
                const QByteArray correlation = correlations.take(wire_id);
                if (correlation.isEmpty()) {
                    return;
                }
                wire_ids.remove(correlation);
                cancelled.remove(correlation);
                emit owner->result_received(
                    correlation,
                    QString::fromLatin1(sdk::result_status_name(result->status)),
                    QString::fromStdString(result->reason),
                    QString::fromStdString(result->message),
                    logonomic_cli_addon_to_qjson(result->payload).toObject());
            }
            return;
        }
        case link::Message_type::STDERR_CHUNK:
            emit owner->stderr_diagnostic(
                QByteArray(reinterpret_cast<const char*>(frame.payload.data()), static_cast<qsizetype>(frame.payload.size())),
                false);
            return;
        case link::Message_type::STDERR_TRUNCATED:
            record("stderr_truncated");
            emit owner->stderr_diagnostic(
                QByteArray(reinterpret_cast<const char*>(frame.payload.data()), static_cast<qsizetype>(frame.payload.size())),
                true);
            return;
        case link::Message_type::VIOLATION: {
            std::string code = string_member(parse_link_payload(frame), "reason");
            if (code.empty()) {
                code = reason::k_frame_violation;
            }
            if (!is_live()) {
                return;
            }
            record("violation", code);
            retire_internal(Retirement_reason_kind::PROTOCOL_VIOLATION, code, "the supervisor stopped protocol I/O", true);
            return;
        }
        case link::Message_type::ADAPTER_EXITED: {
            const std::optional<sdk::Json_value> payload = parse_link_payload(frame);
            const auto code = integer_member(payload, "exit_code");
            const auto signal = integer_member(payload, "signal");
            if (!code || !signal || !std::in_range<int>(*code) || !std::in_range<int>(*signal)) {
                retire_internal(Retirement_reason_kind::PROTOCOL_VIOLATION, reason::k_frame_violation,
                    "the supervisor sent an invalid adapter exit status", true);
                return;
            }
            note_adapter_exit(static_cast<int>(*code), static_cast<int>(*signal));
            return;
        }
        default:
            return;
    }
}

void Generation_client::Impl::on_link_closed()
{
    if (is_live() && !adapter_exited) {
        retire_internal(
            Retirement_reason_kind::SUPERVISOR_LOST,
            reason::k_supervisor_lost,
            "the supervisor link closed",
            true);
    }
}

// ---------------------------------------------------------------------------
// The reader thread

bool Generation_client::Impl::initialize_custody(
    std::string* out_reason,
    std::string* out_detail)
{
    // Derive and cap every path that will enter the private start and
    // supervisor envelopes before preparing process state, creating scratch
    // directories, or acquiring their run lock. The custodian repeats these
    // checks as the trust boundary; this producer preflight prevents an
    // otherwise-doomed activation from leaving local scratch side effects.
    const Generation_record& record = context.record;
    const QString scratch_family = QDir(context.roots.scratch_root).filePath(record.package_id);
    const QString prepared_scratch_directory = QDir(scratch_family).filePath(
        QString::fromLatin1(nonce));
    const QString expected_generation_directory = QDir(
        QDir(context.roots.custody_root).filePath(record.package_id)).filePath(
            QString::fromLatin1(record.generation_nonce_hex));
    const QString expected_adapter_executable = QDir(expected_generation_directory).filePath(
        record.adapter_relative_path);
    const QString recorded_adapter_executable = QDir(record.custody_path).filePath(record.adapter_relative_path);

    const std::string custody_root_native = native_path(context.roots.custody_root);
    const std::string scratch_directory_native = native_path(prepared_scratch_directory);
    const std::string supervisor_executable_native = native_path(context.supervisor_executable);
    const std::string expected_generation_native = native_path(expected_generation_directory);
    const std::string recorded_generation_native = native_path(record.custody_path);
    const std::string expected_adapter_native = native_path(expected_adapter_executable);
    const std::string recorded_adapter_native = native_path(recorded_adapter_executable);
    if (!sdk::is_valid_generation_nonce_hex(nonce) ||
        !sdk::is_valid_generation_nonce_hex(to_std(record.generation_nonce_hex)) ||
        expected_generation_native != recorded_generation_native)
    {
        *out_reason = reason::k_path_escape;
        *out_detail = "invalid activation nonce or frozen package locator";
        return false;
    }
    const auto reject_path = [&](std::string_view label, std::size_t bytes, std::size_t limit) {
        *out_reason = reason::k_payload_too_large;
        *out_detail = "generated " + std::string(label) + " is " + std::to_string(bytes) +
            " bytes, above its private limit of " + std::to_string(limit);
        return false;
    };
    for (const auto& [label, path] : {
             std::pair<std::string_view, const std::string&>{"custody root", custody_root_native},
             {"scratch root", scratch_directory_native},
             {"supervisor executable", supervisor_executable_native},
         })
    {
        if (path.empty() || path.size() > limits::k_max_start_absolute_path_bytes) {
            return reject_path(label, path.size(), limits::k_max_start_absolute_path_bytes);
        }
    }
    for (const auto& [label, path] : {
             std::pair<std::string_view, const std::string&>{"expected generation directory", expected_generation_native},
             {"recorded generation directory", recorded_generation_native},
         })
    {
        if (path.empty() || path.size() > limits::k_max_generation_directory_bytes) {
            return reject_path(label, path.size(), limits::k_max_generation_directory_bytes);
        }
    }
    for (const auto& [label, path] : {
             std::pair<std::string_view, const std::string&>{"expected adapter executable", expected_adapter_native},
             {"recorded adapter executable", recorded_adapter_native},
         })
    {
        if (path.empty() || path.size() > limits::k_max_adapter_executable_bytes) {
            return reject_path(label, path.size(), limits::k_max_adapter_executable_bytes);
        }
    }

#ifdef LOGONOMIC_ENABLE_CLI_PROVIDER_TEST_HOOKS
    std::function<void()> barrier;
    {
        const std::lock_guard lock(g_custody_start_barrier_mutex);
        barrier = g_custody_start_barrier;
    }
    if (barrier) {
        barrier();
    }
#endif

    std::string error;
    if (!process::prepare_parent_process(&error)) {
        *out_reason = reason::k_custodian_lost;
        *out_detail = "cannot prepare the process for pipe ownership: " + error;
        return false;
    }

#ifdef LOGONOMIC_ENABLE_CLI_PROVIDER_TEST_HOOKS
    std::function<void()> scratch_creation_barrier;
    {
        const std::lock_guard lock(g_scratch_creation_barrier_mutex);
        scratch_creation_barrier = g_scratch_creation_barrier;
    }
    if (scratch_creation_barrier) {
        scratch_creation_barrier();
    }
#endif
    QString directory_error;
    if (!create_real_directory_below_root(
            context.roots.scratch_root,
            scratch_family,
            &directory_error))
    {
        *out_reason = reason::k_custody_write_failed;
        *out_detail = "cannot create the package scratch directory: " +
            to_std(directory_error);
        return false;
    }

    scratch_directory = prepared_scratch_directory;
    scratch_lock_path = scratch_directory + QLatin1String(k_run_lock_suffix);
    QString lock_error;
    scratch_lock = Run_lock::try_acquire(scratch_lock_path, &lock_error);
    if (!scratch_lock) {
        *out_reason = reason::k_custody_write_failed;
        *out_detail = "cannot lock the generation scratch directory: " +
            (lock_error.isEmpty() ? std::string("held by another process") : to_std(lock_error));
        return false;
    }
    scratch_path_owned = true;
    if (!create_real_directory_below_root(
            context.roots.scratch_root,
            scratch_directory,
            &directory_error))
    {
        *out_reason = reason::k_custody_write_failed;
        *out_detail = "cannot create the generation scratch directory: " +
            to_std(directory_error);
        return false;
    }

    sdk::Adapter_environment_inputs inputs;
    inputs.platform_id               = to_std(context.platform_id);
    inputs.package_id                = package_id;
    inputs.generation_digest         = digest;
    inputs.generation_nonce          = nonce;
    inputs.package_root              = recorded_generation_native;
    inputs.scratch_directory         = scratch_directory_native;
    inputs.core_protocol_generations.assign(
        sdk::k_supported_core_protocol_generations.begin(),
        sdk::k_supported_core_protocol_generations.end());
    inputs.host_product = k_host_product;
    for (auto it = record.adapter_environment.constBegin(); it != record.adapter_environment.constEnd(); ++it) {
        inputs.manifest_environment.emplace_back(to_std(it.key()), to_std(it.value().toString()));
    }

    const std::vector<std::pair<std::string, std::string>> adapter_environment =
        sdk::build_adapter_environment(inputs, sdk::current_process_environment());
    if (!sdk::adapter_reserved_environment_within_budget(adapter_environment)) {
        *out_reason = reason::k_payload_too_large;
        *out_detail = "generated reserved environment exceeds k_max_reserved_environment_bytes";
        return false;
    }

    protocol::Start_request start;
    std::vector<std::string> custodian_environment;
    for (const auto& [name, value] : adapter_environment) {
        if (sdk::is_reserved_environment_name(name)) {
            continue;
        }
        start.environment.emplace_back(name, value);
        custodian_environment.push_back(name + "=" + value);
    }
    if (!sdk::adapter_start_environment_within_budget(start.environment)) {
        *out_reason = reason::k_payload_too_large;
        *out_detail = "generated start environment exceeds k_max_start_environment_bytes";
        return false;
    }
    start.package_id            = package_id;
    start.generation_digest     = digest;
    start.generation_nonce      = nonce;
    start.snapshot_nonce        = to_std(record.generation_nonce_hex);
    start.custody_root          = custody_root_native;
    start.scratch_root          = scratch_directory_native;
    start.supervisor_executable = supervisor_executable_native;
#if defined(__APPLE__)
    start.darwin_custody = protocol::Darwin_custody{};
#endif
    start.adapter_relative_path = to_std(record.adapter_relative_path);
    for (const QString& argument : record.adapter_argv) {
        start.adapter_argv.push_back(to_std(argument));
    }
    for (const std::uint16_t generation : sdk::k_supported_core_protocol_generations) {
        start.core_protocol_generations.push_back(generation);
    }
    // The inherited link handle is allocated only when the custody channels
    // exist. Its signed 64-bit decimal form is at most 19 bytes, so prove the
    // complete start envelope with that valid positive placeholder before
    // creating either channel or spawning the custodian.
    start.link_child_number = std::numeric_limits<std::int64_t>::max();
    const std::string preflight_start = protocol::encode(protocol::Message(start));
    if (preflight_start.size() > limits::k_max_control_message_bytes) {
        *out_reason = reason::k_payload_too_large;
        *out_detail = "generated start request is " + std::to_string(preflight_start.size()) +
            " bytes, above k_max_control_message_bytes";
        return false;
    }
    start.link_child_number = -1;

    process::Duplex_channel control_child;
    process::Stream_channel link_child;
    if (!process::Duplex_channel::create_pair(
            limits::k_max_control_message_bytes,
            &control,
            &control_child,
            &error) ||
        !process::Stream_channel::create_pair(&link, &link_child, &error) ||
        !process::create_pipe(&wake_read, &wake_write, &error) ||
        !wake_read.prepare_for_owner(&error) ||
        !wake_write.prepare_for_owner(&error) ||
        !control.prepare_for_owner(&error) ||
        !link.prepare_for_owner(&error))
    {
        *out_reason = reason::k_custodian_lost;
        *out_detail = "cannot create the custodian channels: " + error;
        return false;
    }
    {
        const std::lock_guard lock(wake_mutex);
        wake_ready = true;
    }

    if (custody_stop_requested.load()) {
        *out_reason = reason::k_shutdown;
        *out_detail = "generation retirement cancelled custodian creation";
        return false;
    }

    std::string containment_error;
    const bool containment_established = generation_containment.establish(nonce, &containment_error);
#ifdef LOGONOMIC_ENABLE_CLI_PROVIDER_TEST_HOOKS
    native_containment_at_birth.store(containment_established ? 1 : 0);
#endif
    if (!containment_established) {
#if defined(_WIN32)
        *out_reason = reason::k_custodian_lost;
        *out_detail = "cannot establish generation containment before birth: " + containment_error;
        return false;
#else
        // Ordinary Linux remains supported without delegated cgroups. Exact
        // recorded identities cannot prove tree settlement after owner loss.
        post([this, containment_error = std::move(containment_error)]() {
            this->record("generation_containment_unavailable", containment_error);
        });
#endif
    }
    process::Spawn_request spawn;
    generation_containment.configure_spawn(spawn);
    spawn.executable  = native_path(context.custodian_executable);
    spawn.environment = std::move(custodian_environment);
    spawn.inherited   = process::inherit_standard_streams();
    const std::size_t control_index = spawn.inherited.size();
    spawn.inherited.push_back(
        process::Inherited_descriptor{control_child.native(), k_control_child_number});
    const std::size_t link_index = spawn.inherited.size();
    spawn.inherited.push_back(
        process::Inherited_descriptor{link_child.native(), k_link_child_number});
    spawn.argv = {spawn.executable, "--control", process::inherited_argument(control_index)};

    custodian_membership = std::make_unique<Native_child_custody>(
        context.native_family ? context.native_family() : nullptr);
    if (custodian_membership->admission() == Native_child_custody::Admission::CLOSED) {
        *out_reason = reason::k_shutdown;
        *out_detail = "runtime native admission closed before custodian creation";
        return false;
    }
#if defined(_WIN32)
    spawn.job = reinterpret_cast<void*>(custodian_membership->job_handle());
#endif
    const process::Spawn_result spawned = process::spawn(spawn);
    control_child.close();
    link_child.close();
    if (spawned.process_handle != process::k_invalid_handle) {
        custodian_pid.store(spawned.process_id);
        custodian_handle = spawned.process_handle;
        custodian_membership_published = custodian_membership->publish_created(
            static_cast<std::uint32_t>(spawned.process_id), native_reference(spawned.process_handle));
        if (!custodian_membership_published) {
            *out_reason = reason::k_custodian_lost;
            *out_detail = "the original custodian could not join its reserved native family";
            return false;
        }
    }
    else {
        custodian_membership->cancel_uncreated();
    }
    if (!spawned.ok) {
        *out_reason = reason::k_custodian_lost;
        *out_detail = "cannot start the custodian: " + spawned.error;
        return false;
    }
#if defined(_WIN32)
    start.link_child_number = static_cast<std::int64_t>(
        reinterpret_cast<std::uintptr_t>(spawned.inherited_handles[link_index]));
#else
    start.link_child_number = spawned.inherited_handles[link_index];
#endif
    if (custody_stop_requested.load()) {
        *out_reason = reason::k_shutdown;
        *out_detail = "generation retirement cancelled custodian activation";
        return false;
    }

    const std::string encoded = protocol::encode(protocol::Message(start));
    if (encoded.size() > limits::k_max_control_message_bytes) {
        *out_reason = reason::k_payload_too_large;
        *out_detail = "generated start request is " + std::to_string(encoded.size()) +
            " bytes, above k_max_control_message_bytes";
        return false;
    }
    start_request_attempted = true;
    if (control.send(reinterpret_cast<const std::uint8_t*>(encoded.data()), encoded.size()) !=
        process::Io_status::TRANSFERRED)
    {
        *out_reason = reason::k_custodian_lost;
        *out_detail = "the custodian did not accept the start request";
        return false;
    }
    return true;
}

bool Generation_client::Impl::cleanup_worker_state(bool cleanup_complete)
{
    control.close();
    link.close();
    wake_read.close();
    {
        const std::lock_guard lock(wake_mutex);
        wake_ready = false;
        wake_write.close();
        wake_ticket = 0;
    }

    const std::int64_t pid = custodian_pid.load();
    if (pid != 0 && !custodian_reaped.load()) {
        const process::Deadline deadline = process::Deadline::after(k_failed_start_custodian_wait);
        while (!custodian_reaped.load() && (start_request_attempted || !deadline.expired())) {
            if (try_reap_custodian(pid, custodian_handle, false, &observed_custodian_exit)) {
                custodian_reaped.store(true);
                break;
            }
            // Control EOF asks the independent custodian to finish its own
            // generation. Do not kill that sole reaper while it still owns
            // descendants; it retries bounded native cleanup attempts.
            std::this_thread::sleep_for(milliseconds(100));
        }
        if (!custodian_reaped.load()) {
            kill_custodian(pid, custodian_handle);
            custodian_reaped.store(try_reap_custodian(pid, custodian_handle, true, &observed_custodian_exit));
            if (!coordinates.process_ids.empty() && (!custodian_membership ||
                    custodian_membership->admission() != Native_child_custody::Admission::ADMITTED))
            {
                (void)process::terminate_coordinates(
                    coordinates,
                    milliseconds(0),
                    milliseconds(limits::k_reap_deadline_ms));
            }
        }
    }
    if (generation_containment.available() && (pid == 0 || custodian_reaped.load())) {
        // Only the reader reaps the direct child. The retained scope supplies
        // the independent descendant-empty fact, including after owner loss.
        // Failed bounded attempts retain this worker, scope and scratch lock.
        milliseconds retry_delay{100};
        std::string error;
        while (
#ifdef LOGONOMIC_ENABLE_CLI_PROVIDER_TEST_HOOKS
            native_cleanup_observation_withheld.load() ||
#endif
            !generation_containment.terminate_and_observe_empty(
                milliseconds(limits::k_reap_deadline_ms), &error))
        {
            std::this_thread::sleep_for(retry_delay);
            retry_delay = std::min(retry_delay * 2, milliseconds{5000});
        }
        cleanup_complete = true;
    }
    bool membership_settled = true;
    if (custodian_membership) {
        if (pid == 0) {
            custodian_membership->cancel_uncreated();
        }
        else if (custodian_reaped.load()) {
            if (custodian_membership_published) {
                membership_settled = custodian_membership->observe_reaped();
            }
            else {
                // The designated waiter settled this unpublished occurrence.
                custodian_membership->cancel_uncreated();
            }
        }
        else {
            if (!custodian_membership_published) {
                custodian_membership_published = custodian_membership->publish_created(
                    static_cast<std::uint32_t>(pid), native_reference(custodian_handle));
            }
            if (custodian_membership_published) {
                // This is the reader's last native wait. The independent
                // family executor now owns collection; handles, scratch and
                // the generation lock remain retained, never normal finished.
                custodian_membership->handoff_wait_authority();
            }
            membership_settled = false;
        }
    }
    cleanup_complete = cleanup_complete || (custodian_reaped.load() &&
        observed_custodian_exit.signal == 0 &&
        observed_custodian_exit.exit_code == protocol::k_custody_clean_exit_code);
    if (!cleanup_complete || !membership_settled || (pid != 0 && !custodian_reaped.load())) {
        // Transport termination is not native tree containment. The client
        // remains owned by the proxy's pending-generation shutdown barrier.
        return false;
    }
#ifdef LOGONOMIC_ENABLE_CLI_PROVIDER_TEST_HOOKS
    // A healthy custodian can prove settlement even without an independent
    // containment scope. Tests can withhold publication on either path.
    while (native_cleanup_observation_withheld.load()) {
        std::this_thread::sleep_for(milliseconds(50));
    }
#endif
    // This fallback runs only behind the existing native-settlement guard,
    // including independent containment after custodian loss. Ordinary Linux
    // owner loss never reaches it. Closing admission also rejects a queued
    // READY notification that the broker has not processed yet.
    if (!scratch_directory.isEmpty()) {
        const std::string generation = usage_writer_custody::registry_for_scratch(native_path(scratch_directory));
        std::string error;
        while (!usage_writer_custody::close_generation(generation, &error)) {
            std::this_thread::sleep_for(milliseconds(100));
        }
    }
    for (const process::Native_handle handle : coordinates.process_handles) {
        process::close_native_handle(handle);
    }
    coordinates.process_handles.clear();
    generation_containment.release();
    if (custodian_handle != process::k_invalid_handle) {
        process::close_native_handle(custodian_handle);
        custodian_handle = process::k_invalid_handle;
    }

    if (scratch_path_owned && !scratch_directory.isEmpty()) {
        QDir(scratch_directory).removeRecursively();
    }
    scratch_path_owned = false;
    const bool owned_scratch_lock = scratch_lock != nullptr;
    scratch_lock.reset();
    if (owned_scratch_lock && !scratch_lock_path.isEmpty()) {
        QFile::remove(scratch_lock_path);
    }
    return true;
}

void Generation_client::Impl::run_reader()
{
    std::string start_reason;
    std::string start_detail;
    if (!initialize_custody(&start_reason, &start_detail)) {
        const bool cleanup_complete = cleanup_worker_state(!start_request_attempted);
        worker_finished.store(true);
        post([this, cleanup_complete, start_reason = std::move(start_reason), start_detail = std::move(start_detail)]() {
            stop_sent = true;
            if (is_live()) {
                record("spawn_failed");
                retire_internal(
                    Retirement_reason_kind::CUSTODIAN_LOST,
                    start_reason,
                    start_detail,
                    true);
            }
            if (cleanup_complete && !finished_emitted) {
                finished_emitted = true;
                emit owner->finished();
            }
        });
        return;
    }

    std::vector<std::uint8_t> control_buffer(limits::k_max_control_message_bytes);
    std::uint8_t              chunk[k_link_read_chunk_bytes];
    link::Frame_reader        frame_reader(limits::k_max_link_frame_bytes);
    bool control_open              = true;
    bool link_open                 = true;
    bool custodian_alive           = true;
    Custodian_exit custodian_exit;
    bool saw_started               = false;
    bool saw_reaped                = false;
    bool saw_error_before_started  = false;
#if defined(__APPLE__)
    std::optional<bool> deferred_control_close;
#endif
    std::deque<Outgoing> pending;

    const auto close_control = [&]() {
        control.close();
        control_open = false;
        const bool clean = saw_reaped || (saw_error_before_started && !saw_started);
#if defined(__APPLE__)
        if (link_open) {
            deferred_control_close = clean;
        }
        else {
            post([this, clean]() { on_control_closed(clean); });
        }
#else
        post([this, clean]() { on_control_closed(clean); });
#endif
        if (!clean && saw_started) {
            // Contract 14: the custodian is gone without reaping, so the
            // desktop signals the recorded processes itself and never waits
            // them. Their pidfds are the only authority it holds: the
            // recorded group number may already name an unrelated group.
            (void)process::terminate_coordinates(
                coordinates,
                milliseconds(limits::k_terminate_to_kill_ms),
                milliseconds(limits::k_reap_deadline_ms));
        }
    };
    const auto write_control = [&](const std::uint8_t* bytes, std::size_t size) {
#if defined(__APPLE__) && defined(LOGONOMIC_ENABLE_CLI_PROVIDER_TEST_HOOKS)
        if (control_write_probe) control_write_probe(custodian_handle, std::nullopt);
#endif
        const auto status = control.send(bytes, size);
#if defined(__APPLE__) && defined(LOGONOMIC_ENABLE_CLI_PROVIDER_TEST_HOOKS)
        if (control_write_probe) control_write_probe(custodian_handle, status == process::Io_status::CLOSED);
#endif
        if (status == process::Io_status::TRANSFERRED) {
            return;
        }
#if defined(__APPLE__)
        // A grace deadline can race the custodian's final receipt before the
        // broker sees the adapter exit. Drain the readable socket through EOF.
        if (status == process::Io_status::CLOSED && control.valid()) {
            return;
        }
#endif
        close_control();
    };
    const auto close_link = [&]() {
        link.close();
        link_open = false;
        post([this]() { on_link_closed(); });
#if defined(__APPLE__)
        if (deferred_control_close) {
            const bool clean = *deferred_control_close;
            post([this, clean]() { on_control_closed(clean); });
            deferred_control_close.reset();
        }
#endif
    };

    while (true) {
        {
            const std::lock_guard lock(outgoing_mutex);
            while (!outgoing.empty()) {
                pending.push_back(std::move(outgoing.front()));
                outgoing.pop_front();
            }
        }
        if (discard_link_output.load()) {
            if (link_open) {
                // Closing cancels any channel-owned pending copy before its
                // ticket leaves the queue; no completion is orphaned.
                close_link();
            }
            const std::lock_guard lock(outgoing_mutex);
            for (auto item = pending.begin(); item != pending.end();) {
                if (item->kind != Outgoing::Kind::LINK) {
                    ++item;
                    continue;
                }
                outgoing_link_bytes -= item->bytes.size() - item->offset;
                item = pending.erase(item);
            }
        }
        while (!pending.empty()) {
            Outgoing& item = pending.front();
            switch (item.kind) {
                case Outgoing::Kind::LINK: {
                    if (!link_open) {
                        const std::lock_guard lock(outgoing_mutex);
                        outgoing_link_bytes -= item.bytes.size() - item.offset;
                        pending.pop_front();
                        continue;
                    }
                    process::Io_result written;
                    if (item.ticket != 0) {
                        written = link.complete_write(item.ticket);
                        if (written.status != process::Io_status::WOULD_BLOCK) {
                            item.ticket = 0;
                        }
                    }
                    else {
                        const process::Write_submission_result submitted =
                            link.begin_write(item.bytes.data() + item.offset, item.bytes.size() - item.offset);
                        if (submitted.status == process::Write_submission_status::PENDING) {
                            item.ticket   = submitted.ticket;
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
                        break;
                    }
                    if (written.status != process::Io_status::TRANSFERRED) {
                        close_link();
                        continue;
                    }
                    item.offset += written.bytes;
                    {
                        const std::lock_guard lock(outgoing_mutex);
                        outgoing_link_bytes -= written.bytes;
                    }
                    if (item.offset == item.bytes.size()) {
                        pending.pop_front();
                        continue;
                    }
                    break;
                }
                case Outgoing::Kind::CONTROL:
                    if (control_open) write_control(item.bytes.data(), item.bytes.size());
                    pending.pop_front();
                    continue;
                case Outgoing::Kind::CLOSE_CONTROL:
                default:
                    if (control_open) {
                        control.close();
                        control_open = false;
                    }
                    pending.pop_front();
                    continue;
            }
            break;   // the front link write is back-pressured or partial
        }
#if defined(__APPLE__)
        // Exact custodian death does not consume bytes already queued by the
        // relay. Drain to EOF before posting any terminal custody outcome.
        if (!custodian_alive && !link_open && !control_open) {
#else
        if (!custodian_alive && (!control_open || custodian_exit.signal != 0 ||
                custodian_exit.exit_code != protocol::k_custody_clean_exit_code)) {
#endif
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
        const std::size_t wake_index    = add(wake_read.waitable(), true, false);
        const std::size_t control_index = control_open ? add(control.waitable(), true, false) : SIZE_MAX;
        const std::size_t link_index    = link_open ? add(link.waitable(), true, false) : SIZE_MAX;
        const bool link_write_pending = link_open && !pending.empty() &&
            pending.front().kind == Outgoing::Kind::LINK;
        const std::size_t link_write_index = link_write_pending
            ? add(link.write_waitable(), false, true) : SIZE_MAX;
        const std::size_t custodian_index = custodian_alive
            ? add(custodian_handle, true, false) : SIZE_MAX;
        const int ready = process::wait_ready(entries.data(), entries.size(), -1);
        if (ready < 0) {
            break;
        }
        const auto observed = [&entries](std::size_t index) {
            return index != SIZE_MAX &&
                (entries[index].readable || entries[index].closed || entries[index].failed);
        };
        // The write entry wakes this loop; the next pass consumes the
        // completion while reads in this pass keep the reverse direction live.
        (void)link_write_index;

        if (observed(wake_index)) {
            std::uint8_t drained[64];
            while (wake_read.read(drained, sizeof(drained)).status == process::Io_status::TRANSFERRED) {}
        }
        if (observed(control_index)) {
            while (control_open) {
                const process::Receive_result received =
                    control.receive(control_buffer.data(), control_buffer.size(), 0);
                if (received.status == process::Receive_status::TIMEOUT) {
                    break;
                }
                if (received.status != process::Receive_status::MESSAGE) {
                    for (const process::Native_handle handle : received.descriptors) {
                        process::close_native_handle(handle);
                    }
                    close_control();
                    break;
                }
                protocol::Decode_failure failure;
                std::optional<protocol::Message> message = protocol::decode(
                    std::string_view(reinterpret_cast<const char*>(control_buffer.data()), received.size),
                    &failure);
                const protocol::Started* started = message ? std::get_if<protocol::Started>(&*message) : nullptr;
                if (started && !saw_started) {
                    saw_started                 = true;
                    coordinates.process_group   = started->process_group;
                    coordinates.process_ids     = {started->supervisor_pid, started->adapter_pid};
                    coordinates.process_handles = received.descriptors;   // owned here, closed at thread end
                }
                else {
                    for (const process::Native_handle handle : received.descriptors) {
                        process::close_native_handle(handle);
                    }
                }
                if (!message) {
                    post([this, failure = std::move(failure)]() {
                        if (is_live()) {
                            retire_internal(
                                Retirement_reason_kind::PROTOCOL_VIOLATION,
                                failure.reason.empty() ? reason::k_frame_violation : failure.reason,
                                failure.detail.empty()
                                    ? std::string("the custodian sent an unreadable control message")
                                    : failure.detail,
                                true);
                        }
                    });
                    close_control();
                    break;
                }
                if (const auto* reaped = std::get_if<protocol::Reaped>(&*message)) {
#if defined(__APPLE__)
                    saw_reaped = reaped->darwin_custody && reaped->darwin_custody->version == 1 &&
                        reaped->darwin_custody->owned_groups_confirmed;
#else
                    saw_reaped = reaped->complete;
#endif
                }
                if (!saw_started && std::holds_alternative<protocol::Error>(*message)) {
                    saw_error_before_started = true;
                }
                post([this, message = std::move(*message)]() mutable { on_control_message(std::move(message)); });
            }
        }
        if (observed(link_index)) {
            std::size_t remaining = sizeof(chunk);
            while (link_open && remaining != 0) {
                const process::Io_result read = link.read(chunk, remaining);
                if (read.status == process::Io_status::WOULD_BLOCK) {
                    break;
                }
                if (read.status != process::Io_status::TRANSFERRED) {
                    close_link();
                    break;
                }
                remaining -= read.bytes;
                if (frame_reader.feed(chunk, read.bytes) != link::Frame_reader::Feed_status::OK) {
                    close_link();   // the supervisor's stream is not decodable any more
                    break;
                }
                while (std::optional<link::Frame> frame = frame_reader.next()) {
                    if (!admit_inbound(std::move(*frame))) {
                        custody_stop_requested.store(true);
                        discard_link_output.store(true);
                        close_link();
                        if (control_open) {
                            if (saw_started) {
                                protocol::Stop_request stop;
                                stop.grace_ms = 0;
                                const std::string encoded = protocol::encode(protocol::Message(stop));
                                write_control(reinterpret_cast<const std::uint8_t*>(encoded.data()), encoded.size());
                            }
                            else {
                                close_control();
                            }
                        }
                        break;
                    }
                }
            }
        }
        if (observed(custodian_index)) {
            if (try_reap_custodian(custodian_pid.load(), custodian_handle, false, &custodian_exit)) {
                custodian_alive = false;
                observed_custodian_exit = custodian_exit;
                custodian_reaped.store(true);
            }
        }
    }
    const bool cleanup_complete = cleanup_worker_state(saw_reaped);
    worker_finished.store(true);
    post([this, custodian_exit, saw_reaped, cleanup_complete]() {
        on_custodian_exited(custodian_exit, saw_reaped, cleanup_complete);
    });
}

// ---------------------------------------------------------------------------
// Generation_client

Generation_client::Generation_client(Activation_context context, QObject* parent)
:
    QObject(parent),
    m_impl(std::make_unique<Impl>(this, std::move(context)))
{
    Impl& impl = *m_impl;
    connect(&impl.activation_deadline, &QTimer::timeout, this, [this]() {
        Impl& impl = *m_impl;
        if (impl.is_live()) {
            impl.retire_internal(
                Retirement_reason_kind::ACTIVATION_TIMEOUT,
                reason::k_activation_timeout,
                "the adapter did not become ready within the activation deadline",
                true);
        }
    });
    connect(&impl.hello_deadline, &QTimer::timeout, this, [this]() {
        Impl& impl = *m_impl;
        if (impl.state == State::STARTING) {
            impl.record("hello_timeout");
            impl.retire_internal(
                Retirement_reason_kind::ACTIVATION_TIMEOUT,
                reason::k_hello_timeout,
                "no hello and capabilities within the hello deadline",
                true);
        }
    });
    connect(&impl.shutdown_grace, &QTimer::timeout, this, [this]() {
        Impl& impl = *m_impl;
        if (!impl.is_live() || impl.adapter_exited) {
            return;
        }
        if (impl.state == State::IDLE_STOPPING && !impl.shutdown_requested) {
            if (!impl.idle_shutdown_delivered) {
                impl.retire_internal(Retirement_reason_kind::SUPERVISOR_LOST, reason::k_supervisor_lost,
                    "the supervisor did not confirm delivery of idle shutdown", true);
                return;
            }
            // Preserve the link until the supervisor drains final output and
            // reports the native exit. A timer cannot certify a forced stop.
            impl.idle_force_requested = true;
            impl.record("terminated");
            impl.send_control(protocol::Message(protocol::Force_idle_request{}));
            return;
        }
        // The adapter did not honour its shutdown grace: the custodian
        // terminates, then kills; the generation retires as a shutdown.
        impl.record("terminated");
        impl.state             = State::TERMINALLY_UNAVAILABLE;
        impl.retirement_reason = QString::fromLatin1(reason::k_shutdown);
        impl.end_process_tree(false);
        emit retired(
            Retirement_reason_kind::SHUTDOWN,
            impl.retirement_reason,
            QStringLiteral("the adapter did not exit within the shutdown grace"));
    });
}

Generation_client::~Generation_client()
{
    Impl& impl = *m_impl;
    Q_ASSERT(!impl.reader.joinable() || impl.worker_finished.load());
    if (impl.reader.joinable()) {
        impl.reader.join();
    }
}

#ifdef LOGONOMIC_ENABLE_CLI_PROVIDER_TEST_HOOKS
void Generation_client::set_custody_start_barrier_for_tests(std::function<void()> barrier)
{
    const std::lock_guard lock(g_custody_start_barrier_mutex);
    g_custody_start_barrier = std::move(barrier);
}

void Generation_client::set_scratch_creation_barrier_for_tests(
    std::function<void()> in_barrier)
{
    const std::lock_guard lock(g_scratch_creation_barrier_mutex);
    g_scratch_creation_barrier = std::move(in_barrier);
}
#endif

void Generation_client::start()
{
    Q_ASSERT(QThread::currentThread() == thread());
    Impl& impl = *m_impl;
    if (impl.state != State::NOT_STARTED) {
        return;
    }
    impl.state = State::STARTING;
    if (impl.recorder) {
        impl.placeholders.generation_digest = impl.digest;
        impl.placeholders.generation_nonce  = impl.nonce;
        impl.recorder->set_placeholders(impl.placeholders);
    }
    impl.reader = std::thread([&impl]() { impl.run_reader(); });
    impl.activation_deadline.start(limits::k_activation_deadline_ms);
    emit starting();
}

Logonomic_cli_addon_activation_state Generation_client::state() const
{
    return m_impl->state;
}

const Activation_context& Generation_client::context() const
{
    return m_impl->context;
}

QString Generation_client::usage_writer_generation() const
{
    const auto& context = m_impl->context;
    const QString scratch = QDir(QDir(context.roots.scratch_root).filePath(context.record.package_id))
        .filePath(QString::fromLatin1(m_impl->nonce));
    return QString::fromStdString(usage_writer_custody::registry_for_scratch(native_path(scratch)));
}

QByteArray Generation_client::activation_nonce() const
{
    return QByteArray::fromStdString(m_impl->nonce);
}

Idle_stop_outcome Generation_client::idle_stop_outcome() const
{
    return m_impl->finished_emitted ? m_impl->idle_outcome : Idle_stop_outcome::NONE;
}

QString Generation_client::retirement_reason() const
{
    return m_impl->retirement_reason;
}

QList<Declared_capability> Generation_client::live_capabilities() const
{
    return m_impl->state == State::READY ? m_impl->live : QList<Declared_capability>{};
}

bool Generation_client::handshake_declares(const Declared_capability& pair) const
{
    const Impl& impl = *m_impl;
    return impl.tracker &&
        impl.tracker->handshake_declares(
            sdk::Capability_pair{to_std(pair.capability_id), static_cast<std::uint16_t>(pair.version)});
}

Send_outcome Generation_client::send_invoke(
    const QByteArray&  correlation_id,
    const QString&     capability_id,
    uint32_t           version,
    const QString&     operation,
    const QJsonObject& payload,
    int                deadline_ms)
{
    Q_ASSERT(QThread::currentThread() == thread());
    Impl& impl = *m_impl;
    Send_outcome outcome;
    if (impl.state != State::READY) {
        outcome.reason_code = impl.retirement_reason.isEmpty()
            ? QString::fromLatin1(reason::k_registry_not_settled)
            : impl.retirement_reason;
        outcome.message = QStringLiteral("the adapter generation is not ready");
        return outcome;
    }
    if (impl.tracker->live_request_count() >= impl.effective_limits.max_in_flight_requests) {
        impl.record("backpressured");
        outcome.reason_code = QString::fromLatin1(reason::k_backpressured);
        outcome.message     = QStringLiteral("the effective max_in_flight_requests is reached");
        return outcome;
    }

    sdk::Invoke_request request;
    request.request_id  = "r" + std::to_string(impl.next_request);
    request.capability  = sdk::Capability_pair{to_std(capability_id), static_cast<std::uint16_t>(version)};
    request.operation   = to_std(operation);
    request.payload     = logonomic_cli_addon_from_qjson(payload);
    if (!request.payload.is_object()) {
        request.payload = sdk::Json_value::object();
    }
    request.deadline_ms = deadline_ms;
    const sdk::Json_value object = sdk::encode_host_message(sdk::Host_message(request));
    const std::string frame      = sdk::encode_frame(object);
    if (frame.size() > impl.effective_limits.max_frame_bytes) {
        impl.record("payload_too_large", "bytes");
        outcome.reason_code = QString::fromLatin1(reason::k_payload_too_large);
        outcome.message     = QStringLiteral("encoded frame of %1 bytes exceeds %2")
            .arg(frame.size())
            .arg(impl.effective_limits.max_frame_bytes);
        return outcome;
    }
    if (sdk::json_depth(object) > impl.effective_limits.max_json_depth) {
        impl.record("payload_too_large", "depth");
        outcome.reason_code = QString::fromLatin1(reason::k_payload_too_large);
        outcome.message     = QStringLiteral("encoded frame depth %1 exceeds %2")
            .arg(sdk::json_depth(object))
            .arg(impl.effective_limits.max_json_depth);
        return outcome;
    }

    if (!sdk::parse_json(frame, impl.parse_limits, nullptr)) {
        impl.record("payload_too_large", "json_bounds");
        outcome.reason_code = QString::fromLatin1(reason::k_payload_too_large);
        outcome.message = QStringLiteral("encoded request exceeds the negotiated JSON bounds");
        return outcome;
    }

    // The supervisor link carries the JSON body without the SDK framing newline.
    const std::string_view body(frame.data(), frame.size() - 1);
    if (!impl.send_link_frame(object, body)) {
        impl.retire_internal(
            Retirement_reason_kind::PROTOCOL_VIOLATION,
            reason::k_stdin_queue_overflow,
            "the bounded supervisor-link queue could not admit an invoke",
            true);
        outcome.reason_code = QString::fromLatin1(reason::k_stdin_queue_overflow);
        outcome.message     = QStringLiteral("the bounded supervisor-link queue is full");
        return outcome;
    }
    ++impl.next_request;
    impl.wire_ids.insert(correlation_id, request.request_id);
    impl.correlations.insert(QByteArray::fromStdString(request.request_id), correlation_id);
    impl.tracker->note_invoke_sent(request.request_id);
    outcome.sent = true;
    return outcome;
}

void Generation_client::send_cancel(const QByteArray& correlation_id)
{
    Q_ASSERT(QThread::currentThread() == thread());
    Impl& impl = *m_impl;
    if (impl.state != State::READY || impl.cancelled.contains(correlation_id)) {
        return;
    }
    const auto wire_id = impl.wire_ids.constFind(correlation_id);
    if (wire_id == impl.wire_ids.cend()) {
        return;
    }
    impl.cancelled.insert(correlation_id);
    sdk::Cancel_request cancel;
    cancel.request_id = wire_id.value();
    if (!impl.send_link_frame(sdk::encode_host_message(sdk::Host_message(cancel)))) {
        impl.retire_internal(
            Retirement_reason_kind::PROTOCOL_VIOLATION,
            reason::k_stdin_queue_overflow,
            "the bounded supervisor-link queue could not admit a cancel",
            true);
    }
}

void Generation_client::Impl::send_shutdown()
{
    sdk::Shutdown_request shutdown;
    shutdown.grace_ms = limits::k_shutdown_grace_ms;
    const auto message = sdk::encode_host_message(sdk::Host_message(shutdown));
    const auto type = state == State::IDLE_STOPPING ? link::Message_type::IDLE_SHUTDOWN : link::Message_type::ADAPTER_FRAME;
    if (!send_link_frame(message, sdk::serialize_json(message), type)) {
        retire_internal(Retirement_reason_kind::PROTOCOL_VIOLATION, reason::k_stdin_queue_overflow,
            "the bounded supervisor-link queue could not admit shutdown", true);
        return;
    }
    shutdown_grace.start(limits::k_shutdown_grace_ms);
}

bool Generation_client::begin_idle_stop()
{
    Q_ASSERT(QThread::currentThread() == thread());
    Impl& impl = *m_impl;
    if (impl.state != State::READY || impl.shutdown_requested || !impl.wire_ids.isEmpty()) {
        return false;
    }
    impl.state = State::IDLE_STOPPING;
    impl.send_shutdown();
    return true;
}

void Generation_client::begin_shutdown()
{
    Q_ASSERT(QThread::currentThread() == thread());
    Impl& impl = *m_impl;
    if (impl.state == State::IDLE_STOPPING) {
        impl.shutdown_requested = true;
        if (impl.adapter_exited || impl.idle_force_requested) {
            impl.retire_internal(Retirement_reason_kind::SHUTDOWN, reason::k_shutdown,
                "the application closed during idle shutdown", false);
        }
    }
    else
    if (impl.state == State::READY) {
        if (impl.shutdown_requested) {
            return;
        }
        impl.shutdown_requested = true;
        impl.send_shutdown();
        return;
    }
    if (impl.state == State::STARTING) {
        impl.retire_internal(
            Retirement_reason_kind::SHUTDOWN,
            reason::k_shutdown,
            "shutdown requested during activation",
            true);
    }
}

void Generation_client::retire(Retirement_reason_kind kind, QString detail)
{
    Q_ASSERT(QThread::currentThread() == thread());
    m_impl->retire_internal(kind, default_reason_code(kind), to_std(detail), true);
}

void Generation_client::set_transcript_recorder(sdk::Transcript_recorder* recorder)
{
    Impl& impl = *m_impl;
    if (impl.recorder == recorder) {
        return;
    }
    impl.recorder = recorder;
    impl.placeholders.roots.clear();
    impl.placeholders.generation_digest = impl.digest;
    impl.placeholders.generation_nonce = impl.nonce;
    if (recorder) {
        recorder->set_placeholders(impl.placeholders);
    }
}

void Generation_client::note_transcript_root(const QString& root)
{
    Impl& impl = *m_impl;
    if (!impl.recorder) {
        return;
    }
    const std::string value = to_std(root);
    if (std::find(impl.placeholders.roots.cbegin(), impl.placeholders.roots.cend(), value) != impl.placeholders.roots.cend()) {
        return;
    }
    impl.placeholders.generation_digest = impl.digest;
    impl.placeholders.generation_nonce  = impl.nonce;
    impl.placeholders.roots.push_back(value);
    impl.recorder->set_placeholders(impl.placeholders);
}

std::size_t Generation_client::transcript_root_count() const
{
    return m_impl->placeholders.roots.size();
}

void Generation_client::record_event(const char* kind, const QString& detail)
{
    m_impl->record(kind, to_std(detail));
}

void Generation_client::record_request_event(const char* kind, const QByteArray& correlation_id)
{
    const Impl& impl = *m_impl;
    const auto wire_id = impl.wire_ids.constFind(correlation_id);
    m_impl->record(kind, wire_id == impl.wire_ids.cend() ? to_std(correlation_id) : wire_id.value());
}

std::int64_t Generation_client::custodian_process_id() const
{
    return m_impl->custodian_pid.load();
}

std::int64_t Generation_client::process_group() const
{
    return m_impl->group;
}

QList<std::int64_t> Generation_client::descendant_process_ids() const
{
    const Impl& impl = *m_impl;
    if (!impl.started_received) {
        return {};
    }
    return {impl.supervisor_pid, impl.adapter_pid};
}

bool Generation_client::custody_finished() const
{
    return m_impl->finished_emitted;
}

#ifdef LOGONOMIC_ENABLE_CLI_PROVIDER_TEST_HOOKS
std::optional<bool> Generation_client::native_cleanup_result_for_tests() const
{
    return m_impl->observed_native_cleanup_result;
}

#if defined(__APPLE__)
void Generation_client::set_control_write_probe_for_tests(std::function<void(int, std::optional<bool>)> probe)
{
    Q_ASSERT(m_impl->state == State::NOT_STARTED);
    m_impl->control_write_probe = std::move(probe);
}
#endif

void Generation_client::reject_next_link_enqueue_for_tests()
{
    Q_ASSERT(QThread::currentThread() == thread());
    m_impl->reject_next_link_enqueue = true;
}

void Generation_client::set_native_cleanup_observation_withheld_for_tests(bool withheld)
{
    m_impl->native_cleanup_observation_withheld.store(withheld);
}

std::optional<bool> Generation_client::native_containment_available_for_tests() const
{
    const int state = m_impl->native_containment_at_birth.load();
    return state < 0 ? std::nullopt : std::optional<bool>(state != 0);
}

Generation_client::Inbound_mailbox_snapshot Generation_client::inbound_mailbox_snapshot_for_tests() const
{
    const std::lock_guard lock(m_impl->inbound_mutex);
    return m_impl->inbound_snapshot;
}
#endif

} // namespace logonomic::cli_addon_host
