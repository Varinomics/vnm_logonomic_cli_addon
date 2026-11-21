#pragma once

#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/json.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace logonomic::cli_addon {

/**
 * @brief A capability sidecar `capabilities/<capability_id>@<version>.json`:
 * the static declaration of one capability version, with a payload the
 * capability owns and the core never interprets.
 */
struct Capability_sidecar
{
    std::string   capability_id;
    std::uint16_t version = 0;
    Json_value    payload;

    Capability_pair pair() const { return Capability_pair{capability_id, version}; }
};

/// Strict parse of the sidecar bytes. Reason codes: `invalid_sidecar`,
/// `invalid_utf8`, `custody_bound_exceeded` (k_max_sidecar_bytes).
std::optional<Capability_sidecar> parse_sidecar(std::string_view bytes, Contract_failure* out_failure);

/// Compact serialization; parse_sidecar accepts the result.
std::string serialize_sidecar(const Capability_sidecar& sidecar);

/// `<capability_id>@<version>.json`, the file name the sidecar must carry.
std::string sidecar_file_name(std::string_view capability_id, std::uint16_t version);

/// The package-relative path `capabilities/<capability_id>@<version>.json`.
std::string sidecar_relative_path(std::string_view capability_id, std::uint16_t version);

} // namespace logonomic::cli_addon
