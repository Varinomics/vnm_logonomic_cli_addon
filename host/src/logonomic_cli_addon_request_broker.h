#pragma once

#include "logonomic_cli_addon_generation_client.h"
#include "logonomic_cli_addon_request_status.h"
#include "logonomic_cli_addon/limits.h"

#include <QHash>

#include <deque>
#include <functional>
#include <memory>
#include <optional>

namespace logonomic::cli_addon_host {

struct Broker_request
{
    QByteArray          correlation_id;
    Declared_capability capability;
    QString             operation;
    QJsonObject         payload;
    int                 deadline_ms = cli_addon::limits::k_default_invoke_deadline_ms;
    bool                deadline_bounds_silence = false;
};

struct Request_failure
{
    Logonomic_cli_capability_status status;
    QString                         reason_code;
    QString                         message;
};

enum class Request_completion
{
    LOCAL,              // Before transport submission
    SEND_REFUSED,       // Transport refused after preparation, without retiring
    ADAPTER_RESULT,     // Correlated terminal response
    RETIREMENT,         // Native resource settlement is independent of this result
};

struct Broker_result
{
    Logonomic_cli_capability_status status;
    QString                         reason_code;
    QString                         message;
    QJsonObject                     payload;
    Request_completion              completion = Request_completion::LOCAL;
    bool                            deadline_expired = false;
};

/// Owns invocation policy for one generation, on the client's Qt thread.
/// The client outlives the broker. Use a distinct correlation ID for each
/// invocation. Completion may be synchronous, including during send or cancel.
class Request_broker final : public QObject
{
public:
    using Completion = std::function<void(Broker_result)>;
    using Prepare = std::function<std::optional<Request_failure>(QJsonObject&)>;

    explicit Request_broker(Generation_client& client);
    // Hosts that allocate resources before invoking can reject a closed or
    // full generation before doing that work. invoke enforces the bound.
    std::optional<Request_failure> admission_failure() const;
    // Optional consumer-specific authorization or resource acquisition runs
    // only after negotiation and immediately before the transport submission.
    void invoke(Broker_request request, Completion complete, Prepare prepare = {});
    void cancel(const QByteArray& correlation_id);
    void begin_shutdown();

#ifdef LOGONOMIC_ENABLE_CLI_PROVIDER_TEST_HOOKS
    void expire_armed_requests_for_tests();
#endif

private:
    struct Pending;
    void dispatch(const QByteArray& correlation_id);
    void complete(const QByteArray& correlation_id, Broker_result result);
    void receive_traffic();
    void expire(const QByteArray& correlation_id);
    void start_cancel(const QByteArray& correlation_id, bool deadline_expired);
    void retired(const QString& reason_code, const QString& detail);

    Generation_client&                             m_client;
    QHash<QByteArray, std::shared_ptr<Pending>>    m_pending;
    std::deque<QByteArray>                         m_queue;
    QByteArray                                     m_grace_culprit;
    bool                                           m_closing = false;
};

} // namespace logonomic::cli_addon_host
