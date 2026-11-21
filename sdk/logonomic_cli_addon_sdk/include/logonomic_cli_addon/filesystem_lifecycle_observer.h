#pragma once

#include "logonomic_cli_addon/adapter_runtime.h"
#include "logonomic_cli_addon/lifecycle_events.h"
#include "logonomic_cli_addon/model_state.h"

#include <vnm_filesystem_observation.h>

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>

namespace logonomic::cli_addon {

class Filesystem_lifecycle_binding;
using Filesystem_lifecycle_bindings =
    std::map<std::string, std::shared_ptr<Filesystem_lifecycle_binding>>;

// The opt-in filesystem component uses the maintained observation service.
// Native path discovery and evidence decoding remain with the provider.
vnm::filesystem_observation::Subscription_options lifecycle_file_interests(
    QStringList files, QStringList directories);

class Filesystem_lifecycle_binding
{
public:
    virtual ~Filesystem_lifecycle_binding() = default;

    // These operations and provider state are accessed under the driver's lock.
    // Refused START/request facts reserve no terminal identity.
    bool publish(Lifecycle_observation observation);
    bool can_start_child() const;
    void finish(Lifecycle_observation observation);
    void stop_observation();
    void request_rescan();
    std::uint64_t next_sequence() const { return m_next_sequence; }

    virtual vnm::filesystem_observation::Subscription_options interests(
        const Filesystem_lifecycle_bindings& bindings) = 0;
    virtual bool reconcile(Invocation_context* invocation) = 0;
    // The driver reads this snapshot under the binding lock on negotiated requests.
    virtual std::optional<Model_update> root_model_observation() const { return std::nullopt; }
    virtual void check_owner(const Filesystem_lifecycle_bindings&) {}
    virtual void release() {}
    virtual void wait_completed(bool) {}

    std::string session;
    std::string nonce;
    bool closed = false;
    bool retired = false;
    bool watch_started = false;
    bool watch_dirty = true;

private:
    friend class Filesystem_lifecycle_observer;
    std::deque<Lifecycle_observation> m_queue;
    std::set<std::string> m_active_children;
    std::set<std::string> m_active_attention;
    std::uint64_t m_first_sequence = 0;
    std::uint64_t m_next_sequence = 0;
    std::optional<std::string> m_final_cursor;
    std::optional<Model_update> m_reported_root_model;
    bool m_observing = false;
    vnm::filesystem_observation::Subscription m_subscription;
    std::shared_ptr<vnm::filesystem_observation::Wait_channel> m_channel;
    QStringList m_watched_files;
    QStringList m_watched_directories;
};

class Filesystem_lifecycle_observer
{
public:
    Filesystem_lifecycle_observer(
        std::shared_ptr<vnm::filesystem_observation::Service> service,
        vnm::filesystem_observation::Configuration configuration);

    // Keep this lock while looking up, binding or changing provider evidence.
    std::unique_lock<std::mutex> lock() { return std::unique_lock(m_mutex); }
    bool reserved(const std::string& nonce) const { return m_bindings.contains(nonce); }
    std::shared_ptr<Filesystem_lifecycle_binding> find(const std::string& nonce) const;
    const Filesystem_lifecycle_bindings& bindings() const { return m_bindings; }
    void bind(std::shared_ptr<Filesystem_lifecycle_binding> binding);
    bool refresh_interests(Filesystem_lifecycle_binding& binding);

    // Handles reserve, retire, observe and poll; classification stays native.
    bool handle(Invocation_context& context);

private:
    bool observation_covered(const Filesystem_lifecycle_binding& binding) const;
    void observe(Invocation_context& context, const Observe_request& request);

    std::shared_ptr<vnm::filesystem_observation::Service> m_service;
    vnm::filesystem_observation::Configuration m_configuration;
    std::mutex m_mutex;
    Filesystem_lifecycle_bindings m_bindings;
};

} // namespace logonomic::cli_addon
