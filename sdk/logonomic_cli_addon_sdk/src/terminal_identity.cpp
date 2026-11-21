#include "logonomic_cli_addon/terminal_identity.h"

#include "logonomic_cli_addon/capability_ids.h"
#include "logonomic_cli_addon/limits.h"
#include "logonomic_cli_addon/provider_event_diagnostic.h"
#include "strict_json.h"

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace logonomic::cli_addon {
namespace {

bool fail(Contract_failure* failure, const char* reason_code, const char* detail)
{
    if (failure != nullptr) {
        *failure = {reason_code, detail};
    }
    return false;
}

bool classification_failure(
    Contract_failure* failure, const char* reason_code, const std::string& field,
    const Json_value* actual, Provider_event_problem problem, const char* rule,
    std::optional<std::uint64_t> limit = {}, std::optional<std::uint64_t> measured_size = {})
{
    if (failure) {
        auto diagnostic = make_provider_event_diagnostic(
            "classify_event", field, actual ? problem : Provider_event_problem::MISSING, actual, limit, rule);
        if (measured_size) {
            diagnostic.actual_size = measured_size;
        }
        *failure = {reason_code, encode_provider_event_diagnostic(diagnostic)};
    }
    return false;
}

bool object_members(const Json_value& value, std::initializer_list<std::string_view> members,
    Contract_failure* failure, bool classification = false)
{
    if (!value.is_object()) {
        if (classification) {
            return classification_failure(failure, reason::k_frame_violation, "terminal_identity", &value,
                Provider_event_problem::WRONG_TYPE, "object_required");
        }
        return fail(failure, reason::k_frame_violation, "terminal identity value is not an object");
    }
    if (strict::unexpected_member(value, members)) {
        if (classification) {
            return classification_failure(failure, reason::k_unexpected_member, "terminal_identity", &value,
                Provider_event_problem::INVALID_SHAPE, "closed_members");
        }
        return fail(failure, reason::k_unexpected_member, "terminal identity has an unexpected member");
    }
    return true;
}

bool record_version(const Json_value& value, Contract_failure* failure, bool classification = false)
{
    const auto version = strict::integer_member(value, "version", 1, 65535);
    if (!version) {
        if (classification) {
            return classification_failure(failure, reason::k_frame_violation, "terminal_identity.version",
                value.find("version"), Provider_event_problem::INVALID_VALUE, "positive_version");
        }
        return fail(failure, reason::k_frame_violation, "terminal identity needs a positive version");
    }
    if (*version != k_terminal_identity_version) {
        if (classification) {
            return classification_failure(failure, reason::k_unknown_capability, "terminal_identity.version",
                value.find("version"), Provider_event_problem::UNAVAILABLE, "supported_terminal_identity_version");
        }
        return fail(failure, reason::k_unknown_capability, "terminal identity record version is unavailable");
    }
    return true;
}

bool read_context(const Json_value& payload, Json_value& context, Contract_failure* failure, bool classification = false)
{
    const auto* value = strict::member_of_type(payload, "context", Json_type::OBJECT);
    if (value == nullptr) {
        if (classification) {
            return classification_failure(failure, reason::k_frame_violation, "terminal_identity.context", payload.find("context"),
                Provider_event_problem::WRONG_TYPE, "object_required");
        }
        return fail(failure, reason::k_frame_violation, "terminal identity context is not an object");
    }
    const auto bytes = serialize_json_canonical(*value).size();
    if (bytes > k_max_terminal_identity_context_bytes) {
        if (classification) {
            return classification_failure(failure, reason::k_payload_too_large, "terminal_identity.context", value,
                Provider_event_problem::TOO_LARGE, "serialized_json_bytes", k_max_terminal_identity_context_bytes, bytes);
        }
        return fail(failure, reason::k_payload_too_large, "terminal identity context exceeds its bound");
    }
    context = *value;
    return true;
}

bool read_text(const Json_value& payload, const char* member, std::size_t maximum,
    bool allow_empty, std::string& text, Contract_failure* failure)
{
    const auto* value = strict::string_member(payload, member);
    if (value == nullptr || (!allow_empty && value->empty()) || value->find('\0') != std::string::npos) {
        const auto problem = !value ? Provider_event_problem::WRONG_TYPE
            : value->empty() ? Provider_event_problem::EMPTY : Provider_event_problem::CONTROL_CHARACTER;
        return classification_failure(failure, reason::k_frame_violation, "terminal_identity." + std::string(member),
            payload.find(member), problem, "nul_free_text", maximum);
    }
    if (value->size() > maximum) {
        return classification_failure(failure, reason::k_payload_too_large, "terminal_identity." + std::string(member),
            payload.find(member), Provider_event_problem::TOO_LARGE, "utf8_bytes", maximum);
    }
    if (!is_valid_utf8(*value)) {
        return classification_failure(failure, reason::k_invalid_utf8, "terminal_identity." + std::string(member),
            payload.find(member), Provider_event_problem::INVALID_VALUE, "valid_utf8");
    }
    text = *value;
    return true;
}

} // namespace

Json_value encode_terminal_identity_offer(const Terminal_identity_offer&)
{
    Json_value payload = Json_value::object();
    payload.set("version", Json_value(k_terminal_identity_version));
    return payload;
}

std::optional<Terminal_identity_offer> decode_terminal_identity_offer(
    const Json_value& payload, Contract_failure* out_failure)
{
    if (!object_members(payload, {"version"}, out_failure) || !record_version(payload, out_failure)) {
        return std::nullopt;
    }
    return Terminal_identity_offer{};
}

Json_value encode_terminal_identity_preparation(const Terminal_identity_preparation& preparation)
{
    Json_value payload = encode_terminal_identity_offer();
    payload.set("context", preparation.context);
    return payload;
}

std::optional<Terminal_identity_preparation> decode_terminal_identity_preparation(
    const Json_value& payload, Contract_failure* out_failure)
{
    Terminal_identity_preparation preparation;
    if (!object_members(payload, {"version", "context"}, out_failure) || !record_version(payload, out_failure) ||
        !read_context(payload, preparation.context, out_failure))
    {
        return std::nullopt;
    }
    return preparation;
}

Json_value encode_terminal_identity_context(const Terminal_identity_context& context)
{
    Json_value payload = encode_terminal_identity_preparation(context.preparation);
    payload.set("terminal_event", context.terminal_event.value_or(Json_value()));
    return payload;
}

std::optional<Terminal_identity_context> decode_terminal_identity_context(
    const Json_value& payload, Contract_failure* out_failure)
{
    Terminal_identity_context context;
    if (!object_members(payload, {"version", "context", "terminal_event"}, out_failure, true) ||
        !record_version(payload, out_failure, true) || !read_context(payload, context.preparation.context, out_failure, true))
    {
        return std::nullopt;
    }
    const auto* event = payload.find("terminal_event");
    if (event == nullptr || (!event->is_null() && !event->is_object())) {
        classification_failure(out_failure, reason::k_frame_violation, "terminal_identity.terminal_event", event,
            Provider_event_problem::WRONG_TYPE, "object_or_null");
        return std::nullopt;
    }
    if (event->is_object()) {
        const auto bytes = serialize_json_canonical(*event).size();
        if (bytes > k_max_terminal_identity_event_bytes) {
            classification_failure(out_failure, reason::k_payload_too_large, "terminal_identity.terminal_event", event,
                Provider_event_problem::TOO_LARGE, "serialized_json_bytes", k_max_terminal_identity_event_bytes, bytes);
            return std::nullopt;
        }
        context.terminal_event = *event;
    }
    return context;
}

Json_value encode_terminal_identity_observation(const Terminal_identity_observation& observation)
{
    Json_value payload = Json_value::object();
    payload.set("provider_session_id", Json_value(observation.provider_session_id));
    payload.set("title", Json_value(observation.title));
    return payload;
}

std::optional<Terminal_identity_observation> decode_terminal_identity_observation(
    const Json_value& payload, Contract_failure* out_failure)
{
    Terminal_identity_observation observation;
    if (!object_members(payload, {"provider_session_id", "title"}, out_failure, true) ||
        !read_text(payload, "provider_session_id", limits::k_max_lifecycle_identity_bytes,
            false, observation.provider_session_id, out_failure) ||
        !read_text(payload, "title", k_max_terminal_identity_title_bytes, true, observation.title, out_failure))
    {
        return std::nullopt;
    }
    return observation;
}

} // namespace logonomic::cli_addon
