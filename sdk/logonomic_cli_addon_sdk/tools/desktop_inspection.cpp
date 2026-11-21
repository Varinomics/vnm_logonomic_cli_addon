#include "desktop_inspection.h"

#include "logonomic_cli_addon/capability_ids.h"
#include "logonomic_cli_addon/helper_ingress.h"
#include "logonomic_cli_addon/host_simulator.h"
#include "logonomic_cli_addon/launch_preparation.h"
#include "logonomic_cli_addon/lifecycle_correlation_lifetime.h"
#include "logonomic_cli_addon/lifecycle_events.h"
#include "logonomic_cli_addon/managed_configuration.h"
#include "logonomic_cli_addon/session_contracts.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace logonomic::cli_addon::tools {
namespace {

struct desktop_declaration_t
{
    std::vector<std::uint16_t> bootstrap_versions;
    std::vector<std::uint16_t> index_versions;
    std::vector<std::uint16_t> core_generations;
    std::vector<Capability_pair> adopted;
};

bool contains(const auto& values, const auto& value)
{
    return std::ranges::find(values, value) != values.end();
}

bool positive_version(const Json_value* value)
{
    return value && value->is_integer() && value->as_integer() > 0 && is_valid_version(value->as_integer());
}

bool read_versions(const Json_value& document, const char* member, std::vector<std::uint16_t>& versions)
{
    const auto* array = document.find(member);
    if (!array || !array->is_array()) {
        return false;
    }
    for (const auto& item : array->as_array()) {
        if (!positive_version(&item)) {
            return false;
        }
        versions.push_back(static_cast<std::uint16_t>(item.as_integer()));
    }
    std::ranges::sort(versions);
    return std::ranges::adjacent_find(versions) == versions.end();
}

bool pair_less(const Capability_pair& left, const Capability_pair& right)
{
    return std::tie(left.capability_id, left.version) < std::tie(right.capability_id, right.version);
}

std::optional<desktop_declaration_t> read_desktop(const std::string& path, std::string& error)
{
    const Json_parse_limits limits;
    std::ifstream input(path_from_utf8(path), std::ios::binary | std::ios::ate);
    const std::streamoff size = input.tellg();
    if (size < 0 || static_cast<std::uint64_t>(size) > limits.max_bytes) {
        error = "Cannot read desktop declaration within the SDK JSON size bound.";
        return {};
    }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    if (!input.seekg(0).read(bytes.data(), size)) {
        error = "Cannot read desktop declaration.";
        return {};
    }
    Json_parse_error parse_error;
    const auto document = parse_json(bytes, limits, &parse_error);
    if (!document) {
        error = "Invalid desktop declaration JSON at byte " + std::to_string(parse_error.offset) + ": " + parse_error.message;
        return {};
    }
    const auto* version = document->find("compatibility_declaration_version");
    if (!version || !version->is_integer() || version->as_integer() != 1) {
        error = "Expected compatibility_declaration_version 1.";
        return {};
    }
    desktop_declaration_t target;
    if (!read_versions(*document, "bootstrap_versions", target.bootstrap_versions) ||
        !read_versions(*document, "index_versions", target.index_versions) ||
        !read_versions(*document, "core_protocol_generations", target.core_generations))
    {
        error = "Expected distinct positive versions in bootstrap_versions, index_versions and core_protocol_generations.";
        return {};
    }
    const auto* pairs = document->find("adopted_capabilities");
    if (!pairs || !pairs->is_array()) {
        error = "Expected an adopted_capabilities array.";
        return {};
    }
    for (const auto& item : pairs->as_array()) {
        const auto* id = item.find("capability_id");
        const auto* pair_version = item.find("version");
        if (!id || !id->is_string() || !is_valid_capability_id(id->as_string()) || !pair_version ||
            !pair_version->is_integer() || !is_valid_version(pair_version->as_integer()))
        {
            error = "Invalid adopted capability ID or version.";
            return {};
        }
        target.adopted.push_back({id->as_string(), static_cast<std::uint16_t>(pair_version->as_integer())});
    }
    std::ranges::sort(target.adopted, pair_less);
    if (std::ranges::adjacent_find(target.adopted) != target.adopted.end()) {
        error = "Duplicate adopted capability pair.";
        return {};
    }
    return target;
}

const Capability_sidecar* find_sidecar(const Validated_package& package, const Capability_pair& pair)
{
    const auto found = std::ranges::find(package.sidecars, pair, &Capability_sidecar::pair);
    return found == package.sidecars.end() ? nullptr : &*found;
}

// These diagnostics describe missing necessary evidence. Pair presence alone
// cannot establish that the desktop admitted a typed sidecar or a live binding.
void inspect_contract(
    Json_value& row, const Validated_package& package, const Capability_sidecar& sidecar,
    const std::vector<Capability_pair>& available)
{
    const auto pair = sidecar.pair();
    Contract_failure failure;
    if (pair == Capability_pair{k_account_profile_managed_configuration_capability_id,
                                k_account_profile_managed_configuration_version})
    {
        row.set("role", Json_value("static_account_profile_binding"));
        const auto binding = decode_account_profile_managed_configuration_declaration(sidecar.payload, &failure);
        if (binding) {
            for (const auto& required : {Capability_pair{k_accounts_capability_id, k_accounts_launcher_version},
                    Capability_pair{k_managed_configuration_capability_id, binding->managed_configuration_version}})
            {
                if (!contains(available, required)) {
                    failure.detail += "Missing " + required.capability_id + "@" + std::to_string(required.version) + ". ";
                }
            }
        }
    }
    else
    if (pair == Capability_pair{k_coordination_delivery_capability_id, k_coordination_delivery_version}) {
        row.set("role", Json_value("static_coordination_delivery"));
        (void)decode_coordination_delivery_declaration(sidecar.payload, &failure);
    }
    else
    if (pair == Capability_pair{k_interactive_initial_prompt_capability_id, k_interactive_initial_prompt_version}) {
        validate_interactive_initial_prompt_dependency(available, &failure);
    }
    else
    if (pair == Capability_pair{k_terminal_codex_invocation_capability_id, k_terminal_codex_invocation_version}) {
        validate_terminal_codex_invocation_dependency(available, &failure);
    }
    else
    if (pair == Capability_pair{k_lifecycle_correlation_lifetime_capability_id, k_lifecycle_correlation_lifetime_version}) {
        validate_lifecycle_correlation_lifetime_dependency(available, &failure);
    }
    else
    if (pair == Capability_pair{k_helper_ingress_capability_id, k_helper_ingress_degradation_version} ||
        pair == Capability_pair{k_helper_ingress_capability_id, k_helper_ingress_subagent_reconciliation_version})
    {
        validate_helper_ingress_dependency(available, &failure);
    }
    else
    if (pair == Capability_pair{k_delegated_session_capability_id, k_delegated_session_version}) {
        const Capability_pair required{k_structured_session_capability_id, k_structured_session_version};
        const auto* parent = find_sidecar(package, required);
        const auto purposes = decode_purposes_declaration(sidecar.payload, &failure);
        if (purposes) {
            const auto structured = parent && contains(available, required)
                ? decode_purposes_declaration(parent->payload, &failure) : std::nullopt;
            if (failure.detail.empty()) {
                validate_delegated_session_dependency(*purposes, structured ? &*structured : nullptr, &failure);
            }
        }
    }
    if (!failure.detail.empty()) {
        row.set("contract_issue", Json_value(failure.detail));
    }
}

Json_value capability_report(
    const Validated_package& package, const desktop_declaration_t& target,
    const std::vector<Capability_pair>* advertised)
{
    const auto declared = package.declared_capabilities();
    std::vector<Capability_pair> available;
    std::set<std::pair<std::string, std::uint16_t>> pairs;
    for (const auto& pair : declared) {
        pairs.emplace(pair.capability_id, pair.version);
        if (std::ranges::binary_search(target.adopted, pair, pair_less) && (!advertised || contains(*advertised, pair))) {
            available.push_back(pair);
        }
    }
    for (const auto& pair : target.adopted) {
        pairs.emplace(pair.capability_id, pair.version);
    }
    if (advertised) {
        for (const auto& pair : *advertised) {
            pairs.emplace(pair.capability_id, pair.version);
        }
    }
    auto rows = Json_value::array();
    for (const auto& [id, version] : pairs) {
        const Capability_pair pair{id, version};
        const auto* sidecar = find_sidecar(package, pair);
        auto row = Json_value::object();
        row.set("capability_id", Json_value(id));
        row.set("version",       Json_value(version));
        row.set("declared",      Json_value(sidecar != nullptr));
        row.set("adopted",       Json_value(std::ranges::binary_search(target.adopted, pair, pair_less)));
        row.set("advertised",    advertised ? Json_value(contains(*advertised, pair)) : Json_value{});
        if (sidecar) {
            inspect_contract(row, package, *sidecar, available);
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

int write_report(Json_value& report, const std::string& error = {})
{
    if (!error.empty()) {
        report.set("error", Json_value(error));
    }
    std::puts(serialize_json(report).c_str());
    return error.empty() ? 0 : 1;
}

} // namespace

int inspect_desktop(
    const std::string& directory, const std::string& declaration_path,
    const std::string& platform, bool live)
{
    auto report = Json_value::object();
    report.set("package_directory", Json_value(directory));
    report.set("desktop_selection", Json_value("unchecked_supplied_directory"));
    report.set("desktop_declaration", Json_value(declaration_path));
    report.set("platform", Json_value(platform));
    report.set("activation", Json_value(live ? "not_started" : "not_requested"));
    report.set("prerequisite_coverage", Json_value("selected_sdk_contracts_only"));
    report.set("feature_behavior_tested", Json_value(false));
    std::string error;
    const auto target = read_desktop(declaration_path, error);
    if (!target) {
        return write_report(report, error);
    }

    Directory_package_source source(path_from_utf8(directory));
    Validation_options options;
    options.platform_id = platform;
    Contract_failure failure;
    // Check the package's own declarations independently of target adoption.
    const auto bootstrap = Package_validator(options).read_bootstrap(source, &failure);
    if (bootstrap) {
        options.host_core_protocol_generations = bootstrap->core_protocol_generations;
    }
    const auto package = bootstrap ? Package_validator(options).validate(source, &failure) : std::nullopt;
    report.set("package_valid", Json_value(package.has_value()));
    if (!package) {
        return write_report(report, failure.reason_code + ": " + failure.detail);
    }
    report.set("package_id", Json_value(package->bootstrap.package_id));
    report.set("release_version", Json_value(package->bootstrap.release_version));
    report.set("generation_digest", Json_value(package->generation_digest_hex));
    const bool supported_bootstrap = contains(target->bootstrap_versions, package->bootstrap.bootstrap_version);
    const bool supported_index = contains(target->index_versions, package->index.index_version);
    const bool supported_core = std::ranges::any_of(target->core_generations, [&](auto generation) {
        return package->bootstrap.supports_core_generation(generation);
    });
    auto formats = Json_value::object();
    formats.set("bootstrap", Json_value(supported_bootstrap));
    formats.set("index", Json_value(supported_index));
    formats.set("core_protocol", Json_value(supported_core));
    report.set("target_supports", std::move(formats));
    report.set("capabilities", capability_report(*package, *target, nullptr));
    if (!supported_bootstrap || !supported_index || !supported_core) {
        return write_report(report, "Package and target do not share the required formats or core protocol.");
    }
    if (!live) {
        return write_report(report);
    }
    if (platform != host_platform_id()) {
        return write_report(report, "Live inspection requires the local adapter platform.");
    }
    Simulator_options simulator_options;
    simulator_options.package_directory = path_from_utf8(directory);
    simulator_options.adopted_capabilities = target->adopted;
    auto& generations = simulator_options.validation.host_core_protocol_generations;
    generations.clear();
    for (const auto generation : k_supported_core_protocol_generations) {
        if (contains(target->core_generations, generation) && package->bootstrap.supports_core_generation(generation)) {
            generations.push_back(generation);
        }
    }
    if (generations.empty()) {
        return write_report(report, "The SDK simulator cannot negotiate a generation shared by the package and target.");
    }
    Host_simulator simulator(std::move(simulator_options), nullptr);
    if (!simulator.start(&failure)) {
        report.set("activation", Json_value("failed"));
        return write_report(report, failure.reason_code + ": " + failure.detail);
    }
    report.set("activation", Json_value("handshake_completed"));
    if (simulator.generation_digest() != package->generation_digest_hex) {
        return write_report(report, "Package changed between validation and simulator activation.");
    }
    const auto& session = *simulator.session();
    report.set("negotiated_core_generation", Json_value(session.adapter_hello()->core_protocol_generation));
    report.set("capabilities", capability_report(*package, *target, &session.handshake_capabilities()));
    int exit_code = 0;
    simulator.shutdown(limits::k_shutdown_grace_ms, &exit_code);
    return write_report(report);
}

} // namespace logonomic::cli_addon::tools
