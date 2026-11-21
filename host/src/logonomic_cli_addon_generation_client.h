#pragma once

#include "logonomic_cli_addon_registry.h"
#include "logonomic_cli_addon_roots.h"
#include "logonomic_cli_addon_generation_types.h"
#include "logonomic_cli_addon_native_family.h"

#include "logonomic_cli_addon/transcript.h"

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace logonomic::cli_addon_host {

/// Everything one adapter generation is started from. The custodian and
/// supervisor executables ship beside the application; the adopted pairs are
/// the capabilities this host consumes, and a pair is live only when its
/// sidecar, the adapter's handshake and this list all declare it.
struct Activation_context
{
    Generation_record          record;
    Roots                      roots;
    QString                    custodian_executable;
    QString                    supervisor_executable;
    QString                    platform_id;
    QList<Declared_capability> adopted_capabilities;
    // When embedded, read the runtime owner's active family at native admission.
    const Native_child_custody_callbacks* (*native_family)() = nullptr;
    // A cycling host supplies a fresh nonce for every one-shot client. A
    // single-activation host can use the registry's freshly issued nonce.
    QByteArray activation_nonce;
};

enum class Retirement_reason_kind
{
    ADAPTER_EXITED,
    SUPERVISOR_LOST,
    CUSTODIAN_LOST,
    PROTOCOL_VIOLATION,
    ACTIVATION_TIMEOUT,
    CANCEL_GRACE_EXPIRED,
    SHUTDOWN,
    IDLE_STOP,
};

enum class Idle_stop_outcome { NONE, ORDERLY, FORCED };

/// What send_invoke() did with a request: put it on the wire, or refused it
/// locally with a reason code from the capability vocabulary
/// (`payload_too_large`, `backpressured`) or because the generation is not
/// READY (the retirement reason, or `registry_not_settled` before activation).
struct Send_outcome
{
    bool    sent = false;
    QString reason_code;
    QString message;
};

/**
 * @brief The desktop end of one add-on package generation: transfers custody
 * initialization, process ownership and transport I/O to one joinable worker,
 * speaks core protocol generation 1 to the adapter through the SDK codec and
 * Session_tracker, and reports everything to the broker as signals on the
 * broker's thread.
 *
 * Each client owns one activation. A ready, drained client may enter
 * IDLE_STOPPING. Its native and writer settlement determines whether a
 * different client may use the frozen package again. Fault retirement is
 * terminal for the package. The desktop waits only its direct custodian
 * child and never a provider descendant. A retained generation scope is
 * admitted before birth and observed empty before releasing scratch, including
 * after custodian loss. On ordinary Linux without delegated containment, the
 * custodian remains responsible for descendant settlement; its abnormal loss
 * permits exact recorded-identity signals but never confirmed completion.
 *
 * Every public method runs on the thread the object lives on (the broker's).
 * The finished() fact means the worker has reaped its direct custodian and
 * released scratch custody. The proxy retains the client until then, so the
 * destructor joins only an already-finished worker; there is no detach().
 */
class Generation_client final : public QObject
{
    Q_OBJECT

public:
    explicit Generation_client(Activation_context context, QObject* parent = nullptr);
    ~Generation_client() override;

    Generation_client(const Generation_client&)            = delete;
    Generation_client& operator=(const Generation_client&) = delete;

    // NOT_STARTED -> STARTING: transfers an immutable activation request to
    // the custody worker, then awaits `started`, the adapter hello and its
    // capabilities within limits::k_activation_deadline_ms. Emits ready() or
    // retired(); it performs no filesystem, process or transport I/O here.
    void start();

    Logonomic_cli_addon_activation_state state() const;
    const Activation_context& context() const;
    QByteArray activation_nonce() const;
    // Published at finished(), after native scope and writer settlement.
    Idle_stop_outcome idle_stop_outcome() const;
    // Immutable native admission identity. The custodian creates this registry
    // before provider birth; broker dispatch can only join existing admission.
    QString usage_writer_generation() const;
    // The reason code of a TERMINALLY_UNAVAILABLE generation; empty before.
    QString retirement_reason() const;

    // sidecar ∩ handshake ∩ adopted; empty before ready().
    QList<Declared_capability> live_capabilities() const;
    bool handshake_declares(const Declared_capability& pair) const;

    // Only after ready(). correlation_id is the broker's request identity;
    // the wire request ID is issued here and never leaves this class.
    Send_outcome send_invoke(
        const QByteArray&  correlation_id,
        const QString&     capability_id,
        uint32_t           version,
        const QString&     operation,
        const QJsonObject& payload,
        int                deadline_ms);
    // Idempotent; ignored for a request that is not live.
    void send_cancel(const QByteArray& correlation_id);
    // Graceful: the shutdown frame, limits::k_shutdown_grace_ms, then the
    // custodian's terminate/kill.
    void begin_shutdown();
    // Immediate: the custodian kills the tree at once. Used at cancel-grace
    // expiry and by the broker's own retirements.
    void retire(Retirement_reason_kind reason, QString detail);

    // Optional diagnostics sink shared with the SDK host simulator so both
    // record the same normalized transcript for the same adapter.
    void set_transcript_recorder(logonomic::cli_addon::Transcript_recorder* recorder);
    // A host-allocated root the transcript replaces with <root>.
    void note_transcript_root(const QString& root);
    std::size_t transcript_root_count() const;
    // Host-side observations the broker owns (deadline expiry, cancel-grace
    // expiry, profile-storage acknowledgment); request events name the
    // request by the broker's correlation id and are recorded under its wire
    // request ID.
    void record_event(const char* kind, const QString& detail = {});
    void record_request_event(const char* kind, const QByteArray& correlation_id);

    // The recorded process facts, for the custody gate's OS-truth checks.
    std::int64_t custodian_process_id() const;
    std::int64_t process_group() const;
    QList<std::int64_t> descendant_process_ids() const;   // supervisor, adapter
    // True only after the desktop reaped the custodian and observed all
    // process-tree terminal status that preceded its exit.
    bool custody_finished() const;

#ifdef LOGONOMIC_ENABLE_CLI_PROVIDER_TEST_HOOKS
    struct Inbound_mailbox_snapshot
    {
        std::uint64_t peak_retained_bytes = 0;
        std::size_t   peak_pending_notifications = 0;
    };
    // Thread-safe observations of the real worker/GUI admission boundary.
    Inbound_mailbox_snapshot inbound_mailbox_snapshot_for_tests() const;
    // Rejects one link-queue admission on the broker thread. Does not change
    // the production queue bound, retirement, or native cleanup path.
    void reject_next_link_enqueue_for_tests();
    // Fault-inject an unavailable native emptiness observation, never a
    // positive proof. Clearing the gate resumes the real retained-scope query.
    void set_native_cleanup_observation_withheld_for_tests(bool withheld);
    // Immutable birth-time fact, not derived from shutdown's result. Empty
    // until the worker attempts to establish independent containment.
    std::optional<bool> native_containment_available_for_tests() const;
    // The real terminal callback has processed this native cleanup result;
    // absence means it has not arrived, not that custody is settled.
    std::optional<bool> native_cleanup_result_for_tests() const;

    // Pauses the custody worker before its first filesystem, process or I/O
    // operation. The hook itself runs on that worker and is empty normally.
    static void set_custody_start_barrier_for_tests(std::function<void()> barrier);
    // Aligns concurrent workers immediately before package-family scratch
    // creation so the shared-root publication path can be exercised directly.
    static void set_scratch_creation_barrier_for_tests(std::function<void()> in_barrier);
#endif

signals:
    void starting();
    void ready();
    void retired(Retirement_reason_kind reason, QString reason_code, QString detail);
    void result_received(
        QByteArray  correlation_id,
        QString     status,        // success | unavailable | cancelled | error
        QString     reason_code,
        QString     message,
        QJsonObject payload);
    void progress_received(QByteArray correlation_id, Logonomic_cli_capability_progress progress);
    void custodian_error(QString reason_code, QString detail);
    void stderr_diagnostic(QByteArray chunk, bool truncated);
    // The custodian child was reaped by the desktop: nothing of this
    // generation's process tree remains under Logonomic custody.
    void finished();

private:
    friend class Request_broker;
    bool begin_idle_stop();
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace logonomic::cli_addon_host
