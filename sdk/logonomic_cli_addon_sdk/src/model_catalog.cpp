#include "logonomic_cli_addon/model_catalog.h"

#include "logonomic_cli_addon/capability_ids.h"
#include "logonomic_cli_addon/limits.h"
#include "strict_json.h"

#include <charconv>
#include <limits>
#include <set>
#include <system_error>
#include <utility>

namespace logonomic::cli_addon {
namespace {

constexpr strict::Name_entry<Model_catalog_status> k_status_names[] = {
    {Model_catalog_status::UNKNOWN, "unknown"},
    {Model_catalog_status::AVAILABLE, "available"},
    {Model_catalog_status::UNAVAILABLE, "unavailable"},
};

bool bounded_text(std::string_view value, std::size_t maximum)
{
    return !value.empty() && value.size() <= maximum && is_valid_utf8(value) &&
        strict::has_no_control_characters(value);
}

Json_value encode_snapshot(const Model_catalog_update& update)
{
    Json_value result = Json_value::object();
    result.set("provider_session_id", Json_value(update.provider_session_id));
    result.set("transition_nonce", Json_value(update.transition_nonce));
    result.set("sequence", Json_value(std::to_string(update.sequence)));
    result.set("state", Json_value(model_catalog_status_name(update.status)));
    result.set("source", Json_value(update.source));
    result.set("total_count", Json_value(static_cast<std::uint64_t>(update.total_count)));
    result.set("truncated", Json_value(update.truncated));
    Json_value models = Json_value::array();
    for (const auto& model : update.models) {
        Json_value item = Json_value::object();
        item.set("provider", Json_value(model.provider));
        item.set("model", Json_value(model.model));
        if (model.display_name) item.set("display_name", Json_value(*model.display_name));
        if (model.reasoning) item.set("reasoning", Json_value(*model.reasoning));
        models.push_back(std::move(item));
    }
    result.set("models", std::move(models));
    return result;
}

bool read_snapshot(const Json_value& value, Model_catalog_update& update)
{
    if (!value.is_object() || strict::unexpected_member(value, {
            "provider_session_id", "transition_nonce", "sequence", "state", "models", "total_count", "truncated", "source"}) ||
        serialize_json(value).size() > limits::k_max_model_catalog_snapshot_bytes)
    {
        return false;
    }
    const auto* id = strict::string_member(value, "provider_session_id");
    const auto* nonce = strict::string_member(value, "transition_nonce");
    const auto* sequence = strict::string_member(value, "sequence");
    const auto* state = strict::string_member(value, "state");
    const auto* source = strict::string_member(value, "source");
    const auto* models = value.find("models");
    const auto* truncated = value.find("truncated");
    const auto count = strict::integer_member(value, "total_count", 0, std::numeric_limits<std::uint32_t>::max());
    if (!id || !nonce || !sequence || !state || !source || !count || !models || !models->is_array() ||
        models->as_array().size() > limits::k_max_model_catalog_entries || !truncated || !truncated->is_boolean() ||
        sequence->empty() || sequence->size() > 20 ||
        (sequence->size() > 1 && sequence->front() == '0') ||
        sequence->find_first_not_of("0123456789") != std::string::npos)
    {
        return false;
    }
    const auto status = strict::value_of(k_status_names, *state);
    const auto parsed = std::from_chars(sequence->data(), sequence->data() + sequence->size(), update.sequence);
    if (!status || parsed.ec != std::errc{} || parsed.ptr != sequence->data() + sequence->size()) return false;
    update.provider_session_id = *id;
    update.transition_nonce = *nonce;
    update.status = *status;
    update.source = *source;
    update.total_count = static_cast<std::uint32_t>(*count);
    update.truncated = truncated->as_boolean();
    for (const auto& item : models->as_array()) {
        if (!item.is_object() || strict::unexpected_member(item, {"provider", "model", "display_name", "reasoning"})) return false;
        const auto* provider = strict::string_member(item, "provider");
        const auto* model = strict::string_member(item, "model");
        const auto* name = item.find("display_name");
        const auto* reasoning = item.find("reasoning");
        if (!provider || !model || (name && !name->is_string()) || (reasoning && !reasoning->is_boolean())) return false;
        Model_catalog_entry entry{*provider, *model, std::nullopt, std::nullopt};
        if (name) entry.display_name = name->as_string();
        if (reasoning) entry.reasoning = reasoning->as_boolean();
        update.models.push_back(std::move(entry));
    }
    return is_valid_model_catalog_update(update);
}

} // namespace

const char* model_catalog_status_name(Model_catalog_status status)
{
    return strict::name_of(k_status_names, status);
}

bool is_valid_model_catalog_update(const Model_catalog_update& update)
{
    if (!bounded_text(update.provider_session_id, limits::k_max_lifecycle_identity_bytes) ||
        !is_valid_transition_nonce_hex(update.transition_nonce) || !bounded_text(update.source, 128) ||
        update.models.size() > limits::k_max_model_catalog_entries ||
        update.total_count < update.models.size() || update.truncated != (update.total_count > update.models.size()))
    {
        return false;
    }
    if (update.status != Model_catalog_status::AVAILABLE) {
        return (update.status == Model_catalog_status::UNKNOWN || update.status == Model_catalog_status::UNAVAILABLE) &&
            update.models.empty() && update.total_count == 0 && !update.truncated;
    }
    std::set<std::pair<std::string, std::string>> identities;
    for (const auto& model : update.models) {
        if (!bounded_text(model.provider, 512) || !bounded_text(model.model, 512) ||
            (model.display_name && !bounded_text(*model.display_name, 256)) ||
            !identities.emplace(model.provider, model.model).second)
        {
            return false;
        }
    }
    return serialize_json(encode_snapshot(update)).size() <= limits::k_max_model_catalog_snapshot_bytes;
}

bool apply_model_catalog_update(Model_catalog_state& state, const Model_catalog_update& update)
{
    if (!is_valid_model_catalog_update(update) || state.provider_session_id != update.provider_session_id ||
        state.transition_nonce != update.transition_nonce || (state.sequence && update.sequence <= *state.sequence))
    {
        return false;
    }
    state.sequence = update.sequence;
    state.status = update.status;
    state.models = update.models;
    state.total_count = update.total_count;
    state.truncated = update.truncated;
    state.source = update.source;
    return true;
}

void invalidate_model_catalog(Model_catalog_state& state)
{
    state.status = Model_catalog_status::UNKNOWN;
    state.models.clear();
    state.total_count = 0;
    state.truncated = false;
    state.source.clear();
}

void encode_model_catalog_request(Json_value& payload, std::uint32_t version)
{
    if (version != 0) {
        Json_value envelope = Json_value::object();
        envelope.set("version", Json_value(static_cast<std::uint64_t>(version)));
        payload.set("model_catalog", std::move(envelope));
    }
}

void decode_model_catalog_request(
    const Json_value& payload, std::uint32_t& version, std::optional<Contract_failure>& failure)
{
    version = 0;
    failure.reset();
    const auto* envelope = payload.find("model_catalog");
    if (!envelope) return;
    if (!envelope->is_object() || strict::unexpected_member(*envelope, {"version"}) ||
        !strict::integer_member(*envelope, "version", k_model_catalog_version, k_model_catalog_version))
    {
        failure = Contract_failure{reason::k_unsupported_version,
            "model_catalog request is malformed or its version is unsupported"};
        return;
    }
    version = k_model_catalog_version;
}

void encode_model_catalog_update(
    Json_value& payload, const std::optional<Model_catalog_update>& update,
    const std::optional<Contract_failure>& failure)
{
    if (!update && !failure) return;
    Json_value envelope = Json_value::object();
    envelope.set("version", Json_value(k_model_catalog_version));
    if (failure) {
        Json_value encoded = Json_value::object();
        encoded.set("reason_code", Json_value(failure->reason_code));
        encoded.set("detail", Json_value(failure->detail));
        envelope.set("failure", std::move(encoded));
    }
    else {
        envelope.set("snapshot", encode_snapshot(*update));
    }
    payload.set("model_catalog", std::move(envelope));
}

void decode_model_catalog_update(
    const Json_value& payload, std::optional<Model_catalog_update>& update,
    std::optional<Contract_failure>& failure)
{
    update.reset();
    failure.reset();
    const auto* envelope = payload.find("model_catalog");
    if (!envelope) return;
    const auto reject = [&] {
        failure = Contract_failure{reason::k_observation_invalid,
            "model_catalog result is malformed or exceeds its bounds"};
    };
    if (!envelope->is_object() || strict::unexpected_member(*envelope, {"version", "snapshot", "failure"}) ||
        !strict::integer_member(*envelope, "version", k_model_catalog_version, k_model_catalog_version))
    {
        reject();
        return;
    }
    const auto* snapshot = envelope->find("snapshot");
    if (const auto* reported = envelope->find("failure")) {
        if (snapshot || !reported->is_object() || strict::unexpected_member(*reported, {"reason_code", "detail"})) {
            reject();
            return;
        }
        const auto* reason_code = strict::string_member(*reported, "reason_code");
        const auto* detail = strict::string_member(*reported, "detail");
        if (!reason_code || !is_valid_reason_code(*reason_code) || !detail || detail->size() > 1024 ||
            !is_valid_utf8(*detail) || !strict::has_no_control_characters(*detail))
        {
            reject();
            return;
        }
        failure = Contract_failure{*reason_code, *detail};
        return;
    }
    Model_catalog_update decoded;
    if (!snapshot || !read_snapshot(*snapshot, decoded)) {
        reject();
        return;
    }
    update = std::move(decoded);
}

} // namespace logonomic::cli_addon
