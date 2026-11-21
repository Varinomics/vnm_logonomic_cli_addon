#pragma once

#include "logonomic_cli_addon/capability_ids.h"
#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/json.h"

#include <optional>
#include <string>
#include <vector>

namespace logonomic::cli_addon {

// environment-bootstrap@1 declares the bounded process-environment keys a
// provider may obtain from its profile .env files before native configuration
// resolution. It never grants the package access to those files.
struct Environment_bootstrap_declaration
{
    std::vector<std::string> environment_names;
    std::string              home_environment_name;
    std::string              default_home_relative_path;
    std::string              dotenv_relative_path;
};

Json_value encode_environment_bootstrap_declaration(
    const Environment_bootstrap_declaration& declaration);
std::optional<Environment_bootstrap_declaration>
decode_environment_bootstrap_declaration(
    const Json_value& payload,
    Contract_failure* failure);

} // namespace logonomic::cli_addon
