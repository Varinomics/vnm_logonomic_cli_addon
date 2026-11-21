#include "logonomic_cli_addon/adapter_runtime.h"
#include "logonomic_cli_addon/frame.h"
#include "logonomic_cli_addon/json.h"
#include "logonomic_cli_addon/limits.h"
#include "logonomic_cli_addon/version.h"
#include "adapter_stdio.h"

#if defined(_WIN32)
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace logonomic::cli_addon {

namespace {

constexpr const char* k_handler_incomplete_reason = "handler_incomplete";

void bound_utf8_diagnostic(std::string& text, std::size_t max_bytes)
{
    // The public API admits handler text. Invalid caller bytes become fixed
    // ASCII, and valid text stops before a partial UTF-8 scalar.
    if (!is_valid_utf8(text)) {
        text = "invalid UTF-8 diagnostic";
    }
    if (text.size() <= max_bytes) {
        return;
    }
    std::size_t end = max_bytes;
    while (end > 0 &&
        (static_cast<unsigned char>(text[end]) & 0xc0u) == 0x80u)
    {
        --end;
    }
    text.resize(end);
}

std::optional<std::string> checked_adapter_frame(
    const Json_value&        message,
    const Json_parse_limits& parse_limits)
{
    // Reserve the terminator before checking the JSON body against the
    // negotiated parser limits the host will apply to this frame.
    std::string frame = serialize_json(message);
    if (frame.size() >= parse_limits.max_bytes) {
        return std::nullopt;
    }
    Json_parse_limits bounded_limits = parse_limits;
    --bounded_limits.max_bytes;
    if (!parse_json(frame, bounded_limits, nullptr).has_value()) {
        return std::nullopt;
    }
    frame.push_back('\n');
    return frame;
}

using Clock = std::chrono::steady_clock;

// The shortest spacing between a request's paced progress frames.
constexpr auto k_min_progress_spacing = std::chrono::milliseconds(10);
constexpr std::size_t k_progress_initial_burst = 256;

class Request_context;

// Sends the progress a request's pacing held back as soon as the pacing
// allows it, so the newest report reaches the host even when its handler
// falls quiet afterwards (Request_context::report_progress). One thread
// serves the runtime; it starts with the first held report.
class Progress_flusher
{
public:
    ~Progress_flusher();

    void schedule(std::weak_ptr<Request_context> context, Clock::time_point due);

private:
    void run();

    std::mutex                                                       m_mutex;
    std::condition_variable                                          m_wake;
    std::multimap<Clock::time_point, std::weak_ptr<Request_context>> m_due;
    bool                                                             m_stopping = false;
    std::thread                                                      m_thread;
};

// Exit codes of run().
constexpr int k_exit_clean            = 0;
constexpr int k_exit_host_violation   = 2;
constexpr int k_exit_no_shared_generation = 3;

void diagnostic(const std::string& text)
{
    stdio::write_stderr("logonomic-cli-addon runtime: " + text + "\n");
}

[[noreturn]] void abandon_process(int exit_code)
{
#if defined(_WIN32)
    // CRT _Exit can enter ExitProcess and DLL detach code. An abandoned
    // worker may own a lock that code needs; whole-process termination
    // must not wait for its cleanup a second time.
    if (!::TerminateProcess(::GetCurrentProcess(), static_cast<UINT>(exit_code))) {
        const DWORD error = ::GetLastError();
        diagnostic("Process termination failed: " + std::to_string(error) + ".");
        std::_Exit(EXIT_FAILURE);
    }
#endif
    std::_Exit(exit_code);
}

class Frame_writer
{
public:
    bool write_frame(const Json_value& message)
    {
        return write_raw(encode_frame(message));
    }

    bool write_raw(std::string_view bytes)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_broken) {
            return false;
        }
        if (!stdio::write_stdout(bytes)) {
            m_broken = true;
            return false;
        }
        return true;
    }

    bool broken() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_broken;
    }

private:
    mutable std::mutex m_mutex;
    bool               m_broken = false;
};

class Request_context final :
    public Invocation_context,
    public std::enable_shared_from_this<Request_context>
{
public:
    Request_context(
        Invoke_request      request,
        Frame_writer&       writer,
        Progress_flusher&   flusher,
        Json_parse_limits   parse_limits,
        std::uint64_t       max_progress_frames)
    :
        m_request(std::move(request)),
        m_writer(writer),
        m_flusher(flusher),
        m_parse_limits(std::move(parse_limits)),
        m_max_progress_frames(max_progress_frames),
        m_progress_burst(max_progress_frames == limits::k_unlimited_progress_frames ? k_progress_initial_burst
            : std::max<std::uint64_t>({1, max_progress_frames / 16, std::min<std::uint64_t>(max_progress_frames, 16)})),
        m_started(Clock::now())
    {}

    const Invoke_request& request() const override { return m_request; }

    bool cancel_requested() const override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_cancel_requested;
    }

    bool wait_for_cancel(int timeout_ms) override
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_wake.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return m_cancel_requested; });
        return m_cancel_requested;
    }

    // A held report replaces its predecessor, so paced progress has constant
    // retained size even when the request runs indefinitely. Finite limits
    // negotiated with a peer retain their budget-aware spacing.
    void report_progress(std::uint64_t completed, std::optional<std::uint64_t> total, std::string note) override
    {
        Progress_message progress;
        progress.request_id = m_request.request_id;
        progress.completed  = completed;
        progress.total      = total;
        progress.note       = std::move(note);
        bound_utf8_diagnostic(progress.note, limits::k_max_progress_note_bytes);

        std::optional<Clock::time_point> flush_at;
        {
            const std::lock_guard<std::mutex> emit(m_emit_mutex);
            std::unique_lock<std::mutex> lock(m_mutex);
            if (m_finished) {
                return;
            }
            if (m_max_progress_frames != limits::k_unlimited_progress_frames &&
                m_progress_frames >= m_max_progress_frames)
            {
                const bool first = !m_progress_exhausted;
                m_progress_exhausted = true;
                lock.unlock();
                if (first) {
                    stdio::write_stderr("Adapter runtime: Request " + m_request.request_id + " has sent all " +
                        std::to_string(m_max_progress_frames) + " progress frames it may send; its later progress "
                        "is not reported.\n");
                }
                return;
            }
            m_held_progress = std::move(progress);
            const Clock::time_point now = Clock::now();
            const Clock::time_point due = next_progress_frame_at();
            if (now >= due) {
                send_held_progress(now, lock);
            }
            else
            if (!m_flush_scheduled) {
                m_flush_scheduled = true;
                flush_at = due;
            }
        }
        if (flush_at.has_value()) {
            m_flusher.schedule(weak_from_this(), *flush_at);
        }
    }

    // The flusher's call once a held report's time has come.
    void flush_held_progress()
    {
        std::optional<Clock::time_point> flush_at;
        {
            const std::lock_guard<std::mutex> emit(m_emit_mutex);
            std::unique_lock<std::mutex> lock(m_mutex);
            m_flush_scheduled = false;
            if (m_finished || !m_held_progress.has_value()) {
                return;
            }
            const Clock::time_point now = Clock::now();
            const Clock::time_point due = next_progress_frame_at();
            if (now >= due) {
                send_held_progress(now, lock);
            }
            else {
                m_flush_scheduled = true;
                flush_at = due;
            }
        }
        if (flush_at.has_value()) {
            m_flusher.schedule(weak_from_this(), *flush_at);
        }
    }

    void succeed(Json_value payload) override
    {
        Result_message result;
        result.request_id = m_request.request_id;
        result.status     = Result_status::SUCCESS;
        result.payload    = payload.is_object() ? std::move(payload) : Json_value::object();
        finish(std::move(result));
    }

    void fail(Result_status status, std::string reason, std::string message) override
    {
        Result_message result;
        result.request_id = m_request.request_id;
        result.status     = status == Result_status::SUCCESS ? Result_status::FAILED : status;
        result.reason     = is_valid_reason_code(reason) ? std::move(reason) : std::string("error");
        result.message    = std::move(message);
        bound_utf8_diagnostic(result.message, limits::k_max_result_message_bytes);
        finish(std::move(result));
    }

    bool finished() const override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_finished;
    }

    void request_cancel()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_cancel_requested = true;
        }
        m_wake.notify_all();
    }

    // Called by the runtime when the handler returned without a result.
    void finish_incomplete()
    {
        fail(Result_status::FAILED, k_handler_incomplete_reason, "handler returned without a terminal result");
    }

    void release_payload()
    {
        // The input reader can remain blocked after this worker finishes.
        // Keep its identity for cancellation and joining, without retaining
        // the handler's input until another host frame arrives.
        m_request.payload = Json_value();
    }

private:
    void finish(Result_message result)
    {
        std::optional<std::string> frame = checked_adapter_frame(
            encode_adapter_message(Adapter_message(std::move(result))), m_parse_limits);
        if (!frame.has_value()) {
            Result_message fallback;
            fallback.request_id = m_request.request_id;
            fallback.status     = Result_status::FAILED;
            fallback.reason     = reason::k_payload_too_large;
            frame = checked_adapter_frame(
                encode_adapter_message(Adapter_message(std::move(fallback))), m_parse_limits);
        }
        if (!frame.has_value()) {
            return;
        }
        const std::lock_guard<std::mutex> emit(m_emit_mutex);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_finished) {
                return;
            }
            m_finished = true;
            m_held_progress.reset();
        }
        m_writer.write_raw(*frame);
    }

    // When the next frame may go out; called under m_mutex.
    Clock::time_point next_progress_frame_at() const
    {
        if (m_max_progress_frames == limits::k_unlimited_progress_frames) {
            return m_initial_progress_remaining > 0 ? m_started : m_last_progress_frame + k_min_progress_spacing;
        }
        if (m_progress_frames < m_progress_burst) {
            return m_started;
        }
        const Clock::duration age = m_last_progress_frame - m_started;
        return m_last_progress_frame + std::max<Clock::duration>(
            k_min_progress_spacing, age / static_cast<Clock::rep>(m_progress_burst));
    }

    // Sends the held report. Called under both locks; it writes after
    // releasing m_mutex, so a cancel never waits on stdout, and before
    // releasing m_emit_mutex, so no frame of this request follows its result.
    void send_held_progress(Clock::time_point now, std::unique_lock<std::mutex>& lock)
    {
        Progress_message progress = std::move(*m_held_progress);
        m_held_progress.reset();
        if (m_max_progress_frames != limits::k_unlimited_progress_frames) {
            ++m_progress_frames;
        }
        else
        if (m_initial_progress_remaining > 0) {
            --m_initial_progress_remaining;
        }
        m_last_progress_frame = now;
        lock.unlock();
        const std::optional<std::string> frame = checked_adapter_frame(
            encode_adapter_message(Adapter_message(std::move(progress))), m_parse_limits);
        if (frame.has_value()) {
            m_writer.write_raw(*frame);
        }
    }

    Invoke_request                  m_request;
    Frame_writer&                   m_writer;
    Progress_flusher&               m_flusher;
    Json_parse_limits               m_parse_limits;
    std::uint64_t                   m_max_progress_frames;
    std::uint64_t                   m_progress_burst;
    std::size_t                     m_initial_progress_remaining = k_progress_initial_burst;
    Clock::time_point               m_started;
    // Orders this request's frames: taken around every write, and around
    // the finished check that precedes it.
    std::mutex                      m_emit_mutex;
    mutable std::mutex              m_mutex;
    std::condition_variable         m_wake;
    bool                            m_cancel_requested   = false;
    bool                            m_finished           = false;
    std::uint64_t                   m_progress_frames    = 0;
    Clock::time_point               m_last_progress_frame;
    std::optional<Progress_message> m_held_progress;
    bool                            m_flush_scheduled    = false;
    bool                            m_progress_exhausted = false;
};

Progress_flusher::~Progress_flusher()
{
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_stopping = true;
    }
    m_wake.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void Progress_flusher::schedule(std::weak_ptr<Request_context> context, Clock::time_point due)
{
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping) {
            return;
        }
        m_due.emplace(due, std::move(context));
        if (!m_thread.joinable()) {
            m_thread = std::thread([this] { run(); });
        }
    }
    m_wake.notify_all();
}

void Progress_flusher::run()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    while (!m_stopping) {
        if (m_due.empty()) {
            m_wake.wait(lock);
            continue;
        }
        const Clock::time_point next = m_due.begin()->first;
        if (Clock::now() < next) {
            m_wake.wait_until(lock, next);
            continue;
        }
        const std::weak_ptr<Request_context> context = std::move(m_due.begin()->second);
        m_due.erase(m_due.begin());
        lock.unlock();
        if (const std::shared_ptr<Request_context> live = context.lock()) {
            live->flush_held_progress();
        }
        lock.lock();
    }
}

// A terminal result does not end the worker. On Windows, MSVC's
// at-thread-exit future notification can also precede CRT TLS destructors
// (https://github.com/microsoft/STL/issues/3614), so observe the actual
// native thread instead.
class Worker_completion
{
public:
#if defined(_WIN32)
    ~Worker_completion()
    {
        if (const HANDLE thread = m_thread.load(std::memory_order_acquire)) {
            ::CloseHandle(thread);
        }
    }
#endif

    void run(const Adapter_runtime::Invoke_handler& handler, Request_context& context)
    {
#if defined(_WIN32)
        // Duplicate the calling thread's pseudo handle before user code.
        // This owns a real HANDLE independently of std::thread's
        // implementation-defined native_handle_type, including MinGW's.
        HANDLE thread = nullptr;
        if (!::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentThread(),
                ::GetCurrentProcess(), &thread, SYNCHRONIZE, FALSE, 0))
        {
            fail_observation("DuplicateHandle", ::GetLastError());
        }
        m_thread.store(thread, std::memory_order_release);
#endif
        handler(context);
        if (!context.finished()) {
            context.finish_incomplete();
        }
        context.release_payload();
#if !defined(_WIN32)
        // The C++ contract publishes readiness after thread-local
        // destruction, not merely when the handler returns.
        m_exit.set_value_at_thread_exit();
#endif
    }

    bool finished() const
    {
#if defined(_WIN32)
        const HANDLE thread = m_thread.load(std::memory_order_acquire);
        if (thread == nullptr) {
            return false;
        }
        const DWORD status = ::WaitForSingleObject(thread, 0);
        if (status == WAIT_OBJECT_0) {
            return true;
        }
        if (status == WAIT_TIMEOUT) {
            return false;
        }
        fail_observation("WaitForSingleObject", status == WAIT_FAILED ? ::GetLastError() : ERROR_INVALID_DATA);
#else
        return m_exited.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
#endif
    }

private:
#if defined(_WIN32)
    [[noreturn]] static void fail_observation(const char* operation, DWORD error)
    {
        diagnostic("Worker completion " + std::string(operation) + " failed: " + std::to_string(error) + ".");
        abandon_process(EXIT_FAILURE);
    }

    std::atomic<HANDLE> m_thread{nullptr};
#else
    std::promise<void> m_exit;
    std::future<void>  m_exited = m_exit.get_future();
#endif
};

struct Live_request
{
    std::shared_ptr<Request_context>   context;
    std::shared_ptr<Worker_completion> completion;
    std::thread                        worker;
};

} // namespace

struct Adapter_runtime::Impl
{
    Adapter_runtime_options              options;
    Invoke_handler                       invoke_handler;
    Hello_hook                           hello_hook;
    Shutdown_hook                        shutdown_hook;
    Frame_writer                         writer;
    // After the writer, which it writes through, and before the requests,
    // so it stops before the one and after the other are gone.
    Progress_flusher                     progress_flusher;
    std::optional<Host_hello>            host_hello;
    Protocol_limits                      effective_limits;
    std::map<std::string, Live_request>  live;
    Json_parse_limits                    parse_limits;

    void join_finished()
    {
        for (auto it = live.begin(); it != live.end();) {
            if (it->second.completion->finished() && it->second.worker.joinable()) {
                it->second.worker.join();
                it = live.erase(it);
            }
            else {
                ++it;
            }
        }
    }

    void cancel_all()
    {
        for (auto& [request_id, request] : live) {
            request.context->request_cancel();
        }
    }

    // Waits up to grace_ms for every worker, including thread-local cleanup.
    // Workers still running afterwards are abandoned by ending the process:
    // the contract requires the adapter to be gone when the grace expires,
    // and a joinable thread must not outlive main().
    void stop(int grace_ms, int exit_code)
    {
        cancel_all();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(grace_ms);
        while (!live.empty()) {
            join_finished();
            if (live.empty()) {
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                diagnostic("Abandoning " + std::to_string(live.size()) + " unfinished handler(s) at shutdown grace expiry.");
                abandon_process(exit_code);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    bool handle_hello(const Host_hello& hello)
    {
        std::optional<std::uint16_t> generation;
        for (const std::uint16_t offered : hello.core_protocol_generations) {
            const bool supported = std::find(k_supported_core_protocol_generations.begin(),
                k_supported_core_protocol_generations.end(), offered) != k_supported_core_protocol_generations.end();
            if (supported && (!generation.has_value() || offered > *generation)) {
                generation = offered;
            }
        }
        if (!generation.has_value()) {
            diagnostic("The host offered no supported core protocol generation.");
            return false;
        }

        host_hello = hello;
        Adapter_hello answer;
        answer.core_protocol_generation = *generation;
        answer.package_id               = hello.package_id;
        answer.generation_digest        = hello.generation_digest;
        answer.generation_nonce         = hello.generation_nonce;
        answer.adapter_name             = options.adapter_name;
        answer.adapter_version          = options.adapter_version;
        answer.limits                   = options.limits;
        // A limit above the host's is not an announcement the host accepts.
        const auto clamp = [](std::optional<std::uint64_t>& value, std::uint64_t host_value) {
            if (value.has_value() && *value > host_value) {
                value = host_value;
            }
        };
        clamp(answer.limits.max_frame_bytes,                 hello.limits.max_frame_bytes);
        clamp(answer.limits.max_json_depth,                  hello.limits.max_json_depth);
        clamp(answer.limits.max_in_flight_requests,          hello.limits.max_in_flight_requests);
        clamp(answer.limits.max_progress_frames_per_request, hello.limits.max_progress_frames_per_request);

        if (hello_hook) {
            hello_hook(hello, answer);
        }

        effective_limits = hello.limits;
        if (answer.limits.max_frame_bytes)                 { effective_limits.max_frame_bytes                 = std::min(effective_limits.max_frame_bytes,                 *answer.limits.max_frame_bytes); }
        if (answer.limits.max_json_depth)                  { effective_limits.max_json_depth                  = std::min(effective_limits.max_json_depth,                  *answer.limits.max_json_depth); }
        if (answer.limits.max_in_flight_requests)          { effective_limits.max_in_flight_requests          = std::min(effective_limits.max_in_flight_requests,          *answer.limits.max_in_flight_requests); }
        if (answer.limits.max_progress_frames_per_request) { effective_limits.max_progress_frames_per_request = std::min(effective_limits.max_progress_frames_per_request, *answer.limits.max_progress_frames_per_request); }
        parse_limits.max_bytes = effective_limits.max_frame_bytes;
        parse_limits.max_depth = (std::size_t)effective_limits.max_json_depth;

        Capabilities_message capabilities;
        capabilities.capabilities = options.capabilities;
        writer.write_frame(encode_adapter_message(Adapter_message(std::move(answer))));
        writer.write_frame(encode_adapter_message(Adapter_message(std::move(capabilities))));
        return true;
    }

    bool handle_invoke(Invoke_request invoke)
    {
        if (live.count(invoke.request_id) != 0) {
            diagnostic("The host reused live request id " + invoke.request_id + ".");
            return false;
        }
        auto context = std::make_shared<Request_context>(
            std::move(invoke), writer, progress_flusher, parse_limits,
            effective_limits.max_progress_frames_per_request);

        const bool declared = std::find(options.capabilities.begin(), options.capabilities.end(),
            context->request().capability) != options.capabilities.end();
        if (!declared || !invoke_handler) {
            context->reject_unknown_operation();
            return true;
        }

        auto completion = std::make_shared<Worker_completion>();
        Live_request request;
        request.context    = context;
        request.completion = completion;
        request.worker     = std::thread([this, context, completion] {
            completion->run(invoke_handler, *context);
        });
        live.emplace(context->request().request_id, std::move(request));
        return true;
    }

    void handle_cancel(const Cancel_request& cancel)
    {
        const auto it = live.find(cancel.request_id);
        if (it != live.end()) {
            it->second.context->request_cancel();
        }
    }
};

Adapter_runtime::Adapter_runtime(Adapter_runtime_options options)
:
    m_impl(std::make_unique<Impl>())
{
    m_impl->options = std::move(options);
}

Adapter_runtime::~Adapter_runtime() = default;

void Adapter_runtime::set_invoke_handler(Invoke_handler handler) { m_impl->invoke_handler = std::move(handler); }
void Adapter_runtime::set_hello_hook(Hello_hook hook)             { m_impl->hello_hook = std::move(hook); }
void Adapter_runtime::set_shutdown_hook(Shutdown_hook hook)       { m_impl->shutdown_hook = std::move(hook); }

void Adapter_runtime::write_raw(std::string_view bytes)
{
    m_impl->writer.write_raw(bytes);
}

const Host_hello& Adapter_runtime::host_hello() const
{
    static const Host_hello k_none;
    return m_impl->host_hello.has_value() ? *m_impl->host_hello : k_none;
}

Protocol_limits Adapter_runtime::effective_limits() const
{
    return m_impl->effective_limits;
}

int Adapter_runtime::run()
{
    Impl& impl = *m_impl;
    stdio::prepare_binary_streams();

    // Until the handshake fixes the effective limits, the host's defaults
    // bound the hello frame itself.
    impl.parse_limits = Json_parse_limits{};
    Frame_decoder decoder(limits::k_max_frame_bytes);

    char buffer[65536];
    while (true) {
        impl.join_finished();
        if (impl.writer.broken()) {
            impl.stop(limits::k_cancel_grace_ms, k_exit_clean);
            return k_exit_clean;
        }

        const long count = stdio::read_stdin(buffer, sizeof(buffer));
        if (count <= 0) {
            // End of file or a broken host: the adapter must exit.
            impl.stop(limits::k_cancel_grace_ms, k_exit_clean);
            return k_exit_clean;
        }
        if (!decoder.feed(std::string_view(buffer, (std::size_t)count))) {
            diagnostic(std::string("Host frame validation failed: ") + frame_violation_reason_code(decoder.violation()) + ".");
            impl.stop(limits::k_cancel_grace_ms, k_exit_host_violation);
            return k_exit_host_violation;
        }

        std::string frame;
        while (decoder.next_frame(frame)) {
            Contract_failure failure;
            std::optional<Host_message> message = decode_host_message(frame, impl.parse_limits, &failure);
            if (!message.has_value()) {
                diagnostic("The host violated the protocol. " + failure.reason_code + ": " + failure.detail);
                impl.stop(limits::k_cancel_grace_ms, k_exit_host_violation);
                return k_exit_host_violation;
            }

            if (Host_hello* hello = std::get_if<Host_hello>(&*message)) {
                if (impl.host_hello.has_value()) {
                    diagnostic("The host sent a second hello.");
                    impl.stop(limits::k_cancel_grace_ms, k_exit_host_violation);
                    return k_exit_host_violation;
                }
                if (!impl.handle_hello(*hello)) {
                    return k_exit_no_shared_generation;
                }
                continue;
            }
            if (!impl.host_hello.has_value()) {
                diagnostic("The host sent " + std::string(host_message_type(*message)) + " before hello.");
                impl.stop(limits::k_cancel_grace_ms, k_exit_host_violation);
                return k_exit_host_violation;
            }
            if (Invoke_request* invoke = std::get_if<Invoke_request>(&*message)) {
                if (!impl.handle_invoke(std::move(*invoke))) {
                    impl.stop(limits::k_cancel_grace_ms, k_exit_host_violation);
                    return k_exit_host_violation;
                }
            }
            else
            if (const Cancel_request* cancel = std::get_if<Cancel_request>(&*message)) {
                impl.handle_cancel(*cancel);
            }
            else
            if (const Shutdown_request* shutdown = std::get_if<Shutdown_request>(&*message)) {
                if (impl.shutdown_hook) {
                    impl.shutdown_hook(*shutdown);
                }
                // Leave the host a margin to observe the exit inside its grace.
                const int grace = (int)std::max<std::int64_t>(0, shutdown->grace_ms - shutdown->grace_ms / 10);
                impl.stop(grace, k_exit_clean);
                return k_exit_clean;
            }
        }
    }
}

} // namespace logonomic::cli_addon
