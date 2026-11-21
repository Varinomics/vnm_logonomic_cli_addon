#include "logonomic_cli_addon/permission_snapshot.h"

#include "logonomic_cli_addon/capability_ids.h"
#include "logonomic_cli_addon/managed_configuration.h"
#include "strict_json.h"

namespace logonomic::cli_addon {
namespace {

void fail(Contract_failure* failure, const char* reason, const char* detail)
{
    if (failure) *failure = {reason, detail};
}

bool neutral_observation(const Lifecycle_observation& observation)
{
    return !observation.provider_session_id.empty() &&
        observation.session_transition == Session_transition::NONE &&
        observation.phase_transition == Phase_transition::NONE &&
        observation.phase_evidence == Evidence::NONE &&
        observation.outcome_transition == Outcome_transition::NONE &&
        observation.outcome_evidence == Evidence::NONE &&
        observation.attention_transition == Attention_transition::NONE &&
        observation.attention_evidence == Evidence::NONE &&
        observation.subagent_transition == Subagent_transition::NONE &&
        observation.provider_turn_id.empty() && observation.provider_event_id.empty() &&
        observation.provider_sequence.empty() && observation.attention_request_id.empty() &&
        observation.agent_id.empty() && observation.session_source.empty() &&
        observation.session_end_reason.empty() && observation.agent_label.empty() &&
        !observation.resume_record;
}

} // namespace

Json_value encode_permission_snapshot_declaration(const Permission_snapshot_declaration& declaration)
{
    auto payload = Json_value::object();
    payload.set("relative_path", Json_value(declaration.relative_path));
    payload.set("event_name", Json_value(declaration.event_name));
    return payload;
}

std::optional<Permission_snapshot_declaration> decode_permission_snapshot_declaration(
    const Json_value& payload, Contract_failure* out_failure)
{
    const auto* event_name = strict::string_member(payload, "event_name");
    if (!payload.is_object() || strict::unexpected_member(payload, {"relative_path", "event_name"}) ||
        !event_name || event_name->empty() || event_name->size() > limits::k_max_provider_event_name_bytes ||
        event_name->find('\0') != std::string::npos || !is_valid_utf8(*event_name) || *event_name == "NativeSignal")
    {
        fail(out_failure, reason::k_invalid_sidecar, "invalid permission snapshot declaration");
        return std::nullopt;
    }
    auto managed = payload;
    managed.erase("event_name");
    const auto declaration = decode_managed_configuration_declaration(
        managed, out_failure, k_managed_configuration_home_version);
    if (!declaration) return std::nullopt;
    return Permission_snapshot_declaration{declaration->relative_path, *event_name};
}

Json_value encode_permission_snapshot_classification_result(
    const Permission_snapshot_classification_result& result)
{
    auto payload = encode_classify_event_result(result.base);
    if (result.permission_snapshot) {
        const auto& update = *result.permission_snapshot;
        auto snapshot = Json_value::object();
        snapshot.set("version", Json_value(static_cast<std::int64_t>(update.version)));
        snapshot.set("observation_index", Json_value(static_cast<std::int64_t>(update.observation_index)));
        snapshot.set("pending", Json_value(update.pending));
        payload.set("permission_snapshot", std::move(snapshot));
    }
    return payload;
}

std::optional<Permission_snapshot_classification_result> decode_permission_snapshot_classification_result(
    const Json_value& payload, Contract_failure* out_failure, std::uint16_t helper_version)
{
    auto common = payload;
    common.erase("permission_snapshot");
    auto base = decode_classify_event_result(common, out_failure, helper_version);
    if (!base) return std::nullopt;
    if (base->transition || base->lifecycle_degradation || base->subagent_evidence ||
        payload.find("context_updates") || payload.find("context_failure"))
    {
        fail(out_failure, reason::k_classification_invalid, "permission snapshots carry no lifecycle or context updates");
        return std::nullopt;
    }
    Permission_snapshot_classification_result result{std::move(*base), std::nullopt};
    const auto* snapshot = payload.find("permission_snapshot");
    if (!snapshot && result.base.classification == Classification::IGNORED) return result;
    if (!snapshot || !snapshot->is_object() ||
        strict::unexpected_member(*snapshot, {"version", "observation_index", "pending"}) ||
        !strict::integer_member(*snapshot, "version", k_permission_snapshot_version, k_permission_snapshot_version) ||
        !strict::integer_member(*snapshot, "observation_index", 0, 0) ||
        !strict::member_of_type(*snapshot, "pending", Json_type::BOOLEAN) ||
        result.base.classification != Classification::EVENT || result.base.observations.size() != 1 ||
        !neutral_observation(result.base.observations.front()))
    {
        fail(out_failure, reason::k_classification_invalid, "permission snapshot requires one neutral session observation");
        return std::nullopt;
    }
    result.permission_snapshot = Permission_snapshot_update{1, 0, snapshot->find("pending")->as_boolean()};
    return result;
}

} // namespace logonomic::cli_addon
