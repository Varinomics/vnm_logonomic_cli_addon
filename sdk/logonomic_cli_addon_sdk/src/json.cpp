#include "logonomic_cli_addon/json.h"
#include "decimal_parse.h"
#include "strict_json.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <set>
#include <utility>

namespace logonomic::cli_addon {

// ---------------------------------------------------------------------------
// Json_value

Json_value::Json_value(bool value)             : m_value(value) {}
Json_value::Json_value(std::int64_t value)     : m_value(value) {}
Json_value::Json_value(int value)              : m_value((std::int64_t)value) {}
Json_value::Json_value(std::uint64_t value)    : m_value((std::int64_t)value) {}
Json_value::Json_value(double value)           : m_value(value) {}
Json_value::Json_value(std::string value)      : m_value(std::move(value)) {}
Json_value::Json_value(std::string_view value) : m_value(std::string(value)) {}
Json_value::Json_value(const char* value)      : m_value(std::string(value)) {}
Json_value::Json_value(Array value)            : m_value(std::move(value)) {}
Json_value::Json_value(Object value)           : m_value(std::move(value)) {}

Json_value Json_value::object() { return Json_value(Object{}); }
Json_value Json_value::array()  { return Json_value(Array{}); }

Json_type Json_value::type() const
{
    switch (m_value.index()) {
        case 0:  return Json_type::NULL_VALUE;
        case 1:  return Json_type::BOOLEAN;
        case 2:  return Json_type::INTEGER;
        case 3:  return Json_type::NUMBER;
        case 4:  return Json_type::STRING;
        case 5:  return Json_type::ARRAY;
        case 6:  return Json_type::OBJECT;
        default: return Json_type::NULL_VALUE;
    }
}

bool               Json_value::as_boolean() const { return std::get<bool>(m_value); }
std::int64_t       Json_value::as_integer() const { return std::get<std::int64_t>(m_value); }
const std::string& Json_value::as_string()  const { return std::get<std::string>(m_value); }
const Json_value::Array&  Json_value::as_array()  const { return std::get<Array>(m_value); }
Json_value::Array&        Json_value::as_array()        { return std::get<Array>(m_value); }
const Json_value::Object& Json_value::as_object() const { return std::get<Object>(m_value); }
Json_value::Object&       Json_value::as_object()       { return std::get<Object>(m_value); }

double Json_value::as_number() const
{
    if (m_value.index() == 2) {
        return (double)std::get<std::int64_t>(m_value);
    }
    return std::get<double>(m_value);
}

const Json_value* Json_value::find(std::string_view name) const
{
    if (!is_object()) {
        return nullptr;
    }
    for (const Json_member& member : as_object()) {
        if (member.name == name) {
            return &member.value;
        }
    }
    return nullptr;
}

Json_value* Json_value::find(std::string_view name)
{
    return const_cast<Json_value*>(static_cast<const Json_value*>(this)->find(name));
}

void Json_value::set(std::string name, Json_value value)
{
    Object& members = as_object();
    for (Json_member& member : members) {
        if (member.name == name) {
            member.value = std::move(value);
            return;
        }
    }
    members.push_back(Json_member{std::move(name), std::move(value)});
}

bool Json_value::erase(std::string_view name)
{
    Object& members = as_object();
    for (auto it = members.begin(); it != members.end(); ++it) {
        if (it->name == name) {
            members.erase(it);
            return true;
        }
    }
    return false;
}

void Json_value::push_back(Json_value value)
{
    as_array().push_back(std::move(value));
}

bool Json_value::operator==(const Json_value& other) const
{
    return m_value == other.m_value;
}

// ---------------------------------------------------------------------------
// UTF-8

namespace {

// Decodes one UTF-8 scalar starting at bytes[0]; returns the sequence length
// or 0 when the sequence is malformed under RFC 3629 (overlong, surrogate,
// above U+10FFFF, truncated, bad continuation).
std::size_t decode_utf8_scalar(const unsigned char* bytes, std::size_t available, std::uint32_t* out_scalar)
{
    const unsigned char lead = bytes[0];
    std::size_t   length = 0;
    std::uint32_t scalar = 0;
    std::uint32_t minimum = 0;

    if (lead < 0x80) {
        *out_scalar = lead;
        return 1;
    }
    if ((lead & 0xE0) == 0xC0) { length = 2; scalar = lead & 0x1Fu; minimum = 0x80; }     else
    if ((lead & 0xF0) == 0xE0) { length = 3; scalar = lead & 0x0Fu; minimum = 0x800; }    else
    if ((lead & 0xF8) == 0xF0) { length = 4; scalar = lead & 0x07u; minimum = 0x10000; }  else {
        return 0;
    }

    if (available < length) {
        return 0;
    }
    for (std::size_t i = 1; i < length; ++i) {
        const unsigned char continuation = bytes[i];
        if ((continuation & 0xC0) != 0x80) {
            return 0;
        }
        scalar = (scalar << 6) | (continuation & 0x3Fu);
    }
    if (scalar < minimum || scalar > 0x10FFFF || (scalar >= 0xD800 && scalar <= 0xDFFF)) {
        return 0;
    }
    *out_scalar = scalar;
    return length;
}

void append_utf8(std::string& out, std::uint32_t scalar)
{
    if (scalar < 0x80) {
        out.push_back((char)scalar);
    }
    else
    if (scalar < 0x800) {
        out.push_back((char)(0xC0 | (scalar >> 6)));
        out.push_back((char)(0x80 | (scalar & 0x3F)));
    }
    else
    if (scalar < 0x10000) {
        out.push_back((char)(0xE0 | (scalar >> 12)));
        out.push_back((char)(0x80 | ((scalar >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (scalar & 0x3F)));
    }
    else {
        out.push_back((char)(0xF0 | (scalar >> 18)));
        out.push_back((char)(0x80 | ((scalar >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((scalar >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (scalar & 0x3F)));
    }
}

} // namespace

bool is_valid_utf8(std::string_view bytes)
{
    const unsigned char* data = reinterpret_cast<const unsigned char*>(bytes.data());
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        std::uint32_t scalar = 0;
        const std::size_t length = decode_utf8_scalar(data + offset, bytes.size() - offset, &scalar);
        if (length == 0) {
            return false;
        }
        offset += length;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Parser

namespace {

class Json_parser
{
public:
    Json_parser(std::string_view text, const Json_parse_limits& limits, strict::Parse_failure& failure)
    :
        m_text(text),
        m_limits(limits),
        m_failure(failure)
    {}

    std::optional<Json_value> parse_document(Json_parse_error* out_error)
    {
        if (m_text.size() >= 3 &&
            (unsigned char)m_text[0] == 0xEF && (unsigned char)m_text[1] == 0xBB && (unsigned char)m_text[2] == 0xBF)
        {
            return fail("byte-order mark is not allowed", out_error);
        }

        skip_whitespace();
        Json_value value;
        if (!parse_value(0, value, out_error)) {
            return std::nullopt;
        }
        skip_whitespace();
        if (m_offset != m_text.size()) {
            return fail("trailing content after the JSON value", out_error);
        }
        return value;
    }

private:
    std::optional<Json_value> fail(const char* message, Json_parse_error* out_error) const
    {
        if (out_error != nullptr) {
            out_error->offset  = m_offset;
            out_error->message = message;
        }
        return std::nullopt;
    }

    bool fail_bool(const char* message, Json_parse_error* out_error,
        strict::Parse_failure kind = strict::Parse_failure::INVALID_JSON) const
    {
        m_failure = kind;
        fail(message, out_error);
        return false;
    }

    bool at_end() const { return m_offset >= m_text.size(); }
    unsigned char peek() const { return (unsigned char)m_text[m_offset]; }

    void skip_whitespace()
    {
        while (!at_end()) {
            const unsigned char c = peek();
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++m_offset;
            }
            else {
                break;
            }
        }
    }

    bool consume_literal(const char* literal, Json_parse_error* out_error)
    {
        const std::size_t length = std::strlen(literal);
        if (m_text.substr(m_offset, length) != literal) {
            return fail_bool("invalid literal", out_error);
        }
        m_offset += length;
        return true;
    }

    bool parse_value(std::size_t depth, Json_value& out_value, Json_parse_error* out_error)
    {
        if (at_end()) {
            return fail_bool("unexpected end of input", out_error);
        }

        switch (peek()) {
            case '{': return parse_object(depth + 1, out_value, out_error);
            case '[': return parse_array(depth + 1, out_value, out_error);
            case '"': {
                std::string text;
                if (!parse_string(text, out_error)) {
                    return false;
                }
                out_value = Json_value(std::move(text));
                return true;
            }
            case 't':
                if (!consume_literal("true", out_error)) { return false; }
                out_value = Json_value(true);
                return true;
            case 'f':
                if (!consume_literal("false", out_error)) { return false; }
                out_value = Json_value(false);
                return true;
            case 'n':
                if (!consume_literal("null", out_error)) { return false; }
                out_value = Json_value();
                return true;
            default:
                return parse_number(out_value, out_error);
        }
    }

    bool parse_object(std::size_t depth, Json_value& out_value, Json_parse_error* out_error)
    {
        if (depth > m_limits.max_depth) {
            return fail_bool("nesting depth exceeds the limit", out_error, strict::Parse_failure::LIMIT_EXCEEDED);
        }
        ++m_offset;   // '{'

        Json_value::Object members;
        std::set<std::string> member_names;
        skip_whitespace();
        if (!at_end() && peek() == '}') {
            ++m_offset;
            out_value = Json_value(std::move(members));
            return true;
        }

        while (true) {
            skip_whitespace();
            if (at_end() || peek() != '"') {
                return fail_bool("expected a member name", out_error);
            }
            std::string name;
            if (!parse_string(name, out_error)) {
                return false;
            }
            if (!member_names.insert(name).second) {
                return fail_bool("duplicate member name", out_error);
            }
            if (members.size() >= m_limits.max_members) {
                return fail_bool("object member count exceeds the limit", out_error, strict::Parse_failure::LIMIT_EXCEEDED);
            }

            skip_whitespace();
            if (at_end() || peek() != ':') {
                return fail_bool("expected ':' after a member name", out_error);
            }
            ++m_offset;
            skip_whitespace();

            Json_value member_value;
            if (!parse_value(depth, member_value, out_error)) {
                return false;
            }
            members.push_back(Json_member{std::move(name), std::move(member_value)});

            skip_whitespace();
            if (at_end()) {
                return fail_bool("unterminated object", out_error);
            }
            if (peek() == ',') {
                ++m_offset;
                continue;
            }
            if (peek() == '}') {
                ++m_offset;
                out_value = Json_value(std::move(members));
                return true;
            }
            return fail_bool("expected ',' or '}' in an object", out_error);
        }
    }

    bool parse_array(std::size_t depth, Json_value& out_value, Json_parse_error* out_error)
    {
        if (depth > m_limits.max_depth) {
            return fail_bool("nesting depth exceeds the limit", out_error, strict::Parse_failure::LIMIT_EXCEEDED);
        }
        ++m_offset;   // '['

        Json_value::Array items;
        skip_whitespace();
        if (!at_end() && peek() == ']') {
            ++m_offset;
            out_value = Json_value(std::move(items));
            return true;
        }

        while (true) {
            skip_whitespace();
            Json_value item;
            if (!parse_value(depth, item, out_error)) {
                return false;
            }
            items.push_back(std::move(item));

            skip_whitespace();
            if (at_end()) {
                return fail_bool("unterminated array", out_error);
            }
            if (peek() == ',') {
                ++m_offset;
                continue;
            }
            if (peek() == ']') {
                ++m_offset;
                out_value = Json_value(std::move(items));
                return true;
            }
            return fail_bool("expected ',' or ']' in an array", out_error);
        }
    }

    bool parse_hex4(std::uint32_t& out_code_unit, Json_parse_error* out_error)
    {
        if (m_text.size() - m_offset < 4) {
            return fail_bool("truncated \\u escape", out_error);
        }
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            const char c = m_text[m_offset + i];
            std::uint32_t digit = 0;
            if (c >= '0' && c <= '9')      { digit = (std::uint32_t)(c - '0'); }      else
            if (c >= 'a' && c <= 'f')      { digit = (std::uint32_t)(c - 'a' + 10); } else
            if (c >= 'A' && c <= 'F')      { digit = (std::uint32_t)(c - 'A' + 10); } else {
                return fail_bool("invalid hexadecimal digit in \\u escape", out_error);
            }
            value = (value << 4) | digit;
        }
        m_offset += 4;
        out_code_unit = value;
        return true;
    }

    bool parse_string(std::string& out_text, Json_parse_error* out_error)
    {
        ++m_offset;   // opening quote
        out_text.clear();

        while (true) {
            if (at_end()) {
                return fail_bool("unterminated string", out_error);
            }
            if (out_text.size() > m_limits.max_string_bytes) {
                return fail_bool("string length exceeds the limit", out_error, strict::Parse_failure::LIMIT_EXCEEDED);
            }

            const unsigned char c = peek();
            if (c == '"') {
                ++m_offset;
                if (out_text.size() > m_limits.max_string_bytes) {
                    return fail_bool("string length exceeds the limit", out_error, strict::Parse_failure::LIMIT_EXCEEDED);
                }
                return true;
            }
            if (c < 0x20) {
                return fail_bool("control character in string", out_error);
            }
            if (c == '\\') {
                ++m_offset;
                if (at_end()) {
                    return fail_bool("unterminated escape", out_error);
                }
                const char escape = m_text[m_offset++];
                switch (escape) {
                    case '"':  out_text.push_back('"');  break;
                    case '\\': out_text.push_back('\\'); break;
                    case '/':  out_text.push_back('/');  break;
                    case 'b':  out_text.push_back('\b'); break;
                    case 'f':  out_text.push_back('\f'); break;
                    case 'n':  out_text.push_back('\n'); break;
                    case 'r':  out_text.push_back('\r'); break;
                    case 't':  out_text.push_back('\t'); break;
                    case 'u': {
                        std::uint32_t code_unit = 0;
                        if (!parse_hex4(code_unit, out_error)) {
                            return false;
                        }
                        if (code_unit >= 0xDC00 && code_unit <= 0xDFFF) {
                            return fail_bool("lone low surrogate escape", out_error);
                        }
                        if (code_unit >= 0xD800 && code_unit <= 0xDBFF) {
                            if (m_text.substr(m_offset, 2) != "\\u") {
                                return fail_bool("high surrogate escape without a low surrogate", out_error);
                            }
                            m_offset += 2;
                            std::uint32_t low = 0;
                            if (!parse_hex4(low, out_error)) {
                                return false;
                            }
                            if (low < 0xDC00 || low > 0xDFFF) {
                                return fail_bool("high surrogate escape without a low surrogate", out_error);
                            }
                            code_unit = 0x10000 + ((code_unit - 0xD800) << 10) + (low - 0xDC00);
                        }
                        append_utf8(out_text, code_unit);
                        break;
                    }
                    default:
                        return fail_bool("invalid escape", out_error);
                }
                continue;
            }

            if (c < 0x80) {
                const std::size_t start       = m_offset;
                std::size_t maximum_run       = m_text.size() - start;
                const std::uint64_t remaining = m_limits.max_string_bytes - out_text.size();
                if (maximum_run > remaining) {
                    // Include one excess byte so the next iteration preserves
                    // the existing limit offset and unterminated-string error.
                    maximum_run = (std::size_t)remaining + 1;
                }
                const std::size_t end = start + maximum_run;
                do {
                    ++m_offset;
                    if (m_offset == end) {
                        break;
                    }
                    const unsigned char next = peek();
                    if (next < 0x20 || next >= 0x80 || next == '"' || next == '\\') {
                        break;
                    }
                }
                while (true);
                out_text.append(m_text.data() + start, m_offset - start);
                continue;
            }

            std::uint32_t scalar = 0;
            const std::size_t length = decode_utf8_scalar(
                reinterpret_cast<const unsigned char*>(m_text.data()) + m_offset,
                m_text.size() - m_offset,
                &scalar);
            if (length == 0) {
                return fail_bool("invalid UTF-8 in string", out_error, strict::Parse_failure::INVALID_UTF8);
            }
            out_text.append(m_text.data() + m_offset, length);
            m_offset += length;
        }
    }

    bool parse_number(Json_value& out_value, Json_parse_error* out_error)
    {
        const std::size_t start = m_offset;
        if (peek() == '-') {
            ++m_offset;
        }
        if (at_end() || peek() < '0' || peek() > '9') {
            return fail_bool("invalid number", out_error);
        }
        if (peek() == '0') {
            ++m_offset;
        }
        else {
            while (!at_end() && peek() >= '0' && peek() <= '9') {
                ++m_offset;
            }
        }

        bool integral = true;
        if (!at_end() && peek() == '.') {
            integral = false;
            ++m_offset;
            if (at_end() || peek() < '0' || peek() > '9') {
                return fail_bool("invalid fraction", out_error);
            }
            while (!at_end() && peek() >= '0' && peek() <= '9') {
                ++m_offset;
            }
        }
        if (!at_end() && (peek() == 'e' || peek() == 'E')) {
            integral = false;
            ++m_offset;
            if (!at_end() && (peek() == '+' || peek() == '-')) {
                ++m_offset;
            }
            if (at_end() || peek() < '0' || peek() > '9') {
                return fail_bool("invalid exponent", out_error);
            }
            while (!at_end() && peek() >= '0' && peek() <= '9') {
                ++m_offset;
            }
        }

        const std::string_view lexeme = m_text.substr(start, m_offset - start);
        if (integral) {
            std::int64_t value = 0;
            const auto result = std::from_chars(lexeme.data(), lexeme.data() + lexeme.size(), value);
            if (result.ec != std::errc() || result.ptr != lexeme.data() + lexeme.size()) {
                m_offset = start;
                return fail_bool("integer outside the 64-bit range", out_error);
            }
            out_value = Json_value(value);
            return true;
        }

        // Both overflow and underflow report result_out_of_range and are
        // rejected: the contract admits only numbers a double represents
        // finitely and non-degenerately, and the outcome must not depend on
        // whether a standard library stores a substitute value on error.
        double value = 0.0;
        const auto result = detail::parse_decimal(lexeme, value);
        if (result.ec == std::errc::result_out_of_range) {
            m_offset = start;
            return fail_bool("number outside the double range", out_error);
        }
        if (result.ec != std::errc() || result.ptr != lexeme.data() + lexeme.size() || !std::isfinite(value)) {
            m_offset = start;
            return fail_bool("invalid number", out_error);
        }
        out_value = Json_value(value);
        return true;
    }

    std::string_view         m_text;
    const Json_parse_limits& m_limits;
    strict::Parse_failure&   m_failure;
    std::size_t              m_offset = 0;
};

} // namespace

std::optional<Json_value> parse_json(
    std::string_view          text,
    const Json_parse_limits&  limits,
    Json_parse_error*         out_error)
{
    strict::Parse_failure failure;
    return strict::parse_document(text, limits, failure, out_error);
}

std::optional<Json_value> strict::parse_document(
    std::string_view text, const Json_parse_limits& limits, Parse_failure& out_failure, Json_parse_error* out_error)
{
    out_failure = Parse_failure::INVALID_JSON;
    if (text.size() > limits.max_bytes) {
        out_failure = Parse_failure::LIMIT_EXCEEDED;
        if (out_error != nullptr) {
            out_error->offset  = limits.max_bytes;
            out_error->message = "document size exceeds the limit";
        }
        return std::nullopt;
    }
    Json_parser parser(text, limits, out_failure);
    return parser.parse_document(out_error);
}

// ---------------------------------------------------------------------------
// Serialization

namespace {

void append_escaped_string(std::string& out, std::string_view text)
{
    out.push_back('"');
    std::size_t run_start = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const unsigned char byte = (unsigned char)text[i];
        if (byte >= 0x20 && byte != '"' && byte != '\\') {
            continue;
        }
        if (i > run_start) {
            out.append(text.data() + run_start, i - run_start);
        }
        switch (byte) {
            case '"':  out.append("\\\""); break;
            case '\\': out.append("\\\\"); break;
            case '\b': out.append("\\b");  break;
            case '\f': out.append("\\f");  break;
            case '\n': out.append("\\n");  break;
            case '\r': out.append("\\r");  break;
            case '\t': out.append("\\t");  break;
            default: {
                constexpr char k_hex[] = "0123456789abcdef";
                out.append("\\u00");
                out.push_back(k_hex[byte >> 4]);
                out.push_back(k_hex[byte & 0x0F]);
                break;
            }
        }
        run_start = i + 1;
    }
    if (run_start < text.size()) {
        out.append(text.data() + run_start, text.size() - run_start);
    }
    out.push_back('"');
}

void append_number(std::string& out, double value)
{
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    std::string_view rendered(buffer, (std::size_t)(result.ptr - buffer));
    out.append(rendered);
    // A NUMBER whose shortest representation looks integral keeps a fraction
    // so it re-parses as a NUMBER, not an INTEGER.
    if (rendered.find_first_of(".eE") == std::string_view::npos) {
        out.append(".0");
    }
}

void append_value(std::string& out, const Json_value& value, bool canonical)
{
    switch (value.type()) {
        case Json_type::NULL_VALUE:
            out.append("null");
            break;
        case Json_type::BOOLEAN:
            out.append(value.as_boolean() ? "true" : "false");
            break;
        case Json_type::INTEGER: {
            char buffer[32];
            const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value.as_integer());
            out.append(buffer, (std::size_t)(result.ptr - buffer));
            break;
        }
        case Json_type::NUMBER:
            append_number(out, value.as_number());
            break;
        case Json_type::STRING:
            append_escaped_string(out, value.as_string());
            break;
        case Json_type::ARRAY: {
            out.push_back('[');
            bool first = true;
            for (const Json_value& item : value.as_array()) {
                if (!first) {
                    out.push_back(',');
                }
                first = false;
                append_value(out, item, canonical);
            }
            out.push_back(']');
            break;
        }
        case Json_type::OBJECT: {
            const Json_value::Object& members = value.as_object();
            out.push_back('{');
            bool first = true;
            const auto append_member = [&](const Json_member& member) {
                if (!first) {
                    out.push_back(',');
                }
                first = false;
                append_escaped_string(out, member.name);
                out.push_back(':');
                append_value(out, member.value, canonical);
            };
            if (canonical) {
                std::vector<const Json_member*> ordered;
                ordered.reserve(members.size());
                for (const Json_member& member : members) {
                    ordered.push_back(&member);
                }
                std::sort(ordered.begin(), ordered.end(), [](const Json_member* a, const Json_member* b) {
                    return a->name < b->name;
                });
                for (const Json_member* member : ordered) {
                    append_member(*member);
                }
            }
            else {
                for (const Json_member& member : members) {
                    append_member(member);
                }
            }
            out.push_back('}');
            break;
        }
        default:
            out.append("null");
            break;
    }
}

} // namespace

std::string serialize_json(const Json_value& value)
{
    std::string out;
    append_value(out, value, false);
    return out;
}

std::string serialize_json_canonical(const Json_value& value)
{
    std::string out;
    append_value(out, value, true);
    return out;
}

std::size_t json_depth(const Json_value& value)
{
    switch (value.type()) {
        case Json_type::ARRAY: {
            std::size_t deepest = 0;
            for (const Json_value& item : value.as_array()) {
                deepest = std::max(deepest, json_depth(item));
            }
            return deepest + 1;
        }
        case Json_type::OBJECT: {
            std::size_t deepest = 0;
            for (const Json_member& member : value.as_object()) {
                deepest = std::max(deepest, json_depth(member.value));
            }
            return deepest + 1;
        }
        default:
            return 0;
    }
}

} // namespace logonomic::cli_addon
