#include "logonomic_cli_addon/context_state.h"

#include "logonomic_cli_addon/capability_ids.h"
#include "logonomic_cli_addon/limits.h"
#include "strict_json.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace logonomic::cli_addon {
namespace {

constexpr std::int64_t k_max_exact_json_integer = 9007199254740991;

bool read_identity(const Json_value& object, const char* name, std::string& result)
{
    const std::string* value = strict::string_member(object, name);
    if (value == nullptr || value->empty() ||
        value->size() > limits::k_max_lifecycle_identity_bytes ||
        std::any_of(value->begin(), value->end(), [](unsigned char byte) { return byte < 0x20; }))
    {
        return false;
    }
    result = *value;
    return true;
}

bool read_count(
    const Json_value& object, const char* name,
    std::optional<std::uint64_t>& result, std::int64_t minimum = 0)
{
    if (object.find(name) == nullptr) {
        return true;
    }
    const auto value = strict::integer_member(object, name, minimum, k_max_exact_json_integer);
    if (!value) {
        return false;
    }
    result = static_cast<std::uint64_t>(*value);
    return true;
}

bool read_flag(const Json_value& object, const char* name, bool& result)
{
    const Json_value* value = object.find(name);
    if (value == nullptr) {
        return true;
    }
    if (!value->is_boolean()) {
        return false;
    }
    result = value->as_boolean();
    return true;
}

bool read_update(const Json_value& object, Context_update& update)
{
    if (!object.is_object() || strict::unexpected_member(object, {
            "provider_session_id", "event_id", "used_fraction", "used_tokens", "context_window",
            "compaction_count", "compaction_count_revision", "usage_unavailable",
            "compaction_count_complete", "compacted"}))
    {
        return false;
    }
    if (!read_identity(object, "provider_session_id", update.provider_session_id) ||
        !read_identity(object, "event_id", update.event_id) ||
        !read_count(object, "used_tokens", update.used_tokens) ||
        !read_count(object, "context_window", update.context_window, 1) ||
        !read_count(object, "compaction_count", update.compaction_count) ||
        !read_count(object, "compaction_count_revision", update.compaction_count_revision) ||
        !read_flag(object, "usage_unavailable", update.usage_unavailable) ||
        !read_flag(object, "compaction_count_complete", update.compaction_count_complete) ||
        !read_flag(object, "compacted", update.compacted))
    {
        return false;
    }
    if (const Json_value* fraction = object.find("used_fraction")) {
        if (!fraction->is_number() || !std::isfinite(fraction->as_number()) ||
            fraction->as_number() < 0 || fraction->as_number() > 1)
        {
            return false;
        }
        update.used_fraction = fraction->as_number();
    }
    const bool has_usage = update.used_fraction || update.used_tokens || update.context_window;
    return
        !(update.usage_unavailable && has_usage) &&
        (update.compaction_count.has_value() == update.compaction_count_revision.has_value()) &&
        !(update.compacted && update.compaction_count) &&
        !(update.compaction_count_complete && !update.compaction_count) &&
        (has_usage || update.usage_unavailable || update.compacted || update.compaction_count);
}

} // namespace

void apply_context_update(Context_state& state, const Context_update& update)
{
    if (state.provider_session_id != update.provider_session_id) {
        state = {};
        state.provider_session_id = update.provider_session_id;
    }
    if (update.compacted) {
        if (std::find(state.compaction_event_ids.begin(), state.compaction_event_ids.end(), update.event_id) !=
            state.compaction_event_ids.end())
        {
            return;
        }
        state.compaction_event_ids.push_back(update.event_id);
        state.compaction_count = state.compaction_count.value_or(0) + 1;
    }
    if (update.compaction_count &&
        (!state.compaction_count_revision ||
         *update.compaction_count_revision > *state.compaction_count_revision))
    {
        // A newer partial history can prove fewer compactions without erasing
        // earlier knowledge. Only its evidence order decides completeness.
        const auto retained_count = state.compaction_count.value_or(0);
        state.compaction_count = std::max(retained_count, *update.compaction_count);
        state.compaction_count_revision = update.compaction_count_revision;
        state.compaction_count_complete = update.compaction_count_complete &&
            *update.compaction_count >= retained_count;
    }
    if (update.compacted || update.usage_unavailable) {
        state.used_fraction.reset();
        state.used_tokens.reset();
        state.context_window.reset();
    }
    if (update.used_fraction || update.used_tokens || update.context_window) {
        state.used_tokens = update.used_tokens;
        state.context_window = update.context_window;
        state.used_fraction = update.used_fraction;
        if (!state.used_fraction && state.used_tokens && state.context_window) {
            state.used_fraction = std::min(
                1.0, static_cast<double>(*state.used_tokens) / static_cast<double>(*state.context_window));
        }
    }
}

void encode_context_state_request(Json_value& payload, std::uint32_t version)
{
    if (version != 0) {
        payload.set("context_state_version", Json_value(static_cast<std::uint64_t>(version)));
    }
}

void decode_context_state_request(
    const Json_value& payload,
    std::uint32_t& version,
    std::optional<Contract_failure>& failure)
{
    version = 0;
    failure.reset();
    if (payload.find("context_state_version") == nullptr) {
        return;
    }
    const auto value = strict::integer_member(
        payload, "context_state_version", k_context_state_capability_version, k_context_state_capability_version);
    if (!value) {
        failure = Contract_failure{reason::k_unsupported_version, "context_state_version is not supported"};
        return;
    }
    version = static_cast<std::uint32_t>(*value);
}

void encode_context_updates(
    Json_value& payload,
    const std::vector<Context_update>& updates,
    const std::optional<Context_failure>& failure)
{
    if (failure) {
        Json_value encoded = Json_value::object();
        if (!failure->provider_session_id.empty()) {
            encoded.set("provider_session_id", Json_value(failure->provider_session_id));
        }
        encoded.set("reason_code", Json_value(failure->failure.reason_code));
        encoded.set("detail", Json_value(failure->failure.detail));
        payload.set("context_failure", std::move(encoded));
        return;
    }
    if (updates.empty()) {
        return;
    }
    Json_value encoded = Json_value::array();
    for (const Context_update& update : updates) {
        Json_value item = Json_value::object();
        item.set("provider_session_id", Json_value(update.provider_session_id));
        item.set("event_id", Json_value(update.event_id));
        if (update.used_fraction) {
            item.set("used_fraction", Json_value(*update.used_fraction));
        }
        if (update.used_tokens) {
            item.set("used_tokens", Json_value(*update.used_tokens));
        }
        if (update.context_window) {
            item.set("context_window", Json_value(*update.context_window));
        }
        if (update.compaction_count) {
            item.set("compaction_count", Json_value(*update.compaction_count));
        }
        if (update.compaction_count_revision) {
            item.set("compaction_count_revision", Json_value(*update.compaction_count_revision));
        }
        if (update.usage_unavailable) {
            item.set("usage_unavailable", Json_value(true));
        }
        if (update.compaction_count_complete) {
            item.set("compaction_count_complete", Json_value(true));
        }
        if (update.compacted) {
            item.set("compacted", Json_value(true));
        }
        encoded.push_back(std::move(item));
    }
    payload.set("context_updates", std::move(encoded));
}

void decode_context_updates(
    const Json_value& payload,
    std::vector<Context_update>& updates,
    std::optional<Context_failure>& failure)
{
    updates.clear();
    failure.reset();
    const Json_value* encoded = payload.find("context_updates");
    const Json_value* encoded_failure = payload.find("context_failure");
    const auto reject = [&](std::string source = {}) {
        updates.clear();
        failure = Context_failure{std::move(source),
            {reason::k_observation_invalid, "context telemetry is malformed or exceeds its bounds"}};
    };
    if (encoded_failure != nullptr) {
        Context_failure reported;
        if (encoded != nullptr || !encoded_failure->is_object() ||
            strict::unexpected_member(*encoded_failure, {"provider_session_id", "reason_code", "detail"}))
        {
            reject();
            return;
        }
        const auto* reason_code = strict::string_member(*encoded_failure, "reason_code");
        const auto* detail = strict::string_member(*encoded_failure, "detail");
        if (!reason_code || !is_valid_reason_code(*reason_code) || !detail || detail->size() > 1024 ||
            (encoded_failure->find("provider_session_id") &&
                !read_identity(*encoded_failure, "provider_session_id", reported.provider_session_id)))
        {
            reject();
            return;
        }
        reported.failure = {*reason_code, *detail};
        failure = std::move(reported);
        return;
    }
    if (encoded == nullptr) {
        return;
    }
    if (!encoded->is_array() || encoded->as_array().size() > limits::k_max_observations_per_result) {
        reject();
        return;
    }
    // Retain failure scope only when every bounded item independently names
    // the same valid source. A current ledger identity cannot fill this gap.
    std::string source;
    for (const Json_value& item : encoded->as_array()) {
        std::string identity;
        if (!item.is_object() || !read_identity(item, "provider_session_id", identity) ||
            (!source.empty() && source != identity))
        {
            source.clear();
            break;
        }
        source = std::move(identity);
    }
    for (const Json_value& item : encoded->as_array()) {
        Context_update update;
        if (!read_update(item, update)) {
            reject(std::move(source));
            return;
        }
        updates.push_back(std::move(update));
    }
}

} // namespace logonomic::cli_addon
