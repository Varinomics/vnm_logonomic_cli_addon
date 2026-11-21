#pragma once

#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/json.h"
#include "logonomic_cli_addon/launcher_discovery.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace logonomic::cli_addon {

class Invocation_context;

inline constexpr const char* k_launcher_approval_transform_operation = "transform";
inline constexpr const char* k_launcher_approval_comparison_operation = "comparison_key";
inline constexpr std::size_t k_max_launcher_approval_key_bytes = 256;

enum class Launcher_approval_action
{
    YOLOFY,
    UNYOLOFY,
};

enum class Launcher_approval_disposition
{
    CHANGED,
    UNCHANGED,
    UNSUPPORTED,
};

struct Launcher_approval_transform_request
{
    Launcher_request         launcher;
    std::string              platform_id;
    Launcher_approval_action action = Launcher_approval_action::YOLOFY;
};

struct Launcher_approval_transform_result
{
    Launcher_approval_disposition disposition = Launcher_approval_disposition::UNSUPPORTED;
    std::string                   script_body;
};

struct Launcher_approval_comparison_request
{
    Launcher_request launcher;
    std::string      platform_id;
};

struct Launcher_approval_comparison_result
{
    bool        supported = false;
    std::string key;
};

/// The capability declares an empty payload; its policy is provider-owned.
bool validate_launcher_approval_declaration(const Json_value& payload, Contract_failure* out_failure);

Json_value encode_launcher_approval_transform_request(const Launcher_approval_transform_request& request);
std::optional<Launcher_approval_transform_request> decode_launcher_approval_transform_request(
    const Json_value& payload, Contract_failure* out_failure);
Json_value encode_launcher_approval_transform_result(const Launcher_approval_transform_result& result);
std::optional<Launcher_approval_transform_result> decode_launcher_approval_transform_result(
    const Json_value& payload, Contract_failure* out_failure);
Json_value encode_launcher_approval_comparison_request(const Launcher_approval_comparison_request& request);
std::optional<Launcher_approval_comparison_request> decode_launcher_approval_comparison_request(
    const Json_value& payload, Contract_failure* out_failure);
Json_value encode_launcher_approval_comparison_result(const Launcher_approval_comparison_result& result);
std::optional<Launcher_approval_comparison_result> decode_launcher_approval_comparison_result(
    const Json_value& payload, Contract_failure* out_failure);

struct Launcher_approval_mode
{
    std::string              argument;
    std::vector<std::string> bypass_values;
    std::vector<std::string> other_values;
};

/// Repeated occurrences append values in order. An attached value may end
/// consumption, allowing its spelling to preserve a removed approval boundary.
struct Launcher_approval_variadic_argument
{
    std::string argument;
    bool        requires_value = true;
    bool        attached_value_terminates = false;
    bool        empty_values_allowed = true;
    char        value_separator = '\0';
};

/// Providers supply option arities and policy; unknown options are unsupported.
/// Values, scripts and environment settings are never interpreted as policy.
struct Launcher_approval_rules
{
    std::vector<std::string>            program_names;
    std::string                         canonical_argument;
    std::vector<std::string>            bypass_arguments;
    std::vector<Launcher_approval_mode> approval_modes;
    std::vector<std::string>            conflicting_arguments;
    std::vector<std::string>            value_arguments;
    std::vector<std::string>            optional_value_arguments;
    std::vector<std::string>            flag_arguments;
    std::vector<std::string>            session_commands;
    std::vector<std::string>            unsupported_commands;

    // Native option arities; forms needing different operand interpretation
    // are refused instead of treating their values as ordinary positionals.
    bool value_arguments_consume_options = true;
    bool flag_arguments_consume_booleans = false;

    std::vector<Launcher_approval_variadic_argument> variadic_arguments{};

    // An exact native spelling equivalent to omitting the bypass option,
    // including configured policy. Used only to preserve an operand boundary.
    std::string inactive_bypass_argument{};
};

/// Edits one literal invocation, retaining unrelated text and operand meaning.
/// Shell programs and unrecognized command/option forms return unsupported.
Launcher_approval_transform_result transform_launcher_approval(
    const Launcher_approval_transform_request& request, const Launcher_approval_rules& rules);

/// An opaque digest for rescan deduplication within this provider capability.
/// Runner, platform, environment and every non-approval argument remain significant.
Launcher_approval_comparison_result launcher_approval_comparison_key(
    const Launcher_approval_comparison_request& request, const Launcher_approval_rules& rules);

void handle_launcher_approval(Invocation_context& context, const Launcher_approval_rules& rules);

} // namespace logonomic::cli_addon
