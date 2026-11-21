#pragma once

// Internal helpers for the strict-object rule shared by every contract
// decoder: a member set is closed, every member has one type, and integers
// are range-checked; and for the value grammars several decoders share. Not
// part of the public SDK surface.

#include "logonomic_cli_addon/json.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

namespace logonomic::cli_addon::strict {

// The runtime needs to distinguish invalid handler output from valid output
// exceeding negotiated resource limits. The public parser uses this same path.
enum class Parse_failure { INVALID_JSON, INVALID_UTF8, LIMIT_EXCEEDED };
std::optional<Json_value> parse_document(
    std::string_view text, const Json_parse_limits& limits, Parse_failure& out_failure, Json_parse_error* out_detail);

/// Name of the first member that is not in the allowed set, or nullopt.
std::optional<std::string> unexpected_member(
    const Json_value&                       object,
    std::initializer_list<std::string_view> allowed);

/// The member when present with the given type, otherwise nullptr.
const Json_value* member_of_type(const Json_value& object, std::string_view name, Json_type type);

/// Present integer member within [minimum, maximum]; nullopt otherwise.
std::optional<std::int64_t> integer_member(
    const Json_value& object,
    std::string_view  name,
    std::int64_t      minimum,
    std::int64_t      maximum);

/// Present string member, or nullptr.
const std::string* string_member(const Json_value& object, std::string_view name);

/// True when the text has no byte below 0x20 and no 0x7F.
bool has_no_control_characters(std::string_view text);

/// True for a nonempty path of at most limits::k_max_start_absolute_path_bytes
/// that has no NUL, is valid UTF-8 and is absolute on this platform.
bool is_bounded_absolute_path(std::string_view path);

/// One row of an enumerator/wire-name table.
template <typename Enum>
struct Name_entry
{
    Enum        value;
    const char* name;
};

/// The wire name of an enumerator; the table's first row for a value
/// outside it, which the enum types make impossible.
template <typename Enum, std::size_t N>
const char* name_of(const Name_entry<Enum> (&table)[N], Enum value)
{
    for (const Name_entry<Enum>& entry : table) {
        if (entry.value == value) {
            return entry.name;
        }
    }
    return table[0].name;
}

/// The enumerator of a wire name, or nullopt outside the vocabulary.
template <typename Enum, std::size_t N>
std::optional<Enum> value_of(const Name_entry<Enum> (&table)[N], std::string_view name)
{
    for (const Name_entry<Enum>& entry : table) {
        if (name == entry.name) {
            return entry.value;
        }
    }
    return std::nullopt;
}

} // namespace logonomic::cli_addon::strict
