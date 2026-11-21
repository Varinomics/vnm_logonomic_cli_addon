#include "strict_json.h"
#include "logonomic_cli_addon/limits.h"
#include "logonomic_cli_addon/package_source.h"

#include <algorithm>

namespace logonomic::cli_addon::strict {

std::optional<std::string> unexpected_member(
    const Json_value&                       object,
    std::initializer_list<std::string_view> allowed)
{
    for (const Json_member& member : object.as_object()) {
        const bool known = std::any_of(allowed.begin(), allowed.end(), [&member](std::string_view name) {
            return member.name == name;
        });
        if (!known) {
            return member.name;
        }
    }
    return std::nullopt;
}

const Json_value* member_of_type(const Json_value& object, std::string_view name, Json_type type)
{
    const Json_value* value = object.find(name);
    if (value == nullptr || value->type() != type) {
        return nullptr;
    }
    return value;
}

std::optional<std::int64_t> integer_member(
    const Json_value& object,
    std::string_view  name,
    std::int64_t      minimum,
    std::int64_t      maximum)
{
    const Json_value* value = member_of_type(object, name, Json_type::INTEGER);
    if (value == nullptr) {
        return std::nullopt;
    }
    const std::int64_t integer = value->as_integer();
    if (integer < minimum || integer > maximum) {
        return std::nullopt;
    }
    return integer;
}

const std::string* string_member(const Json_value& object, std::string_view name)
{
    const Json_value* value = member_of_type(object, name, Json_type::STRING);
    return value == nullptr ? nullptr : &value->as_string();
}

bool has_no_control_characters(std::string_view text)
{
    return std::none_of(text.begin(), text.end(), [](char c) {
        const unsigned char byte = (unsigned char)c;
        return byte < 0x20 || byte == 0x7F;
    });
}

bool is_bounded_absolute_path(std::string_view path)
{
    return !path.empty() && path.size() <= limits::k_max_start_absolute_path_bytes &&
        path.find('\0') == std::string_view::npos && is_valid_utf8(path) &&
        path_from_utf8(path).is_absolute();
}

} // namespace logonomic::cli_addon::strict
