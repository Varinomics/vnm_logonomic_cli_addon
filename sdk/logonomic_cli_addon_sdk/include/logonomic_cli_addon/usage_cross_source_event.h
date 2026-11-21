#pragma once

#include "logonomic_cli_addon/capability_ids.h"
#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/json.h"

#include <optional>
#include <string>

namespace logonomic::cli_addon {

// Presence attests that the enclosing usage record is one complete atomic
// token event. The namespace is provider-owned and comparable across source
// profiles; different event IDs in one namespace are disjoint events.
struct Usage_cross_source_event
{
    std::string authority_namespace;
    std::string event_id;
};

bool decode_usage_cross_source_event_declaration(
    const Json_value& payload, Contract_failure* out_failure);

Json_value encode_usage_cross_source_event(const Usage_cross_source_event& event);
std::optional<Usage_cross_source_event> decode_usage_cross_source_event(
    const Json_value& payload, Contract_failure* out_failure);

} // namespace logonomic::cli_addon
