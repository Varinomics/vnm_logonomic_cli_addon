#include "logonomic_cli_addon_request_broker.h"

#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/protocol.h"

#include <QPointer>
#include <QScopedPointer>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace logonomic::cli_addon_host {
namespace {

using Status = Logonomic_cli_capability_status;
using State  = Logonomic_cli_addon_activation_state;
namespace sdk = logonomic::cli_addon;

Status result_status(const QString& status)
{
    switch (sdk::parse_result_status(status.toStdString()).value_or(sdk::Result_status::FAILED)) {
        case sdk::Result_status::SUCCESS:     return Status::SUCCESS;
        case sdk::Result_status::UNAVAILABLE: return Status::UNAVAILABLE;
        case sdk::Result_status::CANCELLED:   return Status::CANCELLED;
        case sdk::Result_status::FAILED:
        default:                              return Status::FAILED;
    }
}

std::optional<Request_failure> unavailable_pair(Generation_client& client, const Declared_capability& pair)
{
    const auto same_id = [&pair](const Declared_capability& other) {
        return pair.capability_id == other.capability_id;
    };
    const auto exact = [&same_id, &pair](const Declared_capability& other) {
        return same_id(other) && pair.version == other.version;
    };
    const auto live = client.live_capabilities();
    if (std::any_of(live.cbegin(), live.cend(), exact)) {
        return {};
    }
    const QString pair_text = QStringLiteral("%1@%2").arg(pair.capability_id).arg(pair.version);
    const auto& record = client.context().record;
    const QString message = QStringLiteral("capability %1 is not live for package %2")
        .arg(pair_text, record.package_id);
    if (std::any_of(live.cbegin(), live.cend(), same_id)) {
        client.record_event("unsupported_version", pair_text);
        return Request_failure{Status::UNSUPPORTED_VERSION, QString::fromLatin1(sdk::reason::k_unsupported_version), message};
    }
    const auto& declared = record.declared_capabilities;
    const bool disagreement = std::any_of(declared.cbegin(), declared.cend(), exact) && !client.handshake_declares(pair);
    client.record_event("unknown_capability", pair_text);
    return Request_failure{Status::UNKNOWN_CAPABILITY,
        QString::fromLatin1(disagreement ? sdk::reason::k_sidecar_handshake_disagreement : sdk::reason::k_unknown_capability),
        message};
}

} // namespace

struct Request_broker::Pending
{
    Pending(Broker_request call, Completion complete, Prepare authorize)
    :
        request(std::move(call)),
        callback(std::move(complete)),
        prepare(std::move(authorize))
    {}

    Broker_request request;
    Completion     callback;
    Prepare        prepare;
    // Detached terminal deliveries can outlive the broker. Each accepted
    // request owns its timer, including while its own timeout slot runs.
    QScopedPointer<QTimer, QScopedPointerDeleteLater> timer;
    bool           cancelling = false;
    bool           deadline_expired = false;

    void deliver(Broker_result result)
    {
        if (timer) {
            timer->stop();
            timer.reset();
        }
        result.deadline_expired = deadline_expired;
        callback(std::move(result));
    }
};

Request_broker::Request_broker(Generation_client& client)
:
    m_client(client)
{
    connect(&client, &Generation_client::ready, this, [this]() {
        const QPointer<Request_broker> owner(this);
        std::deque<QByteArray> queue;
        queue.swap(m_queue);
        for (const auto& id : queue) {
            if (!owner) {
                return;
            }
            owner->dispatch(id);
        }
    });
    connect(&client, &Generation_client::retired, this,
        [this](Retirement_reason_kind, const QString& reason, const QString& detail) { retired(reason, detail); });
    connect(&client, &Generation_client::progress_received, this, [this]() { receive_traffic(); });
    connect(&client, &Generation_client::result_received, this, [this](
        const QByteArray& id, const QString& status, QString reason, QString message, QJsonObject payload)
    {
        receive_traffic();
        complete(id, {result_status(status), std::move(reason), std::move(message),
            std::move(payload), Request_completion::ADAPTER_RESULT});
    });
}

Request_broker::~Request_broker()
{
    if (!m_pending.isEmpty()) {
        if (m_client.state() == State::TERMINALLY_UNAVAILABLE) {
            // Another retirement listener can release the broker before
            // Qt reaches its own retired connection.
            retired(m_client.retirement_reason(), QStringLiteral("the add-on generation has retired"));
        }
        else {
            begin_shutdown();
        }
    }
}

std::optional<Request_failure> Request_broker::admission_failure() const
{
    if (m_closing || m_client.state() == State::TERMINALLY_UNAVAILABLE) {
        return Request_failure{Status::PACKAGE_UNAVAILABLE,
            m_closing ? QString::fromLatin1(sdk::reason::k_shutdown) : m_client.retirement_reason(),
            QStringLiteral("the add-on generation is terminally unavailable")};
    }
    if (m_pending.size() >= static_cast<qsizetype>(sdk::limits::k_max_in_flight_requests)) {
        return Request_failure{Status::UNAVAILABLE, QString::fromLatin1(sdk::reason::k_backpressured),
            QStringLiteral("CLI provider request admission limit was reached.")};
    }
    return {};
}

void Request_broker::invoke(Broker_request request, Completion callback, Prepare prepare)
{
    Q_ASSERT(QThread::currentThread() == m_client.thread());
    const QByteArray id = request.correlation_id;
    if (auto failure = admission_failure()) {
        callback({failure->status, std::move(failure->reason_code), std::move(failure->message)});
        return;
    }
    if (id.isEmpty() || m_pending.contains(id)) {
        callback({Status::FAILED, {}, QStringLiteral("a distinct request correlation ID is required")});
        return;
    }
    if (request.deadline_ms <= 0) {
        request.deadline_ms = sdk::limits::k_default_invoke_deadline_ms;
    }
    m_pending.insert(id, std::make_shared<Pending>(
        std::move(request), std::move(callback), std::move(prepare)));
    if (m_client.state() == State::READY) {
        dispatch(id);
        return;
    }
    m_queue.push_back(id);
    if (m_client.state() == State::NOT_STARTED) {
        m_client.start();
    }
}

void Request_broker::dispatch(const QByteArray& id)
{
    // Preparation or link rejection can synchronously retire this generation.
    const auto pending = m_pending.value(id);
    if (!pending) {
        return;
    }
    const QPointer<Request_broker> owner(this);
    auto& request = pending->request;
    auto failure = unavailable_pair(m_client, request.capability);
    if (!failure && pending->prepare) {
        failure = pending->prepare(request.payload);
    }
    if (!owner || owner->m_pending.value(id) != pending) {
        return;
    }
    if (failure) {
        complete(id, {failure->status, std::move(failure->reason_code), std::move(failure->message)});
        return;
    }
    const Send_outcome outcome = m_client.send_invoke(id, request.capability.capability_id,
        request.capability.version, request.operation, request.payload, request.deadline_ms);
    if (!owner || owner->m_pending.value(id) != pending) {
        return;
    }
    if (!outcome.sent) {
        const Status status = outcome.reason_code == QLatin1String(sdk::reason::k_backpressured)
            ? Status::UNAVAILABLE : outcome.reason_code == QLatin1String(sdk::reason::k_payload_too_large)
            ? Status::FAILED : Status::PACKAGE_UNAVAILABLE;
        complete(id, {status, outcome.reason_code, outcome.message, {}, Request_completion::SEND_REFUSED});
        return;
    }
    pending->timer.reset(new QTimer);
    pending->timer->setSingleShot(true);
    pending->timer->setTimerType(Qt::PreciseTimer);
    connect(pending->timer.get(), &QTimer::timeout, this, [this, id]() { expire(id); });
    if (request.deadline_ms != sdk::limits::k_no_invoke_deadline_ms) {
        pending->timer->start(request.deadline_ms);
    }
}

void Request_broker::complete(const QByteArray& id, Broker_result result)
{
    const auto pending = m_pending.take(id);
    if (pending) {
        pending->deliver(std::move(result));
    }
}

// For silence-bounded operations, activity from any request on this adapter
// restarts the deadline. Traffic can never extend a cancellation grace.
void Request_broker::receive_traffic()
{
    for (const auto& pending : std::as_const(m_pending)) {
        const auto& request = pending->request;
        if (request.deadline_bounds_silence && pending->timer && !pending->cancelling &&
            request.deadline_ms != sdk::limits::k_no_invoke_deadline_ms)
        {
            pending->timer->start(request.deadline_ms);
        }
    }
}

void Request_broker::cancel(QByteArray id)
{
    const auto pending = m_pending.value(id);
    if (!pending || m_client.state() == State::TERMINALLY_UNAVAILABLE) {
        return;
    }
    if (!pending->timer) {
        std::erase(m_queue, id);
        complete(id, {Status::CANCELLED, QStringLiteral("cancelled"),
            QStringLiteral("cancelled before the adapter received the request")});
        return;
    }
    if (!pending->cancelling) {
        start_cancel(id, false);
    }
}

void Request_broker::start_cancel(const QByteArray& id, bool deadline_expired)
{
    const auto pending = m_pending.value(id);
    pending->cancelling       = true;
    pending->deadline_expired = deadline_expired;
    if (deadline_expired) {
        m_client.record_request_event("deadline_expired", id);
    }
    const QPointer<Request_broker> owner(this);
    m_client.send_cancel(id);
    if (owner && owner->m_pending.value(id) == pending) {
        pending->timer->start(sdk::limits::k_cancel_grace_ms);
    }
}

void Request_broker::expire(const QByteArray& id)
{
    const auto pending = m_pending.value(id);
    if (!pending || !pending->timer) {
        return;
    }
    if (!pending->cancelling) {
        start_cancel(id, true);
        return;
    }
    m_client.record_request_event("cancel_grace_expired", id);
    m_grace_culprit = id;
    m_client.retire(Retirement_reason_kind::CANCEL_GRACE_EXPIRED,
        QStringLiteral("no terminal result within the cancel grace for %1").arg(pending->request.operation));
}

void Request_broker::retired(const QString& reason, const QString& detail)
{
    m_queue.clear();
    // Commit every disposition before a terminal callback can re-enter and
    // cancel or close its siblings.
    const auto requests = std::exchange(m_pending, {});
    const auto culprit = m_grace_culprit;
    for (auto it = requests.cbegin(); it != requests.cend(); ++it) {
        const auto& pending = it.value();
        Status status = pending->timer ? Status::GENERATION_RETIRED : Status::PACKAGE_UNAVAILABLE;
        QString message = detail;
        if (it.key() == culprit) {
            status  = Status::TIMED_OUT;
            message = QStringLiteral("no terminal result arrived within the cancel grace");
        }
        pending->deliver({status, reason, std::move(message), {}, Request_completion::RETIREMENT});
    }
}

void Request_broker::begin_shutdown()
{
    if (std::exchange(m_closing, true) || m_client.state() == State::TERMINALLY_UNAVAILABLE) {
        return;
    }
    m_queue.clear();
    const auto requests = std::exchange(m_pending, {});
    m_client.begin_shutdown();
    for (const auto& pending : requests) {
        pending->deliver({Status::GENERATION_RETIRED, QString::fromLatin1(sdk::reason::k_shutdown),
            QStringLiteral("CLI provider proxy is shutting down."), {}, Request_completion::RETIREMENT});
    }
}

#ifdef LOGONOMIC_ENABLE_CLI_PROVIDER_TEST_HOOKS
void Request_broker::expire_armed_requests_for_tests()
{
    const QPointer<Request_broker> owner(this);
    const auto ids = m_pending.keys();
    for (const auto& id : ids) {
        if (!owner) {
            return;
        }
        const auto pending = owner->m_pending.value(id);
        if (pending && pending->timer && pending->timer->isActive()) {
            expire(id);
        }
    }
}
#endif

} // namespace logonomic::cli_addon_host
