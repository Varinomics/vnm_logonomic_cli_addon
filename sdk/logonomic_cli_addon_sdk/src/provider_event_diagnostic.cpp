#include "logonomic_cli_addon/provider_event_diagnostic.h"

#include "logonomic_cli_addon/session_contracts.h"

#include "strict_json.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <limits>
#include <utility>

namespace logonomic::cli_addon {
namespace {

constexpr std::size_t k_max_field_bytes = 160;
constexpr std::size_t k_max_rule_bytes  = 64;

constexpr strict::Name_entry<Provider_event_problem> k_problems[] = {
    {Provider_event_problem::MISSING,           "missing"},
    {Provider_event_problem::WRONG_TYPE,        "wrong_type"},
    {Provider_event_problem::EMPTY,             "empty"},
    {Provider_event_problem::TOO_LARGE,         "too_large"},
    {Provider_event_problem::CONTROL_CHARACTER, "control_character"},
    {Provider_event_problem::MISMATCH,          "mismatch"},
    {Provider_event_problem::INVALID_VALUE,     "invalid_value"},
    {Provider_event_problem::INVALID_SHAPE,     "invalid_shape"},
    {Provider_event_problem::DUPLICATE,         "duplicate"},
    {Provider_event_problem::UNAVAILABLE,       "unavailable"},
};

constexpr strict::Name_entry<Json_type> k_types[] = {
    {Json_type::NULL_VALUE, "null"},
    {Json_type::BOOLEAN,    "boolean"},
    {Json_type::INTEGER,    "integer"},
    {Json_type::NUMBER,     "number"},
    {Json_type::STRING,     "string"},
    {Json_type::ARRAY,      "array"},
    {Json_type::OBJECT,     "object"},
};

bool source_label(std::string_view value, std::size_t bound)
{
    if (value.empty() || value.size() > bound) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || byte == '_' || byte == '-' || byte == '.' ||
            byte == '[' || byte == ']' || byte == '$';
    });
}

bool valid_diagnostic(const Provider_event_diagnostic& diagnostic)
{
    const auto known_problem = std::find_if(std::begin(k_problems), std::end(k_problems),
        [&](const auto& entry) { return entry.value == diagnostic.problem; });
    if (known_problem == std::end(k_problems) ||
        diagnostic.event_name.empty() || diagnostic.event_name.size() > limits::k_max_provider_event_name_bytes ||
        diagnostic.event_name.find('\0') != std::string::npos || !is_valid_utf8(diagnostic.event_name) ||
        !source_label(diagnostic.field, k_max_field_bytes) ||
        (!diagnostic.rule.empty() &&
            (diagnostic.rule.size() > k_max_rule_bytes || !is_valid_reason_code(diagnostic.rule))))
    {
        return false;
    }
    if (diagnostic.actual_type) {
        const auto known_type = std::find_if(std::begin(k_types), std::end(k_types),
            [&](const auto& entry) { return entry.value == *diagnostic.actual_type; });
        if (known_type == std::end(k_types)) {
            return false;
        }
    }
    if (diagnostic.actual_size &&
        (!diagnostic.actual_type || (*diagnostic.actual_type != Json_type::STRING &&
            *diagnostic.actual_type != Json_type::ARRAY && *diagnostic.actual_type != Json_type::OBJECT)))
    {
        return false;
    }
    constexpr auto maximum = (std::uint64_t)std::numeric_limits<std::int64_t>::max();
    return (!diagnostic.actual_size || *diagnostic.actual_size <= maximum) &&
        (!diagnostic.limit || *diagnostic.limit <= maximum);
}

} // namespace

Provider_event_diagnostic make_provider_event_diagnostic(
    std::string event_name, std::string field, Provider_event_problem problem,
    const Json_value* actual, std::optional<std::uint64_t> limit, std::string rule)
{
    Provider_event_diagnostic diagnostic;
    diagnostic.event_name = std::move(event_name);
    diagnostic.field      = std::move(field);
    diagnostic.problem    = problem;
    diagnostic.limit      = limit;
    diagnostic.rule       = std::move(rule);
    if (actual) {
        diagnostic.actual_type = actual->type();
        switch (actual->type()) {
            case Json_type::STRING: diagnostic.actual_size = actual->as_string().size(); break;
            case Json_type::ARRAY:  diagnostic.actual_size = actual->as_array().size();  break;
            case Json_type::OBJECT: diagnostic.actual_size = actual->as_object().size(); break;
            default: break;
        }
    }
    return diagnostic;
}

std::string encode_provider_event_diagnostic(const Provider_event_diagnostic& diagnostic)
{
    if (!valid_diagnostic(diagnostic)) {
        throw std::invalid_argument("invalid_provider_event_diagnostic");
    }
    auto body = Json_value::object();
    body.set("version",     Json_value(1));
    body.set("event_name",  Json_value(diagnostic.event_name));
    body.set("field",       Json_value(diagnostic.field));
    body.set("problem",     Json_value(strict::name_of(k_problems, diagnostic.problem)));
    body.set("actual_type", Json_value(diagnostic.actual_type
        ? strict::name_of(k_types, *diagnostic.actual_type) : "missing"));
    if (diagnostic.actual_size) {
        body.set("actual_size", Json_value(*diagnostic.actual_size));
    }
    if (diagnostic.limit) {
        body.set("limit", Json_value(*diagnostic.limit));
    }
    if (!diagnostic.rule.empty()) {
        body.set("rule", Json_value(diagnostic.rule));
    }
    auto envelope = Json_value::object();
    envelope.set(k_provider_event_diagnostic_member, std::move(body));
    auto message = serialize_json(envelope);
    if (message.size() > limits::k_max_result_message_bytes) {
        // Native event names allow controls. Their JSON escapes can exceed the
        // message bound, while base64 preserves all 256 bytes in at most 344.
        auto* encoded = envelope.find(k_provider_event_diagnostic_member);
        encoded->erase("event_name");
        encoded->set("event_name_base64", Json_value(encode_base64(diagnostic.event_name)));
        message = serialize_json(envelope);
    }
    return message;
}

std::optional<Provider_event_diagnostic> decode_provider_event_diagnostic(std::string_view message)
{
    if (message.size() > limits::k_max_result_message_bytes) {
        return std::nullopt;
    }
    Json_parse_limits bounds;
    bounds.max_bytes        = limits::k_max_result_message_bytes;
    bounds.max_depth        = 3;
    bounds.max_members      = 8;
    bounds.max_string_bytes = ((limits::k_max_provider_event_name_bytes + 2) / 3) * 4;
    const auto envelope = parse_json(message, bounds, nullptr);
    if (!envelope || !envelope->is_object() || envelope->as_object().size() != 1) {
        return std::nullopt;
    }
    const auto* body = strict::member_of_type(*envelope, k_provider_event_diagnostic_member, Json_type::OBJECT);
    if (!body || strict::unexpected_member(*body,
        {"version", "event_name", "event_name_base64", "field", "problem", "actual_type", "actual_size", "limit", "rule"}) ||
        strict::integer_member(*body, "version", 1, 1) != 1)
    {
        return std::nullopt;
    }
    if (bool(body->find("event_name")) == bool(body->find("event_name_base64"))) {
        return std::nullopt;
    }
    const auto* event = strict::string_member(*body, "event_name");
    const auto* encoded_event = strict::string_member(*body, "event_name_base64");
    const auto event_name = event ? std::optional(*event)
        : encoded_event ? decode_base64(*encoded_event) : std::nullopt;
    const auto* field   = strict::string_member(*body, "field");
    const auto* problem = strict::string_member(*body, "problem");
    const auto* type    = strict::string_member(*body, "actual_type");
    if (!event_name || !field || !problem || !type) {
        return std::nullopt;
    }
    const auto parsed_problem = strict::value_of(k_problems, *problem);
    if (!parsed_problem) {
        return std::nullopt;
    }
    Provider_event_diagnostic diagnostic;
    diagnostic.event_name = *event_name;
    diagnostic.field      = *field;
    diagnostic.problem    = *parsed_problem;
    if (*type != "missing") {
        diagnostic.actual_type = strict::value_of(k_types, *type);
        if (!diagnostic.actual_type) {
            return std::nullopt;
        }
    }
    for (const auto* member : {"actual_size", "limit"}) {
        if (body->find(member)) {
            const auto size = strict::integer_member(*body, member, 0, std::numeric_limits<std::int64_t>::max());
            if (!size) {
                return std::nullopt;
            }
            (std::string_view(member) == "actual_size" ? diagnostic.actual_size : diagnostic.limit) =
                (std::uint64_t)*size;
        }
    }
    if (body->find("rule")) {
        const auto* rule = strict::string_member(*body, "rule");
        if (!rule || rule->empty()) {
            return std::nullopt;
        }
        diagnostic.rule = *rule;
    }
    return valid_diagnostic(diagnostic) ? std::optional(std::move(diagnostic)) : std::nullopt;
}

std::nullopt_t reject_provider_event(Contract_failure* failure, Provider_event_diagnostic diagnostic)
{
    if (failure) {
        *failure = {"invalid_provider_event", encode_provider_event_diagnostic(diagnostic)};
    }
    return std::nullopt;
}

Provider_event_error::Provider_event_error(Provider_event_diagnostic diagnostic)
:
    std::runtime_error("invalid_provider_event"),
    m_diagnostic(std::move(diagnostic))
{}

const Provider_event_diagnostic& Provider_event_error::diagnostic() const noexcept
{
    return m_diagnostic;
}

} // namespace logonomic::cli_addon
