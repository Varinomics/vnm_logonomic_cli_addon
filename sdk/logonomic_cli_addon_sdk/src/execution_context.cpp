#include "logonomic_cli_addon/execution_context.h"

#include "logonomic_cli_addon/capability_ids.h"
#include "logonomic_cli_addon/limits.h"
#include "strict_json.h"

#include <charconv>
#include <system_error>
#include <utility>

namespace logonomic::cli_addon {
namespace {

constexpr std::size_t k_max_source_bytes = 128;
constexpr std::size_t k_max_failure_detail_bytes = 1024;

constexpr strict::Name_entry<Execution_context_status> k_status_names[] = {
    {Execution_context_status::UNKNOWN, "unknown"},
    {Execution_context_status::CONFIRMED, "confirmed"},
    {Execution_context_status::UNAVAILABLE, "unavailable"},
};

bool bounded_text(std::string_view value, std::size_t maximum)
{
    return !value.empty() && value.size() <= maximum && is_valid_utf8(value) &&
        strict::has_no_control_characters(value);
}

bool read_update(const Json_value& value, Execution_context_update& update)
{
    if (!value.is_object() || strict::unexpected_member(value, {
            "provider_session_id", "transition_nonce", "sequence", "state", "environment", "cwd", "source"}))
    {
        return false;
    }
    const auto* id = strict::string_member(value, "provider_session_id");
    const auto* nonce = strict::string_member(value, "transition_nonce");
    const auto* sequence = strict::string_member(value, "sequence");
    const auto* state = strict::string_member(value, "state");
    const auto* environment = strict::string_member(value, "environment");
    const auto* cwd = strict::string_member(value, "cwd");
    const auto* source = strict::string_member(value, "source");
    if (!id || !nonce || !sequence || !state || !environment || !cwd || !source ||
        sequence->empty() || sequence->size() > 20 ||
        (sequence->size() > 1 && sequence->front() == '0') ||
        sequence->find_first_not_of("0123456789") != std::string::npos)
    {
        return false;
    }
    const auto status = strict::value_of(k_status_names, *state);
    const auto parsed = std::from_chars(
        sequence->data(), sequence->data() + sequence->size(), update.sequence);
    if (!status || parsed.ec != std::errc{} || parsed.ptr != sequence->data() + sequence->size()) {
        return false;
    }
    update.provider_session_id = *id;
    update.transition_nonce = *nonce;
    update.status = *status;
    update.environment = *environment;
    update.cwd = *cwd;
    update.source = *source;
    return is_valid_execution_context_update(update);
}

} // namespace

const char* execution_context_status_name(Execution_context_status status)
{
    return strict::name_of(k_status_names, status);
}

bool is_execution_local_path(std::string_view path)
{
    return strict::has_no_control_characters(path) && strict::is_bounded_absolute_path(path);
}

bool is_valid_execution_context_update(const Execution_context_update& update)
{
    if (!bounded_text(update.provider_session_id, limits::k_max_lifecycle_identity_bytes) ||
        !is_valid_transition_nonce_hex(update.transition_nonce) ||
        !bounded_text(update.source, k_max_source_bytes))
    {
        return false;
    }
    switch (update.status) {
        case Execution_context_status::CONFIRMED:
            return update.environment == "local" && is_execution_local_path(update.cwd);
        case Execution_context_status::UNKNOWN:
            return update.environment.empty() && update.cwd.empty();
        case Execution_context_status::UNAVAILABLE:
            return update.environment == "unsupported" && update.cwd.empty();
    }
    return false;
}

bool apply_execution_context_update(
    Execution_context_state& state,
    const Execution_context_update& update)
{
    if (!is_valid_execution_context_update(update) ||
        state.provider_session_id != update.provider_session_id ||
        state.transition_nonce != update.transition_nonce ||
        (state.sequence && update.sequence <= *state.sequence))
    {
        return false;
    }
    state.sequence = update.sequence;
    state.status = update.status;
    state.environment = update.environment;
    state.cwd = update.cwd;
    state.source = update.source;
    return true;
}

void invalidate_execution_context(Execution_context_state& state)
{
    state.status = Execution_context_status::UNKNOWN;
    state.environment.clear();
    state.cwd.clear();
    state.source.clear();
}

void encode_execution_context_request(Json_value& payload, std::uint32_t version)
{
    if (version != 0) {
        Json_value envelope = Json_value::object();
        envelope.set("version", Json_value(static_cast<std::uint64_t>(version)));
        payload.set("execution_context", std::move(envelope));
    }
}

void decode_execution_context_request(
    const Json_value& payload,
    std::uint32_t& version,
    std::optional<Contract_failure>& failure)
{
    version = 0;
    failure.reset();
    const auto* envelope = payload.find("execution_context");
    if (!envelope) {
        return;
    }
    if (!envelope->is_object() || strict::unexpected_member(*envelope, {"version"}) ||
        !strict::integer_member(*envelope, "version", k_execution_context_version, k_execution_context_version))
    {
        failure = Contract_failure{reason::k_unsupported_version,
            "execution_context request is malformed or its version is unsupported"};
        return;
    }
    version = k_execution_context_version;
}

void encode_execution_updates(
    Json_value& payload,
    const std::vector<Execution_context_update>& updates,
    const std::optional<Contract_failure>& failure)
{
    if (updates.empty() && !failure) {
        return;
    }
    Json_value envelope = Json_value::object();
    envelope.set("version", Json_value(k_execution_context_version));
    if (failure) {
        Json_value encoded = Json_value::object();
        encoded.set("reason_code", Json_value(failure->reason_code));
        encoded.set("detail", Json_value(failure->detail));
        envelope.set("failure", std::move(encoded));
    }
    else {
        Json_value encoded = Json_value::array();
        for (const auto& update : updates) {
            Json_value item = Json_value::object();
            item.set("provider_session_id", Json_value(update.provider_session_id));
            item.set("transition_nonce", Json_value(update.transition_nonce));
            item.set("sequence", Json_value(std::to_string(update.sequence)));
            item.set("state", Json_value(execution_context_status_name(update.status)));
            item.set("environment", Json_value(update.environment));
            item.set("cwd", Json_value(update.cwd));
            item.set("source", Json_value(update.source));
            encoded.push_back(std::move(item));
        }
        envelope.set("updates", std::move(encoded));
    }
    payload.set("execution_context", std::move(envelope));
}

void decode_execution_updates(
    const Json_value& payload,
    std::vector<Execution_context_update>& updates,
    std::optional<Contract_failure>& failure)
{
    updates.clear();
    failure.reset();
    const auto* envelope = payload.find("execution_context");
    if (!envelope) {
        return;
    }
    const auto reject = [&] {
        updates.clear();
        failure = Contract_failure{reason::k_observation_invalid,
            "execution_context result is malformed or exceeds its bounds"};
    };
    if (!envelope->is_object() || strict::unexpected_member(*envelope, {"version", "updates", "failure"}) ||
        !strict::integer_member(*envelope, "version", k_execution_context_version, k_execution_context_version))
    {
        reject();
        return;
    }
    const auto* encoded = envelope->find("updates");
    if (const auto* reported = envelope->find("failure")) {
        if (encoded || !reported->is_object() ||
            strict::unexpected_member(*reported, {"reason_code", "detail"}))
        {
            reject();
            return;
        }
        const auto* reason_code = strict::string_member(*reported, "reason_code");
        const auto* detail = strict::string_member(*reported, "detail");
        if (!reason_code || !is_valid_reason_code(*reason_code) || !detail ||
            detail->size() > k_max_failure_detail_bytes || !is_valid_utf8(*detail) ||
            !strict::has_no_control_characters(*detail))
        {
            reject();
            return;
        }
        failure = Contract_failure{*reason_code, *detail};
        return;
    }
    if (!encoded || !encoded->is_array() || encoded->as_array().empty() ||
        encoded->as_array().size() > limits::k_max_observations_per_result)
    {
        reject();
        return;
    }
    for (const auto& item : encoded->as_array()) {
        Execution_context_update update;
        if (!read_update(item, update)) {
            reject();
            return;
        }
        updates.push_back(std::move(update));
    }
}

} // namespace logonomic::cli_addon
