#pragma once

#include "logonomic_cli_addon/limits.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace logonomic::cli_addon {

struct Json_member;

enum class Json_type
{
    NULL_VALUE,
    BOOLEAN,
    INTEGER,   // a JSON number without fraction or exponent that fits std::int64_t
    NUMBER,    // any other finite JSON number
    STRING,
    ARRAY,
    OBJECT,
};

/**
 * @brief Strict RFC 8259 document model. Integers are exact std::int64_t
 * values, other numbers are finite doubles, strings are valid UTF-8, object
 * member order is preserved as parsed or built, and duplicate member names
 * are rejected at parse time.
 */
class Json_value
{
public:
    using Array  = std::vector<Json_value>;
    using Object = std::vector<Json_member>;

    Json_value() = default;   // null
    explicit Json_value(bool value);
    explicit Json_value(std::int64_t value);
    explicit Json_value(int value);
    explicit Json_value(std::uint64_t value);   // rejected above INT64_MAX by precondition
    explicit Json_value(double value);
    explicit Json_value(std::string value);
    explicit Json_value(std::string_view value);
    explicit Json_value(const char* value);
    explicit Json_value(Array value);
    explicit Json_value(Object value);

    static Json_value object();
    static Json_value array();

    Json_type type() const;
    bool is_null()    const { return type() == Json_type::NULL_VALUE; }
    bool is_boolean() const { return type() == Json_type::BOOLEAN; }
    bool is_integer() const { return type() == Json_type::INTEGER; }
    bool is_number()  const { return type() == Json_type::NUMBER || type() == Json_type::INTEGER; }
    bool is_string()  const { return type() == Json_type::STRING; }
    bool is_array()   const { return type() == Json_type::ARRAY; }
    bool is_object()  const { return type() == Json_type::OBJECT; }

    // Accessors require the matching type; the caller checks first.
    bool               as_boolean() const;
    std::int64_t       as_integer() const;
    double             as_number()  const;   // INTEGER or NUMBER
    const std::string& as_string()  const;
    const Array&       as_array()   const;
    Array&             as_array();
    const Object&      as_object()  const;
    Object&            as_object();

    /// Member lookup on an object; nullptr when absent or when this is not an
    /// object.
    const Json_value* find(std::string_view name) const;
    Json_value*       find(std::string_view name);

    /// Replaces the member of that name or appends it. Requires an object.
    void set(std::string name, Json_value value);

    /// Removes the member of that name; false when absent. Requires an object.
    bool erase(std::string_view name);

    /// Appends to an array. Requires an array.
    void push_back(Json_value value);

    bool operator==(const Json_value& other) const;
    bool operator!=(const Json_value& other) const { return !(*this == other); }

private:
    std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, Array, Object> m_value;
};

struct Json_member
{
    std::string name;
    Json_value  value;

    bool operator==(const Json_member& other) const
    {
        return name == other.name && value == other.value;
    }
};

/**
 * @brief Bounds applied while parsing. Depth counts nested arrays and
 * objects (a top-level scalar has depth 0); the member bound applies per
 * object; the string bound applies to the decoded bytes of each string.
 */
struct Json_parse_limits
{
    std::uint64_t max_bytes        = limits::k_max_frame_bytes;
    std::size_t   max_depth        = limits::k_max_json_depth;
    std::size_t   max_members      = limits::k_max_json_members;
    std::uint64_t max_string_bytes = limits::k_max_json_string_bytes;
};

struct Json_parse_error
{
    std::size_t offset = 0;   // byte offset of the defect, or the input size at end of input
    std::string message;
};

/// Strict parse of one complete JSON text. Rejects a byte-order mark,
/// invalid UTF-8, control characters in strings, lone surrogates, duplicate
/// member names, integers outside std::int64_t, non-finite numbers, trailing
/// content, and every bound in the limits.
std::optional<Json_value> parse_json(
    std::string_view          text,
    const Json_parse_limits&  limits,
    Json_parse_error*         out_error);

/// Compact serialization preserving member order; the frame form.
std::string serialize_json(const Json_value& value);

/// Canonical serialization: members sorted bytewise by name, no whitespace,
/// minimal escapes. The digest-input form.
std::string serialize_json_canonical(const Json_value& value);

/// Nesting depth of a value: 0 for scalars, 1 for an empty array or object.
std::size_t json_depth(const Json_value& value);

/// Strict RFC 3629 validation: no overlong forms, no surrogates, nothing
/// above U+10FFFF, no truncated sequences.
bool is_valid_utf8(std::string_view bytes);

} // namespace logonomic::cli_addon
