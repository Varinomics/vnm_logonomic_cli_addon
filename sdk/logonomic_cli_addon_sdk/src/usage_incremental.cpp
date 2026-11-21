#include "logonomic_cli_addon/usage_incremental.h"
#include "logonomic_cli_addon/capability_ids.h"
#include "logonomic_cli_addon/limits.h"

#include "strict_json.h"

namespace logonomic::cli_addon {
namespace {

bool fail(Contract_failure* out_failure, const char* detail)
{
    if (out_failure) {
        *out_failure = {"usage_incremental_invalid", detail};
    }
    return false;
}

} // namespace

bool decode_usage_incremental_declaration(const Json_value& payload, Contract_failure* out_failure)
{
    if (!payload.is_object() || !payload.as_object().empty()) {
        return fail(out_failure, "usage-incremental declaration must be an empty object");
    }
    return true;
}

Json_value encode_usage_incremental_request(const Usage_scan_request& request)
{
    return encode_usage_scan_request(request, k_usage_scan_acceptance_version);
}

std::optional<Usage_scan_request> decode_usage_incremental_request(
    const Json_value& payload, Contract_failure* out_failure)
{
    auto request = decode_usage_scan_request(payload, out_failure, k_usage_scan_acceptance_version);
    if (!request) return std::nullopt;
    if (request->mode != "incremental" || !request->cache_selection ||
        !request->cache_selection->accepted)
    {
        fail(out_failure, "incremental updates require an accepted baseline");
        return std::nullopt;
    }
    return request;
}

Json_value encode_usage_incremental_result(const Usage_incremental_result& result)
{
    auto payload = Json_value::object();
    payload.set("baseline", encode_usage_cache_reference(result.baseline));
    payload.set("unchanged", Json_value(!result.changes));
    if (result.changes) {
        payload.set("changes", encode_usage_scan_result(*result.changes, k_usage_scan_acceptance_version));
    }
    if (result.account_context) {
        payload.set(k_account_context_member, encode_account_context(*result.account_context));
    }
    if (result.profile_storage) {
        payload.set(k_profile_storage_member, profile_storage_member(*result.profile_storage));
    }
    return payload;
}

std::optional<Usage_incremental_result> decode_usage_incremental_result(
    const Json_value& payload, Contract_failure* out_failure)
{
    const auto* baseline = payload.find("baseline");
    const auto* unchanged = payload.find("unchanged");
    const auto* changes = payload.find("changes");
    if (!payload.is_object() || serialize_json(payload).size() > limits::k_max_exchange_payload_bytes ||
        strict::unexpected_member(payload,
            {"baseline", "unchanged", "changes", k_account_context_member, k_profile_storage_member}) ||
        !baseline || !unchanged || !unchanged->is_boolean() || unchanged->as_boolean() == bool(changes))
    {
        fail(out_failure, "an incremental result must retain its baseline or supply changed observations");
        return std::nullopt;
    }
    const auto reference = decode_usage_cache_reference(*baseline, out_failure);
    if (!reference) return std::nullopt;
    Usage_incremental_result result{*reference, std::nullopt};
    const auto* storage = payload.find(k_profile_storage_member);
    result.profile_storage = storage ? decode_profile_storage_context(*storage) : std::nullopt;
    if (!result.profile_storage || result.profile_storage->version != k_profile_storage_version) {
        fail(out_failure, "incremental updates require a profile-storage acknowledgment");
        return std::nullopt;
    }
    if (const auto* account = payload.find(k_account_context_member)) {
        result.account_context = decode_account_context(*account, out_failure);
        if (!result.account_context) return std::nullopt;
    }
    if (changes) {
        result.changes = decode_usage_scan_result(*changes, out_failure, k_usage_scan_acceptance_version);
        if (!result.changes) return std::nullopt;
    }
    return result;
}

} // namespace logonomic::cli_addon
