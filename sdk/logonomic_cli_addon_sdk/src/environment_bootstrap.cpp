#include "logonomic_cli_addon/environment_bootstrap.h"

#include "strict_json.h"

#include <set>

namespace logonomic::cli_addon {
namespace {

void report(Contract_failure* failure, const char* reason, std::string detail)
{
    if (failure) *failure = {reason, std::move(detail)};
}

bool relative_path(std::string_view value)
{
    if (value.empty() || value.size() > limits::k_max_package_relative_path_bytes ||
        !is_valid_utf8(value) || value.find('\0') != std::string_view::npos ||
        value.front() == '/' || value.find('\\') != std::string_view::npos)
    {
        return false;
    }
    std::size_t count = 0;
    for (std::string_view segment : split_path_segments(value)) {
        ++count;
        if (segment.empty() || segment == "." || segment == ".." || segment.size() > 128 ||
            segment.back() == '.' || segment.back() == ' ')
        {
            return false;
        }
    }
    return count <= limits::k_max_package_path_segments;
}

} // namespace

Json_value encode_environment_bootstrap_declaration(
    const Environment_bootstrap_declaration& declaration)
{
    Json_value names = Json_value::array();
    for (const std::string& name : declaration.environment_names) names.push_back(Json_value(name));
    Json_value payload = Json_value::object();
    payload.set("environment_names", std::move(names));
    payload.set("home_environment_name", Json_value(declaration.home_environment_name));
    payload.set("default_home_relative_path", Json_value(declaration.default_home_relative_path));
    payload.set("dotenv_relative_path", Json_value(declaration.dotenv_relative_path));
    return payload;
}

std::optional<Environment_bootstrap_declaration>
decode_environment_bootstrap_declaration(const Json_value& payload, Contract_failure* failure)
{
    const Json_value* names = strict::member_of_type(payload, "environment_names", Json_type::ARRAY);
    const std::string* home = strict::string_member(payload, "home_environment_name");
    const std::string* default_home = strict::string_member(payload, "default_home_relative_path");
    const std::string* dotenv = strict::string_member(payload, "dotenv_relative_path");
    if (!payload.is_object() || strict::unexpected_member(payload, {
            "environment_names", "home_environment_name", "default_home_relative_path", "dotenv_relative_path"}) ||
        names == nullptr || home == nullptr || default_home == nullptr || dotenv == nullptr ||
        names->as_array().empty() || names->as_array().size() > 4 ||
        !relative_path(*default_home) || !relative_path(*dotenv))
    {
        report(failure, reason::k_invalid_sidecar, "environment bootstrap declaration is invalid");
        return std::nullopt;
    }
    Environment_bootstrap_declaration result;
    std::set<std::string> seen;
    bool contains_home = false;
    for (const Json_value& value : names->as_array()) {
        if (!value.is_string() || !is_valid_environment_name(value.as_string()) ||
            is_reserved_environment_name(value.as_string()) ||
            !seen.insert(ascii_lowercase(value.as_string())).second)
        {
            report(failure, reason::k_invalid_sidecar, "environment bootstrap names are invalid");
            return std::nullopt;
        }
        contains_home = contains_home || value.as_string() == *home;
        result.environment_names.push_back(value.as_string());
    }
    if (!is_valid_environment_name(*home) || is_reserved_environment_name(*home) || !contains_home) {
        report(failure, reason::k_invalid_sidecar, "environment bootstrap home selector is invalid");
        return std::nullopt;
    }
    result.home_environment_name = *home;
    result.default_home_relative_path = *default_home;
    result.dotenv_relative_path = *dotenv;
    return result;
}

} // namespace logonomic::cli_addon
