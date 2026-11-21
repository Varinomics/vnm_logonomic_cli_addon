#include "logonomic_cli_addon_request_broker.h"
#include "logonomic_cli_addon_json_bridge.h"
#include "logonomic_cli_addon/bootstrap.h"
#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/launcher_discovery.h"
#include "logonomic_cli_addon/version.h"
#include "test_profile.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QTimer>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <signal.h>
#include <cerrno>
#endif

#include <cstdio>
#include <filesystem>
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

enum class Stop { ORDERLY, DIRECT_SHUTDOWN, FORCED, FAULT, SHUTDOWN_DURING_IDLE };

bool exercise_activation(host::Activation_context context, Stop stop)
{
    const auto& pair = context.adopted_capabilities.front();
    const auto scratch = QDir(context.roots.scratch_root).filePath(
        context.record.package_id + '/' + QString::fromLatin1(context.activation_nonce));
    host::Generation_client client(context);
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

    ok &= check(results == 1 && client.custody_finished() && !writer_lock,
        "one discovery result, settled native custody and released writer lock");
    ok &= check(!QFileInfo::exists(scratch), "settlement released activation scratch");
    for (auto pid : pids) {
        ok &= check(pid > 0 && process_exited(pid), "each recorded native process exited before completion");
    }
    switch (stop) {
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
    if (!scenario.isEmpty()) {
        const char* mode = scenario == "fault" ? "crash-on-shutdown" : "slow-shutdown";
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
    if (scenario == "forced")   return exercise_activation(context, Stop::FORCED) ? 0 : 1;
    if (scenario == "fault")    return exercise_activation(context, Stop::FAULT) ? 0 : 1;
    if (scenario == "shutdown") return exercise_activation(context, Stop::SHUTDOWN_DURING_IDLE) ? 0 : 1;
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
