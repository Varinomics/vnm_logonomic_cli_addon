#include "logonomic_cli_addon_request_broker.h"
#include "logonomic_cli_addon_json_bridge.h"
#include "logonomic_cli_addon/bootstrap.h"
#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/launcher_discovery.h"
#include "logonomic_cli_addon/version.h"
#include "test_profile.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QThread>
#include <QTimer>

#if defined(__APPLE__) && defined(LOGONOMIC_ENABLE_CLI_PROVIDER_TEST_HOOKS)
#include <vnm_process_custody/process_pipe.h>
#endif

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <signal.h>
#include <cerrno>
#endif

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

namespace {

namespace host = logonomic::cli_addon_host;
namespace sdk  = logonomic::cli_addon;
namespace fs   = std::filesystem;
using Outcome  = host::Idle_stop_outcome;
using Reason   = host::Retirement_reason_kind;
using State    = Logonomic_cli_addon_activation_state;

bool check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
    return condition;
}

bool process_exited(qint64 pid)
{
#ifdef Q_OS_WIN
    const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!process) {
        return GetLastError() == ERROR_INVALID_PARAMETER;
    }
    const bool exited = WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
    CloseHandle(process);
    return exited;
#else
    return kill(static_cast<pid_t>(pid), 0) == -1 && errno == ESRCH;
#endif
}

bool set_shutdown_fixture(const QString& package, const char* mode)
{
    QFile file(QDir(package).filePath(QString::fromLatin1(sdk::k_bootstrap_file_name)));
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    sdk::Contract_failure failure;
    auto manifest = sdk::parse_bootstrap(file.readAll().toStdString(), &failure);
    file.close();
    if (!manifest) {
        return check(false, failure.detail.c_str());
    }
    manifest->adapter_argv.insert(manifest->adapter_argv.end(), {"--probe-mode", mode});
    const auto bytes = QByteArray::fromStdString(sdk::serialize_bootstrap(*manifest));
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size() && file.flush();
}

QByteArray adapter_hash(const QString& path)
{
    QFile file(path);
    QCryptographicHash digest(QCryptographicHash::Sha256);
    return file.open(QIODevice::ReadOnly) && digest.addData(&file) ? digest.result() : QByteArray{};
}

std::atomic_uint32_t g_unconfirmed_custodian = 0;

const logonomic::Native_child_custody_callbacks* unconfirmed_native_family()
{
    static const logonomic::Native_child_custody_callbacks callbacks{
        .reserve = [](void** record) {
            *record = new std::uint32_t{};
            return static_cast<int>(logonomic::Native_child_custody::Admission::ADMITTED);
        },
        .job_handle = [](void*) -> std::uintptr_t { return 0; },
        .publish_created = [](void* record, std::uint32_t pid, std::uintptr_t) {
            *static_cast<std::uint32_t*>(record) = pid;
            return true;
        },
        .cancel_uncreated = [](void*) {},
        .observe_reaped = [](void* record) {
            g_unconfirmed_custodian.store(*static_cast<std::uint32_t*>(record));
            return false;
        },
        .handoff_wait_authority = [](void*) {},
        .destroy = [](void* record) { delete static_cast<std::uint32_t*>(record); }
    };
    return &callbacks;
}

enum class Stop { ORDERLY, DIRECT_SHUTDOWN, FORCED, FAULT, SHUTDOWN_DURING_IDLE, UNSOLICITED_EXIT, UNCONFIRMED };

bool exercise_activation(host::Activation_context context, Stop stop,
    const std::function<void(host::Generation_client&)>& configure = {})
{
    const auto& pair = context.adopted_capabilities.front();
    const auto scratch = QDir(context.roots.scratch_root).filePath(
        context.record.package_id + '/' + QString::fromLatin1(context.activation_nonce));
    host::Generation_client client(context);
    if (configure) configure(client);
    host::Request_broker broker(client);
    QEventLoop loop;
    QTimer writer_check;
    std::unique_ptr<host::Run_lock> writer_lock;
    QList<qint64> pids;
    std::optional<Reason> retirement;
    bool ok = true;
    bool late_refused = false;
    int results = 0;
    ok &= check(!broker.begin_idle_stop() && !client.custodian_process_id(), "unstarted package cannot idle-stop");

    QObject::connect(&client, &host::Generation_client::ready, &loop, [&]() {
        pids.push_back(client.custodian_process_id());
        for (auto pid : client.descendant_process_ids()) {
            pids.push_back(pid);
        }
        ok &= check(pids.size() == 3 && !pids.contains(0), "native custodian, supervisor and adapter started");
        const auto live = client.live_capabilities();
        ok &= check(live.size() == 1 && live.front().capability_id == pair.capability_id &&
            live.front().version == pair.version, "the declared and adopted discovery pair negotiated");
        ok &= check(client.activation_nonce() == context.activation_nonce && QFileInfo(scratch).isDir(),
            "handshake and scratch belong to this activation");
        ok &= check(!broker.begin_idle_stop(), "cannot stop a package with discovery in flight");
    });
    QObject::connect(&client, &host::Generation_client::retired, &loop,
        [&](Reason kind, const QString& reason, const QString& detail) {
            ok &= check(!retirement, "one terminal disposition per activation");
            retirement = kind;
            if (kind != Reason::IDLE_STOP && kind != Reason::SHUTDOWN) {
                std::fprintf(stderr, "Generation retired: %s: %s\n", qPrintable(reason), qPrintable(detail));
            }
            if (stop == Stop::UNCONFIRMED && g_unconfirmed_custodian.load() != 0) {
                loop.quit();
            }
        });
    QObject::connect(&client, &host::Generation_client::finished, &loop, &QEventLoop::quit);

    // Hold the actual admission lock while the custodian closes writers. Even
    // after both descendants exit, scratch and the idle decision must wait.
    QObject::connect(&writer_check, &QTimer::timeout, &loop, [&]() {
        if (pids.size() == 3 && process_exited(pids[1]) && process_exited(pids[2])) {
            ok &= check(!client.custody_finished() && !retirement && client.idle_stop_outcome() == Outcome::NONE &&
                QFileInfo(scratch).isDir(), "descendant exit alone must not settle an idle stop");
            writer_check.stop();
            writer_lock.reset();
        }
    });

    QTimer cleanup_check;
    if (stop == Stop::UNCONFIRMED) {
        QObject::connect(&cleanup_check, &QTimer::timeout, &loop, [&]() {
            if (g_unconfirmed_custodian.load() != 0) {
                cleanup_check.stop();
                // The reader has its final answer. Bound the wait for the
                // broker-thread disposition even if it is never delivered.
                QTimer::singleShot(1000, &loop, &QEventLoop::quit);
            }
        });
        cleanup_check.start(25);
    }
#if defined(__APPLE__) && defined(LOGONOMIC_ENABLE_CLI_PROVIDER_TEST_HOOKS)
    if (configure) {
        // A discarded receipt withholds finished(). End the test once the
        // reader has published its real terminal result in either case.
        QObject::connect(&cleanup_check, &QTimer::timeout, &loop, [&]() {
            if (client.native_cleanup_result_for_tests().has_value()) loop.quit();
        });
        cleanup_check.start(25);
    }
#endif

    const auto request = sdk::encode_discover_request(
        sdk::Json_value::object(), context.roots.per_user_root.toStdString(), sdk::host_platform_id());
    broker.invoke({"discover", pair, QStringLiteral("discover"), logonomic_cli_addon_to_qjson(request).toObject()},
        [&](host::Broker_result result) {
            ++results;
            sdk::Contract_failure failure;
            const auto candidates = sdk::decode_discover_result(
                logonomic_cli_addon_from_qjson(result.payload), sdk::host_platform_id(), &failure);
            ok &= check(result.status == Logonomic_cli_capability_status::SUCCESS &&
                result.completion == host::Request_completion::ADAPTER_RESULT, qPrintable(result.message));
            ok &= check(candidates && candidates->size() == 1 && candidates->front().candidate_id == "unknown_fixture" &&
                candidates->front().script_body == "unknown-fixture-cli", "discover returned the fixture's launcher");
            if (stop == Stop::DIRECT_SHUTDOWN || !ok) {
                client.begin_shutdown();
                return;
            }
            if (stop == Stop::UNSOLICITED_EXIT) {
                // The fixture exits zero immediately after this result. Keep its
                // ordered exit callback queued until native exit is an OS fact.
                QDeadlineTimer deadline(sdk::limits::k_reap_deadline_ms);
                while (!process_exited(pids.back()) && !deadline.hasExpired()) {
                    QThread::msleep(5);
                }
                ok &= check(process_exited(pids.back()), "adapter exited before the host requested a stop");
                (void)broker.begin_idle_stop();
                return;
            }
            if (stop == Stop::ORDERLY) {
                QString error;
                const QString lock = QDir(client.usage_writer_generation()).filePath(QStringLiteral(".lock"));
                writer_lock = host::Run_lock::try_acquire(lock, &error);
                ok &= check(writer_lock != nullptr, qPrintable(error));
                writer_check.start(25);
            }
            if (!check(broker.begin_idle_stop(), "a drained, ready broker must commit the stop")) {
                writer_lock.reset();
                ok = false;
                client.begin_shutdown();
                return;
            }
            ok &= check(client.state() == State::IDLE_STOPPING && client.idle_stop_outcome() == Outcome::NONE &&
                !broker.begin_idle_stop(), "idle commitment is one-shot and awaits native confirmation");
            broker.invoke({"late", pair, QStringLiteral("discover")}, [&](host::Broker_result late) {
                late_refused = late.status == Logonomic_cli_capability_status::PACKAGE_UNAVAILABLE &&
                    late.completion == host::Request_completion::LOCAL;
            });
            ok &= check(late_refused, "work arriving after stop commitment must remain unsent");
            if (stop == Stop::SHUTDOWN_DURING_IDLE) {
                broker.begin_shutdown();
            }
        });
    loop.exec();

    ok &= check(results == 1 && !writer_lock, "one discovery result and released writer lock");
    for (auto pid : pids) {
        ok &= check(pid > 0 && process_exited(pid), "each recorded native process exited before completion");
    }
    if (stop != Stop::UNCONFIRMED) {
        ok &= check(client.custody_finished() && !QFileInfo::exists(scratch),
            "settled native custody releases activation scratch");
    }
    switch (stop) {
        case Stop::UNCONFIRMED: {
            QString error;
            const auto lock = host::Run_lock::try_acquire(scratch + QLatin1String(host::k_run_lock_suffix), &error);
            ok &= check(retirement == Reason::CUSTODIAN_LOST && client.state() == State::TERMINALLY_UNAVAILABLE &&
                g_unconfirmed_custodian.load() == client.custodian_process_id(),
                "the native family's final denial retires the original custodian");
            ok &= check(!client.custody_finished() && client.idle_stop_outcome() == Outcome::NONE &&
                QFileInfo(scratch).isDir() && !lock && error.isEmpty(),
                "unconfirmed custody retains scratch and its live lock");
            bool refused = false;
            broker.invoke({"after-denial", pair, QStringLiteral("discover")}, [&](host::Broker_result result) {
                refused = result.status == Logonomic_cli_capability_status::PACKAGE_UNAVAILABLE &&
                    result.reason_code == QLatin1String(sdk::reason::k_custodian_lost) &&
                    result.completion == host::Request_completion::LOCAL;
            });
            return check(refused, "future work fails immediately with the terminal custody reason") && ok;
        }
        case Stop::ORDERLY:
            return check(retirement == Reason::IDLE_STOP && client.idle_stop_outcome() == Outcome::ORDERLY,
                "cooperative shutdown confirmed as orderly idle") && ok;
        case Stop::FORCED:
#ifdef Q_OS_MACOS
            // The Darwin owner confirms collection but cannot attribute an exit to its stop command.
            return check(retirement && retirement != Reason::IDLE_STOP && retirement != Reason::SHUTDOWN &&
                client.idle_stop_outcome() == Outcome::NONE, "ambiguous forced exit remains a fault") && ok;
#else
            return check(retirement == Reason::IDLE_STOP && client.idle_stop_outcome() == Outcome::FORCED,
                "only the confirmed host-forced exit receives the grace exception") && ok;
#endif
        case Stop::FAULT:
            return check(retirement == Reason::ADAPTER_EXITED && client.idle_stop_outcome() == Outcome::NONE,
                "unexpected nonzero adapter exit while stopping must remain a fault") && ok;
        case Stop::UNSOLICITED_EXIT:
            return check(retirement && retirement != Reason::IDLE_STOP && retirement != Reason::SHUTDOWN &&
                client.idle_stop_outcome() == Outcome::NONE, "earlier unsolicited zero exit must remain a fault") && ok;
        case Stop::DIRECT_SHUTDOWN:
        case Stop::SHUTDOWN_DURING_IDLE:
            return check(retirement == Reason::SHUTDOWN && client.idle_stop_outcome() == Outcome::NONE,
                "owner shutdown never authorizes a successor") && ok;
    }
    return false;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    const QStringList args = application.arguments();
    if (!check(args.size() == 4 || args.size() == 5, "expected fixtures, custodian, supervisor and optional scenario")) {
        return 1;
    }
    host::tests::Test_profile profile;
    const QString id = QStringLiteral("org.example.unknown-fixture-provider-surfaces");
    const auto& roots = profile.roots;
    const QDir installation(QDir(roots.per_user_root).filePath(id));
    if (!check(profile.directory.isValid() && QDir().mkpath(roots.per_user_root), "create isolated profile")) {
        return 1;
    }
    std::error_code error;
    fs::copy(QDir(QDir(args[1]).filePath(id)).filesystemPath(), installation.filesystemPath(),
        fs::copy_options::recursive, error);
    if (!check(!error, "copy the installed discovery fixture")) {
        return 1;
    }
    const QString scenario = args.value(4);
    if (!scenario.isEmpty() && scenario != "unconfirmed") {
        const char* mode = scenario == "unsolicited_exit" ? "result-then-exit" :
                           scenario == "fault"            ? "crash-on-shutdown" : "slow-shutdown";
        if (!check(set_shutdown_fixture(installation.path(), mode), "set fixture shutdown behavior")) {
            return 1;
        }
    }
    const auto platform = QString::fromLatin1(sdk::host_platform_id());
    const auto registry = host::Registry::discover(roots, {platform, {1}});
    const auto record = registry->generation(id);
    if (!record) {
        for (const auto& rejected : registry->unavailable()) {
            std::fprintf(stderr, "%s: %s\n", qPrintable(rejected.package_id), qPrintable(rejected.detail));
        }
        return 1;
    }
    host::Activation_context context{*record, roots, args[2], args[3], platform,
        {{QStringLiteral("launcher-discovery"), 1}}};
    context.activation_nonce = QByteArray::fromStdString(sdk::random_nonce_hex());
#if defined(__APPLE__) && defined(LOGONOMIC_ENABLE_CLI_PROVIDER_TEST_HOOKS)
    if (scenario == "closed_control") {
        std::atomic_bool exited_before_write = false;
        std::atomic_bool peer_closed_at_write = false;
        const auto probe = [&](int exit_observer, std::optional<bool> peer_closed) {
            if (peer_closed.has_value()) {
                peer_closed_at_write.store(*peer_closed);
                return;
            }
            // Keep the force request unsent while the slow fixture cooperates.
            // The original custodian queues Reaped and exits. Its observer
            // becomes readable without consuming the generation reader's wait.
            vnm::process_custody::Wait_entry exit;
            exit.handle    = exit_observer;
            exit.want_read = true;
            const int timeout_ms = 5 * sdk::limits::k_shutdown_grace_ms;
            exited_before_write.store(vnm::process_custody::wait_ready(&exit, 1, timeout_ms) == 1 && exit.readable);
        };
        const bool orderly = exercise_activation(context, Stop::ORDERLY, [&](host::Generation_client& client) {
            client.set_control_write_probe_for_tests(probe);
        });
        return check(exited_before_write.load() && peer_closed_at_write.load(),
            "the real control write found the exited custodian's socket closed") && orderly ? 0 : 1;
    }
#endif
    if (scenario == "unconfirmed") {
        context.native_family = &unconfirmed_native_family;
        return exercise_activation(context, Stop::UNCONFIRMED) ? 0 : 1;
    }
    if (scenario == "forced")   return exercise_activation(context, Stop::FORCED) ? 0 : 1;
    if (scenario == "fault")    return exercise_activation(context, Stop::FAULT) ? 0 : 1;
    if (scenario == "shutdown") return exercise_activation(context, Stop::SHUTDOWN_DURING_IDLE) ? 0 : 1;
    if (scenario == "unsolicited_exit") return exercise_activation(context, Stop::UNSOLICITED_EXIT) ? 0 : 1;
    if (!check(scenario.isEmpty(), "unknown test scenario")) {
        return 1;
    }

    {
        host::Generation_client unstarted(context);
        host::Request_broker broker(unstarted);
        broker.begin_shutdown();
        if (!check(!unstarted.custodian_process_id() && unstarted.idle_stop_outcome() == Outcome::NONE &&
            broker.admission_failure().has_value(), "shutting down an unstarted broker seals it without activation"))
        {
            return 1;
        }
    }
    const auto frozen = QDir(record->custody_path).filePath(record->adapter_relative_path);
    QFile installed(installation.filePath(record->adapter_relative_path));
    const auto original = adapter_hash(installed.fileName());
    if (!check(!original.isEmpty() && adapter_hash(frozen) == original, "admitted bytes match installation") ||
        !check(context.activation_nonce != record->generation_nonce_hex, "separate snapshot and activation identities") ||
        !exercise_activation(context, Stop::ORDERLY))
    {
        return 1;
    }
    const QByteArray replacement("changed installation");
    if (!check(installed.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
        installed.write(replacement) == replacement.size() && installed.flush(), "replace installed adapter bytes"))
    {
        return 1;
    }
    installed.close();
    host::Generation_custody::recover_stale(roots.custody_root);
    const auto first_nonce = context.activation_nonce;
    context.activation_nonce = QByteArray::fromStdString(sdk::random_nonce_hex());
    if (!check(adapter_hash(frozen) == original && adapter_hash(installed.fileName()) != original,
            "the registry kept its original locked snapshot across the stop") ||
        !check(context.activation_nonce != first_nonce, "targeted wake uses a new activation identity"))
    {
        return 1;
    }
    return exercise_activation(context, Stop::DIRECT_SHUTDOWN) ? 0 : 1;
}
