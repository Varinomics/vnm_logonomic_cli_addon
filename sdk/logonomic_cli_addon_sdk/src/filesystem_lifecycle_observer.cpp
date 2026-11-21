#include "logonomic_cli_addon/filesystem_lifecycle_observer.h"
#include "logonomic_cli_addon/capability_ids.h"
#include "logonomic_cli_addon/lifecycle_correlation_lifetime.h"

#include <QFileInfo>
#include <QScopeGuard>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace logonomic::cli_addon {
namespace {

namespace observation = vnm::filesystem_observation;
namespace path_name = vnm::path_name;
using Clock = std::chrono::steady_clock;
constexpr std::size_t k_queue_bound = 4096;
constexpr std::size_t k_active_child_bound = 128;
constexpr std::size_t k_active_attention_bound = 128;

std::uint64_t cursor_sequence(const Session_correlation& correlation, std::string_view value)
{
    if (value.empty()) return 0;
    const auto separator = value.find(':');
    if (separator == std::string_view::npos || value.substr(0, separator) != correlation.transition_nonce) {
        throw std::runtime_error("invalid_observation_cursor");
    }
    std::uint64_t sequence = 0;
    const auto suffix = value.substr(separator + 1);
    const auto parsed = std::from_chars(suffix.data(), suffix.data() + suffix.size(), sequence);
    if (suffix.empty() || parsed.ec != std::errc{} || parsed.ptr != suffix.data() + suffix.size() ||
        std::to_string(sequence) != suffix)
    {
        throw std::runtime_error("invalid_observation_cursor");
    }
    return sequence;
}

std::string cursor_text(const std::string& nonce, std::uint64_t sequence)
{
    return nonce + ':' + std::to_string(sequence);
}

template<typename Request, typename Result>
void project_root_model(const Filesystem_lifecycle_binding& binding, const Request& request, Result& result)
{
    result.model_failure = request.model_failure;
    if (request.model_state_version == k_model_state_version && !result.model_failure) {
        if (auto model = binding.root_model_observation()) {
            result.model_updates.push_back(std::move(*model));
        }
    }
}

} // namespace

observation::Subscription_options lifecycle_file_interests(QStringList files, QStringList directories)
{
    files.removeDuplicates();
    files.sort();
    directories.removeDuplicates();
    directories.sort();
    observation::Subscription_options result;
    result.mode = observation::Watch_mode::DIRECT;
    result.policy_epoch = std::make_shared<path_name::Policy_epoch>();
    result.exact_files = std::move(files);
    QList<path_name::Key> keys;
    for (const auto& path : result.exact_files) keys.append(result.policy_epoch->key(path));
    for (const auto& directory : directories) result.directories.append({directory, 0});
    result.classifier = [keys](const observation::Event& event) {
        return std::any_of(keys.cbegin(), keys.cend(), [&](const auto& key) {
            return path_name::matches_key(event.path, key) || path_name::matches_key(event.old_path, key);
        });
    };
    return result;
}

bool Filesystem_lifecycle_binding::can_start_child() const
{
    // The host admits at most 128 active children. Reserve their terminal
    // facts and root END before admitting another child into this queue.
    return !closed && !retired && m_active_children.size() < k_active_child_bound &&
        m_queue.size() + m_active_children.size() + m_active_attention.size() + 3 <= k_queue_bound;
}

bool Filesystem_lifecycle_binding::publish(Lifecycle_observation event)
{
    if (closed || retired) return false;
    const bool start = event.subagent_transition == Subagent_transition::STARTED;
    const bool stop = event.subagent_transition == Subagent_transition::STOPPED;
    const bool request = event.attention_transition == Attention_transition::REQUESTED &&
        !event.attention_request_id.empty();
    const bool resolve = event.attention_transition == Attention_transition::RESOLVED &&
        !event.attention_request_id.empty();
    if (start && (m_active_children.size() >= k_active_child_bound || m_active_children.contains(event.agent_id))) {
        return false;
    }
    if (stop && !m_active_children.contains(event.agent_id)) return false;
    if (request && (m_active_attention.size() >= k_active_attention_bound ||
        m_active_attention.contains(event.attention_request_id)))
    {
        return false;
    }
    if (resolve && !m_active_attention.contains(event.attention_request_id)) return false;
    const auto retained_terminals = m_active_children.size() + m_active_attention.size() +
        std::size_t(start) + std::size_t(request) - std::size_t(stop) - std::size_t(resolve);
    if (m_queue.size() + retained_terminals + 2 > k_queue_bound) {
        return false;
    }
    if (start) m_active_children.insert(event.agent_id);
    if (stop) m_active_children.erase(event.agent_id);
    if (request) m_active_attention.insert(event.attention_request_id);
    if (resolve) m_active_attention.erase(event.attention_request_id);
    ++m_next_sequence;
    m_queue.push_back(std::move(event));
    return true;
}

void Filesystem_lifecycle_binding::finish(Lifecycle_observation event)
{
    if (closed || retired) return;
    // publish() keeps this slot available even when optional evidence fills
    // the queue. The host owns closure of children without a native finish.
    ++m_next_sequence;
    m_queue.push_back(std::move(event));
    closed = true;
    stop_observation();
}

void Filesystem_lifecycle_binding::stop_observation()
{
    m_subscription.reset();
    if (m_channel) m_channel->cancel();
    m_channel.reset();
    m_watched_files.clear();
    m_watched_directories.clear();
}

void Filesystem_lifecycle_binding::request_rescan()
{
    watch_dirty = true;
    if (m_channel) m_subscription.request_rescan(QStringLiteral("Lifecycle evidence reconciliation pending"));
}

Filesystem_lifecycle_observer::Filesystem_lifecycle_observer(
    std::shared_ptr<observation::Service> service, observation::Configuration configuration)

:
    m_service(std::move(service)),
    m_configuration(configuration)
{
    if (!m_service) throw std::invalid_argument("A lifecycle observer requires a process observation service");
    m_configuration.validate();
}

std::shared_ptr<Filesystem_lifecycle_binding> Filesystem_lifecycle_observer::find(const std::string& nonce) const
{
    const auto found = m_bindings.find(nonce);
    return found == m_bindings.end() ? nullptr : found->second;
}

void Filesystem_lifecycle_observer::bind(std::shared_ptr<Filesystem_lifecycle_binding> binding)
{
    const auto found = m_bindings.find(binding->nonce);
    if (found == m_bindings.end()) throw std::runtime_error("correlation_not_reserved");
    if (found->second) throw std::runtime_error("observation_session_already_bound");
    found->second = std::move(binding);
}

bool Filesystem_lifecycle_observer::refresh_interests(Filesystem_lifecycle_binding& binding)
{
    auto options = binding.interests(m_bindings);
    options.mode = observation::Watch_mode::DIRECT;
    options.interval = m_configuration.general_interval;
    QStringList directories;
    for (const auto& directory : options.directories) directories.append(directory.path);
    directories.removeDuplicates();
    directories.sort();
    options.exact_files.removeDuplicates();
    options.exact_files.sort();
    binding.watch_dirty = false;
    if (binding.m_channel && options.exact_files == binding.m_watched_files &&
        directories == binding.m_watched_directories)
    {
        return false;
    }
    if (options.exact_files.isEmpty() && directories.isEmpty()) {
        const bool replaced = bool(binding.m_channel);
        binding.stop_observation();
        return replaced;
    }
    const auto files = options.exact_files;
    auto channel = std::make_shared<observation::Wait_channel>();
    auto subscription = m_service->subscribe(std::move(options), channel);
    binding.stop_observation();
    binding.m_subscription = std::move(subscription);
    binding.m_channel = std::move(channel);
    binding.m_watched_files = files;
    binding.m_watched_directories = std::move(directories);
    return true;
}

bool Filesystem_lifecycle_observer::observation_covered(const Filesystem_lifecycle_binding& binding) const
{
    for (const auto& file : binding.m_watched_files) {
        if (!binding.m_subscription.covers_directory(QFileInfo(file).absolutePath())) return false;
    }
    for (const auto& directory : binding.m_watched_directories) {
        if (!binding.m_subscription.covers_directory(directory)) return false;
    }
    return true;
}

void Filesystem_lifecycle_observer::observe(Invocation_context& context, const Observe_request& request)
{
    const auto sequence = cursor_sequence(*request.correlation, request.resume_cursor);
    std::shared_ptr<Filesystem_lifecycle_binding> binding;
    std::uint64_t window_sequence = 0;
    {
        auto guard = lock();
        binding = find(request.correlation->transition_nonce);
        if (!binding || binding->session != request.session.provider_session_id) {
            context.fail(Result_status::UNAVAILABLE, "session_start_required");
            return;
        }
        if (binding->m_observing || sequence > binding->m_next_sequence) {
            throw std::runtime_error("invalid_observation_cursor");
        }
        binding->check_owner(m_bindings);
        binding->m_observing = true;
        binding->watch_started = true;
        binding->watch_dirty = true;
        window_sequence = std::max(sequence, binding->m_first_sequence);
    }
    auto observing = qScopeGuard([&] {
        auto guard = lock();
        binding->m_observing = false;
    });
    const auto deadline = Clock::now() + std::chrono::milliseconds(request.window_ms);
    auto reported_sequence = window_sequence;
    const bool wants_model = request.model_state_version == k_model_state_version && !request.model_failure;
    bool check_model = wants_model;
    while (!context.cancel_requested() && Clock::now() < deadline) {
        std::shared_ptr<observation::Wait_channel> channel;
        {
            auto guard = lock();
            if (binding->retired) break;
            const auto model = check_model ? binding->root_model_observation() : std::nullopt;
            check_model = false;
            const auto& previous = binding->m_reported_root_model;
            const bool model_changed = model && (!previous || previous->status != model->status ||
                previous->model != model->model || previous->provider != model->provider ||
                previous->reasoning_effort != model->reasoning_effort);
            if (binding->m_next_sequence > reported_sequence || model_changed) {
                reported_sequence = binding->m_next_sequence;
                const auto available = reported_sequence - window_sequence;
                context.report_progress(available, std::nullopt, observe_progress_note(available));
                if (model_changed) binding->m_reported_root_model = model;
            }
            if (binding->closed) break;
            if (binding->watch_dirty) {
                try { refresh_interests(*binding); }
                catch (const std::exception&) {
                    // Optional acquisition never withdraws the root lifecycle.
                    // Existing service coverage owns retry pacing; with no
                    // coverage, retry only on a new hook or observe window.
                    binding->watch_dirty = false;
                    if (binding->m_channel) binding->m_subscription.request_rescan(
                        QStringLiteral("Lifecycle interest discovery is incomplete"));
                }
            }
            channel = binding->m_channel;
        }
        if (!channel) {
            context.wait_for_cancel(100);
            continue;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        auto delivery = channel->wait(std::min(remaining, std::chrono::milliseconds(100)));
        {
            auto guard = lock();
            binding->wait_completed(delivery.has_value());
            if (delivery && delivery->valid() && !binding->retired && !binding->closed &&
                binding->m_channel == channel)
            {
                bool complete = false;
                try {
                    if (!refresh_interests(*binding)) {
                        check_model = wants_model;
                        complete = binding->reconcile(&context);
                    }
                }
                catch (const std::exception&) {
                    // The provider has no new fact on a failed read. Retain
                    // accepted facts and the outstanding acquisition capture.
                }
                if (!binding->closed && complete && !binding->watch_dirty && observation_covered(*binding)) {
                    binding->m_subscription.acknowledge(delivery->batch.capture);
                }
                else
                if (!binding->closed) {
                    binding->m_subscription.request_rescan(QStringLiteral("Lifecycle evidence acquisition is incomplete"));
                }
            }
        }
        if (delivery) delivery->completion.complete();
    }
    auto guard = lock();
    binding->m_observing = false;
    observing.dismiss();
    if (binding->retired) {
        context.fail(Result_status::CANCELLED, "correlation_retired");
        return;
    }
    Observe_result result{cursor_text(binding->nonce, binding->m_next_sequence)};
    project_root_model(*binding, request, result);
    context.succeed(encode_observe_result(result));
}

bool Filesystem_lifecycle_observer::handle(Invocation_context& context)
{
    const auto& invocation = context.request();
    const bool lifetime = invocation.capability.capability_id == "lifecycle-correlation-lifetime";
    if (!lifetime && invocation.operation != "observe" && invocation.operation != "poll_observations") return false;
    try {
        Contract_failure failure;
        if (lifetime && invocation.operation == k_lifecycle_correlation_lifetime_reserve_operation) {
            const auto request = decode_lifecycle_correlation_reserve_request(invocation.payload, &failure);
            if (!request) { context.fail(Result_status::FAILED, failure.reason_code, failure.detail); return true; }
            auto guard = lock();
            if (context.cancel_requested()) { context.fail(Result_status::CANCELLED, "cancelled"); return true; }
            if (!reserved(request->correlation.transition_nonce)) {
                if (m_bindings.size() >= 256) throw std::runtime_error("observation_session_capacity_exhausted");
                m_bindings.emplace(request->correlation.transition_nonce, nullptr);
            }
            context.succeed(encode_lifecycle_correlation_lifetime_ack());
        }
        else
        if (lifetime && invocation.operation == k_lifecycle_correlation_lifetime_retire_operation) {
            const auto request = decode_lifecycle_correlation_retire_request(invocation.payload, &failure);
            if (!request) { context.fail(Result_status::FAILED, failure.reason_code, failure.detail); return true; }
            auto guard = lock();
            const auto found = m_bindings.find(request->correlation.transition_nonce);
            if (found != m_bindings.end()) {
                const auto binding = found->second;
                if (request->disposition == Lifecycle_correlation_retire_disposition::TERMINAL_CONSUMED &&
                    (!binding || !binding->closed || !binding->m_final_cursor || binding->m_final_cursor != request->consumed_cursor))
                {
                    throw std::runtime_error("terminal_cursor_not_consumed");
                }
                if (binding) {
                    binding->retired = true;
                    binding->stop_observation();
                    binding->release();
                    binding->m_queue.clear();
                    binding->m_active_children.clear();
                    binding->m_active_attention.clear();
                }
                m_bindings.erase(found);
            }
            context.succeed(encode_lifecycle_correlation_lifetime_ack());
        }
        else
        if (lifetime) {
            context.reject_unknown_operation();
        }
        else
        if (invocation.operation == "poll_observations") {
            const auto request = decode_poll_observations_request(invocation.payload, &failure);
            if (!request) { context.fail(Result_status::FAILED, failure.reason_code, failure.detail); return true; }
            if (!request->correlation) { context.fail(Result_status::UNAVAILABLE, "session_start_required"); return true; }
            const auto sequence = cursor_sequence(*request->correlation, request->resume_cursor);
            auto guard = lock();
            const auto binding = find(request->correlation->transition_nonce);
            if (!binding) throw std::runtime_error("unknown_observation_cursor");
            if (sequence < binding->m_first_sequence || sequence > binding->m_next_sequence) {
                throw std::runtime_error("invalid_observation_cursor");
            }
            while (binding->m_first_sequence < sequence) {
                binding->m_queue.pop_front();
                ++binding->m_first_sequence;
            }
            Poll_observations_result result;
            const auto count = std::min(request->limit, binding->m_queue.size());
            result.observations.assign(binding->m_queue.begin(), binding->m_queue.begin() + static_cast<std::ptrdiff_t>(count));
            result.resume_cursor = cursor_text(binding->nonce, sequence + count);
            result.more = count < binding->m_queue.size();
            project_root_model(*binding, *request, result);
            if (binding->closed && !result.more) binding->m_final_cursor = result.resume_cursor;
            context.succeed(encode_poll_observations_result(result));
        }
        else {
            const auto request = decode_observe_request(invocation.payload, &failure);
            if (!request) { context.fail(Result_status::FAILED, failure.reason_code, failure.detail); return true; }
            if (!request->correlation) { context.fail(Result_status::UNAVAILABLE, "session_start_required"); return true; }
            observe(context, *request);
        }
    }
    catch (const std::exception& error) { context.fail(Result_status::FAILED, error.what()); }
    return true;
}

} // namespace logonomic::cli_addon
