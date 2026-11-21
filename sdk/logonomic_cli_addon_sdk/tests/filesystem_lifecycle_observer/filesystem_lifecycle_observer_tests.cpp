#include "logonomic_cli_addon/filesystem_lifecycle_observer.h"
#include "logonomic_cli_addon/capability_ids.h"
#include "logonomic_cli_addon/lifecycle_correlation_lifetime.h"
#include "logonomic_cli_addon/limits.h"

#include <iostream>
#include <stdexcept>
#include <utility>

namespace {

namespace addon = logonomic::cli_addon;
namespace observation = vnm::filesystem_observation;

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

class Context final : public addon::Invocation_context
{
public:
    const addon::Invoke_request& request() const override { return invocation; }
    bool cancel_requested() const override { return cancelled; }
    bool wait_for_cancel(int) override
    {
        if (fail_wait) throw std::runtime_error("wait_failed");
        cancelled = true;
        return true;
    }
    void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override
    {
        if (fail_progress) throw std::runtime_error("progress_failed");
        ++progress_calls;
    }
    void succeed(addon::Json_value payload) override { result = std::move(payload); }
    void fail(addon::Result_status status, std::string reason, std::string) override
    {
        failure_status = status;
        failure_reason = std::move(reason);
    }
    bool finished() const override { return result.has_value() || failure_status.has_value(); }

    addon::Invoke_request invocation;
    std::optional<addon::Json_value> result;
    std::optional<addon::Result_status> failure_status;
    std::string failure_reason;
    unsigned progress_calls = 0;
    bool fail_progress = false;
    bool fail_wait = false;
    bool cancelled = false;
};

class Binding final : public addon::Filesystem_lifecycle_binding
{
public:
    observation::Subscription_options interests(const addon::Filesystem_lifecycle_bindings&) override
    {
        return {};
    }
    bool reconcile(addon::Invocation_context*) override { return true; }
    std::optional<addon::Model_update> root_model_observation() const override { return model; }
    std::optional<addon::Model_update> model;
};

void model_only_results_keep_the_lifecycle_cursor()
{
    addon::Filesystem_lifecycle_observer observer(std::make_shared<observation::Service>(), {});
    const addon::Session_correlation correlation{std::string(32, 'c')};
    Context reserve;
    reserve.invocation.capability = {"lifecycle-correlation-lifetime", 1};
    reserve.invocation.operation  = "reserve";
    reserve.invocation.payload    = addon::encode_lifecycle_correlation_reserve_request({correlation});
    require(observer.handle(reserve) && reserve.result, "model observer reservation failed");

    auto binding = std::make_shared<Binding>();
    binding->nonce   = correlation.transition_nonce;
    binding->session = "root-session";
    addon::Model_update sample;
    sample.provider_session_id = binding->session;
    sample.transition_nonce    = binding->nonce;
    sample.status              = addon::Model_status::CONFIRMED;
    sample.source              = "fixture.request";
    {
        auto guard = observer.lock();
        observer.bind(binding);
    }

    const auto cursor = binding->nonce + ":0";
    addon::Contract_failure failure;
    struct Window { unsigned version; const char* model; std::uint64_t sequence; unsigned wakes; };
    const Window windows[] = {
        {0, "root-model", 615, 0}, {1, "root-model", 615, 1}, {1, "root-model", 615, 0},
        {1, "root-model", 782, 0}, {2, "second-model", 890, 0}, {0, "second-model", 890, 0},
        {1, "second-model", 890, 1}
    };
    for (const auto& [version, model, sequence, wakes] : windows) {
        {
            auto guard = observer.lock();
            sample.model = model;
            sample.sequence = sequence;
            binding->model = sample;
        }
        addon::Observe_request watch;
        watch.session.provider_session_id = binding->session;
        watch.session.working_directory   = ".";
        watch.session.launcher_argv       = {"fixture"};
        watch.window_ms                   = addon::limits::k_observe_window_ms;
        watch.correlation                 = correlation;
        watch.model_state_version         = version;
        Context observe;
        observe.invocation.capability = {"lifecycle-events", 2};
        observe.invocation.operation  = "observe";
        observe.invocation.payload    = addon::encode_observe_request(watch);
        require(observer.handle(observe) && observe.result, "model-only observe failed");
        require(observe.progress_calls == wakes, "only a newly negotiated model change should wake the host");
        const auto window = addon::decode_observe_result(*observe.result, &failure);
        require(window && window->resume_cursor == cursor, "model-only observe changed the lifecycle cursor");

        addon::Poll_observations_request read;
        read.correlation         = correlation;
        read.limit               = 1;
        read.resume_cursor       = cursor;
        read.model_state_version = version;
        Context poll;
        poll.invocation.capability = {"lifecycle-events", 2};
        poll.invocation.operation  = "poll_observations";
        poll.invocation.payload    = addon::encode_poll_observations_request(read);
        require(observer.handle(poll) && poll.result, "model-only poll failed");
        const auto page = addon::decode_poll_observations_result(*poll.result, read.limit, &failure);
        require(page && page->observations.empty() && page->resume_cursor == cursor && !page->more,
            "model-only poll changed the lifecycle page");
        const std::vector<addon::Model_update> expected =
            version == addon::k_model_state_version ? std::vector{sample} : std::vector<addon::Model_update>{};
        require(window->model_updates == expected && page->model_updates == expected,
            "observe and poll must carry the source sample only when requested");
        require(window->model_failure.has_value() == (version > 1) && page->model_failure.has_value() == (version > 1),
            "unsupported model requests must leave lifecycle usable");
    }
}

void exception_releases_observe_window(bool fail_progress)
{
    auto service = std::make_shared<observation::Service>();
    addon::Filesystem_lifecycle_observer observer(service, {});
    const addon::Session_correlation correlation{std::string(32, 'a')};
    Context reserve;
    reserve.invocation.capability = {"lifecycle-correlation-lifetime", 1};
    reserve.invocation.operation = "reserve";
    reserve.invocation.payload = addon::encode_lifecycle_correlation_reserve_request({correlation});
    require(observer.handle(reserve) && reserve.result.has_value(), "reservation failed");

    auto binding = std::make_shared<Binding>();
    binding->nonce = correlation.transition_nonce;
    binding->session = "root-session";
    addon::Lifecycle_observation event;
    event.provider_event_name = "root_started";
    event.provider_session_id = binding->session;
    event.session_transition = addon::Session_transition::STARTED;
    {
        auto guard = observer.lock();
        observer.bind(binding);
        require(binding->publish(event), "root event was refused");
    }

    addon::Observe_request request;
    request.session.provider_session_id = binding->session;
    request.session.working_directory = ".";
    request.session.launcher_argv = {"fixture"};
    request.window_ms = addon::limits::k_observe_window_ms;
    request.correlation = correlation;
    Context first;
    first.invocation.capability = {"lifecycle-events", 2};
    first.invocation.operation = "observe";
    first.invocation.payload = addon::encode_observe_request(request);
    first.fail_progress = fail_progress;
    first.fail_wait = !fail_progress;
    require(observer.handle(first), "first observe was not handled");
    const std::string expected = fail_progress ? "progress_failed" : "wait_failed";
    require(first.failure_status == addon::Result_status::FAILED && first.failure_reason == expected,
        "first observe did not reach the injected callback: " + first.failure_reason);

    Context next;
    next.invocation = first.invocation;
    require(observer.handle(next), "next observe was not handled");
    require(next.result.has_value(), "next observe failed: " + next.failure_reason);
    addon::Contract_failure failure;
    const auto result = addon::decode_observe_result(*next.result, &failure);
    require(result && result->resume_cursor == binding->nonce + ":1" && next.progress_calls == 1,
        "next observe lost the accepted event or continuation cursor");

    addon::Poll_observations_request poll_request;
    poll_request.correlation = correlation;
    poll_request.limit = 1;
    Context poll;
    poll.invocation.capability = {"lifecycle-events", 2};
    poll.invocation.operation = "poll_observations";
    poll.invocation.payload = addon::encode_poll_observations_request(poll_request);
    require(observer.handle(poll) && poll.result.has_value(), "poll failed after observe exception");
    const auto page = addon::decode_poll_observations_result(*poll.result, poll_request.limit, &failure);
    require(page && page->observations.size() == 1 &&
        page->observations.front().provider_event_name == event.provider_event_name,
        "observe exception consumed the accepted root fact");
    std::cout << expected << ": next valid observe and retained root fact passed\n";
}

} // namespace

int main()
{
    try {
        exception_releases_observe_window(true);
        exception_releases_observe_window(false);
        model_only_results_keep_the_lifecycle_cursor();
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
