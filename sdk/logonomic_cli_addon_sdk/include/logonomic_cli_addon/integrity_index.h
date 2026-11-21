#pragma once

#include "logonomic_cli_addon/ids.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace logonomic::cli_addon {

/// Resource classification recorded for every indexed file; each kind is
/// fixed by the first path segment.
enum class Resource_kind
{
    EXECUTABLE,   // adapter/
    SIDECAR,      // capabilities/
    HELPER,       // helpers/
    ASSET,        // assets/
    NOTICE,       // notices/
    PROVENANCE,   // provenance/
};

enum class Resource_origin
{
    PACKAGE,       // original package-owned resource, recorded explicitly
    THIRD_PARTY,   // requires digest-bound notice and provenance records
};

const char* resource_kind_name(Resource_kind kind);
const char* resource_origin_name(Resource_origin origin);
std::optional<Resource_kind>   parse_resource_kind(std::string_view name);
std::optional<Resource_origin> parse_resource_origin(std::string_view name);

/// The kind a canonical path's first segment classifies, or nullopt when the
/// first segment is not a package directory.
std::optional<Resource_kind> resource_kind_for_path(std::string_view path);

/**
 * @brief One entry of `package.files.json`. `notice` and `provenance` are
 * present exactly when `origin` is THIRD_PARTY.
 */
struct Index_entry
{
    std::string     path;
    std::uint64_t   bytes = 0;
    std::string     sha256_hex;
    Resource_kind   kind   = Resource_kind::ASSET;
    Resource_origin origin = Resource_origin::PACKAGE;
    std::string     notice;
    std::string     provenance;
};

/**
 * @brief The integrity index: every package byte except the bootstrap and the
 * index itself, sorted bytewise by path (docs/package_contract.md,
 * "Integrity index").
 */
struct Integrity_index
{
    std::uint16_t            index_version = 0;
    std::vector<Index_entry> files;

    const Index_entry* find(std::string_view path) const;
    std::uint64_t total_bytes() const;
};

/// Strict parse of the index bytes, including every check that needs no file
/// content: grammar, ordering, kind/segment agreement, origin rules,
/// third-party references (present, indexed, non-empty), bounds. Reason
/// codes: `invalid_index`, `invalid_utf8`, `path_escape`,
/// `third_party_record_invalid`, `custody_bound_exceeded`.
std::optional<Integrity_index> parse_integrity_index(std::string_view bytes, Contract_failure* out_failure);

/// Canonical serialization (entries sorted by path, fixed member order); the
/// index tool writes this form and parse_integrity_index requires it.
std::string serialize_integrity_index(const Integrity_index& index);

} // namespace logonomic::cli_addon
