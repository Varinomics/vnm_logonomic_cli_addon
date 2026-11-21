#pragma once

#include "logonomic_cli_addon/json.h"

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

namespace logonomic::cli_addon::json {

/// Returns a null value when the member is absent or value is not an object.
inline const Json_value& member(const Json_value& value, std::string_view key)
{
    static const Json_value absent;
    const auto* found = value.find(key);
    return found ? *found : absent;
}

/// Returns an empty string when the member is absent or is not a string.
inline std::string text(const Json_value& value, std::string_view key)
{
    const auto& found = member(value, key);
    return found.is_string() ? found.as_string() : std::string{};
}

inline Json_value object(std::initializer_list<Json_member> members)
{
    return Json_value(Json_value::Object(members));
}

/// Every member name must be allowed; allowed names need not all be present.
inline bool only_keys(const Json_value& value, std::initializer_list<std::string_view> keys)
{
    return value.is_object() && std::all_of(value.as_object().begin(), value.as_object().end(),
        [&](const auto& entry) { return std::find(keys.begin(), keys.end(), entry.name) != keys.end(); });
}

inline bool exact_keys(const Json_value& value, std::initializer_list<std::string_view> keys)
{
    return only_keys(value, keys) && value.as_object().size() == keys.size();
}

/// Checks byte length and excludes NUL. Empty strings require explicit opt-in.
inline bool bounded_string(const Json_value& value, std::size_t maximum, bool empty = false)
{
    return value.is_string() && (empty || !value.as_string().empty()) &&
        value.as_string().size() <= maximum && value.as_string().find('\0') == std::string::npos;
}

inline std::optional<Json_value> parse(std::string_view bytes)
{
    return parse_json(bytes, {}, nullptr);
}

} // namespace logonomic::cli_addon::json
