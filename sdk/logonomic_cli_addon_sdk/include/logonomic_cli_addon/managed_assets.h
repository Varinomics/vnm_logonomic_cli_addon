#pragma once

#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/integrity_index.h"
#include "logonomic_cli_addon/json.h"

#include <optional>
#include <string>
#include <vector>

namespace logonomic::cli_addon {

/**
 * @brief managed-assets@1 (docs/managed_assets_1.md): a record-backed
 * capability naming the package assets the host may copy, exactly as
 * custodied, into host-owned storage and hand to the CLI through a
 * launch-preparation@3 patch. It becomes authoritative only through
 * sidecar/handshake/adoption intersection. Assets are never executed.
 */
struct Managed_assets_declaration
{
    std::vector<std::string> assets;   // 1-64 canonical paths below assets/, each indexed as kind asset
};

/// Strict decode and policy against the index: canonical paths below
/// assets/ (`path_escape`), no duplicates and 1-64 entries
/// (`invalid_sidecar`), every path indexed with kind asset
/// (`asset_reference_invalid`), each at most k_max_managed_asset_bytes and
/// all together at most k_max_managed_assets_bytes (`asset_too_large`).
std::optional<Managed_assets_declaration> decode_managed_assets_declaration(
    const Json_value&      payload,
    const Integrity_index& index,
    Contract_failure*      out_failure);

Json_value encode_managed_assets_declaration(const Managed_assets_declaration& declaration);

} // namespace logonomic::cli_addon
