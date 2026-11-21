#pragma once

#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/json.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace logonomic::cli_addon {

/**
 * @brief launcher-discovery@1 (docs/launcher_discovery_1.md): an adapter
 * capability with two operations, `discover` for candidate launchers and
 * `recognize` for a user-authored launcher. Both see only the ambient view:
 * eight baseline environment names plus the names the sidecar declares.
 */
inline constexpr const char* k_discover_operation  = "discover";
inline constexpr const char* k_recognize_operation = "recognize";

/// The launcher service's runner keys. `direct` and `bash` run on every
/// platform; `powershell` and `cmd` invoke powershell.exe and cmd.exe and
/// run on Windows only.
inline constexpr const char* k_runner_direct     = "direct";
inline constexpr const char* k_runner_bash       = "bash";
inline constexpr const char* k_runner_powershell = "powershell";
inline constexpr const char* k_runner_cmd        = "cmd";

bool is_runner_key(std::string_view runner_key);
bool is_runner_supported_on_platform(std::string_view runner_key, std::string_view platform_id);

/// The environment names every ambient view carries.
inline constexpr std::array<const char*, 8> k_ambient_baseline_names = {
    "PATH", "PATHEXT", "HOME", "USERPROFILE", "LOCALAPPDATA", "APPDATA", "XDG_CONFIG_HOME", "XDG_DATA_HOME",
};

struct Launcher_discovery_declaration
{
    std::vector<std::string> environment_names;   // 0-32 names the adapter may read beyond the baseline
};

/// Strict decode of the sidecar payload. Reasons: `invalid_sidecar`,
/// `reserved_environment_variable`.
std::optional<Launcher_discovery_declaration> decode_launcher_discovery_declaration(
    const Json_value& payload, Contract_failure* out_failure);

Json_value encode_launcher_discovery_declaration(const Launcher_discovery_declaration& declaration);

/// The ambient view of an environment: the baseline names and the declared
/// names, each with a value of at most k_max_environment_value_bytes (a
/// longer value is omitted). Names are matched by byte equality on Linux
/// and ASCII case-insensitively on Windows, where a baseline name is
/// emitted in its canonical spelling. The result is a JSON object.
Json_value ambient_view(
    const std::vector<std::pair<std::string, std::string>>& environment,
    const Launcher_discovery_declaration&                   declaration,
    std::string_view                                        platform_id);

bool validate_ambient_environment(
    const Json_value& environment, std::string_view platform_id, std::string* out_detail);

/// The frozen, allowlisted effective input of launch-preparation@3 and the
/// session capabilities that prepare against the same launcher snapshot.
struct Environment_bootstrap_evidence
{
    std::string resolved_home;
};

struct Launch_environment
{
    std::string platform_id;
    Json_value  effective_environment = Json_value::object();
    std::optional<Environment_bootstrap_evidence> bootstrap = std::nullopt;
};

Json_value encode_launch_environment(const Launch_environment& environment);
std::optional<Launch_environment> decode_launch_environment(
    const Json_value& payload, Contract_failure* out_failure);

struct Discover_request
{
    Json_value  ambient_environment = Json_value::object();
    std::string default_home_fallback;
    std::string platform_id;
};

/// The `discover` payload the host sends.
Json_value encode_discover_request(
    const Json_value& ambient_environment,
    std::string_view  default_home_fallback,
    std::string_view  platform_id);

/// Adapter side: strict decode of the `discover` payload. Reasons:
/// `frame_violation`, `unexpected_member`.
std::optional<Discover_request> decode_discover_request(const Json_value& payload, Contract_failure* out_failure);

/// The user-authored launcher fields shared byte-for-byte by `recognize` and
/// `prepare`. Keeping one typed record prevents the two capability contracts
/// from drifting into different launcher grammars.
struct Launcher_request
{
    std::string script_body;        // 1-k_max_script_body_bytes, no NUL; one line for direct
    std::string runner_key;         // one of the runner keys
    std::string environment_text;   // at most k_max_environment_text_bytes, no NUL
};

Json_value encode_launcher_request(const Launcher_request& launcher);

/// Strict decode of the shared launcher record. Reasons: `frame_violation`,
/// `unexpected_member`.
std::optional<Launcher_request> decode_launcher_request(
    const Json_value& value,
    Contract_failure* out_failure);

struct Recognize_request
{
    Launcher_request launcher;
    Json_value       ambient_environment = Json_value::object();
    std::string      platform_id;
};

Json_value encode_recognize_request(
    const Launcher_request& launcher,
    const Json_value&       ambient_environment,
    std::string_view        platform_id);

/// Adapter side: strict decode of the `recognize` payload. Reasons:
/// `frame_violation`, `unexpected_member`.
std::optional<Recognize_request> decode_recognize_request(
    const Json_value& payload,
    Contract_failure* out_failure);

struct Launcher_candidate
{
    std::string candidate_id;       // operation-name grammar, 1-64 bytes, unique per result
    std::string display_name;       // the display-name rule of presentation@1
    std::string script_body;        // 1-4096 bytes, no NUL, one line for `direct`
    std::string runner_key;         // one of the runner keys
    std::string environment_text;   // at most 4096 bytes, no NUL; grammar is the launcher service's
    bool        is_default = false;
    Json_value  evidence   = Json_value::object();   // diagnostics only, at most k_max_evidence_bytes canonical
};

struct Launcher_recognition
{
    bool                                               recognized = false;
    std::string                                        display_name;            // recognized only
    std::optional<std::pair<std::string, std::string>> compatibility_warning;   // code (reason-code grammar), message
    Json_value                                         evidence = Json_value::object();
};

/// Host side: strict decode of a `discover` success payload. A payload
/// defect - shape, more than k_max_discovery_candidates, a candidate
/// outside its grammar, a duplicate candidate_id, more than one default -
/// fails the whole result with `discovery_candidate_invalid`. A candidate
/// whose runner is not supported on `platform_id` is dropped and reported
/// through `out_rejected` while the others survive.
std::optional<std::vector<Launcher_candidate>> decode_discover_result(
    const Json_value&              payload,
    std::string_view               platform_id,
    Contract_failure*              out_failure,
    std::vector<Contract_failure>* out_rejected = nullptr);

/// Host side: strict decode of a `recognize` success payload. Reasons:
/// `frame_violation`, `unexpected_member`.
std::optional<Launcher_recognition> decode_recognize_result(const Json_value& payload, Contract_failure* out_failure);

/// Adapter side: the success payloads.
Json_value encode_candidates(const std::vector<Launcher_candidate>& candidates);
Json_value encode_recognition(const Launcher_recognition& recognition);

} // namespace logonomic::cli_addon
