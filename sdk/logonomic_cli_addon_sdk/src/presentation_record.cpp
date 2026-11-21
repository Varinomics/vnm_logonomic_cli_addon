#include "logonomic_cli_addon/presentation_record.h"
#include "logonomic_cli_addon/limits.h"
#include "logonomic_cli_addon/svg_subset.h"
#include "strict_json.h"

#include <algorithm>
#include <vector>

namespace logonomic::cli_addon {

namespace {

constexpr std::string_view k_png_signature = "\x89PNG\r\n\x1A\n";

std::optional<Presentation_record> fail(Contract_failure* out_failure, std::string detail)
{
    if (out_failure != nullptr) {
        out_failure->reason_code = reason::k_presentation_invalid;
        out_failure->detail      = std::move(detail);
    }
    return std::nullopt;
}

// Decodes valid UTF-8 into scalar values.
std::vector<std::uint32_t> scalars_of(std::string_view text)
{
    std::vector<std::uint32_t> scalars;
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char lead = (unsigned char)text[i];
        std::size_t   length = 1;
        std::uint32_t scalar = lead;
        if (lead >= 0xF0)      { length = 4; scalar = lead & 0x07; }
        else if (lead >= 0xE0) { length = 3; scalar = lead & 0x0F; }
        else if (lead >= 0xC0) { length = 2; scalar = lead & 0x1F; }
        for (std::size_t k = 1; k < length; ++k) {
            scalar = (scalar << 6) | ((unsigned char)text[i + k] & 0x3F);
        }
        scalars.push_back(scalar);
        i += length;
    }
    return scalars;
}

void append_utf8(std::string& out, std::uint32_t scalar)
{
    if (scalar < 0x80) {
        out += (char)scalar;
    }
    else
    if (scalar < 0x800) {
        out += (char)(0xC0 | (scalar >> 6));
        out += (char)(0x80 | (scalar & 0x3F));
    }
    else
    if (scalar < 0x10000) {
        out += (char)(0xE0 | (scalar >> 12));
        out += (char)(0x80 | ((scalar >> 6) & 0x3F));
        out += (char)(0x80 | (scalar & 0x3F));
    }
    else {
        out += (char)(0xF0 | (scalar >> 18));
        out += (char)(0x80 | ((scalar >> 12) & 0x3F));
        out += (char)(0x80 | ((scalar >> 6) & 0x3F));
        out += (char)(0x80 | (scalar & 0x3F));
    }
}

// The Unicode White_Space property.
bool is_white_space(std::uint32_t scalar)
{
    return (scalar >= 0x09 && scalar <= 0x0D) || scalar == 0x20 || scalar == 0x85 || scalar == 0xA0 ||
           scalar == 0x1680 || (scalar >= 0x2000 && scalar <= 0x200A) || scalar == 0x2028 ||
           scalar == 0x2029 || scalar == 0x202F || scalar == 0x205F || scalar == 0x3000;
}

// C0 and C1 controls and DEL; the Unicode line and paragraph separators
// count as line breaks.
bool is_control_or_line_break(std::uint32_t scalar)
{
    return scalar < 0x20 || scalar == 0x7F || (scalar >= 0x80 && scalar <= 0x9F) ||
           scalar == 0x2028 || scalar == 0x2029;
}

bool is_hex_digit(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// `#RRGGBB` in either case, normalized to uppercase.
std::optional<std::string> normalized_accent(const std::string& text)
{
    if (text.size() != 7 || text[0] != '#') {
        return std::nullopt;
    }
    std::string normalized = text;
    for (std::size_t i = 1; i < normalized.size(); ++i) {
        const unsigned char c = (unsigned char)normalized[i];
        if (!is_hex_digit(c)) {
            return std::nullopt;
        }
        if (c >= 'a' && c <= 'f') {
            normalized[i] = (char)(c - 'a' + 'A');
        }
    }
    return normalized;
}

bool decode_icon_path(const Json_value& icon, const char* member, std::string& out_path, Contract_failure* out_failure)
{
    const std::string* path = strict::string_member(icon, member);
    if (path == nullptr) {
        fail(out_failure, std::string("icon.") + member + " is missing or not a string");
        return false;
    }
    if (!is_valid_package_relative_path(*path) || path_first_segment(*path) != k_segment_assets) {
        fail(out_failure, std::string("icon.") + member + " is not a canonical path below assets/: " + *path);
        return false;
    }
    out_path = *path;
    return true;
}

std::uint32_t read_be32(std::string_view bytes, std::size_t offset)
{
    return ((std::uint32_t)(unsigned char)bytes[offset] << 24) |
           ((std::uint32_t)(unsigned char)bytes[offset + 1] << 16) |
           ((std::uint32_t)(unsigned char)bytes[offset + 2] << 8) |
           (std::uint32_t)(unsigned char)bytes[offset + 3];
}

std::uint32_t png_crc(std::string_view bytes)
{
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const unsigned char byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

bool is_png_chunk_type(std::string_view type)
{
    if (type.size() != 4) {
        return false;
    }
    const bool letters = std::all_of(type.begin(), type.end(), [](unsigned char byte) {
        return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z');
    });
    // PNG reserves the third type bit; it must be uppercase in every valid
    // stream, including for private ancillary chunks.
    return letters && type[2] >= 'A' && type[2] <= 'Z';
}

std::optional<Png_header> png_failure(Contract_failure* out_failure, std::string detail)
{
    if (out_failure != nullptr) {
        out_failure->reason_code = reason::k_icon_media_unsupported;
        out_failure->detail      = std::move(detail);
    }
    return std::nullopt;
}

bool is_xml_space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// Skips an XML prologue item - declaration, processing instruction,
// comment or DOCTYPE - so the sniff finds the first element.
bool skip_prologue_item(std::string_view bytes, std::size_t& pos)
{
    if (bytes.substr(pos, 4) == "<!--") {
        const std::size_t end = bytes.find("-->", pos + 4);
        if (end == std::string_view::npos) {
            return false;
        }
        pos = end + 3;
        return true;
    }
    if (bytes.substr(pos, 2) == "<?" || bytes.substr(pos, 2) == "<!") {
        const std::size_t end = bytes.find('>', pos + 2);
        if (end == std::string_view::npos) {
            return false;
        }
        pos = end + 1;
        return true;
    }
    return false;
}

} // namespace

std::optional<Presentation_record> decode_presentation_record(const Json_value& payload, Contract_failure* out_failure)
{
    if (!payload.is_object()) {
        return fail(out_failure, "payload is not an object");
    }
    const std::optional<std::string> unexpected = strict::unexpected_member(payload, {"display_name", "icon", "accent"});
    if (unexpected.has_value()) {
        return fail(out_failure, "unexpected member " + *unexpected);
    }

    Presentation_record record;
    const std::string* display_name = strict::string_member(payload, "display_name");
    if (display_name == nullptr) {
        return fail(out_failure, "display_name is missing or not a string");
    }
    std::string detail;
    const std::optional<std::string> normalized = normalized_display_name(*display_name, &detail);
    if (!normalized.has_value()) {
        return fail(out_failure, "display_name " + detail);
    }
    record.display_name = *normalized;

    const Json_value* icon = strict::member_of_type(payload, "icon", Json_type::OBJECT);
    if (icon == nullptr) {
        return fail(out_failure, "icon is missing or not an object");
    }
    const std::optional<std::string> unexpected_icon = strict::unexpected_member(*icon, {"light", "dark"});
    if (unexpected_icon.has_value()) {
        return fail(out_failure, "unexpected member icon." + *unexpected_icon);
    }
    if (!decode_icon_path(*icon, "light", record.light_icon_path, out_failure) ||
        !decode_icon_path(*icon, "dark", record.dark_icon_path, out_failure))
    {
        return std::nullopt;
    }

    if (const Json_value* accent = payload.find("accent"); accent != nullptr) {
        if (!accent->is_object()) {
            return fail(out_failure, "accent is not an object");
        }
        const std::optional<std::string> unexpected_accent = strict::unexpected_member(*accent, {"light", "dark"});
        if (unexpected_accent.has_value()) {
            return fail(out_failure, "unexpected member accent." + *unexpected_accent);
        }
        Presentation_accent colours;
        for (const auto& [member, target] : {std::pair{"light", &colours.light_hex}, std::pair{"dark", &colours.dark_hex}}) {
            const std::string* text = strict::string_member(*accent, member);
            const std::optional<std::string> hex = text == nullptr ? std::nullopt : normalized_accent(*text);
            if (!hex.has_value()) {
                return fail(out_failure, std::string("accent.") + member + " is not a #RRGGBB colour");
            }
            *target = *hex;
        }
        record.accent = std::move(colours);
    }
    return record;
}

Json_value encode_presentation_record(const Presentation_record& record)
{
    Json_value icon = Json_value::object();
    icon.set("light", Json_value(record.light_icon_path));
    icon.set("dark",  Json_value(record.dark_icon_path));

    Json_value payload = Json_value::object();
    payload.set("display_name", Json_value(record.display_name));
    payload.set("icon",         std::move(icon));
    if (record.accent.has_value()) {
        Json_value accent = Json_value::object();
        accent.set("light", Json_value(record.accent->light_hex));
        accent.set("dark",  Json_value(record.accent->dark_hex));
        payload.set("accent", std::move(accent));
    }
    return payload;
}

std::optional<std::string> normalized_display_name(std::string_view name, std::string* out_detail)
{
    std::vector<std::uint32_t> scalars = scalars_of(name);
    std::size_t begin = 0;
    while (begin < scalars.size() && is_white_space(scalars[begin])) {
        ++begin;
    }
    std::size_t end = scalars.size();
    while (end > begin && is_white_space(scalars[end - 1])) {
        --end;
    }
    if (begin == end) {
        *out_detail = "is empty after trimming white space";
        return std::nullopt;
    }
    if (end - begin > limits::k_max_display_name_scalars) {
        *out_detail = "exceeds " + std::to_string(limits::k_max_display_name_scalars) + " Unicode scalar values";
        return std::nullopt;
    }
    if (std::any_of(
            scalars.begin() + static_cast<std::ptrdiff_t>(begin),
            scalars.begin() + static_cast<std::ptrdiff_t>(end),
            is_control_or_line_break))
    {
        *out_detail = "contains a control character or line break";
        return std::nullopt;
    }
    std::string trimmed;
    for (std::size_t i = begin; i < end; ++i) {
        append_utf8(trimmed, scalars[i]);
    }
    if (trimmed.size() > limits::k_max_display_name_bytes) {
        *out_detail = "exceeds " + std::to_string(limits::k_max_display_name_bytes) + " UTF-8 bytes";
        return std::nullopt;
    }
    return trimmed;
}

std::optional<std::string> fitted_display_name(std::string_view text)
{
    std::vector<std::uint32_t> scalars = scalars_of(text);
    std::size_t begin = 0;
    std::size_t end   = scalars.size();
    while (begin < end && is_white_space(scalars[begin])) {
        ++begin;
    }
    while (end > begin && is_white_space(scalars[end - 1])) {
        --end;
    }
    if (begin == end) {
        return std::nullopt;
    }
    const auto encoded = [&scalars](std::size_t first, std::size_t last) {
        std::string result;
        for (std::size_t i = first; i < last; ++i) {
            append_utf8(result, is_control_or_line_break(scalars[i]) ? 0xFFFDu : scalars[i]);
        }
        return result;
    };
    std::string whole = encoded(begin, end);
    if (end - begin <= limits::k_max_display_name_scalars && whole.size() <= limits::k_max_display_name_bytes) {
        return whole;
    }
    // Fewer kept scalars always fit: the ellipsis alone is one scalar of
    // three bytes, so the search ends by the time nothing is kept.
    for (std::size_t kept = limits::k_max_display_name_scalars - 1;; --kept) {
        const std::size_t head = (kept + 1) / 2;
        const std::size_t tail = kept / 2;
        std::string fitted = encoded(begin, begin + head) + "\xE2\x80\xA6" + encoded(end - tail, end);
        if (fitted.size() <= limits::k_max_display_name_bytes) {
            return fitted;
        }
    }
}

const char* icon_media_name(Icon_media media)
{
    switch (media) {
        case Icon_media::PNG:         return "png";
        case Icon_media::SVG:         return "svg";
        case Icon_media::UNSUPPORTED: return "unsupported";
        default:                      return "unsupported";
    }
}

Icon_media sniff_icon_media(std::string_view bytes)
{
    if (bytes.substr(0, k_png_signature.size()) == k_png_signature) {
        return Icon_media::PNG;
    }
    std::size_t pos = 0;
    if (bytes.substr(0, 3) == "\xEF\xBB\xBF") {
        pos = 3;
    }
    while (true) {
        while (pos < bytes.size() && is_xml_space((unsigned char)bytes[pos])) {
            ++pos;
        }
        if (pos >= bytes.size() || bytes[pos] != '<') {
            return Icon_media::UNSUPPORTED;
        }
        if (!skip_prologue_item(bytes, pos)) {
            break;
        }
    }
    if (bytes.substr(pos, 4) != "<svg") {
        return Icon_media::UNSUPPORTED;
    }
    const std::size_t after = pos + 4;
    const bool delimited = after >= bytes.size() || is_xml_space((unsigned char)bytes[after]) ||
                           bytes[after] == '>' || bytes[after] == '/';
    return delimited ? Icon_media::SVG : Icon_media::UNSUPPORTED;
}

std::optional<Png_header> read_png_header(std::string_view bytes, Contract_failure* out_failure)
{
    if (bytes.substr(0, k_png_signature.size()) != k_png_signature) {
        return png_failure(out_failure, "bytes do not begin with the PNG signature");
    }
    constexpr std::size_t k_chunk_overhead = 12;   // length, type, crc
    std::size_t pos = k_png_signature.size();
    if (bytes.size() < pos + k_chunk_overhead + 13 || bytes.substr(pos + 4, 4) != "IHDR" || read_be32(bytes, pos) != 13) {
        return png_failure(out_failure, "the first chunk is not a 13-byte IHDR");
    }
    Png_header header;
    header.width            = read_be32(bytes, pos + 8);
    header.height           = read_be32(bytes, pos + 12);
    header.bit_depth        = (std::uint8_t)bytes[pos + 16];
    header.color_type       = (std::uint8_t)bytes[pos + 17];
    const std::uint8_t compression = (std::uint8_t)bytes[pos + 18];
    const std::uint8_t filter      = (std::uint8_t)bytes[pos + 19];
    header.interlace_method = (std::uint8_t)bytes[pos + 20];
    if (header.width == 0 || header.height == 0 || header.width > 0x7FFFFFFFu || header.height > 0x7FFFFFFFu) {
        return png_failure(out_failure, "IHDR dimensions are zero or above 2^31 - 1");
    }
    const bool format_ok =
        (header.color_type == 0 &&
            (header.bit_depth == 1 || header.bit_depth == 2 ||
             header.bit_depth == 4 || header.bit_depth == 8 ||
             header.bit_depth == 16)) ||
        (header.color_type == 2 &&
            (header.bit_depth == 8 || header.bit_depth == 16)) ||
        (header.color_type == 3 &&
            (header.bit_depth == 1 || header.bit_depth == 2 ||
             header.bit_depth == 4 || header.bit_depth == 8)) ||
        (header.color_type == 4 &&
            (header.bit_depth == 8 || header.bit_depth == 16)) ||
        (header.color_type == 6 &&
            (header.bit_depth == 8 || header.bit_depth == 16));
    if (!format_ok || compression != 0 || filter != 0 || header.interlace_method > 1) {
        return png_failure(out_failure, "IHDR carries a bit depth, colour type, compression, filter or interlace method outside the specification");
    }
    if (png_crc(bytes.substr(pos + 4, 4 + 13)) != read_be32(bytes, pos + 8 + 13)) {
        return png_failure(out_failure, "IHDR CRC does not match its type and data");
    }
    pos += k_chunk_overhead + 13;

    bool seen_plte       = false;
    bool seen_idat       = false;
    bool idat_sequence_ended = false;
    while (pos < bytes.size()) {
        if (bytes.size() < pos + k_chunk_overhead) {
            return png_failure(out_failure, "chunk sequence is truncated");
        }
        const std::uint32_t length = read_be32(bytes, pos);
        const std::string_view type = bytes.substr(pos + 4, 4);
        if (!is_png_chunk_type(type)) {
            return png_failure(out_failure, "chunk type is not four letters with a valid reserved bit");
        }
        if (length > 0x7FFFFFFFu || bytes.size() - pos - k_chunk_overhead < length) {
            return png_failure(out_failure, std::string("chunk ") + std::string(type) + " is truncated");
        }
        const std::size_t data = pos + 8;
        const std::size_t crc  = data + length;
        if (png_crc(bytes.substr(pos + 4, 4 + length)) != read_be32(bytes, crc)) {
            return png_failure(out_failure, std::string("chunk ") + std::string(type) + " has a mismatched CRC");
        }
        if (type == "acTL" || type == "fcTL" || type == "fdAT") {
            return png_failure(out_failure, "animated PNG chunk " + std::string(type));
        }
        if (type == "IHDR") {
            return png_failure(out_failure, "IHDR appears more than once");
        }
        if (type == "PLTE") {
            if (seen_plte || seen_idat || length == 0 || length % 3 != 0 || length > 768 ||
                header.color_type == 0 || header.color_type == 4)
            {
                return png_failure(out_failure, "PLTE is duplicate, misplaced, malformed or forbidden for the colour type");
            }
            if (header.color_type == 3 && length / 3 > (1u << header.bit_depth)) {
                return png_failure(out_failure, "PLTE has more entries than the indexed bit depth permits");
            }
            seen_plte = true;
        }
        if (type == "IDAT") {
            if (idat_sequence_ended) {
                return png_failure(out_failure, "IDAT chunks are not consecutive");
            }
            if (header.color_type == 3 && !seen_plte) {
                return png_failure(out_failure, "indexed-colour PNG has IDAT before its required PLTE");
            }
            seen_idat = true;
        }
        else
        if (seen_idat) {
            idat_sequence_ended = true;
        }
        if (type == "IEND") {
            if (length != 0 || !seen_idat) {
                return png_failure(out_failure, "IEND is non-empty or appears before IDAT");
            }
            pos += k_chunk_overhead;
            if (pos != bytes.size()) {
                return png_failure(out_failure, "bytes follow IEND");
            }
            return header;
        }
        // Unknown critical chunks (uppercase first letter) cannot be safely
        // interpreted. Known critical chunks were handled above.
        if (type[0] >= 'A' && type[0] <= 'Z' &&
            type != "PLTE" && type != "IDAT")
        {
            return png_failure(out_failure, "unknown critical chunk " + std::string(type));
        }
        pos += k_chunk_overhead + length;
    }
    return png_failure(out_failure, "PNG has no terminal IEND chunk");
}

bool icon_aspect_in_bounds(double width, double height)
{
    return width > 0.0 && height > 0.0 &&
           width  <= height * limits::k_max_icon_aspect_ratio &&
           height <= width  * limits::k_max_icon_aspect_ratio;
}

bool icon_pixel_dimensions_in_bounds(std::uint32_t width, std::uint32_t height)
{
    return width  >= limits::k_min_icon_pixels && width  <= limits::k_max_icon_pixels &&
           height >= limits::k_min_icon_pixels && height <= limits::k_max_icon_pixels &&
           icon_aspect_in_bounds(width, height);
}

std::optional<Icon_inspection> inspect_icon(std::string_view bytes, Contract_failure* out_failure)
{
    const auto failure = [out_failure](const char* reason_code, std::string detail) {
        if (out_failure != nullptr) {
            out_failure->reason_code = reason_code;
            out_failure->detail      = std::move(detail);
        }
        return std::nullopt;
    };
    if (bytes.size() > limits::k_max_icon_source_bytes) {
        return failure(reason::k_icon_too_large, "icon exceeds k_max_icon_source_bytes");
    }

    Icon_inspection inspection;
    inspection.media = sniff_icon_media(bytes);
    switch (inspection.media) {
        case Icon_media::PNG: {
            const std::optional<Png_header> header = read_png_header(bytes, out_failure);
            if (!header.has_value()) {
                return std::nullopt;
            }
            if (!icon_pixel_dimensions_in_bounds(header->width, header->height)) {
                return failure(reason::k_icon_dimensions_invalid,
                    "PNG is " + std::to_string(header->width) + "x" + std::to_string(header->height) +
                    " pixels; each side must be within [" + std::to_string(limits::k_min_icon_pixels) + ", " +
                    std::to_string(limits::k_max_icon_pixels) + "] and the aspect within [1:" +
                    std::to_string(limits::k_max_icon_aspect_ratio) + ", " + std::to_string(limits::k_max_icon_aspect_ratio) + ":1]");
            }
            inspection.width  = header->width;
            inspection.height = header->height;
            return inspection;
        }
        case Icon_media::SVG: {
            const std::optional<Svg_canonical> canonical =
                sanitize_svg_subset(bytes, Svg_subset_limits{}, k_icon_light_current_color, out_failure);
            if (!canonical.has_value()) {
                return std::nullopt;
            }
            if (!icon_aspect_in_bounds(canonical->view_box_width, canonical->view_box_height)) {
                return failure(reason::k_icon_dimensions_invalid, "SVG viewBox aspect is outside [1:" +
                    std::to_string(limits::k_max_icon_aspect_ratio) + ", " + std::to_string(limits::k_max_icon_aspect_ratio) + ":1]");
            }
            inspection.width  = canonical->view_box_width;
            inspection.height = canonical->view_box_height;
            return inspection;
        }
        case Icon_media::UNSUPPORTED:
        default:
            return failure(reason::k_icon_media_unsupported, "icon bytes are neither a PNG stream nor an svg document");
    }
}

} // namespace logonomic::cli_addon
