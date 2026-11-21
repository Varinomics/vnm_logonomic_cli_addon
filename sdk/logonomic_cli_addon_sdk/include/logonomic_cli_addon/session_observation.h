#pragma once

#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/json.h"

#include <optional>
#include <string>

namespace logonomic::cli_addon {

// These are public native identities. They select an observation only after
// its parent authority has been authenticated; they never grant authority.
struct Session_observation_identity
{
    std::string instance_id;
    std::string group_id;
    std::string seat_id;
    std::string occupant_id;

    bool operator==(const Session_observation_identity&) const = default;
};

struct Session_observation_registration
{
    std::string request_nonce;
    Session_observation_identity identity;
    std::string native_package_id;
    std::string working_directory;
    std::optional<std::string> replaces_session_id;

    bool operator==(const Session_observation_registration&) const = default;
};

struct Session_observation_authority
{
    std::string session_id;
    std::string session_capability;
    std::string activation_id;

    bool operator==(const Session_observation_authority&) const = default;
};

struct Session_observation_retirement
{
    std::string session_id;
    std::string activation_id;
};

Json_value encode_session_observation_registration(const Session_observation_registration& registration);
std::optional<Session_observation_registration> decode_session_observation_registration(
    const Json_value& payload, Contract_failure* failure);
Json_value encode_session_observation_authority(const Session_observation_authority& authority);
std::optional<Session_observation_authority> decode_session_observation_authority(
    const Json_value& payload, Contract_failure* failure);
Json_value encode_session_observation_retirement(const Session_observation_retirement& retirement);
std::optional<Session_observation_retirement> decode_session_observation_retirement(
    const Json_value& payload, Contract_failure* failure);

} // namespace logonomic::cli_addon
