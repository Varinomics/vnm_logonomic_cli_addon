#include "logonomic_cli_addon_registry.h"

#include "logonomic_cli_addon_generation_custody.h"
#include "logonomic_cli_addon_usage_writer_custody.h"

#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/limits.h"
#include "logonomic_cli_addon/package_source.h"
#include "logonomic_cli_addon/package_validator.h"

#include <QByteArray>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>

#include <algorithm>
#include <array>
#include <map>
#include <queue>
#include <string>
#include <utility>
#include <vector>

namespace logonomic::cli_addon_host {

namespace sdk    = logonomic::cli_addon;
namespace limits = logonomic::cli_addon::limits;
namespace reason = logonomic::cli_addon::reason;

namespace {

struct Candidate
{
    Root_level level;
    QString    root;
    QString    directory;
};

using Candidates_by_id = std::map<std::string, std::vector<Candidate>>;

} // namespace

struct Registry::Impl
{
    QList<Generation_record>               generations;
    QList<Unavailable_package>             unavailable;
    std::vector<std::unique_ptr<Run_lock>> run_locks;

    void scan_root(
        Root_level                    level,
        const QString&                root,
        const sdk::Package_validator& validator,
        Candidates_by_id&             candidates_by_id,
        std::stop_token               cancellation);

    void publish_package(
        const std::string&            package_id,
        const std::vector<Candidate>& candidates,
        const sdk::Package_validator& validator,
        const QString&                custody_root,
        Custody_ledger&               ledger,
        std::stop_token               cancellation);
};

namespace {

struct Root_entry
{
    QString    name;
    QByteArray sort_key;
    bool       is_link;
};

struct Root_entry_less
{
    bool operator()(const Root_entry& a, const Root_entry& b) const
    {
        return a.sort_key < b.sort_key;
    }
};

struct Root_listing
{
    std::vector<Root_entry> entries;
    std::size_t             total_entries = 0;
};

sdk::Directory_package_source directory_source(
    const QString& directory,
    std::stop_token cancellation)
{
    const QByteArray utf8 = directory.toUtf8();
    return sdk::Directory_package_source(
        sdk::path_from_utf8(std::string_view(utf8.constData(), (std::size_t)utf8.size())),
        cancellation);
}

sdk::Validation_options validation_options_for(const Registry_options& options)
{
    sdk::Validation_options validation;
    validation.platform_id = options.platform_id.toStdString();
    validation.host_core_protocol_generations.clear();
    for (const uint32_t generation : options.host_core_protocol_generations) {
        // A version above the grammar's ceiling names no generation a package
        // can declare, so it intersects nothing.
        if (generation <= limits::k_max_version) {
            validation.host_core_protocol_generations.push_back((std::uint16_t)generation);
        }
    }
    return validation;
}

// Retains only the bytewise-first candidate entries while streaming the
// immediate root. Regular files are not candidates; links are retained so
// they can be reported, but never followed.
Root_listing list_root_entries(const QString& root, std::stop_token cancellation)
{
    std::priority_queue<Root_entry, std::vector<Root_entry>, Root_entry_less> retained;
    std::size_t total_entries = 0;
    QDirIterator iterator(
        root,
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
        QDirIterator::NoIteratorFlags);
    while (!cancellation.stop_requested() && iterator.hasNext()) {
        const QFileInfo info = iterator.nextFileInfo();
        const bool is_link = info.isSymLink();
        if (!is_link && !info.isDir()) {
            continue;
        }
        Root_entry entry{info.fileName(), info.fileName().toUtf8(), is_link};
        ++total_entries;
        if (retained.size() < limits::k_max_candidates_per_root) {
            retained.push(std::move(entry));
        }
        else
        if (entry.sort_key < retained.top().sort_key) {
            retained.pop();
            retained.push(std::move(entry));
        }
    }

    Root_listing listing;
    listing.total_entries = total_entries;
    listing.entries.reserve(retained.size());
    while (!retained.empty()) {
        listing.entries.push_back(retained.top());
        retained.pop();
    }
    std::sort(listing.entries.begin(), listing.entries.end(), [](const Root_entry& a, const Root_entry& b) {
        return a.sort_key < b.sort_key;
    });
    return listing;
}

Generation_record candidate_record(const sdk::Validated_package& validated, const Candidate& selected)
{
    Generation_record record;
    record.package_id            = QString::fromStdString(validated.bootstrap.package_id);
    record.release_version       = QString::fromStdString(validated.bootstrap.release_version);
    record.source_root           = selected.root;
    record.source_level          = selected.level;
    record.adapter_relative_path = QString::fromStdString(validated.adapter_relative_path);
    for (const std::string& argument : validated.bootstrap.adapter_argv) {
        record.adapter_argv.push_back(QString::fromStdString(argument));
    }
    for (const auto& [name, value] : validated.bootstrap.adapter_environment) {
        record.adapter_environment.insert(QString::fromStdString(name), QString::fromStdString(value));
    }
    for (const std::uint16_t generation : validated.bootstrap.core_protocol_generations) {
        record.core_protocol_generations.push_back(generation);
    }
    for (const sdk::Capability_sidecar& sidecar : validated.sidecars) {
        record.declared_capabilities.push_back({QString::fromStdString(sidecar.capability_id), sidecar.version});
    }
    record.bytes = validated.total_bytes;
    return record;
}

} // namespace

void Registry::Impl::scan_root(
    Root_level                    level,
    const QString&                root,
    const sdk::Package_validator& validator,
    Candidates_by_id&             candidates_by_id,
    std::stop_token               cancellation)
{
    // An unconfigured or absent root holds no candidates; the installation
    // root in particular exists only where packages ship beside the app.
    if (cancellation.stop_requested() || root.isEmpty() || !QFileInfo(root).isDir()) {
        return;
    }
    const Root_listing listing = list_root_entries(root, cancellation);
    if (cancellation.stop_requested()) {
        return;
    }
    if (listing.total_entries > listing.entries.size()) {
        const std::size_t omitted = listing.total_entries - listing.entries.size();
        unavailable.push_back({QString(), root, QLatin1StringView(reason::k_candidate_limit_exceeded),
            QStringLiteral("k_max_candidates_per_root (%1) retained; omitted %2 of %3 candidate entries in %4")
                .arg(limits::k_max_candidates_per_root)
                .arg(omitted)
                .arg(listing.total_entries)
                .arg(root)});
    }
    for (const Root_entry& entry : listing.entries) {
        if (cancellation.stop_requested()) {
            return;
        }
        const QString directory = root + QLatin1Char('/') + entry.name;
        if (entry.is_link) {
            unavailable.push_back({QString(), directory, QLatin1StringView(reason::k_link_entry),
                QStringLiteral("candidate entry is a link: %1").arg(directory)});
            continue;
        }
        const sdk::Directory_package_source source = directory_source(directory, cancellation);
        sdk::Contract_failure failure;
        const std::optional<sdk::Bootstrap_manifest> bootstrap = validator.read_bootstrap(source, &failure);
        if (cancellation.stop_requested()) {
            return;
        }
        if (!bootstrap.has_value()) {
            unavailable.push_back({QString(), directory,
                QString::fromStdString(failure.reason_code), QString::fromStdString(failure.detail)});
            continue;
        }
        candidates_by_id[bootstrap->package_id].push_back({level, root, directory});
    }
}

// Selects the source for one package ID, validates it, custodies it and
// publishes the record; every failure is recorded for this ID alone.
void Registry::Impl::publish_package(
    const std::string&            package_id,
    const std::vector<Candidate>& candidates,
    const sdk::Package_validator& validator,
    const QString&                custody_root,
    Custody_ledger&               ledger,
    std::stop_token               cancellation)
{
    const QString    id             = QString::fromStdString(package_id);
    const Root_level selected_level = candidates.front().level;
    QStringList same_level;
    QStringList lower;
    for (const Candidate& candidate : candidates) {
        (candidate.level == selected_level ? same_level : lower).push_back(candidate.directory);
    }
    const QString ignored_note = lower.isEmpty()
        ? QString()
        : QStringLiteral("; lower-precedence candidates ignored without fallback: %1")
              .arg(lower.join(QStringLiteral(", ")));

    if (same_level.size() > 1) {
        const QString joined = same_level.join(QStringLiteral(", "));
        unavailable.push_back({id, same_level.first(), QLatin1StringView(reason::k_duplicate_package_id),
            QStringLiteral("%1 candidates at the %2 root declare %3: %4%5")
                .arg(same_level.size())
                .arg(QLatin1StringView(root_level_name(selected_level)), id, joined, ignored_note)});
        return;
    }

    const Candidate& selected = candidates.front();
    const sdk::Directory_package_source source = directory_source(selected.directory, cancellation);
    sdk::Contract_failure failure;
    const std::optional<sdk::Validated_package> validated = validator.validate(source, &failure);
    if (cancellation.stop_requested()) {
        return;
    }
    if (!validated.has_value()) {
        unavailable.push_back({id, selected.directory,
            QString::fromStdString(failure.reason_code), QString::fromStdString(failure.detail) + ignored_note});
        return;
    }
    // The bootstrap was read once to group candidates and again inside
    // validation; an installer replacing the package in between shows up here.
    if (validated->bootstrap.package_id != package_id) {
        unavailable.push_back({id, selected.directory, QLatin1StringView(reason::k_source_mutated),
            QStringLiteral("bootstrap declared %1 at discovery and %2 at validation")
                .arg(id, QString::fromStdString(validated->bootstrap.package_id))});
        return;
    }

    const Generation_record record = candidate_record(*validated, selected);
    Materialization_result  result = Generation_custody::materialize(
        selected.directory, *validated, record, custody_root, ledger, cancellation);
    if (cancellation.stop_requested()) {
        return;
    }
    if (!result.ok) {
        unavailable.push_back({id, selected.directory, result.reason_code, result.detail});
        return;
    }
    generations.push_back(*result.record);
    run_locks.push_back(std::move(result.run_lock));
}

std::unique_ptr<Registry> Registry::discover(
    const Roots&            roots,
    const Registry_options& options,
    std::stop_token         cancellation)
{
    if (cancellation.stop_requested()) {
        return nullptr;
    }
    auto impl = std::make_unique<Impl>();

    usage_writer_custody::recover_closed_generations(roots.scratch_root.toStdString());

    Generation_custody::recover_stale(
        roots.custody_root,
        Generation_custody::Published_key::GENERATION_NONCE,
        cancellation);
    Generation_custody::recover_stale(
        roots.scratch_root,
        Generation_custody::Published_key::GENERATION_NONCE,
        cancellation);
    Generation_custody::recover_stale(
        roots.managed_assets_root,
        Generation_custody::Published_key::SHA256_DIGEST,
        cancellation);

    const sdk::Package_validator validator(validation_options_for(options));

    Candidates_by_id candidates_by_id;
    const std::array<std::pair<Root_level, const QString*>, 3> levels = {{
        {Root_level::DEVELOPMENT,  &roots.development_root},
        {Root_level::PER_USER,     &roots.per_user_root},
        {Root_level::INSTALLATION, &roots.installation_root},
    }};
    for (const auto& [level, root] : levels) {
        if (cancellation.stop_requested()) {
            return nullptr;
        }
        impl->scan_root(level, *root, validator, candidates_by_id, cancellation);
    }

    // std::map iterates package IDs in bytewise order: the selection order
    // the aggregate bounds are applied in.
    Custody_ledger ledger;
    for (const auto& [package_id, candidates] : candidates_by_id) {
        if (cancellation.stop_requested()) {
            return nullptr;
        }
        impl->publish_package(package_id, candidates, validator, roots.custody_root, ledger, cancellation);
    }
    if (cancellation.stop_requested()) {
        return nullptr;
    }
    return std::unique_ptr<Registry>(new Registry(std::move(impl)));
}

Registry::Registry(std::unique_ptr<Impl> impl)
:
    m_impl(std::move(impl))
{}

Registry::~Registry() = default;

const QList<Generation_record>& Registry::generations() const
{
    return m_impl->generations;
}

const QList<Unavailable_package>& Registry::unavailable() const
{
    return m_impl->unavailable;
}

std::optional<Generation_record> Registry::generation(const QString& package_id) const
{
    for (const Generation_record& record : m_impl->generations) {
        if (record.package_id == package_id) {
            return record;
        }
    }
    return std::nullopt;
}

} // namespace logonomic::cli_addon_host
