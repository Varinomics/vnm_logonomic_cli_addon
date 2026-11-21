#pragma once

#include "logonomic_cli_addon/ids.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace logonomic::cli_addon {

/**
 * @brief The bootstrap manifest `logonomic-cli-addon.json`: the only file a
 * reader needs to identify a package, pick the adapter for its platform and
 * choose a core protocol generation (docs/package_contract.md, "Bootstrap
 * manifest"). Member order of the maps follows the manifest.
 */
struct Bootstrap_manifest
{
    std::uint16_t                                    bootstrap_version = 0;
    std::string                                      package_id;
    std::string                                      release_version;
    std::vector<std::string>                         platforms;
    std::vector<std::pair<std::string, std::string>> adapter_path;          // platform ID -> canonical path
    std::vector<std::string>                         adapter_argv;
    std::vector<std::pair<std::string, std::string>> adapter_environment;   // name -> value
    std::vector<std::uint16_t>                       core_protocol_generations;
    std::string                                      integrity_index;

    const std::string* adapter_path_for(std::string_view platform_id) const;
    bool supports_platform(std::string_view platform_id) const;
    bool supports_core_generation(std::uint16_t generation) const;
};

/// Strict parse of the manifest bytes. Failure reason codes:
/// `invalid_bootstrap`, `unsupported_bootstrap_version`, `invalid_utf8`,
/// `reserved_environment_variable`, `shell_adapter_entry_point` (extension
/// rules that need no file bytes). Platform membership, core-generation
/// intersection and the executable-format check belong to the validator.
std::optional<Bootstrap_manifest> parse_bootstrap(std::string_view bytes, Contract_failure* out_failure);

/// Compact serialization in the member order of the contract; parse_bootstrap
/// accepts the result.
std::string serialize_bootstrap(const Bootstrap_manifest& manifest);

/// True for the extensions the contract names as shell entry points:
/// `.bat`, `.cmd`, `.ps1`, `.sh` (ASCII case-insensitive).
bool has_shell_script_extension(std::string_view path);

/// True when the path ends in `.exe` (ASCII case-insensitive).
bool has_windows_executable_extension(std::string_view path);

} // namespace logonomic::cli_addon
