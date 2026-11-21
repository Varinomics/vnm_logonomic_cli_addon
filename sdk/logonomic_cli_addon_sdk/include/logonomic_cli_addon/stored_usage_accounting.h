#pragma once

#include "logonomic_cli_addon/usage_scan.h"

namespace logonomic::cli_addon {

// Current provider-owned accounting records retain coverage and source metadata
// without asserting aggregate evidence or carrying observation capabilities.
// The serialized representation is stable because providers bind its exact bytes
// into current cache identities and ownership proofs.
Json_value encode_stored_usage_accounting(const Usage_record& record);
std::optional<Usage_record> decode_stored_usage_accounting(
    const Json_value& payload, Contract_failure* out_failure);

} // namespace logonomic::cli_addon
