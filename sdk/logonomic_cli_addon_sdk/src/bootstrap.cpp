#include "logonomic_cli_addon/bootstrap.h"
#include "logonomic_cli_addon/json.h"
#include "logonomic_cli_addon/limits.h"
#include "logonomic_cli_addon/version.h"
#include "strict_json.h"

#include <algorithm>

namespace logonomic::cli_addon {

namespace {

std::optional<Bootstrap_manifest> fail(
    Contract_failure* out_failure,
    const char*       reason_code,
    std::string       detail)
{
    if (out_failure != nullptr) {
        out_failure->reason_code = reason_code;
        out_failure->detail      = std::move(detail);
    }
    return std::nullopt;
}

bool has_extension(std::string_view path, std::string_view extension)
{
    if (path.size() < extension.size()) {
        return false;
    }
    return ascii_lowercase(path.substr(path.size() - extension.size())) == extension;
}

// The static half of the shell-entry-point rule: extensions the contract
// names, and the Windows requirement of `.exe`.
bool adapter_path_is_shell_entry_point(std::string_view platform_id, std::string_view path)
{
    if (has_shell_script_extension(path)) {
        return true;
    }
    if (platform_id == k_platform_windows_x86_64 && !has_windows_executable_extension(path)) {
        return true;
    }
    return false;
}

} // namespace

bool has_shell_script_extension(std::string_view path)
{
    return has_extension(path, ".bat") || has_extension(path, ".cmd") ||
           has_extension(path, ".ps1") || has_extension(path, ".sh");
}

bool has_windows_executable_extension(std::string_view path)
{
    return has_extension(path, ".exe");
}

const std::string* Bootstrap_manifest::adapter_path_for(std::string_view platform_id) const
{
    for (const auto& [platform, path] : adapter_path) {
        if (platform == platform_id) {
            return &path;
        }
    }
    return nullptr;
}

bool Bootstrap_manifest::supports_platform(std::string_view platform_id) const
{
    return std::find(platforms.begin(), platforms.end(), platform_id) != platforms.end();
}

bool Bootstrap_manifest::supports_core_generation(std::uint16_t generation) const
{
    return std::find(core_protocol_generations.begin(), core_protocol_generations.end(), generation) !=
           core_protocol_generations.end();
}

std::optional<Bootstrap_manifest> parse_bootstrap(std::string_view bytes, Contract_failure* out_failure)
{
    if (bytes.size() > limits::k_max_bootstrap_bytes) {
        return fail(out_failure, reason::k_invalid_bootstrap, "manifest exceeds k_max_bootstrap_bytes");
    }
    if (!is_valid_utf8(bytes)) {
        return fail(out_failure, reason::k_invalid_utf8, "manifest is not valid UTF-8");
    }

    Json_parse_limits parse_limits;
    parse_limits.max_bytes = limits::k_max_bootstrap_bytes;
    Json_parse_error parse_error;
    const std::optional<Json_value> document = parse_json(bytes, parse_limits, &parse_error);
    if (!document.has_value()) {
        return fail(out_failure, reason::k_invalid_bootstrap,
            "manifest is not valid JSON at byte " + std::to_string(parse_error.offset) + ": " + parse_error.message);
    }
    if (!document->is_object()) {
        return fail(out_failure, reason::k_invalid_bootstrap, "manifest is not a JSON object");
    }

    // The version decides whether any other member may be read at all.
    const Json_value* version_value = document->find("bootstrap_version");
    if (version_value == nullptr || !version_value->is_integer()) {
        return fail(out_failure, reason::k_invalid_bootstrap, "bootstrap_version is missing or not an integer");
    }
    if (version_value->as_integer() != k_bootstrap_version) {
        return fail(out_failure, reason::k_unsupported_bootstrap_version,
            "bootstrap_version " + std::to_string(version_value->as_integer()) + " is not supported");
    }

    const std::optional<std::string> unexpected = strict::unexpected_member(*document, {
        "bootstrap_version", "package_id", "release_version", "platforms",
        "adapter", "core_protocol_generations", "integrity_index",
    });
    if (unexpected.has_value()) {
        return fail(out_failure, reason::k_invalid_bootstrap, "unexpected member " + *unexpected);
    }

    Bootstrap_manifest manifest;
    manifest.bootstrap_version = k_bootstrap_version;

    const std::string* package_id = strict::string_member(*document, "package_id");
    if (package_id == nullptr || !is_valid_package_id(*package_id)) {
        return fail(out_failure, reason::k_invalid_bootstrap, "package_id is missing or not a canonical package ID");
    }
    manifest.package_id = *package_id;

    const std::string* release_version = strict::string_member(*document, "release_version");
    if (release_version == nullptr || !is_valid_release_version(*release_version)) {
        return fail(out_failure, reason::k_invalid_bootstrap, "release_version is missing or out of grammar");
    }
    manifest.release_version = *release_version;

    const Json_value* platforms = strict::member_of_type(*document, "platforms", Json_type::ARRAY);
    if (platforms == nullptr || platforms->as_array().empty() ||
        platforms->as_array().size() > limits::k_max_platforms_per_package)
    {
        return fail(out_failure, reason::k_invalid_bootstrap, "platforms must list 1-8 platform IDs");
    }
    for (const Json_value& platform : platforms->as_array()) {
        if (!platform.is_string() || !is_valid_platform_id(platform.as_string())) {
            return fail(out_failure, reason::k_invalid_bootstrap, "platforms contains a value out of grammar");
        }
        if (manifest.supports_platform(platform.as_string())) {
            return fail(out_failure, reason::k_invalid_bootstrap, "platforms lists " + platform.as_string() + " twice");
        }
        manifest.platforms.push_back(platform.as_string());
    }

    const Json_value* adapter = strict::member_of_type(*document, "adapter", Json_type::OBJECT);
    if (adapter == nullptr) {
        return fail(out_failure, reason::k_invalid_bootstrap, "adapter is missing or not an object");
    }
    const std::optional<std::string> unexpected_adapter = strict::unexpected_member(*adapter, {"path", "argv", "environment"});
    if (unexpected_adapter.has_value()) {
        return fail(out_failure, reason::k_invalid_bootstrap, "unexpected member adapter." + *unexpected_adapter);
    }

    const Json_value* adapter_path = strict::member_of_type(*adapter, "path", Json_type::OBJECT);
    if (adapter_path == nullptr) {
        return fail(out_failure, reason::k_invalid_bootstrap, "adapter.path is missing or not an object");
    }
    for (const Json_member& member : adapter_path->as_object()) {
        if (!manifest.supports_platform(member.name)) {
            return fail(out_failure, reason::k_invalid_bootstrap,
                "adapter.path names platform " + member.name + " that platforms does not list");
        }
        if (!member.value.is_string() || !is_valid_package_relative_path(member.value.as_string()) ||
            path_first_segment(member.value.as_string()) != k_segment_adapter)
        {
            return fail(out_failure, reason::k_invalid_bootstrap,
                "adapter.path." + member.name + " is not a canonical path below adapter/");
        }
        if (adapter_path_is_shell_entry_point(member.name, member.value.as_string())) {
            return fail(out_failure, reason::k_shell_adapter_entry_point,
                "adapter.path." + member.name + " names a shell entry point: " + member.value.as_string());
        }
        manifest.adapter_path.emplace_back(member.name, member.value.as_string());
    }
    for (const std::string& platform : manifest.platforms) {
        if (manifest.adapter_path_for(platform) == nullptr) {
            return fail(out_failure, reason::k_invalid_bootstrap, "adapter.path has no entry for platform " + platform);
        }
    }

    const Json_value* argv = strict::member_of_type(*adapter, "argv", Json_type::ARRAY);
    if (argv == nullptr || argv->as_array().size() > limits::k_max_argv_entries) {
        return fail(
            out_failure,
            reason::k_invalid_bootstrap,
            "adapter.argv is missing or has more than " + std::to_string(limits::k_max_argv_entries) + " entries");
    }
    std::size_t argv_total_bytes = 0;
    for (const Json_value& argument : argv->as_array()) {
        if (!argument.is_string() || argument.as_string().empty() ||
            argument.as_string().size() > limits::k_max_argv_entry_bytes ||
            !strict::has_no_control_characters(argument.as_string()))
        {
            return fail(out_failure, reason::k_invalid_bootstrap, "adapter.argv contains an entry out of grammar");
        }
        argv_total_bytes += argument.as_string().size();
        manifest.adapter_argv.push_back(argument.as_string());
    }
    if (argv_total_bytes > limits::k_max_argv_total_bytes) {
        return fail(out_failure, reason::k_invalid_bootstrap, "adapter.argv exceeds k_max_argv_total_bytes");
    }

    if (const Json_value* environment = adapter->find("environment"); environment != nullptr) {
        if (!environment->is_object() || environment->as_object().size() > limits::k_max_environment_entries) {
            return fail(out_failure, reason::k_invalid_bootstrap,
                "adapter.environment is not an object of at most 16 entries");
        }
        for (const Json_member& member : environment->as_object()) {
            if (!is_valid_environment_name(member.name)) {
                return fail(out_failure, reason::k_invalid_bootstrap,
                    "adapter.environment name out of grammar: " + member.name);
            }
            if (is_reserved_environment_name(member.name)) {
                return fail(out_failure, reason::k_reserved_environment_variable,
                    "adapter.environment sets reserved variable " + member.name);
            }
            if (!member.value.is_string() || member.value.as_string().size() > limits::k_max_environment_value_bytes ||
                member.value.as_string().find('\0') != std::string::npos)
            {
                return fail(out_failure, reason::k_invalid_bootstrap,
                    "adapter.environment value out of grammar for " + member.name);
            }
            manifest.adapter_environment.emplace_back(member.name, member.value.as_string());
        }
    }

    const Json_value* generations = strict::member_of_type(*document, "core_protocol_generations", Json_type::ARRAY);
    if (generations == nullptr || generations->as_array().empty() ||
        generations->as_array().size() > limits::k_max_core_generations_per_package)
    {
        return fail(out_failure, reason::k_invalid_bootstrap, "core_protocol_generations must list 1-8 versions");
    }
    for (const Json_value& generation : generations->as_array()) {
        if (!generation.is_integer() || !is_valid_version(generation.as_integer())) {
            return fail(out_failure, reason::k_invalid_bootstrap, "core_protocol_generations contains a non-version");
        }
        const auto version = (std::uint16_t)generation.as_integer();
        if (manifest.supports_core_generation(version)) {
            return fail(out_failure, reason::k_invalid_bootstrap, "core_protocol_generations lists a version twice");
        }
        manifest.core_protocol_generations.push_back(version);
    }

    const std::string* integrity_index = strict::string_member(*document, "integrity_index");
    if (integrity_index == nullptr || *integrity_index != k_index_file_name) {
        return fail(out_failure, reason::k_invalid_bootstrap,
            std::string("integrity_index must be \"") + k_index_file_name + "\"");
    }
    manifest.integrity_index = *integrity_index;

    return manifest;
}

std::string serialize_bootstrap(const Bootstrap_manifest& manifest)
{
    Json_value platforms = Json_value::array();
    for (const std::string& platform : manifest.platforms) {
        platforms.push_back(Json_value(platform));
    }

    Json_value path = Json_value::object();
    for (const auto& [platform, adapter_path] : manifest.adapter_path) {
        path.set(platform, Json_value(adapter_path));
    }

    Json_value argv = Json_value::array();
    for (const std::string& argument : manifest.adapter_argv) {
        argv.push_back(Json_value(argument));
    }

    Json_value environment = Json_value::object();
    for (const auto& [name, value] : manifest.adapter_environment) {
        environment.set(name, Json_value(value));
    }

    Json_value adapter = Json_value::object();
    adapter.set("path", std::move(path));
    adapter.set("argv", std::move(argv));
    adapter.set("environment", std::move(environment));

    Json_value generations = Json_value::array();
    for (const std::uint16_t generation : manifest.core_protocol_generations) {
        generations.push_back(Json_value((std::int64_t)generation));
    }

    Json_value document = Json_value::object();
    document.set("bootstrap_version", Json_value((std::int64_t)manifest.bootstrap_version));
    document.set("package_id", Json_value(manifest.package_id));
    document.set("release_version", Json_value(manifest.release_version));
    document.set("platforms", std::move(platforms));
    document.set("adapter", std::move(adapter));
    document.set("core_protocol_generations", std::move(generations));
    document.set("integrity_index", Json_value(manifest.integrity_index));
    return serialize_json(document);
}

} // namespace logonomic::cli_addon
