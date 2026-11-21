#pragma once

#include "logonomic_cli_addon/launch_preparation.h"

namespace logonomic::cli_addon::launch_patch_fields {

// Environment, working-directory and optional account/profile/invocation fields
// shared by the current launch and resume contracts. Each caller owns its
// operation-specific envelope and acknowledgment requirements.
Json_value encode_fields(const Launch_patch& patch);
std::optional<Launch_patch> decode_fields(const Json_value& payload, Contract_failure* out_failure);

} // namespace logonomic::cli_addon::launch_patch_fields
