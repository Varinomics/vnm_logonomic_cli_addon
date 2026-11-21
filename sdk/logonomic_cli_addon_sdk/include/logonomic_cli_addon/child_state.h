#pragma once

#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/json.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace logonomic::cli_addon {

enum class Child_status { RUNNING, SUCCEEDED, FAILED, INTERRUPTED, UNKNOWN };

const char* child_status_name(Child_status status);

// Descriptive native evidence for an independently admitted lifecycle child.
// Empty parent_agent_id means the root. Parent/depth come from native structure,
// never from parsing an identity. All optional fields replace the prior sample.
struct Child_update
{
    std::string provider_session_id;
    std::string transition_nonce;
    std::uint64_t sequence = 0;
    std::string agent_id;
    std::string parent_agent_id;
    std::optional<std::uint32_t> depth;
    Child_status status = Child_status::UNKNOWN;
    std::optional<std::string> description;
    std::optional<std::string> current_tool;
    std::optional<double> progress;
    std::string source;

    bool operator==(const Child_update&) const = default;
};

// A loss belongs to the same root activation and single source sequence as
// child updates. It withdraws child liveness without completing any child or
// changing the root lifecycle. Later evidence must exceed this barrier.
struct Child_loss
{
    std::string provider_session_id;
    std::string transition_nonce;
    std::uint64_t sequence = 0;
    std::string reason_code;
    std::string source;

    bool operator==(const Child_loss&) const = default;
};

bool is_valid_child_update(const Child_update& update);
bool is_valid_child_loss(const Child_loss& loss);

void encode_child_state_request(Json_value& payload, std::uint32_t version);
void decode_child_state_request(
    const Json_value& payload,
    std::uint32_t& version,
    std::optional<Contract_failure>& failure);
void encode_child_updates(
    Json_value& payload,
    const std::vector<Child_update>& updates,
    const std::optional<Child_loss>& loss = std::nullopt,
    const std::optional<Contract_failure>& failure = std::nullopt);
void decode_child_updates(
    const Json_value& payload,
    std::vector<Child_update>& updates,
    std::optional<Child_loss>& loss,
    std::optional<Contract_failure>& failure);

} // namespace logonomic::cli_addon
