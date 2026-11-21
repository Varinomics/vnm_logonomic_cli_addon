#include "logonomic_cli_addon_request_broker.h"
#include "logonomic_cli_addon/ids.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>
#include <filesystem>
#include <utility>

namespace host = logonomic::cli_addon_host;
namespace sdk  = logonomic::cli_addon;
namespace fs   = std::filesystem;
using Status   = Logonomic_cli_capability_status;

namespace {

bool check(bool passed, const char* message)
{
    if (!passed) {
        std::fprintf(stderr, "%s\n", message);
    }
    return passed;
}

bool exercise(const QStringList& arguments)
{
    QTemporaryDir profile;
    if (!check(profile.isValid(), "Could not allocate the fixture profile.")) {
        return false;
    }
    const QString ordinary = QStringLiteral("org.example.unknown-fixture");
    const QString hanging  = ordinary + QStringLiteral("-hang-on-invoke");
    auto roots = host::Roots::defaults(profile.path(), {});
    if (!QDir().mkpath(roots.per_user_root)) {
        return check(false, "Could not create the package directory.");
    }
    for (const auto& id : {ordinary, hanging}) {
        std::error_code error;
        const fs::path name = QDir(id).filesystemPath();
        fs::copy(QDir(arguments.at(1)).filesystemPath() / name,
            QDir(roots.per_user_root).filesystemPath() / name, fs::copy_options::recursive, error);
        if (!check(!error, "Could not copy the assembled fixture package.")) {
            return false;
        }
    }
    const QString platform = QString::fromLatin1(sdk::host_platform_id());
    const auto registry = host::Registry::discover(roots, {platform, {1}});
    const auto positive = registry->generation(ordinary);
    const auto blocked  = registry->generation(hanging);
    if (!check(positive && blocked, "Both fixture generations must pass package admission.")) {
        for (const auto& failure : registry->unavailable()) {
            std::fprintf(stderr, "%s: %s\n", qPrintable(failure.package_id), qPrintable(failure.detail));
        }
        return false;
    }

    const host::Declared_capability probe{QStringLiteral("conformance-probe"), 1};
    host::Generation_client healthy_client({*positive, roots, arguments.at(2), arguments.at(3), platform, {probe}});
    host::Generation_client hanging_client({*blocked,  roots, arguments.at(2), arguments.at(3), platform, {probe}});
    host::Request_broker healthy(healthy_client);
    host::Request_broker hung(hanging_client);
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&healthy_client, &host::Generation_client::finished, &loop, &QEventLoop::quit);
    QObject::connect(&hanging_client, &host::Generation_client::finished, &loop, &QEventLoop::quit);
    const auto wait_until = [&](const auto& ready, int budget_ms) {
        timeout.start(budget_ms);
        while (!ready() && timeout.isActive()) {
            loop.exec();
        }
        timeout.stop();
        return ready();
    };
    QHash<QByteArray, host::Broker_result> results;
    bool unique = true;
    const auto invoke = [&](host::Request_broker& broker, host::Broker_request request) {
        const auto id = request.correlation_id;
        broker.invoke(std::move(request), [&, id](host::Broker_result result) {
            unique &= !results.contains(id);
            results.insert(id, std::move(result));
            loop.quit();
        });
    };

    const QString echo = QStringLiteral("echo");
    const QJsonObject marker{{QStringLiteral("marker"), QStringLiteral("request broker")}};
    invoke(healthy, {"optional", {probe.capability_id, 2}, echo});
    invoke(healthy, {"healthy", probe, echo, marker});
    invoke(healthy, {"queued", probe, echo});
    healthy.cancel("queued");
    invoke(hung, {"culprit", probe, QStringLiteral("sleep"), {}, 300});
    invoke(hung, {"sibling", probe, echo, {}, sdk::limits::k_no_invoke_deadline_ms});

    bool ok = check(wait_until([&]() { return results.size() == 5 && hanging_client.custody_finished(); },
        sdk::limits::k_activation_deadline_ms + sdk::limits::k_cancel_grace_ms + 2 * sdk::limits::k_reap_deadline_ms),
        "Missing a terminal request result or native retirement receipt.");
    const auto check_result = [&](const char* id, Status status, host::Request_completion completion,
                                  const char* reason = nullptr) {
        const auto found = results.constFind(id);
        if (found != results.cend() && found->status == status && found->completion == completion &&
            (!reason || found->reason_code == QLatin1String(reason)))
        {
            return true;
        }
        std::fprintf(stderr, "Unexpected result for %s: %s (%s)\n", id,
            found == results.cend() ? "missing" : logonomic_cli_capability_status_name(found->status),
            found == results.cend() ? "" : qPrintable(found->reason_code));
        return false;
    };
    using Completion = host::Request_completion;
    ok &= check_result("optional", Status::UNSUPPORTED_VERSION, Completion::LOCAL, sdk::reason::k_unsupported_version);
    ok &= check_result("healthy",  Status::SUCCESS,             Completion::ADAPTER_RESULT);
    ok &= check_result("queued",   Status::CANCELLED,           Completion::LOCAL);
    ok &= check_result("culprit",  Status::TIMED_OUT,           Completion::RETIREMENT, sdk::reason::k_cancel_grace_expired);
    ok &= check_result("sibling",  Status::GENERATION_RETIRED,   Completion::RETIREMENT, sdk::reason::k_cancel_grace_expired);
    ok &= check(results.value("healthy").payload == marker, "The healthy generation did not echo the supplied marker.");
    ok &= check(results.value("culprit").deadline_expired && !results.value("sibling").deadline_expired,
        "Only the culprit expired its operation deadline.");
    invoke(healthy, {"after-retirement", probe, echo, marker});
    ok &= check(wait_until([&]() { return results.contains("after-retirement"); }, sdk::limits::k_default_invoke_deadline_ms),
        "The healthy generation did not answer after the other generation retired.");
    ok &= check_result("after-retirement", Status::SUCCESS, Completion::ADAPTER_RESULT);
    healthy.begin_shutdown();
    hung.begin_shutdown();
    ok &= check(wait_until([&]() { return healthy_client.custody_finished() && hanging_client.custody_finished(); },
        2 * sdk::limits::k_reap_deadline_ms), "Native custody did not finish for both generations.");
    return ok && check(unique, "A request received two terminal results.");
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    return check(argc == 4, "Expected assembled fixtures, custodian and supervisor paths.") &&
        exercise(app.arguments()) ? 0 : 1;
}
