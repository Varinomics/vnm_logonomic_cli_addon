#pragma once

#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/json.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace logonomic::cli_addon {

inline constexpr const char* k_startup_observer_mechanism = "http-json-post";
inline constexpr const char* k_classify_startup_operation = "classify_startup";
inline constexpr std::size_t k_max_startup_observation_bytes = 1024 * 1024;
inline constexpr std::size_t k_max_startup_observation_ids = 64;

struct Startup_observer_declaration
{
    std::uint16_t version = 1;
    std::string mechanism = k_startup_observer_mechanism;
    std::string confirmation = "observed-identity";
};

struct Startup_observer_offer
{
    std::uint16_t version = 1;
    std::string mechanism = k_startup_observer_mechanism;
    std::string endpoint;
    std::string authorization;   // private HTTP header value; never argv or persisted artifacts
    std::string confirmation = "observed-identity";
};

struct Resume_declaration
{
    std::vector<std::string> environment_names;
    std::optional<Startup_observer_declaration> startup_observer;
};

Json_value encode_resume_declaration(const Resume_declaration& declaration, std::uint16_t version = 2);
std::optional<Resume_declaration> decode_resume_declaration(
    const Json_value& payload, Contract_failure* failure, std::uint16_t version = 2);
Json_value encode_startup_observer_offer(const Startup_observer_offer& offer);
std::optional<Startup_observer_offer> decode_startup_observer_offer(
    const Json_value& payload, Contract_failure* failure);

struct Startup_observation_request
{
    std::string body;   // authenticated HTTP body, base64 on the capability wire
};

struct Startup_observation_result
{
    std::vector<std::string> provider_session_ids;
};

Json_value encode_startup_observation_request(const Startup_observation_request& request);
std::optional<Startup_observation_request> decode_startup_observation_request(
    const Json_value& payload, Contract_failure* failure);
Json_value encode_startup_observation_result(const Startup_observation_result& result);
std::optional<Startup_observation_result> decode_startup_observation_result(
    const Json_value& payload, Contract_failure* failure);

} // namespace logonomic::cli_addon
