#include "logonomic_cli_addon/model_state.h"

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

constexpr strict::Name_entry<Model_status> k_status_names[] = {
    {Model_status::UNKNOWN, "unknown"},
    {Model_status::CONFIRMED, "confirmed"},
    {Model_status::UNAVAILABLE, "unavailable"},
};

bool bounded_text(std::string_view value, std::size_t maximum)
{
    return !value.empty() && value.size() <= maximum && is_valid_utf8(value) &&
        strict::has_no_control_characters(value);
}

bool read_update(const Json_value& value, Model_update& update)
{
    if (!value.is_object() || strict::unexpected_member(value, {
            "provider_session_id", "transition_nonce", "sequence", "state", "agent_id", "provider", "model", "reasoning_effort", "source"}))
    {
        return false;
    }
    const auto* id = strict::string_member(value, "provider_session_id");
    const auto* nonce = strict::string_member(value, "transition_nonce");
    const auto* sequence = strict::string_member(value, "sequence");
    const auto* state = strict::string_member(value, "state");
    const auto* agent_id = strict::string_member(value, "agent_id");
    const auto* provider = strict::string_member(value, "provider");
    const auto* model = strict::string_member(value, "model");
    const auto* effort = value.find("reasoning_effort");
    const auto* source = strict::string_member(value, "source");
    if (!id || !nonce || !sequence || !state || !agent_id || !model || !source ||
        (value.find("provider") && !provider) ||
        (effort && !effort->is_string()) ||
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
    update.agent_id = *agent_id;
    update.provider = provider ? *provider : std::string{};
    update.model = *model;
    if (effort) update.reasoning_effort = effort->as_string();
    update.source = *source;
    return is_valid_model_update(update);
}

} // namespace

const char* model_status_name(Model_status status)
{
    return strict::name_of(k_status_names, status);
}

bool is_valid_model_label(std::string_view model)
{
    return bounded_text(model, 512);
}

bool is_valid_structured_model_observation(const Structured_model_observation& observation)
{
    return (observation.provider.empty() || bounded_text(observation.provider, 512)) &&
        is_valid_model_label(observation.model) &&
        bounded_text(observation.source, k_max_source_bytes);
}

bool is_valid_model_update(const Model_update& update)
{
    if (!bounded_text(update.provider_session_id, limits::k_max_lifecycle_identity_bytes) ||
        !is_valid_transition_nonce_hex(update.transition_nonce) ||
        (!update.agent_id.empty() && !bounded_text(update.agent_id, limits::k_max_lifecycle_identity_bytes)) ||
        !bounded_text(update.source, k_max_source_bytes))
    {
        return false;
    }
    switch (update.status) {
        case Model_status::CONFIRMED:
            return (update.provider.empty() || bounded_text(update.provider, 512)) &&
                is_valid_model_label(update.model) &&
                (!update.reasoning_effort || bounded_text(*update.reasoning_effort, 128));
        case Model_status::UNKNOWN:
        case Model_status::UNAVAILABLE:
            return update.provider.empty() && update.model.empty() && !update.reasoning_effort;
    }
    return false;
}

bool apply_model_update(
    Model_state& state,
    const Model_update& update)
{
    if (!is_valid_model_update(update) ||
        state.provider_session_id != update.provider_session_id ||
        state.transition_nonce != update.transition_nonce ||
        state.agent_id != update.agent_id ||
        (state.sequence && update.sequence <= *state.sequence))
    {
        return false;
    }
    state.sequence = update.sequence;
    state.status = update.status;
    state.provider = update.provider;
    state.model = update.model;
    state.reasoning_effort = update.reasoning_effort;
    state.source = update.source;
    return true;
}

void invalidate_model_state(Model_state& state)
{
    state.status = Model_status::UNKNOWN;
    state.provider.clear();
    state.model.clear();
    state.reasoning_effort.reset();
    state.source.clear();
}

void encode_model_state_request(Json_value& payload, std::uint32_t version)
{
    if (version != 0) {
        Json_value envelope = Json_value::object();
        envelope.set("version", Json_value(static_cast<std::uint64_t>(version)));
        payload.set("model_state", std::move(envelope));
    }
}

void decode_model_state_request(
    const Json_value& payload,
    std::uint32_t& version,
    std::optional<Contract_failure>& failure)
{
    version = 0;
    failure.reset();
    const auto* envelope = payload.find("model_state");
    if (!envelope) {
        return;
    }
    if (!envelope->is_object() || strict::unexpected_member(*envelope, {"version"}) ||
        !strict::integer_member(*envelope, "version", k_model_state_version, k_model_state_version))
    {
        failure = Contract_failure{reason::k_unsupported_version,
            "model_state request is malformed or its version is unsupported"};
        return;
    }
    version = k_model_state_version;
}

void encode_model_updates(
    Json_value& payload,
    const std::vector<Model_update>& updates,
    const std::optional<Contract_failure>& failure)
{
    if (updates.empty() && !failure) {
        return;
    }
    Json_value envelope = Json_value::object();
    envelope.set("version", Json_value(k_model_state_version));
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
            item.set("state", Json_value(model_status_name(update.status)));
            item.set("agent_id", Json_value(update.agent_id));
            // Older readers require the empty member to accept withdrawals.
            if (!update.provider.empty() || update.status != Model_status::CONFIRMED) {
                item.set("provider", Json_value(update.provider));
            }
            item.set("model", Json_value(update.model));
            if (update.reasoning_effort) {
                item.set("reasoning_effort", Json_value(*update.reasoning_effort));
            }
            item.set("source", Json_value(update.source));
            encoded.push_back(std::move(item));
        }
        envelope.set("updates", std::move(encoded));
    }
    payload.set("model_state", std::move(envelope));
}

void decode_model_updates(
    const Json_value& payload,
    std::vector<Model_update>& updates,
    std::optional<Contract_failure>& failure)
{
    updates.clear();
    failure.reset();
    const auto* envelope = payload.find("model_state");
    if (!envelope) {
        return;
    }
    const auto reject = [&] {
        updates.clear();
        failure = Contract_failure{reason::k_observation_invalid,
            "model_state result is malformed or exceeds its bounds"};
    };
    if (!envelope->is_object() || strict::unexpected_member(*envelope, {"version", "updates", "failure"}) ||
        !strict::integer_member(*envelope, "version", k_model_state_version, k_model_state_version))
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
        Model_update update;
        if (!read_update(item, update)) {
            reject();
            return;
        }
        updates.push_back(std::move(update));
    }
}

void encode_structured_model_observation(
    Json_value& payload,
    const std::optional<Structured_model_observation>& observation,
    const std::optional<Contract_failure>& failure)
{
    if (failure) {
        encode_model_updates(payload, {}, failure);
        return;
    }
    if (!observation) {
        return;
    }
    if (!is_valid_structured_model_observation(*observation)) {
        encode_model_updates(payload, {}, Contract_failure{reason::k_observation_invalid,
            "structured model_state result is malformed or exceeds its bounds"});
        return;
    }
    Json_value envelope = Json_value::object();
    envelope.set("version", Json_value(k_model_state_version));
    Json_value value = Json_value::object();
    if (!observation->provider.empty()) {
        value.set("provider", Json_value(observation->provider));
    }
    value.set("model", Json_value(observation->model));
    value.set("source", Json_value(observation->source));
    envelope.set("observation", std::move(value));
    payload.set("model_state", std::move(envelope));
}

void decode_structured_model_observation(
    const Json_value& payload,
    std::optional<Structured_model_observation>& observation,
    std::optional<Contract_failure>& failure)
{
    observation.reset();
    failure.reset();
    const auto* envelope = payload.find("model_state");
    if (!envelope) {
        return;
    }
    const auto reject = [&] {
        failure = Contract_failure{reason::k_observation_invalid,
            "structured model_state result is malformed or exceeds its bounds"};
    };
    if (!envelope->is_object() ||
        strict::unexpected_member(*envelope, {"version", "observation", "failure"}) ||
        !strict::integer_member(*envelope, "version", k_model_state_version, k_model_state_version))
    {
        reject();
        return;
    }
    if (envelope->find("failure")) {
        if (envelope->find("observation")) {
            reject();
            return;
        }
        std::vector<Model_update> ignored;
        decode_model_updates(payload, ignored, failure);
        return;
    }
    const auto* value = envelope->find("observation");
    if (!value || !value->is_object() || strict::unexpected_member(*value, {"provider", "model", "source"})) {
        reject();
        return;
    }
    const auto* provider = strict::string_member(*value, "provider");
    const auto* model = strict::string_member(*value, "model");
    const auto* source = strict::string_member(*value, "source");
    if ((value->find("provider") && !provider) || !model || !source ||
        !is_valid_structured_model_observation({provider ? *provider : std::string{}, *model, *source}))
    {
        reject();
        return;
    }
    observation = Structured_model_observation{provider ? *provider : std::string{}, *model, *source};
}

} // namespace logonomic::cli_addon
