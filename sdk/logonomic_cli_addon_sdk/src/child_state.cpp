#include "logonomic_cli_addon/child_state.h"

#include "logonomic_cli_addon/capability_ids.h"
#include "logonomic_cli_addon/limits.h"
#include "strict_json.h"

#include <charconv>
#include <cmath>
#include <limits>
#include <system_error>
#include <utility>

namespace logonomic::cli_addon {
namespace {

constexpr strict::Name_entry<Child_status> k_status_names[] = {
    {Child_status::RUNNING, "running"},
    {Child_status::SUCCEEDED, "succeeded"},
    {Child_status::FAILED, "failed"},
    {Child_status::INTERRUPTED, "interrupted"},
    {Child_status::UNKNOWN, "unknown"},
};

bool bounded_text(std::string_view value, std::size_t maximum)
{
    return !value.empty() && value.size() <= maximum && is_valid_utf8(value) &&
        strict::has_no_control_characters(value);
}

bool bounded_description(std::string_view value)
{
    if (value.size() > 512 || !is_valid_utf8(value)) return false;
    for (const unsigned char byte : value) {
        if ((byte < 0x20 && byte != '\n' && byte != '\t') || byte == 0x7f) return false;
    }
    return true;
}

bool read_scope(const Json_value& value, std::string& id, std::string& nonce, std::uint64_t& order)
{
    const auto* session = strict::string_member(value, "provider_session_id");
    const auto* transition = strict::string_member(value, "transition_nonce");
    const auto* sequence = strict::string_member(value, "sequence");
    if (!session || !transition || !sequence || sequence->empty() || sequence->size() > 20 ||
        (sequence->size() > 1 && sequence->front() == '0') ||
        sequence->find_first_not_of("0123456789") != std::string::npos)
    {
        return false;
    }
    const auto parsed = std::from_chars(sequence->data(), sequence->data() + sequence->size(), order);
    if (parsed.ec != std::errc{} || parsed.ptr != sequence->data() + sequence->size()) return false;
    id = *session;
    nonce = *transition;
    return bounded_text(id, limits::k_max_lifecycle_identity_bytes) && is_valid_transition_nonce_hex(nonce);
}

Json_value encode_scope(const std::string& id, const std::string& nonce, std::uint64_t sequence)
{
    Json_value result = Json_value::object();
    result.set("provider_session_id", Json_value(id));
    result.set("transition_nonce", Json_value(nonce));
    result.set("sequence", Json_value(std::to_string(sequence)));
    return result;
}

bool read_update(const Json_value& value, Child_update& update)
{
    if (!value.is_object() || strict::unexpected_member(value, {
            "provider_session_id", "transition_nonce", "sequence", "agent_id", "parent_agent_id",
            "depth", "state", "description", "current_tool", "progress", "source"}) ||
        !read_scope(value, update.provider_session_id, update.transition_nonce, update.sequence))
    {
        return false;
    }
    const auto* id = strict::string_member(value, "agent_id");
    const auto* parent = strict::string_member(value, "parent_agent_id");
    const auto* state = strict::string_member(value, "state");
    const auto* source = strict::string_member(value, "source");
    if (!id || !parent || !state || !source) return false;
    const auto status = strict::value_of(k_status_names, *state);
    if (!status) return false;
    update.agent_id = *id;
    update.parent_agent_id = *parent;
    update.status = *status;
    update.source = *source;
    if (value.find("depth")) {
        const auto depth = strict::integer_member(value, "depth", 1, std::numeric_limits<std::uint32_t>::max());
        if (!depth) return false;
        update.depth = static_cast<std::uint32_t>(*depth);
    }
    for (const auto& [name, target] : {
             std::pair{"description", &update.description}, std::pair{"current_tool", &update.current_tool}})
    {
        if (const auto* field = value.find(name)) {
            if (!field->is_string()) return false;
            *target = field->as_string();
        }
    }
    if (const auto* progress = value.find("progress")) {
        if (!progress->is_number()) return false;
        update.progress = progress->as_number();
    }
    return is_valid_child_update(update);
}

} // namespace

const char* child_status_name(Child_status status)
{
    return strict::name_of(k_status_names, status);
}

bool is_valid_child_update(const Child_update& update)
{
    return bounded_text(update.provider_session_id, limits::k_max_lifecycle_identity_bytes) &&
        is_valid_transition_nonce_hex(update.transition_nonce) &&
        bounded_text(update.agent_id, limits::k_max_lifecycle_identity_bytes) &&
        (update.parent_agent_id.empty() || bounded_text(update.parent_agent_id, limits::k_max_lifecycle_identity_bytes)) &&
        update.parent_agent_id != update.agent_id &&
        (!update.depth || (*update.depth != 0 && (update.parent_agent_id.empty() ? *update.depth == 1 : *update.depth > 1))) &&
        (update.status == Child_status::RUNNING || update.status == Child_status::SUCCEEDED ||
         update.status == Child_status::FAILED || update.status == Child_status::INTERRUPTED ||
         update.status == Child_status::UNKNOWN) &&
        (!update.description || bounded_description(*update.description)) &&
        (!update.current_tool || bounded_text(*update.current_tool, 128)) &&
        (!update.progress || (std::isfinite(*update.progress) && *update.progress >= 0 && *update.progress <= 1)) &&
        bounded_text(update.source, 128);
}

bool is_valid_child_loss(const Child_loss& loss)
{
    return bounded_text(loss.provider_session_id, limits::k_max_lifecycle_identity_bytes) &&
        is_valid_transition_nonce_hex(loss.transition_nonce) &&
        is_valid_reason_code(loss.reason_code) && bounded_text(loss.source, 128);
}

void encode_child_state_request(Json_value& payload, std::uint32_t version)
{
    if (version != 0) {
        Json_value envelope = Json_value::object();
        envelope.set("version", Json_value(static_cast<std::uint64_t>(version)));
        payload.set("child_state", std::move(envelope));
    }
}

void decode_child_state_request(
    const Json_value& payload, std::uint32_t& version, std::optional<Contract_failure>& failure)
{
    version = 0;
    failure.reset();
    const auto* envelope = payload.find("child_state");
    if (!envelope) return;
    if (!envelope->is_object() || strict::unexpected_member(*envelope, {"version"}) ||
        !strict::integer_member(*envelope, "version", k_child_state_version, k_child_state_version))
    {
        failure = Contract_failure{reason::k_unsupported_version,
            "child_state request is malformed or its version is unsupported"};
        return;
    }
    version = k_child_state_version;
}

void encode_child_updates(
    Json_value& payload, const std::vector<Child_update>& updates,
    const std::optional<Child_loss>& loss, const std::optional<Contract_failure>& failure)
{
    if (updates.empty() && !loss && !failure) return;
    Json_value envelope = Json_value::object();
    envelope.set("version", Json_value(k_child_state_version));
    if (failure) {
        Json_value encoded = Json_value::object();
        encoded.set("reason_code", Json_value(failure->reason_code));
        encoded.set("detail", Json_value(failure->detail));
        envelope.set("failure", std::move(encoded));
    }
    else if (loss) {
        auto encoded = encode_scope(loss->provider_session_id, loss->transition_nonce, loss->sequence);
        encoded.set("reason_code", Json_value(loss->reason_code));
        encoded.set("source", Json_value(loss->source));
        envelope.set("loss", std::move(encoded));
    }
    else {
        Json_value encoded = Json_value::array();
        for (const auto& update : updates) {
            auto item = encode_scope(update.provider_session_id, update.transition_nonce, update.sequence);
            item.set("agent_id", Json_value(update.agent_id));
            item.set("parent_agent_id", Json_value(update.parent_agent_id));
            item.set("state", Json_value(child_status_name(update.status)));
            item.set("source", Json_value(update.source));
            if (update.depth) item.set("depth", Json_value(static_cast<std::uint64_t>(*update.depth)));
            if (update.description) item.set("description", Json_value(*update.description));
            if (update.current_tool) item.set("current_tool", Json_value(*update.current_tool));
            if (update.progress) item.set("progress", Json_value(*update.progress));
            encoded.push_back(std::move(item));
        }
        envelope.set("updates", std::move(encoded));
    }
    payload.set("child_state", std::move(envelope));
}

void decode_child_updates(
    const Json_value& payload, std::vector<Child_update>& updates,
    std::optional<Child_loss>& loss, std::optional<Contract_failure>& failure)
{
    updates.clear();
    loss.reset();
    failure.reset();
    const auto* envelope = payload.find("child_state");
    if (!envelope) return;
    const auto reject = [&] {
        updates.clear();
        loss.reset();
        failure = Contract_failure{reason::k_observation_invalid,
            "child_state result is malformed or exceeds its bounds"};
    };
    if (!envelope->is_object() || strict::unexpected_member(*envelope, {"version", "updates", "loss", "failure"}) ||
        !strict::integer_member(*envelope, "version", k_child_state_version, k_child_state_version))
    {
        reject();
        return;
    }
    const auto* encoded = envelope->find("updates");
    const auto* lost = envelope->find("loss");
    const auto* reported = envelope->find("failure");
    if (static_cast<int>(encoded != nullptr) + static_cast<int>(lost != nullptr) +
        static_cast<int>(reported != nullptr) != 1)
    {
        reject();
        return;
    }
    if (reported) {
        if (!reported->is_object() || strict::unexpected_member(*reported, {"reason_code", "detail"})) {
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
    if (lost) {
        Child_loss observed;
        if (!lost->is_object() || strict::unexpected_member(*lost, {
                "provider_session_id", "transition_nonce", "sequence", "reason_code", "source"}) ||
            !read_scope(*lost, observed.provider_session_id, observed.transition_nonce, observed.sequence))
        {
            reject();
            return;
        }
        const auto* reason_code = strict::string_member(*lost, "reason_code");
        const auto* source = strict::string_member(*lost, "source");
        if (!reason_code || !source) {
            reject();
            return;
        }
        observed.reason_code = *reason_code;
        observed.source = *source;
        if (!is_valid_child_loss(observed)) {
            reject();
            return;
        }
        loss = std::move(observed);
        return;
    }
    if (!encoded->is_array() || encoded->as_array().empty() ||
        encoded->as_array().size() > limits::k_max_observations_per_result)
    {
        reject();
        return;
    }
    for (const auto& item : encoded->as_array()) {
        Child_update update;
        if (!read_update(item, update) || (!updates.empty() &&
            (update.provider_session_id != updates.front().provider_session_id ||
             update.transition_nonce != updates.front().transition_nonce ||
             update.sequence <= updates.back().sequence)))
        {
            reject();
            return;
        }
        updates.push_back(std::move(update));
    }
}

} // namespace logonomic::cli_addon
