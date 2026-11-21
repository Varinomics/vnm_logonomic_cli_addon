#include "logonomic_cli_addon/auth_status.h"
#include "logonomic_cli_addon/quota.h"
#include "strict_json.h"

#include <cmath>
#include <stdexcept>

namespace logonomic::cli_addon {

namespace {

constexpr std::int64_t k_max_exact_integer = 9007199254740991;
constexpr std::size_t  k_max_label_bytes   = 128;

constexpr strict::Name_entry<Auth_status> k_auth_names[] = {
    {Auth_status::AUTHENTICATED, "authenticated"},
    {Auth_status::NOT_LOGGED_IN, "not_logged_in"},
    {Auth_status::UNKNOWN,       "unknown"},
    {Auth_status::UNAVAILABLE,   "unavailable"},
    {Auth_status::FAILED,        "error"},
};

constexpr strict::Name_entry<Quota_status> k_quota_names[] = {
    {Quota_status::READY,       "ready"},
    {Quota_status::NOT_LOGGED_IN, "not_logged_in"},
    {Quota_status::UNAVAILABLE, "unavailable"},
    {Quota_status::FAILED,      "error"},
};

bool fail(Contract_failure* out_failure, const char* detail)
{
    if (out_failure != nullptr) {
        out_failure->reason_code = "observation_invalid";
        out_failure->detail      = detail;
    }
    return false;
}

bool valid_label(const Json_value& value)
{
    return value.is_string() && !value.as_string().empty() &&
        value.as_string().size() <= k_max_label_bytes && is_valid_utf8(value.as_string()) &&
        strict::has_no_control_characters(value.as_string());
}

bool valid_credit_decimal(const Json_value& value)
{
    if (!value.is_string() || value.as_string().empty() || value.as_string().size() > 64) {
        return false;
    }
    const std::string& text = value.as_string();
    std::size_t offset = text.front() == '-' ? 1 : 0;
    if (offset == text.size()) {
        return false;
    }
    const std::size_t first = offset;
    while (offset < text.size() && text[offset] >= '0' && text[offset] <= '9') {
        ++offset;
    }
    if (offset == first || (offset - first > 1 && text[first] == '0')) {
        return false;
    }
    if (offset == text.size()) {
        return true;
    }
    if (text[offset++] != '.' || offset == text.size()) {
        return false;
    }
    for (; offset < text.size(); ++offset) {
        if (text[offset] < '0' || text[offset] > '9') {
            return false;
        }
    }
    return true;
}

bool empty_declaration(const Json_value& payload, Contract_failure* out_failure)
{
    if (!payload.is_object() || !payload.as_object().empty()) {
        if (out_failure != nullptr) {
            out_failure->reason_code = reason::k_invalid_sidecar;
            out_failure->detail      = "probe declaration must be an empty object";
        }
        return false;
    }
    return true;
}

template <typename Result>
bool decode_envelope(
    const Json_value& payload, bool final_inspection, Result& result, Contract_failure* out_failure)
{
    if (!payload.is_object() || strict::unexpected_member(payload, {"complete", "protocol_state", "observation"})) {
        return fail(out_failure, "probe result has invalid members");
    }
    const auto* complete = strict::member_of_type(payload, "complete", Json_type::BOOLEAN);
    const auto* state    = strict::string_member(payload, "protocol_state");
    if (complete == nullptr || state == nullptr || state->size() > limits::k_max_protocol_state_bytes ||
        state->find('\0') != std::string::npos || !is_valid_utf8(*state))
    {
        return fail(out_failure, "probe completion or protocol state is missing or invalid");
    }
    result.complete       = complete->as_boolean();
    result.protocol_state = *state;
    if ((final_inspection && !result.complete) || result.complete != (payload.find("observation") != nullptr)) {
        return fail(out_failure, "only a completed probe must carry one observation, and final inspection must complete");
    }
    return true;
}

template <typename Result>
Json_value encode_envelope(const Result& result)
{
    Json_value payload = Json_value::object();
    payload.set("complete",       Json_value(result.complete));
    payload.set("protocol_state", Json_value(result.protocol_state));
    return payload;
}

Json_value encode_window(const Quota_window& window)
{
    Json_value value = Json_value::object();
    value.set("used_percent", Json_value(window.used_percent));
    if (window.window_minutes) {
        value.set("window_minutes", Json_value(*window.window_minutes));
    }
    if (window.resets_at_ms) {
        value.set("resets_at_ms", Json_value(*window.resets_at_ms));
    }
    if (window.reset_text) {
        value.set("reset_text", Json_value(*window.reset_text));
    }
    if (window.label) {
        value.set("label", Json_value(*window.label));
    }
    return value;
}

std::optional<Quota_window> decode_window(const Json_value& value, Contract_failure* out_failure)
{
    const Json_value* percent = value.find("used_percent");
    if (!value.is_object() || strict::unexpected_member(value, {
            "used_percent", "window_minutes", "resets_at_ms", "label", "reset_text"}) ||
        percent == nullptr || !percent->is_number() || !std::isfinite(percent->as_number()))
    {
        fail(out_failure, "quota window percentage is not finite");
        return std::nullopt;
    }
    Quota_window window;
    window.used_percent = percent->as_number();
    if (const Json_value* text = value.find("reset_text"); text != nullptr) {
        if (!valid_label(*text)) {
            fail(out_failure, "quota reset text is invalid");
            return std::nullopt;
        }
        window.reset_text = text->as_string();
    }
    for (const char* member : {"window_minutes", "resets_at_ms"}) {
        if (value.find(member) == nullptr) {
            continue;
        }
        const auto integer = strict::integer_member(value, member, 0, k_max_exact_integer);
        if (!integer || (std::string_view(member) == "window_minutes" && *integer == 0)) {
            fail(out_failure, "quota window time is outside its exact integer bound");
            return std::nullopt;
        }
        (std::string_view(member) == "window_minutes" ? window.window_minutes : window.resets_at_ms) =
            static_cast<std::uint64_t>(*integer);
    }
    if (const Json_value* label = value.find("label"); label != nullptr) {
        if (!valid_label(*label)) {
            fail(out_failure, "quota window label is invalid");
            return std::nullopt;
        }
        window.label = label->as_string();
    }
    return window;
}

} // namespace

Json_value encode_observation_scope(const Observation_scope& scope)
{
    Json_value value = Json_value::object();
    value.set("source_account_key", scope.source_account_key ? Json_value(*scope.source_account_key) : Json_value());
    value.set("observed_at_ms",     Json_value(scope.observed_at_ms));
    value.set("fresh_for_ms",       Json_value(scope.fresh_for_ms));
    return value;
}

std::optional<Observation_scope> decode_observation_scope(const Json_value& value, Contract_failure* out_failure)
{
    const Json_value* key = value.find("source_account_key");
    const auto observed  = strict::integer_member(value, "observed_at_ms", 0, k_max_exact_integer);
    const auto freshness = strict::integer_member(value, "fresh_for_ms", 0, k_max_exact_integer);
    if (!value.is_object() || strict::unexpected_member(value, {
            "source_account_key", "observed_at_ms", "fresh_for_ms"}) ||
        key == nullptr || (!key->is_null() && (!key->is_string() || !is_valid_adapter_account_key(key->as_string()))) ||
        !observed || !freshness)
    {
        fail(out_failure, "observation requires its exact source scope and bounded integer observation/freshness times");
        return std::nullopt;
    }
    Observation_scope scope;
    if (!key->is_null()) {
        scope.source_account_key = key->as_string();
    }
    scope.observed_at_ms = static_cast<std::uint64_t>(*observed);
    scope.fresh_for_ms   = static_cast<std::uint64_t>(*freshness);
    return scope;
}

const char* auth_status_name(Auth_status state)   { return strict::name_of(k_auth_names, state); }
const char* quota_status_name(Quota_status state) { return strict::name_of(k_quota_names, state); }

bool decode_current_account_declaration(const Json_value& payload, Contract_failure* out_failure)
{
    return empty_declaration(payload, out_failure);
}

Json_value encode_current_account_request()
{
    Json_value value = Json_value::object();
    value.set("version", Json_value((std::int64_t)k_current_account_version));
    return value;
}

bool decode_current_account_request(const Json_value& value)
{
    return value.is_object() && !strict::unexpected_member(value, {"version"}) &&
        strict::integer_member(value, "version", 1, 1).has_value();
}

Json_value encode_current_account_observation(const Current_account_observation& observation)
{
    Json_value value = encode_current_account_request();
    value.set("scope", encode_observation_scope(observation.scope));
    value.set("identity_key", Json_value(observation.identity_key));
    value.set("is_default_profile", Json_value(observation.is_default_profile));
    return value;
}

std::optional<Current_account_observation> decode_current_account_observation(const Json_value& value)
{
    if (!value.is_object() || strict::unexpected_member(value, {
            "version", "scope", "identity_key", "is_default_profile"}) ||
        !strict::integer_member(value, "version", 1, 1))
    {
        return std::nullopt;
    }
    const auto* key = strict::string_member(value, "identity_key");
    const auto* profile = strict::member_of_type(value, "is_default_profile", Json_type::BOOLEAN);
    const auto* scope = value.find("scope");
    if (!key || !is_valid_adapter_account_key(*key) || !profile || !scope) {
        return std::nullopt;
    }
    const auto decoded_scope = decode_observation_scope(*scope, nullptr);
    if (!decoded_scope || !decoded_scope->source_account_key) {
        return std::nullopt;
    }
    return Current_account_observation{*decoded_scope, *key, profile->as_boolean()};
}

Json_value encode_auth_inspect_result(const Auth_inspect_result& result)
{
    Json_value payload = encode_envelope(result);
    if (result.complete && result.current_account) {
        payload.set("current_account", encode_current_account_observation(*result.current_account));
    }
    if (result.observation) {
        Json_value observation = Json_value::object();
        observation.set("scope", encode_observation_scope(result.observation->scope));
        observation.set("state", Json_value(auth_status_name(result.observation->state)));
        payload.set("observation", std::move(observation));
    }
    return payload;
}

std::optional<Auth_inspect_result> decode_auth_inspect_result(
    const Json_value& payload, bool final_inspection, Contract_failure* out_failure)
{
    Auth_inspect_result result;
    Json_value envelope = payload;
    // This independently negotiated observation cannot invalidate auth.
    envelope.erase("current_account");
    if (!decode_envelope(envelope, final_inspection, result, out_failure)) {
        return std::nullopt;
    }
    if (!result.complete) {
        return result;
    }
    const Json_value& observation = *payload.find("observation");
    const auto* state = strict::string_member(observation, "state");
    const auto* scope = observation.find("scope");
    if (!observation.is_object() || strict::unexpected_member(observation, {"scope", "state"}) ||
        state == nullptr || scope == nullptr)
    {
        fail(out_failure, "auth observation has invalid members or an unknown state");
        return std::nullopt;
    }
    const auto parsed_state = strict::value_of(k_auth_names, *state);
    if (!parsed_state)
    {
        fail(out_failure, "auth observation has invalid members or an unknown state");
        return std::nullopt;
    }
    const auto parsed_scope = decode_observation_scope(*scope, out_failure);
    if (!parsed_scope) {
        return std::nullopt;
    }
    result.observation = Auth_observation{*parsed_scope, *parsed_state};
    if (const auto* current = payload.find("current_account");
        current && *parsed_state == Auth_status::AUTHENTICATED)
    {
        auto identity = decode_current_account_observation(*current);
        if (identity && identity->scope.source_account_key == parsed_scope->source_account_key &&
            identity->scope.observed_at_ms == parsed_scope->observed_at_ms &&
            identity->scope.fresh_for_ms == parsed_scope->fresh_for_ms)
        {
            result.current_account = std::move(identity);
        }
    }
    return result;
}

Json_value encode_quota_inspect_result(const Quota_inspect_result& result, std::uint16_t version)
{
    if (version != 2) {
        throw std::invalid_argument("unsupported quota result version");
    }
    Json_value payload = encode_envelope(result);
    if (result.stdin_bytes) {
        payload.set("stdin_base64", Json_value(encode_base64(*result.stdin_bytes)));
    }
    if (result.failure_reason) {
        payload.set("failure_reason", Json_value(*result.failure_reason));
    }
    if (!result.observation) {
        return payload;
    }
    const auto& source = *result.observation;
    Json_value value = Json_value::object();
    value.set("scope", encode_observation_scope(source.scope));
    value.set("state", Json_value(quota_status_name(source.state)));
    if (source.primary) {
        value.set("primary", encode_window(*source.primary));
    }
    if (source.secondary) {
        value.set("secondary", encode_window(*source.secondary));
    }
    if (source.plan) {
        value.set("plan", Json_value(*source.plan));
    }
    if (source.reset_credits) {
        value.set("reset_credits", Json_value(*source.reset_credits));
    }
    if (source.credits_balance) {
        value.set("credits_balance", Json_value(*source.credits_balance));
    }
    payload.set("observation", std::move(value));
    return payload;
}

std::optional<Quota_inspect_result> decode_quota_inspect_result(
    const Json_value& payload, bool final_inspection, Contract_failure* out_failure, std::uint16_t version)
{
    if (version != 2) {
        fail(out_failure, "unsupported quota result version");
        return std::nullopt;
    }
    if (!payload.is_object()) {
        fail(out_failure, "quota result is not an object");
        return std::nullopt;
    }
    Quota_inspect_result result;
    Json_value envelope = payload;
    envelope.erase("stdin_base64");
    envelope.erase("failure_reason");
    if (!decode_envelope(envelope, final_inspection, result, out_failure)) {
        return std::nullopt;
    }
    if (payload.find("stdin_base64") != nullptr) {
        const auto* input = strict::string_member(payload, "stdin_base64");
        if (result.complete || input == nullptr) {
            fail(out_failure, "only an incomplete quota exchange may write stdin");
            return std::nullopt;
        }
        result.stdin_bytes = decode_base64(*input);
        if (!result.stdin_bytes || result.stdin_bytes->empty() ||
            result.stdin_bytes->size() > limits::k_max_structured_stream_bytes)
        {
            fail(out_failure, "quota stdin is invalid or exceeds its bound");
            return std::nullopt;
        }
    }
    if (!result.complete) {
        if (payload.find("failure_reason")) {
            fail(out_failure, "incomplete quota exchange cannot report an authentication failure");
            return std::nullopt;
        }
        return result;
    }
    const Json_value& value = *payload.find("observation");
    const auto* state = strict::string_member(value, "state");
    const auto* scope = value.find("scope");
    const auto parsed_state = state != nullptr ? strict::value_of(k_quota_names, *state) : std::nullopt;
    if (!value.is_object() || strict::unexpected_member(value, {
            "scope", "state", "primary", "secondary", "plan", "reset_credits", "credits_balance"}) ||
        !parsed_state || scope == nullptr)
    {
        fail(out_failure, "quota observation has invalid members or an unknown state");
        return std::nullopt;
    }
    const auto parsed_scope = decode_observation_scope(*scope, out_failure);
    if (!parsed_scope) {
        return std::nullopt;
    }
    Quota_observation observation;
    observation.scope = *parsed_scope;
    observation.state = *parsed_state;
    if (observation.state != Quota_status::READY && value.as_object().size() != 2) {
        fail(out_failure, "unavailable or failed quota cannot carry quota values");
        return std::nullopt;
    }
    for (const char* member : {"primary", "secondary"}) {
        if (const Json_value* window = value.find(member); window != nullptr) {
            auto parsed_window = decode_window(*window, out_failure);
            if (!parsed_window) {
                return std::nullopt;
            }
            (std::string_view(member) == "primary" ? observation.primary : observation.secondary) = *parsed_window;
        }
    }
    if (const Json_value* plan = value.find("plan"); plan != nullptr) {
        if (!valid_label(*plan)) {
            fail(out_failure, "quota plan is invalid");
            return std::nullopt;
        }
        observation.plan = plan->as_string();
    }
    if (value.find("reset_credits") != nullptr) {
        observation.reset_credits = strict::integer_member(value, "reset_credits", 0, k_max_exact_integer);
        if (!observation.reset_credits) {
            fail(out_failure, "quota reset credits are outside their exact integer bound");
            return std::nullopt;
        }
    }
    if (const Json_value* balance = value.find("credits_balance"); balance != nullptr) {
        if (!valid_credit_decimal(*balance)) {
            fail(out_failure, "quota credit balance must be a bounded decimal string");
            return std::nullopt;
        }
        observation.credits_balance = balance->as_string();
    }
    if (payload.find("failure_reason")) {
        const auto* reason = strict::string_member(payload, "failure_reason");
        if (!reason || *reason != "authentication_required" || observation.state != Quota_status::FAILED) {
            fail(out_failure, "quota failure reason must identify authentication loss on a failed observation");
            return std::nullopt;
        }
        result.failure_reason = *reason;
    }
    result.observation = std::move(observation);
    return result;
}

} // namespace logonomic::cli_addon
