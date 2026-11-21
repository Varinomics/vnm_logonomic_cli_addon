#include "logonomic_cli_addon/svg_subset.h"
#include "decimal_parse.h"
#include "logonomic_cli_addon/json.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <initializer_list>
#include <map>
#include <set>
#include <vector>

namespace logonomic::cli_addon {

namespace {

constexpr std::size_t k_npos = std::string_view::npos;

constexpr std::string_view k_svg_namespace   = "http://www.w3.org/2000/svg";
constexpr std::string_view k_xlink_namespace = "http://www.w3.org/1999/xlink";

// The 147 colour keywords of SVG 1.1 section 4.4, sorted bytewise.
constexpr std::array<std::string_view, 147> k_color_keywords = {
    "aliceblue", "antiquewhite", "aqua", "aquamarine", "azure", "beige", "bisque", "black",
    "blanchedalmond", "blue", "blueviolet", "brown", "burlywood", "cadetblue", "chartreuse",
    "chocolate", "coral", "cornflowerblue", "cornsilk", "crimson", "cyan", "darkblue", "darkcyan",
    "darkgoldenrod", "darkgray", "darkgreen", "darkgrey", "darkkhaki", "darkmagenta",
    "darkolivegreen", "darkorange", "darkorchid", "darkred", "darksalmon", "darkseagreen",
    "darkslateblue", "darkslategray", "darkslategrey", "darkturquoise", "darkviolet", "deeppink",
    "deepskyblue", "dimgray", "dimgrey", "dodgerblue", "firebrick", "floralwhite", "forestgreen",
    "fuchsia", "gainsboro", "ghostwhite", "gold", "goldenrod", "gray", "green", "greenyellow",
    "grey", "honeydew", "hotpink", "indianred", "indigo", "ivory", "khaki", "lavender",
    "lavenderblush", "lawngreen", "lemonchiffon", "lightblue", "lightcoral", "lightcyan",
    "lightgoldenrodyellow", "lightgray", "lightgreen", "lightgrey", "lightpink", "lightsalmon",
    "lightseagreen", "lightskyblue", "lightslategray", "lightslategrey", "lightsteelblue",
    "lightyellow", "lime", "limegreen", "linen", "magenta", "maroon", "mediumaquamarine",
    "mediumblue", "mediumorchid", "mediumpurple", "mediumseagreen", "mediumslateblue",
    "mediumspringgreen", "mediumturquoise", "mediumvioletred", "midnightblue", "mintcream",
    "mistyrose", "moccasin", "navajowhite", "navy", "oldlace", "olive", "olivedrab", "orange",
    "orangered", "orchid", "palegoldenrod", "palegreen", "paleturquoise", "palevioletred",
    "papayawhip", "peachpuff", "peru", "pink", "plum", "powderblue", "purple", "red", "rosybrown",
    "royalblue", "saddlebrown", "salmon", "sandybrown", "seagreen", "seashell", "sienna", "silver",
    "skyblue", "slateblue", "slategray", "slategrey", "snow", "springgreen", "steelblue", "tan",
    "teal", "thistle", "tomato", "turquoise", "violet", "wheat", "white", "whitesmoke", "yellow",
    "yellowgreen",
};

bool is_xml_space(unsigned char c)   { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
bool is_ascii_alpha(unsigned char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
bool is_digit(unsigned char c)       { return c >= '0' && c <= '9'; }
bool is_hex_digit(unsigned char c)   { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
bool is_name_start(unsigned char c)  { return is_ascii_alpha(c) || c == '_' || c == ':'; }
bool is_name_char(unsigned char c)   { return is_name_start(c) || is_digit(c) || c == '-' || c == '.'; }

bool one_of(std::string_view value, std::initializer_list<std::string_view> names)
{
    return std::find(names.begin(), names.end(), value) != names.end();
}

// The first bytes of a value, for a diagnostic.
std::string excerpt(std::string_view text)
{
    constexpr std::size_t k_limit = 48;
    if (text.size() <= k_limit) {
        return std::string(text);
    }
    return std::string(text.substr(0, k_limit)) + "...";
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

struct Attribute
{
    std::string name;
    std::string value;   // after XML attribute-value normalization and reference replacement
};

struct Element
{
    std::string              name;
    std::vector<Attribute>   attributes;
    std::vector<std::size_t> children;
    std::size_t              parent = k_npos;
    std::size_t              depth  = 0;   // the root is 1

    const std::string* attribute(std::string_view attribute_name) const
    {
        for (const Attribute& attribute : attributes) {
            if (attribute.name == attribute_name) {
                return &attribute.value;
            }
        }
        return nullptr;
    }
};

// ---------------------------------------------------------------------------
// XML layer

class Subset_parser
{
public:
    Subset_parser(std::string_view bytes, const Svg_subset_limits& limits)
    :
        m_bytes(bytes),
        m_limits(limits)
    {}

    /// Parses the document into elements; false with the first violation.
    bool parse(std::string* out_detail)
    {
        m_detail = out_detail;
        if (m_bytes.substr(0, 3) == "\xEF\xBB\xBF") {
            return fail("byte-order mark");
        }
        if (m_bytes.find('\0') != k_npos) {
            return fail("NUL byte");
        }
        if (at("<?xml") && m_bytes.size() > 5 && is_xml_space((unsigned char)m_bytes[5])) {
            if (!parse_declaration()) {
                return false;
            }
        }
        while (true) {
            if (at_end()) {
                if (m_elements.empty()) {
                    return fail("no root element");
                }
                if (!m_root_closed) {
                    return fail("unexpected end of document inside element " + m_elements[m_open.back()].name);
                }
                return true;
            }
            if (peek() != '<') {
                if (!parse_text()) {
                    return false;
                }
                continue;
            }
            if (at("<?"))        { return fail("processing instruction"); }
            if (at("<!--"))      { return fail("comment"); }
            if (at("<![CDATA[")) { return fail("CDATA section"); }
            if (at("<!DOCTYPE")) { return fail("DOCTYPE declaration"); }
            if (at("<!"))        { return fail("markup declaration"); }
            if (at("</")) {
                if (!parse_end_tag()) {
                    return false;
                }
            }
            else
            if (!parse_start_tag()) {
                return false;
            }
        }
    }

    std::vector<Element>& elements() { return m_elements; }

private:
    bool fail(std::string detail)
    {
        *m_detail = std::move(detail);
        return false;
    }

    bool at(std::string_view literal) const { return m_bytes.substr(m_pos, literal.size()) == literal; }
    bool at_end() const                     { return m_pos >= m_bytes.size(); }
    unsigned char peek() const              { return (unsigned char)m_bytes[m_pos]; }

    void skip_space()
    {
        while (!at_end() && is_xml_space(peek())) {
            ++m_pos;
        }
    }

    bool parse_name(std::string& out_name)
    {
        if (at_end() || !is_name_start(peek())) {
            return fail("malformed name at byte " + std::to_string(m_pos));
        }
        const std::size_t start = m_pos;
        while (!at_end() && is_name_char(peek())) {
            ++m_pos;
        }
        out_name = std::string(m_bytes.substr(start, m_pos - start));
        return true;
    }

    // `<?xml version="1.0" encoding="UTF-8" standalone="yes"?>`, members in
    // that order, only the first required (XML 1.0 section 2.8).
    bool parse_declaration()
    {
        m_pos = 5;
        std::string name;
        std::string value;
        skip_space();
        if (!parse_pseudo_attribute(name, value) || name != "version" || value != "1.0") {
            return fail("XML declaration must begin with version=\"1.0\"");
        }
        bool seen_encoding   = false;
        bool seen_standalone = false;
        while (true) {
            const bool had_space = skip_space_counting();
            if (at("?>")) {
                m_pos += 2;
                return true;
            }
            if (!had_space || !parse_pseudo_attribute(name, value)) {
                return fail("malformed XML declaration");
            }
            if (name == "encoding" && !seen_encoding && !seen_standalone) {
                seen_encoding = true;
                if (ascii_lowercase(value) != "utf-8") {
                    return fail("XML declaration encoding is not UTF-8: " + excerpt(value));
                }
            }
            else
            if (name == "standalone" && !seen_standalone) {
                seen_standalone = true;
                if (value != "yes" && value != "no") {
                    return fail("XML declaration standalone is not yes or no");
                }
            }
            else {
                return fail("unexpected XML declaration member " + name);
            }
        }
    }

    bool skip_space_counting()
    {
        const std::size_t start = m_pos;
        skip_space();
        return m_pos > start;
    }

    bool parse_pseudo_attribute(std::string& out_name, std::string& out_value)
    {
        if (!parse_name(out_name)) {
            return false;
        }
        skip_space();
        if (at_end() || peek() != '=') {
            return false;
        }
        ++m_pos;
        skip_space();
        if (at_end() || (peek() != '"' && peek() != '\'')) {
            return false;
        }
        const char quote = (char)peek();
        ++m_pos;
        const std::size_t start = m_pos;
        while (!at_end() && peek() != (unsigned char)quote) {
            ++m_pos;
        }
        if (at_end()) {
            return false;
        }
        out_value = std::string(m_bytes.substr(start, m_pos - start));
        ++m_pos;
        return true;
    }

    // After `&`: the five predefined entities or a character reference to a
    // scalar of U+0020 and above, tab, LF or CR. No other entity exists
    // because the document may not declare any.
    bool parse_reference(std::string& out_value)
    {
        const std::size_t semicolon = m_bytes.find(';', m_pos);
        if (semicolon == k_npos || semicolon - m_pos > 10) {
            return fail("malformed entity reference at byte " + std::to_string(m_pos - 1));
        }
        const std::string_view body = m_bytes.substr(m_pos, semicolon - m_pos);
        m_pos = semicolon + 1;
        if (body == "lt")   { out_value += '<';  return true; }
        if (body == "gt")   { out_value += '>';  return true; }
        if (body == "amp")  { out_value += '&';  return true; }
        if (body == "quot") { out_value += '"';  return true; }
        if (body == "apos") { out_value += '\''; return true; }
        if (body.size() < 2 || body[0] != '#') {
            return fail("entity reference &" + std::string(body) + ";");
        }
        std::uint32_t scalar = 0;
        if (body[1] == 'x') {
            const std::string_view digits = body.substr(2);
            if (digits.empty() || digits.size() > 6 ||
                !std::all_of(digits.begin(), digits.end(), [](char c) { return is_hex_digit((unsigned char)c); }))
            {
                return fail("malformed character reference &" + std::string(body) + ";");
            }
            for (const char c : digits) {
                const unsigned char byte = (unsigned char)c;
                const std::uint32_t digit = is_digit(byte) ? byte - '0' : (byte | 0x20) - 'a' + 10;
                scalar = scalar * 16 + digit;
            }
        }
        else {
            const std::string_view digits = body.substr(1);
            if (digits.empty() || digits.size() > 7 ||
                !std::all_of(digits.begin(), digits.end(), [](char c) { return is_digit((unsigned char)c); }))
            {
                return fail("malformed character reference &" + std::string(body) + ";");
            }
            for (const char c : digits) {
                scalar = scalar * 10 + (std::uint32_t)(c - '0');
            }
        }
        const bool allowed_control = scalar == 0x9 || scalar == 0xA || scalar == 0xD;
        const bool surrogate       = scalar >= 0xD800 && scalar <= 0xDFFF;
        const bool non_character   = scalar == 0xFFFE || scalar == 0xFFFF;
        if (!allowed_control && (scalar < 0x20 || scalar > 0x10FFFF || surrogate || non_character)) {
            return fail("character reference &" + std::string(body) + "; is outside the allowed range");
        }
        append_utf8(out_value, scalar);
        return true;
    }

    // Attribute-value normalization of XML 1.0 section 3.3.3: literal
    // white space becomes a space, references are replaced.
    bool parse_attribute_value(std::string& out_value)
    {
        if (at_end() || (peek() != '"' && peek() != '\'')) {
            return fail("attribute value is not quoted at byte " + std::to_string(m_pos));
        }
        const unsigned char quote = peek();
        ++m_pos;
        while (true) {
            if (at_end()) {
                return fail("unterminated attribute value");
            }
            const unsigned char c = peek();
            if (c == quote) {
                ++m_pos;
                return true;
            }
            if (c == '<') {
                return fail("'<' in an attribute value");
            }
            ++m_pos;
            if (c == '&') {
                if (!parse_reference(out_value)) {
                    return false;
                }
            }
            else
            if (is_xml_space(c)) {
                out_value += ' ';
            }
            else {
                out_value += (char)c;
            }
        }
    }

    bool parse_start_tag()
    {
        ++m_pos;   // '<'
        if (m_root_closed) {
            return fail("content after the root element");
        }
        Element element;
        if (!parse_name(element.name)) {
            return false;
        }
        if (!m_open.empty()) {
            element.parent = m_open.back();
            element.depth  = m_elements[element.parent].depth + 1;
        }
        else {
            element.depth = 1;
        }
        if (element.depth > m_limits.max_depth) {
            return fail("element depth exceeds max_depth at element " + element.name);
        }
        if (m_elements.size() >= m_limits.max_elements) {
            return fail("element count exceeds max_elements");
        }

        bool closed = false;
        while (true) {
            const bool had_space = skip_space_counting();
            if (at_end()) {
                return fail("unterminated start tag " + element.name);
            }
            if (at("/>")) {
                m_pos += 2;
                closed = true;
                break;
            }
            if (peek() == '>') {
                ++m_pos;
                break;
            }
            if (!had_space) {
                return fail("missing white space before an attribute of element " + element.name);
            }
            Attribute attribute;
            if (!parse_name(attribute.name)) {
                return false;
            }
            skip_space();
            if (at_end() || peek() != '=') {
                return fail("attribute " + attribute.name + " on element " + element.name + " has no value");
            }
            ++m_pos;
            skip_space();
            if (!parse_attribute_value(attribute.value)) {
                return false;
            }
            if (element.attribute(attribute.name) != nullptr) {
                return fail("duplicate attribute " + attribute.name + " on element " + element.name);
            }
            const std::size_t bound = attribute.name == "d" || attribute.name == "points"
                ? m_limits.max_path_data_bytes
                : m_limits.max_attribute_bytes;
            if (attribute.value.size() > bound) {
                return fail("attribute " + attribute.name + " on element " + element.name + " exceeds its byte bound");
            }
            if (element.attributes.size() >= m_limits.max_attributes_per_element) {
                return fail("attribute count on element " + element.name + " exceeds max_attributes_per_element");
            }
            element.attributes.push_back(std::move(attribute));
        }

        const std::size_t index  = m_elements.size();
        const std::size_t parent = element.parent;
        m_elements.push_back(std::move(element));
        if (parent != k_npos) {
            m_elements[parent].children.push_back(index);
        }
        if (closed) {
            if (m_open.empty()) {
                m_root_closed = true;
            }
        }
        else {
            m_open.push_back(index);
        }
        return true;
    }

    bool parse_end_tag()
    {
        m_pos += 2;   // "</"
        if (m_open.empty()) {
            return fail("content after the root element");
        }
        std::string name;
        if (!parse_name(name)) {
            return false;
        }
        skip_space();
        if (at_end() || peek() != '>') {
            return fail("malformed end tag " + name);
        }
        ++m_pos;
        const std::string& open = m_elements[m_open.back()].name;
        if (name != open) {
            return fail("end tag " + name + " does not match open element " + open);
        }
        m_open.pop_back();
        if (m_open.empty()) {
            m_root_closed = true;
        }
        return true;
    }

    // Text is discarded, but it must still be well formed, and it is only
    // allowed inside title and desc.
    bool parse_text()
    {
        bool non_space = false;
        std::string decoded;
        while (!at_end() && peek() != '<') {
            const unsigned char c = peek();
            if (at("]]>")) {
                return fail("']]>' in text content");
            }
            ++m_pos;
            if (c == '&') {
                if (!parse_reference(decoded)) {
                    return false;
                }
                non_space = true;
                continue;
            }
            if (!is_xml_space(c)) {
                non_space = true;
            }
        }
        if (!non_space) {
            return true;
        }
        if (m_open.empty()) {
            return fail("text outside the root element");
        }
        const std::string& open = m_elements[m_open.back()].name;
        if (open != "title" && open != "desc") {
            return fail("text content in element " + open);
        }
        return true;
    }

    std::string_view         m_bytes;
    const Svg_subset_limits& m_limits;
    std::size_t              m_pos = 0;
    std::vector<Element>     m_elements;
    std::vector<std::size_t> m_open;   // indices of the open elements, root first
    bool                     m_root_closed = false;
    std::string*             m_detail      = nullptr;
};

// ---------------------------------------------------------------------------
// Value grammars

// One SVG number: sign, digits with an optional fraction, optional exponent.
// Returns the end of the number or npos.
std::size_t scan_number(std::string_view text, std::size_t pos)
{
    std::size_t i = pos;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
        ++i;
    }
    std::size_t digits = 0;
    while (i < text.size() && is_digit((unsigned char)text[i])) {
        ++i;
        ++digits;
    }
    if (i < text.size() && text[i] == '.') {
        ++i;
        std::size_t fraction = 0;
        while (i < text.size() && is_digit((unsigned char)text[i])) {
            ++i;
            ++fraction;
        }
        digits += fraction;
    }
    if (digits == 0) {
        return k_npos;
    }
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        std::size_t j = i + 1;
        if (j < text.size() && (text[j] == '+' || text[j] == '-')) {
            ++j;
        }
        std::size_t exponent_digits = 0;
        while (j < text.size() && is_digit((unsigned char)text[j])) {
            ++j;
            ++exponent_digits;
        }
        if (exponent_digits == 0) {
            return k_npos;
        }
        i = j;
    }
    return i;
}

// The finite value of a token scan_number accepted; nullopt when it does
// not fit a double.
std::optional<double> number_value(std::string_view token)
{
    if (!token.empty() && token.front() == '+') {
        token.remove_prefix(1);
    }
    double value = 0.0;
    const std::from_chars_result result = detail::parse_decimal(token, value);
    if (result.ec != std::errc() || result.ptr != token.data() + token.size() || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

// `wsp+ ','? wsp*` or `',' wsp*`; true when anything was consumed.
bool skip_comma_wsp(std::string_view text, std::size_t& pos)
{
    const std::size_t start = pos;
    while (pos < text.size() && is_xml_space((unsigned char)text[pos])) {
        ++pos;
    }
    if (pos < text.size() && text[pos] == ',') {
        ++pos;
        while (pos < text.size() && is_xml_space((unsigned char)text[pos])) {
            ++pos;
        }
    }
    return pos > start;
}

void skip_wsp(std::string_view text, std::size_t& pos)
{
    while (pos < text.size() && is_xml_space((unsigned char)text[pos])) {
        ++pos;
    }
}

// A whole value that is one number.
std::optional<double> parse_number(std::string_view value)
{
    const std::size_t end = scan_number(value, 0);
    if (end != value.size()) {
        return std::nullopt;
    }
    return number_value(value);
}

struct Length
{
    double value      = 0.0;
    bool   percentage = false;
};

// A number with an optional `px` or `%` unit.
std::optional<Length> parse_length(std::string_view value, std::size_t& pos, bool allow_percent)
{
    const std::size_t end = scan_number(value, pos);
    if (end == k_npos) {
        return std::nullopt;
    }
    const std::optional<double> number = number_value(value.substr(pos, end - pos));
    if (!number.has_value()) {
        return std::nullopt;
    }
    Length length;
    length.value = *number;
    pos = end;
    if (value.substr(pos, 2) == "px") {
        pos += 2;
    }
    else
    if (pos < value.size() && value[pos] == '%') {
        if (!allow_percent) {
            return std::nullopt;
        }
        length.percentage = true;
        ++pos;
    }
    return length;
}

std::optional<Length> parse_whole_length(std::string_view value, bool allow_percent)
{
    std::size_t pos = 0;
    const std::optional<Length> length = parse_length(value, pos, allow_percent);
    if (!length.has_value() || pos != value.size()) {
        return std::nullopt;
    }
    return length;
}

bool is_id_value(std::string_view value)
{
    if (value.empty() || value.size() > limits::k_max_svg_id_bytes) {
        return false;
    }
    const unsigned char first = (unsigned char)value.front();
    if (!(is_ascii_alpha(first) || first == '_')) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](char c) {
        const unsigned char byte = (unsigned char)c;
        return is_ascii_alpha(byte) || is_digit(byte) || byte == '_' || byte == '.' || byte == ':' || byte == '-';
    });
}

// `url(#id)`; the id when the form matches.
std::optional<std::string_view> url_reference(std::string_view value)
{
    if (value.substr(0, 5) != "url(#" || value.empty() || value.back() != ')') {
        return std::nullopt;
    }
    const std::string_view id = value.substr(5, value.size() - 6);
    if (!is_id_value(id)) {
        return std::nullopt;
    }
    return id;
}

// `rgb(r, g, b)` with three integers in [0, 255] or three percentages in
// [0, 100], never mixed (SVG 1.1 section 4.2).
bool is_rgb_function(std::string_view value)
{
    if (value.substr(0, 4) != "rgb(" || value.back() != ')') {
        return false;
    }
    const std::string_view inner = value.substr(4, value.size() - 5);
    std::array<std::string_view, 3> parts;
    std::size_t start = 0;
    for (std::size_t i = 0; i < 3; ++i) {
        const std::size_t comma = inner.find(',', start);
        if ((i < 2) != (comma != k_npos)) {
            return false;
        }
        std::string_view part = inner.substr(start, comma == k_npos ? k_npos : comma - start);
        while (!part.empty() && is_xml_space((unsigned char)part.front())) { part.remove_prefix(1); }
        while (!part.empty() && is_xml_space((unsigned char)part.back()))  { part.remove_suffix(1); }
        parts[i] = part;
        start    = comma + 1;
    }
    const bool percentages = !parts[0].empty() && parts[0].back() == '%';
    for (const std::string_view part : parts) {
        if (percentages) {
            if (part.empty() || part.back() != '%') {
                return false;
            }
            const std::optional<double> number = parse_number(part.substr(0, part.size() - 1));
            if (!number.has_value() || *number < 0.0 || *number > 100.0) {
                return false;
            }
        }
        else {
            if (part.empty() || part.size() > 3 ||
                !std::all_of(part.begin(), part.end(), [](char c) { return is_digit((unsigned char)c); }))
            {
                return false;
            }
            int integer = 0;
            for (const char c : part) {
                integer = integer * 10 + (c - '0');
            }
            if (integer > 255) {
                return false;
            }
        }
    }
    return true;
}

bool is_hex_color(std::string_view value)
{
    if (value.empty() || value.front() != '#' || (value.size() != 4 && value.size() != 7)) {
        return false;
    }
    return std::all_of(value.begin() + 1, value.end(), [](char c) { return is_hex_digit((unsigned char)c); });
}

enum class Color_context
{
    PAINT,    // fill, stroke: none and url(#gradient) allowed
    COLOR,    // color, stop-color: a colour only
};

// A colour or paint value; `out_reference` receives the gradient id of a
// url(#id) paint.
bool is_color_value(std::string_view value, Color_context context, std::optional<std::string_view>* out_reference)
{
    if (value == "currentColor") {
        return true;
    }
    if (context == Color_context::PAINT) {
        if (value == "none") {
            return true;
        }
        const std::optional<std::string_view> reference = url_reference(value);
        if (reference.has_value()) {
            *out_reference = reference;
            return true;
        }
    }
    return is_hex_color(value) || is_rgb_function(value) || is_svg_color_keyword(value);
}

// `transform` and `gradientTransform`: at most k_max_svg_transforms of
// matrix, translate, scale, rotate, skewX and skewY with finite arguments.
bool is_transform_list(std::string_view value)
{
    std::size_t pos   = 0;
    std::size_t count = 0;
    skip_wsp(value, pos);
    while (pos < value.size()) {
        const std::size_t name_start = pos;
        while (pos < value.size() && is_ascii_alpha((unsigned char)value[pos])) {
            ++pos;
        }
        const std::string_view name = value.substr(name_start, pos - name_start);
        skip_wsp(value, pos);
        if (pos >= value.size() || value[pos] != '(') {
            return false;
        }
        ++pos;
        skip_wsp(value, pos);
        std::size_t arguments = 0;
        while (true) {
            const std::size_t end = scan_number(value, pos);
            if (end == k_npos) {
                break;
            }
            if (!number_value(value.substr(pos, end - pos)).has_value()) {
                return false;
            }
            ++arguments;
            pos = end;
            if (!skip_comma_wsp(value, pos)) {
                break;
            }
        }
        if (pos >= value.size() || value[pos] != ')') {
            return false;
        }
        ++pos;
        const bool count_ok =
            (name == "matrix"    && arguments == 6)                    ||
            (name == "translate" && (arguments == 1 || arguments == 2)) ||
            (name == "scale"     && (arguments == 1 || arguments == 2)) ||
            (name == "rotate"    && (arguments == 1 || arguments == 3)) ||
            (name == "skewX"     && arguments == 1)                    ||
            (name == "skewY"     && arguments == 1);
        if (!count_ok) {
            return false;
        }
        if (++count > limits::k_max_svg_transforms) {
            return false;
        }
        const bool separated = skip_comma_wsp(value, pos);
        if (pos < value.size() && !separated) {
            return false;
        }
    }
    return true;
}

// `viewBox`: four numbers, the last two positive.
bool parse_view_box(std::string_view value, double* out_width, double* out_height)
{
    std::size_t pos = 0;
    skip_wsp(value, pos);
    std::array<double, 4> numbers{};
    for (std::size_t i = 0; i < 4; ++i) {
        if (i > 0 && !skip_comma_wsp(value, pos)) {
            return false;
        }
        const std::size_t end = scan_number(value, pos);
        if (end == k_npos) {
            return false;
        }
        const std::optional<double> number = number_value(value.substr(pos, end - pos));
        if (!number.has_value()) {
            return false;
        }
        numbers[i] = *number;
        pos        = end;
    }
    skip_wsp(value, pos);
    if (pos != value.size() || numbers[2] <= 0.0 || numbers[3] <= 0.0) {
        return false;
    }
    *out_width  = numbers[2];
    *out_height = numbers[3];
    return true;
}

bool is_path_command(char command)
{
    return std::string_view("MmZzLlHhVvCcSsQqTtAa").find(command) != k_npos;
}

std::size_t path_command_arity(char command)
{
    switch (command) {
        case 'M': case 'm': case 'L': case 'l': case 'T': case 't': return 2;
        case 'H': case 'h': case 'V': case 'v':                     return 1;
        case 'C': case 'c':                                         return 6;
        case 'S': case 's': case 'Q': case 'q':                     return 4;
        case 'A': case 'a':                                         return 7;
        default:                                                     return 0;
    }
}

// SVG 1.1 path-data grammar. The parser deliberately validates syntax and
// finite numeric tokens only; the renderer owns geometry. One argument group
// is one segment for the resource bound.
bool is_path_data(std::string_view value, std::size_t max_segments)
{
    std::size_t pos      = 0;
    std::size_t segments = 0;
    char command         = '\0';
    bool first_command   = true;
    skip_wsp(value, pos);
    while (pos < value.size()) {
        if (is_ascii_alpha((unsigned char)value[pos])) {
            command = value[pos++];
            if (!is_path_command(command) ||
                (first_command && command != 'M' && command != 'm'))
            {
                return false;
            }
            first_command = false;
            if (command == 'Z' || command == 'z') {
                if (++segments > max_segments) {
                    return false;
                }
                skip_wsp(value, pos);
                if (pos < value.size() && !is_ascii_alpha((unsigned char)value[pos])) {
                    return false;
                }
                continue;
            }
        }
        else
        if (command == '\0' || command == 'Z' || command == 'z') {
            return false;
        }

        const std::size_t arity = path_command_arity(command);
        bool parsed_group = false;
        while (pos < value.size() && !is_ascii_alpha((unsigned char)value[pos])) {
            skip_wsp(value, pos);
            if (pos >= value.size() || is_ascii_alpha((unsigned char)value[pos])) {
                break;
            }
            for (std::size_t argument = 0; argument < arity; ++argument) {
                if (argument > 0) {
                    skip_comma_wsp(value, pos);
                }
                if ((command == 'A' || command == 'a') && (argument == 3 || argument == 4)) {
                    if (pos >= value.size() || (value[pos] != '0' && value[pos] != '1')) {
                        return false;
                    }
                    ++pos;
                    continue;
                }
                const std::size_t end = scan_number(value, pos);
                if (end == k_npos) {
                    return false;
                }
                const std::optional<double> number = number_value(value.substr(pos, end - pos));
                if (!number.has_value() ||
                    ((command == 'A' || command == 'a') && argument < 2 && *number < 0.0))
                {
                    return false;
                }
                pos = end;
            }
            parsed_group = true;
            if (++segments > max_segments) {
                return false;
            }
            const std::size_t before_separator = pos;
            skip_comma_wsp(value, pos);
            if (pos >= value.size() || is_ascii_alpha((unsigned char)value[pos])) {
                if (pos < value.size() && pos > before_separator && value[before_separator] == ',') {
                    return false;
                }
                break;
            }
        }
        if (!parsed_group) {
            return false;
        }
    }
    return !first_command;
}

bool is_points_data(std::string_view value, std::size_t max_points)
{
    std::size_t pos    = 0;
    std::size_t points = 0;
    skip_wsp(value, pos);
    while (pos < value.size()) {
        for (int coordinate = 0; coordinate < 2; ++coordinate) {
            if (coordinate != 0) {
                skip_comma_wsp(value, pos);
            }
            const std::size_t end = scan_number(value, pos);
            if (end == k_npos || !number_value(value.substr(pos, end - pos)).has_value()) {
                return false;
            }
            pos = end;
        }
        if (++points > max_points) {
            return false;
        }
        const std::size_t before_separator = pos;
        skip_comma_wsp(value, pos);
        if (pos == value.size()) {
            return before_separator == pos || value[before_separator] != ',';
        }
    }
    return points > 0;
}

// `none` or a comma-wsp separated list of non-negative lengths.
bool is_dash_array(std::string_view value)
{
    if (value == "none") {
        return true;
    }
    std::size_t pos = 0;
    skip_wsp(value, pos);
    std::size_t count = 0;
    while (pos < value.size()) {
        const std::optional<Length> length = parse_length(value, pos, true);
        if (!length.has_value() || length->value < 0.0) {
            return false;
        }
        ++count;
        const bool separated = skip_comma_wsp(value, pos);
        if (pos < value.size() && !separated) {
            return false;
        }
    }
    return count > 0;
}

bool is_preserve_aspect_ratio(std::string_view value)
{
    std::size_t pos = 0;
    skip_wsp(value, pos);
    std::size_t token_end = pos;
    while (token_end < value.size() && is_ascii_alpha((unsigned char)value[token_end])) {
        ++token_end;
    }
    const std::string_view align = value.substr(pos, token_end - pos);
    pos = token_end;
    if (align == "none") {
        skip_wsp(value, pos);
        return pos == value.size();
    }
    if (!one_of(align, {"xMinYMin", "xMidYMin", "xMaxYMin", "xMinYMid", "xMidYMid", "xMaxYMid",
                        "xMinYMax", "xMidYMax", "xMaxYMax"}))
    {
        return false;
    }
    const std::size_t before_space = pos;
    skip_wsp(value, pos);
    if (pos == value.size()) {
        return true;
    }
    if (pos == before_space) {
        return false;
    }
    const std::string_view mode = value.substr(pos);
    std::size_t mode_end = 0;
    while (mode_end < mode.size() && is_ascii_alpha((unsigned char)mode[mode_end])) {
        ++mode_end;
    }
    if (!one_of(mode.substr(0, mode_end), {"meet", "slice"})) {
        return false;
    }
    pos += mode_end;
    skip_wsp(value, pos);
    return pos == value.size();
}

// A number, or a percentage, within [0, 1] or [0 %, 100 %].
bool is_offset(std::string_view value)
{
    const std::optional<Length> length = parse_whole_length(value, true);
    if (!length.has_value()) {
        return false;
    }
    if (value.size() >= 2 && value.substr(value.size() - 2) == "px") {
        return false;
    }
    const double maximum = length->percentage ? 100.0 : 1.0;
    return length->value >= 0.0 && length->value <= maximum;
}

// ---------------------------------------------------------------------------
// SVG layer

bool is_shape(std::string_view name)
{
    return one_of(name, {"path", "rect", "circle", "ellipse", "line", "polyline", "polygon"});
}

bool is_gradient(std::string_view name)
{
    return name == "linearGradient" || name == "radialGradient";
}

bool is_subset_element(std::string_view name)
{
    return one_of(name, {"svg", "g", "defs", "title", "desc", "stop", "clipPath", "use"}) ||
           is_shape(name) || is_gradient(name);
}

bool is_paint_attribute(std::string_view name)
{
    return one_of(name, {
        "fill", "fill-opacity", "fill-rule", "stroke", "stroke-width", "stroke-linecap", "stroke-linejoin",
        "stroke-miterlimit", "stroke-dasharray", "stroke-dashoffset", "stroke-opacity", "opacity", "color",
        "clip-rule", "vector-effect", "shape-rendering",
    });
}

// The geometry attributes SVG 1.1 defines for each shape.
bool is_geometry_attribute(std::string_view element, std::string_view attribute)
{
    if (element == "path")     { return attribute == "d"; }
    if (element == "rect")     { return one_of(attribute, {"x", "y", "width", "height", "rx", "ry"}); }
    if (element == "circle")   { return one_of(attribute, {"cx", "cy", "r"}); }
    if (element == "ellipse")  { return one_of(attribute, {"cx", "cy", "rx", "ry"}); }
    if (element == "line")     { return one_of(attribute, {"x1", "y1", "x2", "y2"}); }
    if (element == "polyline") { return attribute == "points"; }
    if (element == "polygon")  { return attribute == "points"; }
    return false;
}

bool is_attribute_allowed(std::string_view element, std::string_view attribute)
{
    if (element == "svg") {
        return one_of(attribute, {"xmlns", "xmlns:xlink", "version", "viewBox", "width", "height", "preserveAspectRatio"}) ||
               is_paint_attribute(attribute);
    }
    if (element == "g") {
        return one_of(attribute, {"id", "transform", "clip-path"}) || is_paint_attribute(attribute);
    }
    if (is_shape(element)) {
        return one_of(attribute, {"id", "transform", "clip-path"}) || is_paint_attribute(attribute) ||
               is_geometry_attribute(element, attribute);
    }
    if (element == "defs") {
        return attribute == "id";
    }
    if (element == "linearGradient") {
        return one_of(attribute, {"id", "x1", "y1", "x2", "y2", "gradientUnits", "gradientTransform", "spreadMethod",
                                  "href", "xlink:href"});
    }
    if (element == "radialGradient") {
        return one_of(attribute, {"id", "cx", "cy", "r", "fx", "fy", "gradientUnits", "gradientTransform", "spreadMethod",
                                  "href", "xlink:href"});
    }
    if (element == "stop") {
        return one_of(attribute, {"offset", "stop-color", "stop-opacity"});
    }
    if (element == "clipPath") {
        return one_of(attribute, {"id", "clipPathUnits", "transform"});
    }
    if (element == "use") {
        return one_of(attribute, {"href", "xlink:href", "x", "y", "width", "height", "transform"}) ||
               is_paint_attribute(attribute);
    }
    return false;   // title, desc
}

// The content model: which subset elements each element may contain.
bool is_child_allowed(std::string_view parent, std::string_view child)
{
    if (one_of(parent, {"svg", "g", "defs"})) {
        return child != "svg" && child != "stop";
    }
    if (parent == "clipPath") {
        return one_of(child, {"title", "desc", "use"}) || is_shape(child);
    }
    if (is_gradient(parent)) {
        return one_of(child, {"stop", "title", "desc"});
    }
    if (is_shape(parent) || parent == "use") {
        return child == "title" || child == "desc";
    }
    return false;   // stop, title, desc
}

struct Reference
{
    std::size_t element;
    std::string attribute;
    std::string target_id;
};

class Subset_validator
{
public:
    Subset_validator(const std::vector<Element>& elements, const Svg_subset_limits& limits)
    :
        m_elements(elements),
        m_limits(limits)
    {}

    bool validate(std::string* out_detail, double* out_view_box_width, double* out_view_box_height)
    {
        m_detail = out_detail;
        const Element& root = m_elements.front();
        if (root.name != "svg") {
            return fail("root element " + root.name + " is not svg");
        }
        for (std::size_t index = 0; index < m_elements.size(); ++index) {
            if (!validate_element(index)) {
                return false;
            }
        }
        const std::string* xmlns = root.attribute("xmlns");
        if (xmlns == nullptr || *xmlns != k_svg_namespace) {
            return fail("root xmlns is not the SVG namespace");
        }
        const std::string* xlink = root.attribute("xmlns:xlink");
        if (xlink != nullptr && *xlink != k_xlink_namespace) {
            return fail("xmlns:xlink is not the XLink namespace");
        }
        if ((xlink != nullptr) != m_uses_xlink_href) {
            return fail(m_uses_xlink_href
                ? "xlink:href is used without an xmlns:xlink declaration on the root"
                : "xmlns:xlink is declared but xlink:href is not used");
        }
        const std::string* view_box = root.attribute("viewBox");
        if (view_box == nullptr) {
            return fail("root element has no viewBox");
        }
        if (!parse_view_box(*view_box, out_view_box_width, out_view_box_height)) {
            return fail("viewBox is not four numbers with positive width and height: " + excerpt(*view_box));
        }
        return resolve_references();
    }

private:
    bool fail(std::string detail)
    {
        *m_detail = std::move(detail);
        return false;
    }

    bool validate_element(std::size_t index)
    {
        const Element& element = m_elements[index];
        if (!is_subset_element(element.name)) {
            return fail("element " + element.name + " is not in the subset");
        }
        if (element.parent != k_npos) {
            const std::string& parent = m_elements[element.parent].name;
            if (!is_child_allowed(parent, element.name)) {
                return fail("element " + parent + " may not contain " + element.name);
            }
        }
        bool has_href = false;
        for (const Attribute& attribute : element.attributes) {
            const std::string& name = attribute.name;
            if (name.find(':') != std::string::npos && name != "xmlns:xlink" && name != "xlink:href") {
                return fail("prefixed attribute " + name + " on element " + element.name + " is not in the subset");
            }
            if ((name == "xmlns" || name == "xmlns:xlink") && index != 0) {
                return fail("namespace declaration " + name + " below the root element " + element.name);
            }
            if (!is_attribute_allowed(element.name, name)) {
                return fail("attribute " + name + " on element " + element.name + " is not in the subset");
            }
            if (name == "href" || name == "xlink:href") {
                if (has_href) {
                    return fail("element " + element.name + " carries both href and xlink:href");
                }
                has_href = true;
                if (name == "xlink:href") {
                    m_uses_xlink_href = true;
                }
            }
            if (!validate_value(index, attribute)) {
                return false;
            }
        }
        if (element.name == "use" && !has_href) {
            return fail("use has no href");
        }
        return true;
    }

    bool value_violation(const Element& element, const Attribute& attribute)
    {
        return fail("attribute " + attribute.name + " on element " + element.name +
            " has a value outside its grammar: " + excerpt(attribute.value));
    }

    bool validate_value(std::size_t index, const Attribute& attribute)
    {
        const Element& element  = m_elements[index];
        const std::string& name = attribute.name;
        const std::string_view value = attribute.value;

        if (name == "xmlns" || name == "xmlns:xlink") {
            return true;   // compared against the namespaces by validate()
        }
        if (name == "id") {
            if (!is_id_value(value)) {
                return value_violation(element, attribute);
            }
            if (!m_ids.emplace(std::string(value), index).second) {
                return fail("duplicate id " + std::string(value));
            }
            return true;
        }
        if (name == "href" || name == "xlink:href") {
            if (value.empty() || value.front() != '#' || !is_id_value(value.substr(1))) {
                return fail("attribute " + name + " on element " + element.name +
                    " is not a fragment reference to a document id: " + excerpt(value));
            }
            m_hrefs.push_back(Reference{index, name, std::string(value.substr(1))});
            return true;
        }
        if (name == "fill" || name == "stroke") {
            std::optional<std::string_view> reference;
            if (!is_color_value(value, Color_context::PAINT, &reference)) {
                return value_violation(element, attribute);
            }
            if (reference.has_value()) {
                m_paint_references.push_back(Reference{index, name, std::string(*reference)});
            }
            return true;
        }
        if (name == "color" || name == "stop-color") {
            std::optional<std::string_view> reference;
            return is_color_value(value, Color_context::COLOR, &reference) || value_violation(element, attribute);
        }
        if (name == "clip-path") {
            if (value == "none") {
                return true;
            }
            const std::optional<std::string_view> reference = url_reference(value);
            if (!reference.has_value()) {
                return value_violation(element, attribute);
            }
            m_clip_references.push_back(Reference{index, name, std::string(*reference)});
            return true;
        }
        if (one_of(name, {"fill-opacity", "stroke-opacity", "opacity", "stop-opacity"})) {
            const std::optional<double> number = parse_number(value);
            return (number.has_value() && *number >= 0.0 && *number <= 1.0) || value_violation(element, attribute);
        }
        if (name == "stroke-miterlimit") {
            const std::optional<double> number = parse_number(value);
            return (number.has_value() && *number >= 1.0) || value_violation(element, attribute);
        }
        if (name == "version") {
            return parse_number(value).has_value() || value_violation(element, attribute);
        }
        if (name == "viewBox") {
            double width  = 0.0;
            double height = 0.0;
            return parse_view_box(value, &width, &height) || value_violation(element, attribute);
        }
        if (one_of(name, {"width", "height", "r", "rx", "ry", "stroke-width"})) {
            const std::optional<Length> length = parse_whole_length(value, true);
            return (length.has_value() && length->value >= 0.0) || value_violation(element, attribute);
        }
        if (one_of(name, {"x", "y", "cx", "cy", "x1", "y1", "x2", "y2", "fx", "fy", "stroke-dashoffset"})) {
            return parse_whole_length(value, true).has_value() || value_violation(element, attribute);
        }
        if (name == "offset") {
            return is_offset(value) || value_violation(element, attribute);
        }
        if (name == "stroke-dasharray") {
            return is_dash_array(value) || value_violation(element, attribute);
        }
        if (name == "transform" || name == "gradientTransform") {
            return is_transform_list(value) || value_violation(element, attribute);
        }
        if (name == "d") {
            return is_path_data(value, m_limits.max_path_segments) || value_violation(element, attribute);
        }
        if (name == "points") {
            return is_points_data(value, m_limits.max_path_segments) || value_violation(element, attribute);
        }
        if (name == "preserveAspectRatio") {
            return is_preserve_aspect_ratio(value) || value_violation(element, attribute);
        }
        if (name == "gradientUnits" || name == "clipPathUnits") {
            return one_of(value, {"userSpaceOnUse", "objectBoundingBox"}) || value_violation(element, attribute);
        }
        if (name == "spreadMethod") {
            return one_of(value, {"pad", "reflect", "repeat"}) || value_violation(element, attribute);
        }
        if (name == "fill-rule" || name == "clip-rule") {
            return one_of(value, {"nonzero", "evenodd"}) || value_violation(element, attribute);
        }
        if (name == "stroke-linecap") {
            return one_of(value, {"butt", "round", "square"}) || value_violation(element, attribute);
        }
        if (name == "stroke-linejoin") {
            return one_of(value, {"miter", "round", "bevel"}) || value_violation(element, attribute);
        }
        if (name == "vector-effect") {
            return one_of(value, {"none", "non-scaling-stroke"}) || value_violation(element, attribute);
        }
        if (name == "shape-rendering") {
            return one_of(value, {"auto", "optimizeSpeed", "crispEdges", "geometricPrecision"}) ||
                   value_violation(element, attribute);
        }
        return fail("attribute " + name + " on element " + element.name + " has no value grammar in the subset");
    }

    bool is_ancestor_or_self(std::size_t candidate, std::size_t element) const
    {
        for (std::size_t current = element; current != k_npos; current = m_elements[current].parent) {
            if (current == candidate) {
                return true;
            }
        }
        return false;
    }

    std::optional<std::size_t> target_of(const Reference& reference) const
    {
        const auto found = m_ids.find(reference.target_id);
        if (found == m_ids.end()) {
            return std::nullopt;
        }
        return found->second;
    }

    void collect_uses(std::size_t element, std::vector<std::size_t>& out_uses) const
    {
        for (const std::size_t child : m_elements[element].children) {
            if (m_elements[child].name == "use") {
                out_uses.push_back(child);
            }
            collect_uses(child, out_uses);
        }
    }

    const Reference* href_of(std::size_t element) const
    {
        for (const Reference& reference : m_hrefs) {
            if (reference.element == element) {
                return &reference;
            }
        }
        return nullptr;
    }

    // Hops from a `use` through the `use` elements below its target; nullopt
    // marks a cycle.
    std::optional<std::size_t> use_chain(std::size_t use, std::set<std::size_t>& visiting)
    {
        const auto memoized = m_use_chain.find(use);
        if (memoized != m_use_chain.end()) {
            return memoized->second;
        }
        if (!visiting.insert(use).second) {
            return std::nullopt;
        }
        const Reference* reference = href_of(use);
        std::size_t chain = 0;
        if (reference != nullptr) {
            const std::optional<std::size_t> target = target_of(*reference);
            std::vector<std::size_t> uses;
            collect_uses(*target, uses);
            chain = 1;
            for (const std::size_t nested : uses) {
                const std::optional<std::size_t> nested_chain = use_chain(nested, visiting);
                if (!nested_chain.has_value()) {
                    return std::nullopt;
                }
                chain = std::max(chain, 1 + *nested_chain);
            }
        }
        visiting.erase(use);
        m_use_chain[use] = chain;
        return chain;
    }

    bool resolve_references()
    {
        for (const Reference& reference : m_hrefs) {
            const Element& source = m_elements[reference.element];
            const std::optional<std::size_t> target_index = target_of(reference);
            if (!target_index.has_value()) {
                return fail(reference.attribute + " on element " + source.name + " references an undefined id: #" + reference.target_id);
            }
            const Element& target = m_elements[*target_index];
            if (source.name == "use") {
                if (target.name != "g" && !is_shape(target.name)) {
                    return fail("use references a " + target.name + " element: #" + reference.target_id);
                }
                if (is_ancestor_or_self(*target_index, reference.element)) {
                    return fail("use references an ancestor: #" + reference.target_id);
                }
            }
            else
            if (target.name != source.name) {
                return fail(source.name + " references a " + target.name + " element: #" + reference.target_id);
            }
        }
        for (const Reference& reference : m_paint_references) {
            const std::optional<std::size_t> target = target_of(reference);
            if (!target.has_value() || !is_gradient(m_elements[*target].name)) {
                return fail(reference.attribute + " on element " + m_elements[reference.element].name +
                    " references something other than a gradient in the document: url(#" + reference.target_id + ")");
            }
        }
        for (const Reference& reference : m_clip_references) {
            const std::optional<std::size_t> target = target_of(reference);
            if (!target.has_value() || m_elements[*target].name != "clipPath") {
                return fail("clip-path on element " + m_elements[reference.element].name +
                    " references something other than a clipPath in the document: url(#" + reference.target_id + ")");
            }
        }
        // Chains: gradient hrefs hop gradient to gradient; use hrefs hop
        // through the uses below the target.
        for (std::size_t index = 0; index < m_elements.size(); ++index) {
            const Element& element = m_elements[index];
            if (is_gradient(element.name) && href_of(index) != nullptr) {
                std::set<std::size_t> visited{index};
                std::size_t hops = 0;
                for (const Reference* hop = href_of(index); hop != nullptr; hop = href_of(*target_of(*hop))) {
                    ++hops;
                    if (!visited.insert(*target_of(*hop)).second) {
                        return fail("gradient reference cycle at #" + hop->target_id);
                    }
                    if (hops > limits::k_max_svg_reference_chain) {
                        return fail("gradient reference chain longer than " + std::to_string(limits::k_max_svg_reference_chain) +
                            " at element " + element.name);
                    }
                }
            }
            if (element.name == "use") {
                std::set<std::size_t> visiting;
                const std::optional<std::size_t> chain = use_chain(index, visiting);
                if (!chain.has_value()) {
                    return fail("use reference cycle at #" + href_of(index)->target_id);
                }
                if (*chain > limits::k_max_svg_reference_chain) {
                    return fail("use reference chain longer than " + std::to_string(limits::k_max_svg_reference_chain) +
                        " at #" + href_of(index)->target_id);
                }
            }
        }
        std::set<std::size_t> visiting;
        const std::optional<std::size_t> expansion = expanded_subtree(0, visiting);
        return expansion.has_value() || fail("use expansion exceeds max_expanded_elements");
    }

    std::optional<std::size_t> expanded_subtree(
        std::size_t              index,
        std::set<std::size_t>&   visiting)
    {
        if (!visiting.insert(index).second) {
            return std::nullopt;
        }
        std::size_t count = 1;
        for (const std::size_t child : m_elements[index].children) {
            const Element& child_element = m_elements[child];
            std::optional<std::size_t> child_count;
            if (child_element.name == "use") {
                const Reference* reference = href_of(child);
                if (reference == nullptr) {
                    return std::nullopt;
                }
                const std::optional<std::size_t> target = target_of(*reference);
                if (!target.has_value()) {
                    return std::nullopt;
                }
                child_count = expanded_subtree(*target, visiting);
                if (child_count.has_value()) {
                    if (*child_count >= m_limits.max_expanded_elements) {
                        return std::nullopt;
                    }
                    ++*child_count;   // the use element plus the target instance
                }
            }
            else {
                child_count = expanded_subtree(child, visiting);
            }
            if (!child_count.has_value() ||
                *child_count > m_limits.max_expanded_elements -
                    std::min(count, m_limits.max_expanded_elements))
            {
                return std::nullopt;
            }
            count += *child_count;
        }
        visiting.erase(index);
        return count;
    }

    const std::vector<Element>&        m_elements;
    const Svg_subset_limits&           m_limits;
    std::string*                       m_detail = nullptr;
    std::map<std::string, std::size_t> m_ids;
    std::vector<Reference>             m_hrefs;
    std::vector<Reference>             m_paint_references;
    std::vector<Reference>             m_clip_references;
    std::map<std::size_t, std::size_t> m_use_chain;
    bool                               m_uses_xlink_href = false;
};

// ---------------------------------------------------------------------------
// Canonical serialization

void append_escaped(std::string& out, std::string_view value)
{
    for (const char c : value) {
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '"':  out += "&quot;"; break;
            case '\t': out += "&#9;";   break;
            case '\n': out += "&#10;";  break;
            case '\r': out += "&#13;";  break;
            default:   out += c;        break;
        }
    }
}

void serialize_element(
    const std::vector<Element>& elements,
    std::size_t                 index,
    std::string_view            current_color_hex,
    std::string&                out)
{
    const Element& element = elements[index];
    std::vector<Attribute> attributes = element.attributes;
    if (index == 0) {
        const auto color = std::find_if(attributes.begin(), attributes.end(), [](const Attribute& attribute) {
            return attribute.name == "color";
        });
        if (color != attributes.end()) {
            color->value = std::string(current_color_hex);
        }
        else {
            attributes.push_back(Attribute{"color", std::string(current_color_hex)});
        }
    }
    std::sort(attributes.begin(), attributes.end(), [](const Attribute& a, const Attribute& b) {
        return a.name < b.name;
    });

    out += '<';
    out += element.name;
    for (const Attribute& attribute : attributes) {
        out += ' ';
        out += attribute.name;
        out += "=\"";
        append_escaped(out, attribute.value);
        out += '"';
    }
    if (element.children.empty()) {
        out += "/>";
        return;
    }
    out += '>';
    for (const std::size_t child : element.children) {
        serialize_element(elements, child, current_color_hex, out);
    }
    out += "</";
    out += element.name;
    out += '>';
}

} // namespace

bool is_svg_color_keyword(std::string_view name)
{
    return std::binary_search(k_color_keywords.begin(), k_color_keywords.end(), name);
}

std::optional<Svg_canonical> sanitize_svg_subset(
    std::string_view         bytes,
    const Svg_subset_limits& limits,
    std::string_view         current_color_hex,
    Contract_failure*        out_failure)
{
    const auto violation = [out_failure](const char* reason_code, std::string detail) {
        if (out_failure != nullptr) {
            out_failure->reason_code = reason_code;
            out_failure->detail      = std::move(detail);
        }
        return std::nullopt;
    };

    if (bytes.size() > limits.max_bytes) {
        return violation(reason::k_svg_subset_violation, "document exceeds max_bytes");
    }
    if (!is_valid_utf8(bytes)) {
        return violation(reason::k_invalid_utf8, "document is not valid UTF-8");
    }

    std::string detail;
    Subset_parser parser(bytes, limits);
    if (!parser.parse(&detail)) {
        return violation(reason::k_svg_subset_violation, std::move(detail));
    }

    Svg_canonical canonical;
    Subset_validator validator(parser.elements(), limits);
    if (!validator.validate(&detail, &canonical.view_box_width, &canonical.view_box_height)) {
        return violation(reason::k_svg_subset_violation, std::move(detail));
    }

    canonical.canonical_bytes = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    serialize_element(parser.elements(), 0, current_color_hex, canonical.canonical_bytes);
    canonical.canonical_bytes += '\n';
    return canonical;
}

} // namespace logonomic::cli_addon
