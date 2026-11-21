#pragma once

#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/json.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace logonomic::cli_addon {

enum class Model_status { UNKNOWN, CONFIRMED, UNAVAILABLE };

const char* model_status_name(Model_status status);

// Native observations, never requested launch/delegation settings. Empty
// agent_id denotes the root; a child uses its admitted lifecycle identity.
// Capture activation and source order before asynchronous read or delivery.
struct Model_update
{
    std::string provider_session_id;
    std::string transition_nonce;
    std::uint64_t sequence = 0;
    std::string agent_id;
    Model_status status = Model_status::UNKNOWN;
    std::string provider;
    std::string model;
    std::optional<std::string> reasoning_effort;
    std::string source;

    bool operator==(const Model_update&) const = default;
};

// One actor in one accepted root activation. Unknown observations and host
// loss withdraw facts while retaining the sequence replay barrier.
struct Model_state
{
    std::string provider_session_id;
    std::string transition_nonce;
    std::string agent_id;
    std::optional<std::uint64_t> sequence;
    Model_status status = Model_status::UNKNOWN;
    std::string provider;
    std::string model;
    std::optional<std::string> reasoning_effort;
    std::string source;

    bool operator==(const Model_state&) const = default;
};

// An inspection is already bound to one host-owned structured execution and
// delivered serially. It therefore carries only that execution's own native
// observation, without interactive activation or child identity fields.
struct Structured_model_observation
{
    std::string provider;
    std::string model;
    std::string source;

    bool operator==(const Structured_model_observation&) const = default;
};

bool is_valid_model_label(std::string_view model);
bool is_valid_structured_model_observation(const Structured_model_observation& observation);
bool is_valid_model_update(const Model_update& update);
bool apply_model_update(Model_state& state, const Model_update& update);
void invalidate_model_state(Model_state& state);

// Presence-tagged optional envelope. Malformed telemetry cannot discard an
// adjacent valid lifecycle event. Omission means no update, not withdrawal.
void encode_model_state_request(Json_value& payload, std::uint32_t version);
void decode_model_state_request(
    const Json_value& payload,
    std::uint32_t& version,
    std::optional<Contract_failure>& failure);
void encode_model_updates(
    Json_value& payload,
    const std::vector<Model_update>& updates,
    const std::optional<Contract_failure>& failure = std::nullopt);
void decode_model_updates(
    const Json_value& payload,
    std::vector<Model_update>& updates,
    std::optional<Contract_failure>& failure);
void encode_structured_model_observation(
    Json_value& payload,
    const std::optional<Structured_model_observation>& observation,
    const std::optional<Contract_failure>& failure = std::nullopt);
void decode_structured_model_observation(
    const Json_value& payload,
    std::optional<Structured_model_observation>& observation,
    std::optional<Contract_failure>& failure);

} // namespace logonomic::cli_addon
