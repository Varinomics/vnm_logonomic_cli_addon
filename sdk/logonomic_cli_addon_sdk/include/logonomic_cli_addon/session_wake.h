#pragma once

#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/json.h"
#include "logonomic_cli_addon/launcher_discovery.h"
#include "logonomic_cli_addon/lifecycle_events.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace logonomic::cli_addon {

inline constexpr const char* k_session_wake_member = "session_wake";
inline constexpr const char* k_session_wake_inspect_operation = "inspect";
inline constexpr const char* k_session_wake_notify_operation = "notify";
inline constexpr std::size_t k_max_session_wake_binding_bytes = 16384;
inline constexpr std::size_t k_max_session_wake_identity_bytes = 256;
inline constexpr std::size_t k_max_session_wake_instruction_bytes = 4096;

/// An independently negotiated offer in interactive preparation and resume.
struct Session_wake_offer
{};

/// Package-owned launch evidence, retained only by the live bound session.
/// Unavailable preparation leaves ordinary launching available.
struct Session_wake_preparation
{
    bool        available = false;
    Json_value  binding = Json_value::object();
    std::string reason;
};

using Session_wake_declaration = Launcher_discovery_declaration;

std::optional<Session_wake_declaration> decode_session_wake_declaration(
    const Json_value& payload, Contract_failure* out_failure);
bool validate_session_wake_declaration(const Json_value& payload, Contract_failure* out_failure);

Json_value encode_session_wake_offer(const Session_wake_offer& offer = {});
std::optional<Session_wake_offer> decode_session_wake_offer(
    const Json_value& payload, Contract_failure* out_failure);
Json_value encode_session_wake_preparation(const Session_wake_preparation& preparation);
std::optional<Session_wake_preparation> decode_session_wake_preparation(
    const Json_value& payload, Contract_failure* out_failure);

/// The host supplies exact admitted native identity and transition correlation.
/// Environment is the frozen launch input filtered by the capability declaration;
/// binding is the package's preparation result, never caller-authored authority.
struct Session_wake_target
{
    std::string              provider_session_id;
    std::string              working_directory;
    std::vector<std::string> launcher_argv;
    Launch_environment       launch_environment;
    Session_correlation      correlation;
    Json_value               binding = Json_value::object();
};

struct Session_wake_inspect_request
{
    Session_wake_target target;
};

struct Session_wake_readiness
{
    bool        available = false;
    std::string reason;
};

struct Session_wake_notify_request
{
    Session_wake_target target;
    std::string         notification_id;
    std::string         instruction;
};

enum class Session_wake_disposition
{
    ACCEPTED,
    DEFERRED,
    UNAVAILABLE,
    UNCERTAIN,
};

/// ACCEPTED means the native API accepted the notification, never read or ACK.
/// UNCERTAIN is terminal for automatic retries: the native side may have accepted.
struct Session_wake_notify_result
{
    Session_wake_disposition    disposition = Session_wake_disposition::UNAVAILABLE;
    std::string                reason;
    std::optional<std::string> receipt;
};

const char* session_wake_disposition_name(Session_wake_disposition disposition);
Json_value encode_session_wake_inspect_request(const Session_wake_inspect_request& request);
std::optional<Session_wake_inspect_request> decode_session_wake_inspect_request(
    const Json_value& payload, Contract_failure* out_failure);
Json_value encode_session_wake_readiness(const Session_wake_readiness& readiness);
std::optional<Session_wake_readiness> decode_session_wake_readiness(
    const Json_value& payload, Contract_failure* out_failure);
Json_value encode_session_wake_notify_request(const Session_wake_notify_request& request);
std::optional<Session_wake_notify_request> decode_session_wake_notify_request(
    const Json_value& payload, Contract_failure* out_failure);
Json_value encode_session_wake_notify_result(const Session_wake_notify_result& result);
std::optional<Session_wake_notify_result> decode_session_wake_notify_result(
    const Json_value& payload, Contract_failure* out_failure);

} // namespace logonomic::cli_addon
