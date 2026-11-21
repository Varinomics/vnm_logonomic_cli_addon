#pragma once

#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/limits.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace logonomic::cli_addon {

/**
 * @brief Bounds of the XML layer of the SVG subset (docs/svg_subset.md).
 * The defaults are the contract's values from limits.h; a caller lowers
 * them only for tests.
 */
struct Svg_subset_limits
{
    std::uint64_t max_bytes                  = limits::k_max_icon_source_bytes;
    std::size_t   max_elements               = limits::k_max_svg_elements;
    std::size_t   max_depth                  = limits::k_max_svg_depth;
    std::size_t   max_attributes_per_element = limits::k_max_svg_attributes_per_element;
    std::size_t   max_path_data_bytes        = limits::k_max_svg_path_data_bytes;   // `d` and `points`
    std::size_t   max_path_segments          = limits::k_max_svg_path_segments;
    std::size_t   max_attribute_bytes        = limits::k_max_svg_attribute_bytes;   // every other attribute
    std::size_t   max_expanded_elements      = limits::k_max_svg_expanded_elements;
};

/**
 * @brief The canonical form of a document that passed the subset: the XML
 * declaration, then the elements in document order with attributes sorted
 * bytewise by name, values re-escaped minimally, `title` and `desc`
 * emptied, no whitespace between elements, `\n` line endings, and the root
 * `color` attribute set to the current colour the caller asked for.
 */
struct Svg_canonical
{
    std::string canonical_bytes;
    double      view_box_width  = 0.0;
    double      view_box_height = 0.0;
};

/// Parses the strict XML layer, validates the closed SVG subset and returns
/// the canonical bytes. The subset is closed: every element, attribute,
/// value form and reference that docs/svg_subset.md does not list is the
/// violation `svg_subset_violation`, whose detail names the element,
/// attribute or rule. Bytes outside RFC 3629 are `invalid_utf8`.
/// `current_color_hex` is the `#RRGGBB` the root `color` attribute is set
/// to, so that `currentColor` rasterizes deterministically per variant.
std::optional<Svg_canonical> sanitize_svg_subset(
    std::string_view         bytes,
    const Svg_subset_limits& limits,
    std::string_view         current_color_hex,
    Contract_failure*        out_failure);

/// One of the 147 colour keywords of SVG 1.1 section 4.4, in lowercase, the
/// only keyword spelling the subset accepts.
bool is_svg_color_keyword(std::string_view name);

} // namespace logonomic::cli_addon
