#include "logonomic_cli_addon_generation_client.h"
#include "logonomic_cli_addon_json_bridge.h"
#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/launcher_discovery.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QUuid>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <signal.h>
#include <cerrno>
#endif

#include <cstdio>

namespace {

namespace host = logonomic::cli_addon_host;
namespace sdk  = logonomic::cli_addon;

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

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    const QStringList args = application.arguments();
    if (!check(args.size() == 4, "expected a package root, custodian and supervisor")) {
        return 1;
    }
    QTemporaryDir profile;
    if (!check(profile.isValid(), "create isolated profile")) {
        return 1;
    }

    const auto roots    = host::Roots::defaults(profile.path(), args[1]);
    const auto platform = QString::fromLatin1(sdk::host_platform_id());
    const auto registry = host::Registry::discover(roots, {platform, {1}});
    const auto record   = registry->generation("org.example.unknown-fixture-provider-surfaces");
    if (!record) {
        for (const auto& rejected : registry->unavailable()) {
            std::fprintf(stderr, "%s: %s\n", qPrintable(rejected.package_id), qPrintable(rejected.detail));
        }
        std::fprintf(stderr, "FAIL: registry did not admit the SDK launcher fixture\n");
        return 1;
    }
    const host::Declared_capability pair{QStringLiteral("launcher-discovery"), 1};
    const auto scratch = QDir(roots.scratch_root).filePath(
        record->package_id + '/' + QString::fromLatin1(record->generation_nonce_hex));
    host::Generation_client client({*record, roots, args[2], args[3], platform, {pair}});
    const QByteArray correlation = QUuid::createUuid().toByteArray(QUuid::WithoutBraces);
    QList<qint64> pids;
    bool ok = true;
    bool discovered = false;
    bool orderly_shutdown = false;
    QObject::connect(&client, &host::Generation_client::ready, &application, [&]() {
        pids.push_back(client.custodian_process_id());
        for (auto pid : client.descendant_process_ids()) {
            pids.push_back(pid);
        }
        ok &= check(pids.size() == 3 && !pids.contains(0), "native custodian, supervisor and adapter started");
        const auto live = client.live_capabilities();
        ok &= check(live.size() == 1 && live.front().capability_id == pair.capability_id &&
            live.front().version == pair.version, "the declared and adopted discovery pair negotiated");
        ok &= check(QFileInfo(scratch).isDir(), "active generation holds its scratch directory");

        const auto payload = sdk::encode_discover_request(
            sdk::Json_value::object(), profile.path().toStdString(), sdk::host_platform_id());
        const auto sent = client.send_invoke(correlation, pair.capability_id, pair.version, "discover",
            logonomic_cli_addon_to_qjson(payload).toObject(), 5000);
        ok &= check(sent.sent, qPrintable(sent.message));
        if (!sent.sent) {
            client.begin_shutdown();
        }
    });
    QObject::connect(&client, &host::Generation_client::result_received, &application,
        [&](const QByteArray& id, const QString& status, const QString&, const QString& message,
            const QJsonObject& payload) {
            ok &= check(id == correlation, "result belongs to the discovery request");
            ok &= check(status == "success", qPrintable(message));
            sdk::Contract_failure failure;
            const auto candidates = sdk::decode_discover_result(
                logonomic_cli_addon_from_qjson(payload), sdk::host_platform_id(), &failure);
            discovered = candidates && candidates->size() == 1 &&
                candidates->front().candidate_id == "unknown_fixture" &&
                candidates->front().script_body == "unknown-fixture-cli";
            ok &= check(discovered, "discover returned the fixture's launcher");
            client.begin_shutdown();
        });
    QObject::connect(&client, &host::Generation_client::retired, &application,
        [&](host::Retirement_reason_kind kind, const QString& reason, const QString& detail) {
            orderly_shutdown = kind == host::Retirement_reason_kind::SHUTDOWN;
            if (!orderly_shutdown) {
                std::fprintf(stderr, "Generation retired: %s: %s\n", qPrintable(reason), qPrintable(detail));
            }
        });
    QObject::connect(&client, &host::Generation_client::finished, &application, &QCoreApplication::quit);
    client.start();
    application.exec();

    ok &= check(discovered && orderly_shutdown && client.custody_finished(), "discovery and native shutdown completed");
    ok &= check(!QFileInfo::exists(scratch), "native shutdown released generation scratch");
    for (auto pid : pids) {
        ok &= check(pid > 0 && process_exited(pid), "each recorded native process exited before completion");
    }
    return ok ? 0 : 1;
}
