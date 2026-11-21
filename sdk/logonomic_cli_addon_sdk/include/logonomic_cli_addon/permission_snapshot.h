#pragma once

#include "logonomic_cli_addon/helper_ingress.h"

namespace logonomic::cli_addon {

inline constexpr const char* k_permission_snapshot_capability_id = "permission-snapshot";
inline constexpr std::uint16_t k_permission_snapshot_version = 1;
/// Enabled only when this host-owned value equals the nonempty provider
/// generation nonce. An inherited marker from another generation is inert.
inline constexpr const char* k_permission_snapshot_enabled_environment =
    "LOGONOMIC_PERMISSION_SNAPSHOT_ENABLED";

/// Optional native permission level observer. Its transform operation uses the
/// managed-configuration@3 fixed-home authority and transform wire contract.
/// Only event_name is classified through this capability; all other helper
/// events retain their negotiated helper-ingress or attention companion.
struct Permission_snapshot_declaration
{
    std::string relative_path;
    std::string event_name;
};

Json_value encode_permission_snapshot_declaration(const Permission_snapshot_declaration& declaration);
std::optional<Permission_snapshot_declaration> decode_permission_snapshot_declaration(
    const Json_value& payload, Contract_failure* out_failure);

struct Permission_snapshot_update
{
    std::uint16_t version = 1;
    std::size_t observation_index = 0;
    bool pending = false;
};

/// classify_event takes Classify_event_request. A snapshot accompanies exactly
/// one neutral current-session observation. The host supplies episode identity;
/// the callback cannot begin an epoch, advance a turn or resolve other attention.
struct Permission_snapshot_classification_result
{
    Classify_event_result base;
    std::optional<Permission_snapshot_update> permission_snapshot;
};

Json_value encode_permission_snapshot_classification_result(
    const Permission_snapshot_classification_result& result);
std::optional<Permission_snapshot_classification_result> decode_permission_snapshot_classification_result(
    const Json_value& payload, Contract_failure* out_failure, std::uint16_t helper_version = 2);

} // namespace logonomic::cli_addon
