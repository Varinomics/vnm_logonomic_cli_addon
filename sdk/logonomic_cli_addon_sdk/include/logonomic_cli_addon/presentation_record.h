#pragma once

#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/json.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace logonomic::cli_addon {

/**
 * @brief The presentation@1 sidecar payload (docs/presentation_1.md): a
 * display name, one light and one dark icon below assets/, and optional
 * accent hints. This record is the sidecar payload; the host consumes it only
 * when the same pair is also in the adapter handshake and host adoption set.
 */

/// Accent colours as `#RRGGBB` with uppercase hex digits.
struct Presentation_accent
{
    std::string light_hex;
    std::string dark_hex;
};

struct Presentation_record
{
    std::string                        display_name;      // trimmed
    std::string                        light_icon_path;   // canonical, first segment assets
    std::string                        dark_icon_path;    // may equal light_icon_path
    std::optional<Presentation_accent> accent;
};

/// Strict decode of the payload. Index membership of the icon paths is the
/// caller's check (the record does not carry the index). Reason:
/// `presentation_invalid`, the detail naming the member.
std::optional<Presentation_record> decode_presentation_record(const Json_value& payload, Contract_failure* out_failure);

/// The payload form; decode_presentation_record accepts the result.
Json_value encode_presentation_record(const Presentation_record& record);

/// Applies the display-name rule shared by presentation@1 and
/// launcher-discovery@1: ASCII and Unicode white space trimmed, then 1-40
/// Unicode scalar values, at most 120 UTF-8 bytes, no control characters,
/// no line breaks. Returns the trimmed name, or nullopt with the violated
/// rule in `out_detail`. The input must be valid UTF-8.
std::optional<std::string> normalized_display_name(std::string_view name, std::string* out_detail);

/// Fits text to that display-name rule. Surrounding white space is trimmed and
/// controls and line breaks become U+FFFD. Text still longer than the rule
/// allows keeps as many of its leading and trailing scalar values as fit
/// around U+2026, the leading part never shorter than the trailing part.
/// Returns nullopt only when nothing remains after trimming. The input must be
/// valid UTF-8.
std::optional<std::string> fitted_display_name(std::string_view text);

/// Icon media is decided by content, never by file extension.
enum class Icon_media
{
    PNG,
    SVG,
    UNSUPPORTED,
};

const char* icon_media_name(Icon_media media);

/// PNG when the bytes begin with the PNG signature; SVG when the first
/// element of the document is `svg`, whatever prologue precedes it (the
/// sanitizer then decides whether that prologue is allowed); otherwise
/// UNSUPPORTED.
Icon_media sniff_icon_media(std::string_view bytes);

/// The IHDR facts of a PNG stream (ISO/IEC 15948 section 11.2.2).
struct Png_header
{
    std::uint32_t width            = 0;
    std::uint32_t height           = 0;
    std::uint8_t  bit_depth        = 0;
    std::uint8_t  color_type       = 0;
    std::uint8_t  interlace_method = 0;
};

/// Validates the complete single-frame PNG structure without decoding pixels:
/// signature, unique first IHDR, legal critical-chunk order, every chunk CRC,
/// consecutive IDAT, terminal zero-length IEND with no trailing bytes, and no
/// APNG chunks. A violation is `icon_media_unsupported`; dimension bounds are
/// inspect_icon's job.
std::optional<Png_header> read_png_header(std::string_view bytes, Contract_failure* out_failure);

/// The root `color` the host sets when rasterizing each variant, so that
/// `currentColor` icons rasterize deterministically under presentation@1.
inline constexpr const char* k_icon_light_current_color = "#1F1F1F";
inline constexpr const char* k_icon_dark_current_color  = "#F2F2F2";

/// True when both dimensions are within [k_min_icon_pixels,
/// k_max_icon_pixels] and the aspect ratio is within [1:4, 4:1]. SVG
/// viewBox dimensions are checked against the aspect bound only.
bool icon_pixel_dimensions_in_bounds(std::uint32_t width, std::uint32_t height);
bool icon_aspect_in_bounds(double width, double height);

struct Icon_inspection
{
    Icon_media media  = Icon_media::UNSUPPORTED;
    double     width  = 0.0;   // pixels for PNG, viewBox units for SVG
    double     height = 0.0;
};

/// Everything the SDK can check about icon bytes without a rasterizer: the
/// source size bound (`icon_too_large`), the media sniff
/// (`icon_media_unsupported`), the PNG header and its dimension bounds
/// (`icon_dimensions_invalid`), or the SVG subset (`svg_subset_violation`,
/// `invalid_utf8`) and its viewBox aspect (`icon_dimensions_invalid`).
std::optional<Icon_inspection> inspect_icon(std::string_view bytes, Contract_failure* out_failure);

} // namespace logonomic::cli_addon
