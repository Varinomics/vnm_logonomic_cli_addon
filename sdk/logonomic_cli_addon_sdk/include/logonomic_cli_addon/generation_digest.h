#pragma once

#include "logonomic_cli_addon/integrity_index.h"
#include "logonomic_cli_addon/sha256.h"

#include <string>
#include <string_view>
#include <vector>

namespace logonomic::cli_addon {

/**
 * @brief The generation digest (docs/package_contract.md, "Generation
 * digest"): SHA-256 over the length-prefixed bootstrap bytes, the
 * length-prefixed index bytes, and the raw 32-byte digest of every indexed
 * file in index order. Length prefixes are 8-byte big-endian byte counts.
 * Rendered as 64 lowercase hexadecimal characters.
 */
std::string compute_generation_digest_hex(
    std::string_view                  bootstrap_bytes,
    std::string_view                  index_bytes,
    const std::vector<Sha256_digest>& file_digests_in_index_order);

/// The same digest from an index whose entry digests the caller has verified
/// against the bytes it holds custody of. The index is required to be a
/// parse_integrity_index result, so every digest string is well-formed.
std::string compute_generation_digest_hex(
    std::string_view       bootstrap_bytes,
    std::string_view       index_bytes,
    const Integrity_index& verified_index);

} // namespace logonomic::cli_addon
