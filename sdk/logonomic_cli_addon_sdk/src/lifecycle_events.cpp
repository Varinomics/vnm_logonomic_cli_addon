#include "logonomic_cli_addon/lifecycle_events.h"
#include "logonomic_cli_addon/limits.h"
#include "logonomic_cli_addon/provider_event_diagnostic.h"
#include "strict_json.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <utility>

namespace logonomic::cli_addon {

namespace {

using strict::Name_entry;
using strict::name_of;
using strict::value_of;

bool report(Contract_failure* out_failure, const char* reason_code, std::string detail)
{
    if (out_failure != nullptr) {
        out_failure->reason_code = reason_code;
        out_failure->detail      = std::move(detail);
    }
    return false;
}

bool report_event(
    Contract_failure* out_failure,
    const char* reason_code,
    std::string event,
    std::string field,
    Provider_event_problem problem,
    const Json_value* actual,
    std::optional<std::uint64_t> limit = {},
    std::string rule = {})
{
    return report(out_failure, reason_code, encode_provider_event_diagnostic(
        make_provider_event_diagnostic(
            std::move(event), std::move(field), problem, actual, limit, std::move(rule))));
}

bool has_nul(std::string_view text) { return text.find('\0') != std::string_view::npos; }

constexpr Name_entry<Feature_support> k_feature_support_names[] = {
    {Feature_support::SUPPORTED,   "supported"},
    {Feature_support::UNSUPPORTED, "unsupported"},
};

constexpr Name_entry<Observation_mode> k_observation_mode_names[] = {
    {Observation_mode::EVENTS,     "events"},
    {Observation_mode::CONTINUOUS, "continuous"},
};

constexpr Name_entry<Session_transition> k_session_transition_names[] = {
    {Session_transition::NONE,    "none"},
    {Session_transition::STARTED, "started"},
    {Session_transition::ENDED,   "ended"},
};

constexpr Name_entry<Phase_transition> k_phase_transition_names[] = {
    {Phase_transition::NONE,           "none"},
    {Phase_transition::WORKING,        "working"},
    {Phase_transition::AWAITING_INPUT, "awaiting_input"},
};

constexpr Name_entry<Evidence> k_evidence_names[] = {
    {Evidence::NONE,      "none"},
    {Evidence::INFERRED,  "inferred"},
    {Evidence::CONFIRMED, "confirmed"},
};

constexpr Name_entry<Outcome_transition> k_outcome_transition_names[] = {
    {Outcome_transition::NONE,        "none"},
    {Outcome_transition::SUCCEEDED,   "succeeded"},
    {Outcome_transition::FAILED,      "failed"},
    {Outcome_transition::INTERRUPTED, "interrupted"},
};

constexpr Name_entry<Attention_transition> k_attention_transition_names[] = {
    {Attention_transition::NONE,      "none"},
    {Attention_transition::REQUESTED, "requested"},
    {Attention_transition::RESOLVED,  "resolved"},
};

constexpr Name_entry<Subagent_transition> k_subagent_transition_names[] = {
    {Subagent_transition::NONE,    "none"},
    {Subagent_transition::STARTED, "started"},
    {Subagent_transition::STOPPED, "stopped"},
};

// The eight feature fields, in declaration order.
struct Feature_field
{
    const char* name;
    Feature_support Lifecycle_features::* member;
};

constexpr Feature_field k_feature_fields[] = {
    {"root_turn_start",         &Lifecycle_features::root_turn_start},
    {"root_turn_completion",    &Lifecycle_features::root_turn_completion},
    {"confirmed_phase_status",  &Lifecycle_features::confirmed_phase_status},
    {"attention",               &Lifecycle_features::attention},
    {"paired_attention",        &Lifecycle_features::paired_attention},
    {"subagents",               &Lifecycle_features::subagents},
    {"cooperative_description", &Lifecycle_features::cooperative_description},
    {"blockable_completion",    &Lifecycle_features::blockable_completion},
};

constexpr const char* k_member_profile_id              = "profile_id";
constexpr const char* k_member_features                = "features";
constexpr const char* k_member_turn_origin_correlation = "turn_origin_correlation";
constexpr const char* k_member_observation             = "observation";
constexpr const char* k_member_resume_record           = "resume_record";

// A required enumeration member of a sidecar: any defect is `invalid_sidecar`.
template <typename Enum, std::size_t N>
bool read_sidecar_enum(
    const Json_value&         object,
    std::string_view          name,
    const Name_entry<Enum>  (&table)[N],
    Enum&                     out_value,
    Contract_failure*         out_failure,
    const std::string&        context)
{
    const std::string* text = strict::string_member(object, name);
    const std::optional<Enum> value = text == nullptr ? std::nullopt : value_of(table, *text);
    if (!value.has_value()) {
        return report(out_failure, reason::k_invalid_sidecar,
            context + std::string(name) + " is missing or outside its vocabulary");
    }
    out_value = *value;
    return true;
}

// A required enumeration member of an observation: absent, mistyped or
// outside the vocabulary is `frame_violation`.
template <typename Enum, std::size_t N>
bool read_observation_enum(
    const Json_value&         object,
    std::string_view          name,
    const Name_entry<Enum>  (&table)[N],
    Enum&                     out_value,
    Contract_failure*         out_failure)
{
    const std::string* text = strict::string_member(object, name);
    const std::optional<Enum> value = text == nullptr ? std::nullopt : value_of(table, *text);
    if (!value.has_value()) {
        const auto* actual = object.find(name);
        const auto problem = !actual ? Provider_event_problem::MISSING
            : !text ? Provider_event_problem::WRONG_TYPE : Provider_event_problem::INVALID_VALUE;
        return report_event(out_failure, reason::k_frame_violation, "lifecycle_observation",
            "observation." + std::string(name), problem, actual, {}, "known_enum_value");
    }
    out_value = *value;
    return true;
}

// A required string member of an observation under an identity or label
// bound; empty is allowed unless `required`.
bool read_observation_string(
    const Json_value& object,
    std::string_view  name,
    std::size_t       max_bytes,
    bool              required,
    std::string&      out_value,
    Contract_failure* out_failure)
{
    const std::string* text = strict::string_member(object, name);
    if (text == nullptr || (required && text->empty())) {
        const auto* actual = object.find(name);
        const auto problem = !actual ? Provider_event_problem::MISSING
            : !text ? Provider_event_problem::WRONG_TYPE : Provider_event_problem::EMPTY;
        return report_event(out_failure, reason::k_frame_violation, "lifecycle_observation",
            "observation." + std::string(name), problem, actual, {},
            required ? "nonempty_string" : "string");
    }
    if (text->size() > max_bytes || has_nul(*text)) {
        const auto problem = text->size() > max_bytes
            ? Provider_event_problem::TOO_LARGE : Provider_event_problem::CONTROL_CHARACTER;
        return report_event(out_failure, reason::k_payload_too_large, "lifecycle_observation",
            "observation." + std::string(name), problem, object.find(name), max_bytes, "bounded_nul_free_string");
    }
    out_value = *text;
    return true;
}

std::optional<std::size_t> agent_label_utf16_units(std::string_view text)
{
    std::size_t units = 0;
    for (std::size_t index = 0; index < text.size();) {
        const auto first = static_cast<unsigned char>(text[index++]);
        std::uint32_t codepoint = first;
        std::uint32_t minimum = 0;
        std::size_t remaining = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            codepoint = first & 0x1f;
            minimum = 0x80;
            remaining = 1;
        }
        else if (first >= 0xe0 && first <= 0xef) {
            codepoint = first & 0x0f;
            minimum = 0x800;
            remaining = 2;
        }
        else if (first >= 0xf0 && first <= 0xf4) {
            codepoint = first & 0x07;
            minimum = 0x10000;
            remaining = 3;
        }
        else if (first >= 0x80) {
            return std::nullopt;
        }
        if (remaining > text.size() - index) {
            return std::nullopt;
        }
        for (std::size_t offset = 0; offset < remaining; ++offset) {
            const auto byte = static_cast<unsigned char>(text[index++]);
            if ((byte & 0xc0) != 0x80) {
                return std::nullopt;
            }
            codepoint = (codepoint << 6) | (byte & 0x3f);
        }
        if (codepoint < minimum || codepoint > 0x10ffff ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff))
        {
            return std::nullopt;
        }
        units += codepoint >= 0x10000 ? 2 : 1;
    }
    return units;
}

bool is_canonical_sequence(std::string_view text)
{
    if (text.empty()) {
        return true;
    }
    if (text.size() > 1 && text.front() == '0') {
        return false;
    }
    if (!std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        return false;
    }
    std::uint64_t value = 0;
    const std::from_chars_result parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc() && parsed.ptr == text.data() + text.size();
}

bool read_cursor(const Json_value& object, std::string& out_cursor, Contract_failure* out_failure, const char* context)
{
    const std::string* cursor = strict::string_member(object, "resume_cursor");
    if (cursor == nullptr) {
        const auto* actual = object.find("resume_cursor");
        return report_event(out_failure, reason::k_frame_violation, context, "resume_cursor",
            actual ? Provider_event_problem::WRONG_TYPE : Provider_event_problem::MISSING,
            actual, {}, "string");
    }
    if (cursor->size() > limits::k_max_observe_cursor_bytes || has_nul(*cursor)) {
        const auto problem = cursor->size() > limits::k_max_observe_cursor_bytes
            ? Provider_event_problem::TOO_LARGE : Provider_event_problem::CONTROL_CHARACTER;
        return report_event(out_failure, reason::k_payload_too_large, context, "resume_cursor",
            problem, object.find("resume_cursor"), limits::k_max_observe_cursor_bytes, "bounded_nul_free_string");
    }
    out_cursor = *cursor;
    return true;
}

void encode_correlation(Json_value& payload, const std::optional<Session_correlation>& correlation)
{
    if (correlation) {
        Json_value value = Json_value::object();
        value.set("version", Json_value(std::int64_t{1}));
        value.set("transition_nonce", Json_value(correlation->transition_nonce));
        payload.set("correlation", std::move(value));
    }
}

bool read_correlation(
    const Json_value& payload,
    const char* event,
    std::optional<Session_correlation>& correlation,
    Contract_failure* out_failure)
{
    const Json_value* value = payload.find("correlation");
    if (value == nullptr) {
        return true;
    }
    if (!value->is_object()) {
        return report_event(out_failure, reason::k_frame_violation, event, "correlation",
            Provider_event_problem::WRONG_TYPE, value, {}, "object");
    }
    if (strict::unexpected_member(*value, {"version", "transition_nonce"})) {
        return report_event(out_failure, reason::k_unexpected_member, event, "correlation",
            Provider_event_problem::INVALID_SHAPE, value, {}, "closed_members");
    }
    const std::string* nonce = strict::string_member(*value, "transition_nonce");
    const auto version      = strict::integer_member(*value, "version", 1, 1);
    if (!version || nonce == nullptr || !is_valid_transition_nonce_hex(*nonce)) {
        const auto* field  = !version ? "version" : "transition_nonce";
        const auto* actual = value->find(field);
        const auto expected = !version ? Json_type::INTEGER : Json_type::STRING;
        const auto problem = !actual ? Provider_event_problem::MISSING
            : actual->type() != expected ? Provider_event_problem::WRONG_TYPE : Provider_event_problem::INVALID_VALUE;
        return report_event(out_failure, reason::k_frame_violation, event, "correlation." + std::string(field),
            problem, actual, {}, !version ? "version_1" : "transition_nonce_hex");
    }
    correlation = Session_correlation{*nonce};
    return true;
}

Json_value encode_string_array(const std::vector<std::string>& values)
{
    Json_value array = Json_value::array();
    for (const std::string& value : values) {
        array.push_back(Json_value(value));
    }
    return array;
}

} // namespace

// ---------------------------------------------------------------------------
// Names

const char* feature_support_name(Feature_support support)             { return name_of(k_feature_support_names, support); }
const char* observation_mode_name(Observation_mode mode)              { return name_of(k_observation_mode_names, mode); }
const char* session_transition_name(Session_transition transition)    { return name_of(k_session_transition_names, transition); }
const char* phase_transition_name(Phase_transition transition)        { return name_of(k_phase_transition_names, transition); }
const char* evidence_name(Evidence evidence)                          { return name_of(k_evidence_names, evidence); }
const char* outcome_transition_name(Outcome_transition transition)    { return name_of(k_outcome_transition_names, transition); }
const char* attention_transition_name(Attention_transition transition) { return name_of(k_attention_transition_names, transition); }
const char* subagent_transition_name(Subagent_transition transition)  { return name_of(k_subagent_transition_names, transition); }

std::optional<Feature_support> parse_feature_support(std::string_view name)   { return value_of(k_feature_support_names, name); }
std::optional<Observation_mode> parse_observation_mode(std::string_view name) { return value_of(k_observation_mode_names, name); }

// ---------------------------------------------------------------------------
// Declarations

bool is_valid_lifecycle_profile_id(std::string_view profile_id)
{
    if (profile_id.empty() || profile_id.size() > limits::k_max_lifecycle_profile_id_bytes) {
        return false;
    }
    return std::all_of(profile_id.begin(), profile_id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    });
}

std::optional<Lifecycle_declaration> decode_lifecycle_declaration(
    const Json_value& payload, Contract_failure* out_failure)
{
    if (!payload.is_object()) {
        report(out_failure, reason::k_invalid_sidecar, "payload is not an object");
        return std::nullopt;
    }
    const std::optional<std::string> unexpected = strict::unexpected_member(payload, {
        k_member_profile_id, k_member_features, k_member_turn_origin_correlation, k_member_observation,
    });
    if (unexpected.has_value()) {
        report(out_failure, reason::k_invalid_sidecar, "unexpected member " + *unexpected);
        return std::nullopt;
    }
    Lifecycle_declaration declaration;
    const std::string* profile_id = strict::string_member(payload, k_member_profile_id);
    if (profile_id == nullptr || !is_valid_lifecycle_profile_id(*profile_id)) {
        report(out_failure, reason::k_invalid_sidecar, "profile_id is missing or outside its grammar");
        return std::nullopt;
    }
    declaration.profile_id = *profile_id;

    const Json_value* features = strict::member_of_type(payload, k_member_features, Json_type::OBJECT);
    if (features == nullptr) {
        report(out_failure, reason::k_invalid_sidecar, "features is missing or not an object");
        return std::nullopt;
    }
    for (const Json_member& member : features->as_object()) {
        const bool known = std::any_of(
            std::begin(k_feature_fields), std::end(k_feature_fields),
            [&member](const Feature_field& field) { return member.name == field.name; });
        if (!known) {
            report(out_failure, reason::k_invalid_sidecar, "unexpected member features." + member.name);
            return std::nullopt;
        }
    }
    for (const Feature_field& field : k_feature_fields) {
        Feature_support& support = declaration.features.*field.member;
        if (!read_sidecar_enum(*features, field.name, k_feature_support_names, support, out_failure, "features.")) {
            return std::nullopt;
        }
    }
    if (!read_sidecar_enum(payload, k_member_turn_origin_correlation, k_feature_support_names,
            declaration.turn_origin_correlation, out_failure, "") ||
        !read_sidecar_enum(payload, k_member_observation, k_observation_mode_names,
            declaration.observation, out_failure, ""))
    {
        return std::nullopt;
    }
    return declaration;
}

Json_value encode_lifecycle_declaration(const Lifecycle_declaration& declaration)
{
    Json_value features = Json_value::object();
    for (const Feature_field& field : k_feature_fields) {
        features.set(field.name, Json_value(feature_support_name(declaration.features.*field.member)));
    }
    Json_value payload = Json_value::object();
    payload.set(k_member_profile_id,              Json_value(declaration.profile_id));
    payload.set(k_member_features,                std::move(features));
    payload.set(k_member_turn_origin_correlation,
        Json_value(feature_support_name(declaration.turn_origin_correlation)));
    payload.set(k_member_observation,             Json_value(observation_mode_name(declaration.observation)));
    return payload;
}

std::optional<Coordination_delivery_declaration> decode_coordination_delivery_declaration(
    const Json_value& payload, Contract_failure* out_failure)
{
    if (!payload.is_object()) {
        report(out_failure, reason::k_invalid_sidecar, "payload is not an object");
        return std::nullopt;
    }
    const std::optional<std::string> unexpected =
        strict::unexpected_member(payload, {"startup_instruction_delivery", "turn_context_delivery"});
    if (unexpected.has_value()) {
        report(out_failure, reason::k_invalid_sidecar, "unexpected member " + *unexpected);
        return std::nullopt;
    }
    Coordination_delivery_declaration declaration;
    if (!read_sidecar_enum(payload, "startup_instruction_delivery", k_feature_support_names,
            declaration.startup_instruction_delivery, out_failure, "") ||
        !read_sidecar_enum(payload, "turn_context_delivery", k_feature_support_names,
            declaration.turn_context_delivery, out_failure, ""))
    {
        return std::nullopt;
    }
    return declaration;
}

Json_value encode_coordination_delivery_declaration(const Coordination_delivery_declaration& declaration)
{
    Json_value payload = Json_value::object();
    payload.set("startup_instruction_delivery",
        Json_value(feature_support_name(declaration.startup_instruction_delivery)));
    payload.set("turn_context_delivery",        Json_value(feature_support_name(declaration.turn_context_delivery)));
    return payload;
}

// ---------------------------------------------------------------------------
// Observations

Json_value encode_lifecycle_observation(const Lifecycle_observation& observation)
{
    Json_value object = Json_value::object();
    object.set("provider_event_name",  Json_value(observation.provider_event_name));
    object.set("session_transition",   Json_value(session_transition_name(observation.session_transition)));
    object.set("phase_transition",     Json_value(phase_transition_name(observation.phase_transition)));
    object.set("phase_evidence",       Json_value(evidence_name(observation.phase_evidence)));
    object.set("outcome_transition",   Json_value(outcome_transition_name(observation.outcome_transition)));
    object.set("outcome_evidence",     Json_value(evidence_name(observation.outcome_evidence)));
    object.set("attention_transition", Json_value(attention_transition_name(observation.attention_transition)));
    object.set("attention_evidence",   Json_value(evidence_name(observation.attention_evidence)));
    object.set("subagent_transition",  Json_value(subagent_transition_name(observation.subagent_transition)));
    object.set("provider_session_id",  Json_value(observation.provider_session_id));
    object.set("session_source",       Json_value(observation.session_source));
    object.set("session_end_reason",   Json_value(observation.session_end_reason));
    object.set("provider_turn_id",     Json_value(observation.provider_turn_id));
    object.set("provider_event_id",    Json_value(observation.provider_event_id));
    object.set("provider_sequence",    Json_value(observation.provider_sequence));
    object.set("attention_request_id", Json_value(observation.attention_request_id));
    object.set("agent_id",             Json_value(observation.agent_id));
    object.set("agent_label",          Json_value(observation.agent_label));
    if (observation.resume_record.has_value()) {
        object.set(k_member_resume_record, *observation.resume_record);
    }
    return object;
}

std::optional<Lifecycle_observation> decode_lifecycle_observation(
    const Json_value& value, Contract_failure* out_failure, Lifecycle_observation_policy policy)
{
    if (!value.is_object()) {
        report_event(out_failure, reason::k_frame_violation, "lifecycle_observation", "observation",
            Provider_event_problem::WRONG_TYPE, &value, {}, "object");
        return std::nullopt;
    }
    const std::optional<std::string> unexpected = strict::unexpected_member(value, {
        "provider_event_name", "session_transition", "phase_transition", "phase_evidence",
        "outcome_transition", "outcome_evidence", "attention_transition", "attention_evidence",
        "subagent_transition", "provider_session_id", "session_source", "session_end_reason",
        "provider_turn_id", "provider_event_id", "provider_sequence", "attention_request_id",
        "agent_id", "agent_label", k_member_resume_record,
    });
    if (unexpected.has_value()) {
        report_event(out_failure, reason::k_unexpected_member, "lifecycle_observation", "observation",
            Provider_event_problem::INVALID_SHAPE, &value, {}, "closed_members");
        return std::nullopt;
    }

    const std::size_t identity = limits::k_max_lifecycle_identity_bytes;
    const std::size_t label    = limits::k_max_lifecycle_label_bytes;
    const std::size_t agent_label = policy == Lifecycle_observation_policy::CONTINUOUS ? 768 : label;
    Lifecycle_observation observation;
    if (!read_observation_string(value, "provider_event_name", identity, true, observation.provider_event_name, out_failure) ||
        !read_observation_enum(value, "session_transition",   k_session_transition_names,   observation.session_transition,   out_failure) ||
        !read_observation_enum(value, "phase_transition",     k_phase_transition_names,     observation.phase_transition,     out_failure) ||
        !read_observation_enum(value, "phase_evidence",       k_evidence_names,             observation.phase_evidence,       out_failure) ||
        !read_observation_enum(value, "outcome_transition",   k_outcome_transition_names,   observation.outcome_transition,   out_failure) ||
        !read_observation_enum(value, "outcome_evidence",     k_evidence_names,             observation.outcome_evidence,     out_failure) ||
        !read_observation_enum(value, "attention_transition", k_attention_transition_names, observation.attention_transition, out_failure) ||
        !read_observation_enum(value, "attention_evidence",   k_evidence_names,             observation.attention_evidence,   out_failure) ||
        !read_observation_enum(value, "subagent_transition",  k_subagent_transition_names,  observation.subagent_transition,  out_failure) ||
        !read_observation_string(value, "provider_session_id",  identity, false, observation.provider_session_id,  out_failure) ||
        !read_observation_string(value, "session_source",       label,    false, observation.session_source,       out_failure) ||
        !read_observation_string(value, "session_end_reason",   label,    false, observation.session_end_reason,   out_failure) ||
        !read_observation_string(value, "provider_turn_id",     identity, false, observation.provider_turn_id,     out_failure) ||
        !read_observation_string(value, "provider_event_id",    identity, false, observation.provider_event_id,    out_failure) ||
        !read_observation_string(value, "provider_sequence",    identity, false, observation.provider_sequence,    out_failure) ||
        !read_observation_string(value, "attention_request_id", identity, false, observation.attention_request_id, out_failure) ||
        !read_observation_string(value, "agent_id",             identity, false, observation.agent_id,             out_failure) ||
        !read_observation_string(value, "agent_label",          agent_label, false, observation.agent_label,       out_failure))
    {
        return std::nullopt;
    }
    if (policy == Lifecycle_observation_policy::CONTINUOUS) {
        const auto units = agent_label_utf16_units(observation.agent_label);
        if (!units || *units > 256) {
            auto diagnostic = make_provider_event_diagnostic(
                "lifecycle_observation", "observation.agent_label",
                units ? Provider_event_problem::TOO_LARGE : Provider_event_problem::INVALID_VALUE,
                value.find("agent_label"), units ? std::optional<std::uint64_t>{256} : std::nullopt,
                units ? "utf16_units" : "valid_utf8");
            if (units) {
                diagnostic.actual_size = *units;
            }
            report(out_failure, reason::k_payload_too_large, encode_provider_event_diagnostic(diagnostic));
            return std::nullopt;
        }
    }
    if (const Json_value* record = value.find(k_member_resume_record); record != nullptr) {
        if (!record->is_object()) {
            report_event(out_failure, reason::k_frame_violation, "lifecycle_observation", "observation.resume_record",
                Provider_event_problem::WRONG_TYPE, record, {}, "object");
            return std::nullopt;
        }
        const auto record_bytes = serialize_json_canonical(*record).size();
        if (record_bytes > limits::k_max_resume_record_bytes) {
            auto diagnostic = make_provider_event_diagnostic(
                "lifecycle_observation", "observation.resume_record", Provider_event_problem::TOO_LARGE,
                record, limits::k_max_resume_record_bytes, "canonical_json_bytes");
            diagnostic.actual_size = record_bytes;
            report(out_failure, reason::k_payload_too_large, encode_provider_event_diagnostic(diagnostic));
            return std::nullopt;
        }
        observation.resume_record = *record;
    }

    const bool phase_pair_valid =
        (observation.phase_transition == Phase_transition::NONE) == (observation.phase_evidence == Evidence::NONE);
    const bool outcome_pair_valid =
        (observation.outcome_transition == Outcome_transition::NONE) ==
        (observation.outcome_evidence == Evidence::NONE);
    const bool attention_pair_valid =
        (observation.attention_transition == Attention_transition::NONE) ==
        (observation.attention_evidence == Evidence::NONE);
    const bool outcome_identity_valid =
        observation.outcome_transition == Outcome_transition::NONE || !observation.provider_turn_id.empty();
    const bool session_identity_valid =
        observation.session_transition == Session_transition::NONE || !observation.provider_session_id.empty();
    const bool subagent_identity_valid =
        observation.subagent_transition == Subagent_transition::NONE || !observation.agent_id.empty();
    const bool anonymous_attention_valid =
        !observation.attention_request_id.empty() ||
        observation.attention_transition == Attention_transition::NONE ||
        (observation.attention_transition == Attention_transition::REQUESTED &&
         observation.attention_evidence == Evidence::INFERRED);
    const bool confirmed_attention_identity_valid =
        observation.attention_evidence != Evidence::CONFIRMED || !observation.attention_request_id.empty();
    const bool resume_record_placement_valid =
        !observation.resume_record.has_value() || observation.session_transition == Session_transition::STARTED;

    if (!phase_pair_valid || !outcome_pair_valid || !attention_pair_valid) {
        const auto* field = !phase_pair_valid ? "phase_evidence"
            : !outcome_pair_valid ? "outcome_evidence" : "attention_evidence";
        const auto* rule = !phase_pair_valid ? "phase_transition_evidence_pair"
            : !outcome_pair_valid ? "outcome_transition_evidence_pair" : "attention_transition_evidence_pair";
        report_event(out_failure, reason::k_observation_invalid, "lifecycle_observation",
            "observation." + std::string(field), Provider_event_problem::MISMATCH, value.find(field), {}, rule);
        return std::nullopt;
    }
    if (!outcome_identity_valid || !session_identity_valid || !subagent_identity_valid) {
        const auto* field = !outcome_identity_valid ? "provider_turn_id"
            : !session_identity_valid ? "provider_session_id" : "agent_id";
        const auto* rule = !outcome_identity_valid ? "outcome_requires_turn"
            : !session_identity_valid ? "session_transition_requires_session" : "subagent_transition_requires_agent";
        report_event(out_failure, reason::k_observation_invalid, "lifecycle_observation",
            "observation." + std::string(field), Provider_event_problem::EMPTY, value.find(field), {}, rule);
        return std::nullopt;
    }
    if (!is_canonical_sequence(observation.provider_sequence)) {
        report_event(out_failure, reason::k_observation_invalid, "lifecycle_observation", "observation.provider_sequence",
            Provider_event_problem::INVALID_VALUE, value.find("provider_sequence"), {}, "empty_or_canonical_uint64");
        return std::nullopt;
    }
    if (!anonymous_attention_valid || !confirmed_attention_identity_valid) {
        report_event(out_failure, reason::k_observation_invalid, "lifecycle_observation", "observation.attention_request_id",
            Provider_event_problem::EMPTY, value.find("attention_request_id"), {},
            !anonymous_attention_valid
                ? "anonymous_attention_requires_inferred_request"
                : "confirmed_attention_requires_request");
        return std::nullopt;
    }
    if (!resume_record_placement_valid) {
        report_event(out_failure, reason::k_observation_invalid, "lifecycle_observation", "observation.resume_record",
            Provider_event_problem::INVALID_VALUE, value.find("resume_record"), {}, "resume_record_requires_session_start");
        return std::nullopt;
    }
    return observation;
}

std::optional<std::vector<Lifecycle_observation>> decode_observations(
    const Json_value&             array,
    std::size_t                   limit,
    Contract_failure*             out_failure,
    Lifecycle_observation_policy policy)
{
    if (!array.is_array()) {
        report_event(out_failure, reason::k_frame_violation, "lifecycle_observation", "observations",
            Provider_event_problem::WRONG_TYPE, &array, {}, "array");
        return std::nullopt;
    }
    if (array.as_array().size() > limit) {
        report_event(out_failure, reason::k_payload_too_large, "lifecycle_observation", "observations",
            Provider_event_problem::TOO_LARGE, &array, limit, "observation_count");
        return std::nullopt;
    }
    std::vector<Lifecycle_observation> observations;
    std::size_t index = 0;
    for (const Json_value& value : array.as_array()) {
        std::optional<Lifecycle_observation> observation = decode_lifecycle_observation(value, out_failure, policy);
        if (!observation.has_value()) {
            if (out_failure) {
                auto diagnostic = decode_provider_event_diagnostic(out_failure->detail).value();
                diagnostic.field.replace(0, std::string_view("observation").size(),
                    "observations[" + std::to_string(index) + "]");
                out_failure->detail = encode_provider_event_diagnostic(diagnostic);
            }
            return std::nullopt;
        }
        observations.push_back(std::move(*observation));
        ++index;
    }
    return observations;
}

Json_value encode_observations(const std::vector<Lifecycle_observation>& observations)
{
    Json_value array = Json_value::array();
    for (const Lifecycle_observation& observation : observations) {
        array.push_back(encode_lifecycle_observation(observation));
    }
    return array;
}

bool lifecycle_profile_admits(const Lifecycle_features& features, const Lifecycle_observation& observation)
{
    const auto supported = [](Feature_support support) { return support == Feature_support::SUPPORTED; };
    if ((observation.phase_evidence   == Evidence::CONFIRMED ||
         observation.outcome_evidence == Evidence::CONFIRMED) &&
        !supported(features.confirmed_phase_status))
    {
        return false;
    }
    if (observation.phase_transition == Phase_transition::WORKING && !supported(features.root_turn_start)) {
        return false;
    }
    if (observation.phase_transition == Phase_transition::AWAITING_INPUT &&
        !supported(features.root_turn_completion))
    {
        return false;
    }
    if (observation.attention_transition != Attention_transition::NONE) {
        if (!supported(features.attention)) {
            return false;
        }
        if (observation.attention_request_id.empty()) {
            if (observation.attention_transition != Attention_transition::REQUESTED ||
                observation.attention_evidence   != Evidence::INFERRED)
            {
                return false;
            }
        }
        else
        if (!supported(features.paired_attention)) {
            return false;
        }
    }
    return observation.subagent_transition == Subagent_transition::NONE || supported(features.subagents);
}

// ---------------------------------------------------------------------------
// Continuous mode

Json_value encode_observe_request(const Observe_request& request)
{
    Json_value session = Json_value::object();
    session.set("working_directory",   Json_value(request.session.working_directory));
    session.set("launcher_argv",       encode_string_array(request.session.launcher_argv));
    session.set("provider_session_id", Json_value(request.session.provider_session_id));
    Json_value payload = Json_value::object();
    payload.set("session",       std::move(session));
    payload.set("resume_cursor", Json_value(request.resume_cursor));
    payload.set("window_ms",     Json_value((std::int64_t)request.window_ms));
    encode_correlation(payload, request.correlation);
    encode_context_state_request(payload, request.context_state_version);
    encode_execution_context_request(payload, request.execution_context_version);
    encode_model_state_request(payload, request.model_state_version);
    encode_model_catalog_request(payload, request.model_catalog_version);
    encode_child_state_request(payload, request.child_state_version);
    return payload;
}

std::optional<Observe_request> decode_observe_request(
    const Json_value& payload, Contract_failure* out_failure)
{
    if (!payload.is_object()) {
        report_event(out_failure, reason::k_frame_violation, "observe", "$",
            Provider_event_problem::WRONG_TYPE, &payload, {}, "object");
        return std::nullopt;
    }
    const std::optional<std::string> unexpected =
        strict::unexpected_member(payload,
            {"session", "resume_cursor", "window_ms", "correlation", "context_state_version", "execution_context", "model_state", "child_state", "model_catalog"});
    if (unexpected.has_value()) {
        report_event(out_failure, reason::k_unexpected_member, "observe", "$",
            Provider_event_problem::INVALID_SHAPE, &payload, {}, "closed_members");
        return std::nullopt;
    }
    const Json_value* session = strict::member_of_type(payload, "session", Json_type::OBJECT);
    if (session == nullptr) {
        const auto* actual = payload.find("session");
        report_event(out_failure, reason::k_frame_violation, "observe", "session",
            actual ? Provider_event_problem::WRONG_TYPE : Provider_event_problem::MISSING, actual, {}, "object");
        return std::nullopt;
    }
    const std::optional<std::string> unexpected_session =
        strict::unexpected_member(*session, {"working_directory", "launcher_argv", "provider_session_id"});
    if (unexpected_session.has_value()) {
        report_event(out_failure, reason::k_unexpected_member, "observe", "session",
            Provider_event_problem::INVALID_SHAPE, session, {}, "closed_members");
        return std::nullopt;
    }
    Observe_request request;
    if (!read_correlation(payload, "observe", request.correlation, out_failure)) {
        return std::nullopt;
    }
    decode_context_state_request(payload, request.context_state_version, request.context_failure);
    decode_execution_context_request(payload, request.execution_context_version, request.execution_failure);
    decode_model_state_request(payload, request.model_state_version, request.model_failure);
    decode_model_catalog_request(payload, request.model_catalog_version, request.catalog_failure);
    decode_child_state_request(payload, request.child_state_version, request.child_failure);
    const std::string* working_directory = strict::string_member(*session, "working_directory");
    const std::string* provider_session_id = strict::string_member(*session, "provider_session_id");
    const Json_value*  argv = strict::member_of_type(*session, "launcher_argv", Json_type::ARRAY);
    if (working_directory == nullptr || provider_session_id == nullptr || argv == nullptr || argv->as_array().empty()) {
        const auto* field = !working_directory ? "working_directory"
            : !provider_session_id ? "provider_session_id" : "launcher_argv";
        const auto* actual = session->find(field);
        const auto problem = !actual ? Provider_event_problem::MISSING
            : argv && field == std::string_view("launcher_argv") ? Provider_event_problem::EMPTY
            : Provider_event_problem::WRONG_TYPE;
        report_event(out_failure, reason::k_frame_violation, "observe", "session." + std::string(field),
            problem, actual, {}, field == std::string_view("launcher_argv") ? "nonempty_array" : "string");
        return std::nullopt;
    }
    if (working_directory->size() > limits::k_max_working_directory_bytes || has_nul(*working_directory) ||
        provider_session_id->size() > limits::k_max_lifecycle_identity_bytes || has_nul(*provider_session_id) ||
        argv->as_array().size() > limits::k_max_argv_entries)
    {
        const bool invalid_directory =
            working_directory->size() > limits::k_max_working_directory_bytes || has_nul(*working_directory);
        const bool invalid_identity =
            provider_session_id->size() > limits::k_max_lifecycle_identity_bytes || has_nul(*provider_session_id);
        const auto* field = invalid_directory ? "working_directory"
            : invalid_identity ? "provider_session_id" : "launcher_argv";
        const auto* actual = session->find(field);
        const auto bound = invalid_directory ? limits::k_max_working_directory_bytes
            : invalid_identity ? limits::k_max_lifecycle_identity_bytes : limits::k_max_argv_entries;
        const auto problem = actual->is_string() && actual->as_string().size() <= bound
            ? Provider_event_problem::CONTROL_CHARACTER : Provider_event_problem::TOO_LARGE;
        report_event(out_failure, reason::k_payload_too_large, "observe", "session." + std::string(field),
            problem, actual, bound, actual->is_string() ? "bounded_nul_free_string" : "argv_entry_count");
        return std::nullopt;
    }
    std::size_t argv_bytes = 0;
    std::size_t argument_index = 0;
    for (const Json_value& argument : argv->as_array()) {
        if (!argument.is_string()) {
            report_event(out_failure, reason::k_frame_violation, "observe",
                "session.launcher_argv[" + std::to_string(argument_index) + "]",
                Provider_event_problem::WRONG_TYPE, &argument, {}, "string");
            return std::nullopt;
        }
        if (argument.as_string().size() > limits::k_max_argv_entry_bytes || has_nul(argument.as_string())) {
            const auto problem = argument.as_string().size() > limits::k_max_argv_entry_bytes
                ? Provider_event_problem::TOO_LARGE : Provider_event_problem::CONTROL_CHARACTER;
            report_event(out_failure, reason::k_payload_too_large, "observe",
                "session.launcher_argv[" + std::to_string(argument_index) + "]",
                problem, &argument, limits::k_max_argv_entry_bytes, "bounded_nul_free_string");
            return std::nullopt;
        }
        argv_bytes += argument.as_string().size();
        request.session.launcher_argv.push_back(argument.as_string());
        ++argument_index;
    }
    if (argv_bytes > limits::k_max_argv_total_bytes) {
        auto diagnostic = make_provider_event_diagnostic(
            "observe", "session.launcher_argv", Provider_event_problem::TOO_LARGE,
            argv, limits::k_max_argv_total_bytes, "argv_total_bytes");
        diagnostic.actual_size = argv_bytes;
        report(out_failure, reason::k_payload_too_large, encode_provider_event_diagnostic(diagnostic));
        return std::nullopt;
    }
    request.session.working_directory   = *working_directory;
    request.session.provider_session_id = *provider_session_id;
    if (!read_cursor(payload, request.resume_cursor, out_failure, "observe")) {
        return std::nullopt;
    }
    const std::optional<std::int64_t> window_ms = strict::integer_member(payload, "window_ms", 1, INT32_MAX);
    if (!window_ms.has_value()) {
        const auto* actual = payload.find("window_ms");
        const auto problem = !actual ? Provider_event_problem::MISSING
            : !actual->is_integer() ? Provider_event_problem::WRONG_TYPE : Provider_event_problem::INVALID_VALUE;
        report_event(out_failure, reason::k_frame_violation, "observe", "window_ms",
            problem, actual, INT32_MAX, "positive_int32");
        return std::nullopt;
    }
    request.window_ms = (std::int32_t)*window_ms;
    return request;
}

Json_value encode_observe_result(const Observe_result& result)
{
    Json_value payload = Json_value::object();
    payload.set("resume_cursor", Json_value(result.resume_cursor));
    encode_context_updates(payload, result.context_updates, result.context_failure);
    encode_execution_updates(payload, result.execution_updates, result.execution_failure);
    encode_model_updates(payload, result.model_updates, result.model_failure);
    encode_model_catalog_update(payload, result.catalog_update, result.catalog_failure);
    encode_child_updates(payload, result.child_updates, result.child_loss, result.child_failure);
    return payload;
}

std::optional<Observe_result> decode_observe_result(const Json_value& payload, Contract_failure* out_failure)
{
    if (!payload.is_object()) {
        report_event(out_failure, reason::k_frame_violation, "observe", "$",
            Provider_event_problem::WRONG_TYPE, &payload, {}, "object");
        return std::nullopt;
    }
    const std::optional<std::string> unexpected =
        strict::unexpected_member(payload, {"resume_cursor", "context_updates", "context_failure", "execution_context", "model_state", "child_state", "model_catalog"});
    if (unexpected.has_value()) {
        report_event(out_failure, reason::k_unexpected_member, "observe", "$",
            Provider_event_problem::INVALID_SHAPE, &payload, {}, "closed_members");
        return std::nullopt;
    }
    Observe_result result;
    decode_context_updates(payload, result.context_updates, result.context_failure);
    decode_execution_updates(payload, result.execution_updates, result.execution_failure);
    decode_model_updates(payload, result.model_updates, result.model_failure);
    decode_model_catalog_update(payload, result.catalog_update, result.catalog_failure);
    decode_child_updates(payload, result.child_updates, result.child_loss, result.child_failure);
    if (!read_cursor(payload, result.resume_cursor, out_failure, "observe")) {
        return std::nullopt;
    }
    return result;
}

std::string observe_progress_note(std::uint64_t available)
{
    Json_value note = Json_value::object();
    note.set("available", Json_value(available));
    return serialize_json(note);
}

Json_value encode_poll_observations_request(const Poll_observations_request& request)
{
    Json_value payload = Json_value::object();
    payload.set("resume_cursor", Json_value(request.resume_cursor));
    payload.set("limit",         Json_value((std::uint64_t)request.limit));
    encode_correlation(payload, request.correlation);
    encode_context_state_request(payload, request.context_state_version);
    encode_execution_context_request(payload, request.execution_context_version);
    encode_model_state_request(payload, request.model_state_version);
    encode_model_catalog_request(payload, request.model_catalog_version);
    encode_child_state_request(payload, request.child_state_version);
    return payload;
}

std::optional<Poll_observations_request> decode_poll_observations_request(
    const Json_value& payload, Contract_failure* out_failure)
{
    if (!payload.is_object()) {
        report_event(out_failure, reason::k_frame_violation, "poll_observations", "$",
            Provider_event_problem::WRONG_TYPE, &payload, {}, "object");
        return std::nullopt;
    }
    const std::optional<std::string> unexpected =
        strict::unexpected_member(payload, {"resume_cursor", "limit", "correlation", "context_state_version", "execution_context", "model_state", "child_state", "model_catalog"});
    if (unexpected.has_value()) {
        report_event(out_failure, reason::k_unexpected_member, "poll_observations", "$",
            Provider_event_problem::INVALID_SHAPE, &payload, {}, "closed_members");
        return std::nullopt;
    }
    Poll_observations_request request;
    if (!read_correlation(payload, "poll_observations", request.correlation, out_failure)) {
        return std::nullopt;
    }
    decode_context_state_request(payload, request.context_state_version, request.context_failure);
    decode_execution_context_request(payload, request.execution_context_version, request.execution_failure);
    decode_model_state_request(payload, request.model_state_version, request.model_failure);
    decode_model_catalog_request(payload, request.model_catalog_version, request.catalog_failure);
    decode_child_state_request(payload, request.child_state_version, request.child_failure);
    if (!read_cursor(payload, request.resume_cursor, out_failure, "poll_observations")) {
        return std::nullopt;
    }
    const std::optional<std::int64_t> limit =
        strict::integer_member(payload, "limit", 1, (std::int64_t)limits::k_max_observations_per_result);
    if (!limit.has_value()) {
        const auto* actual = payload.find("limit");
        const auto problem = !actual ? Provider_event_problem::MISSING
            : !actual->is_integer() ? Provider_event_problem::WRONG_TYPE : Provider_event_problem::INVALID_VALUE;
        report_event(out_failure, reason::k_frame_violation, "poll_observations", "limit",
            problem, actual, limits::k_max_observations_per_result, "positive_bounded_limit");
        return std::nullopt;
    }
    request.limit = (std::size_t)*limit;
    return request;
}

Json_value encode_poll_observations_result(const Poll_observations_result& result)
{
    Json_value payload = Json_value::object();
    payload.set("observations",  encode_observations(result.observations));
    payload.set("resume_cursor", Json_value(result.resume_cursor));
    payload.set("more",          Json_value(result.more));
    encode_context_updates(payload, result.context_updates, result.context_failure);
    encode_execution_updates(payload, result.execution_updates, result.execution_failure);
    encode_model_updates(payload, result.model_updates, result.model_failure);
    encode_model_catalog_update(payload, result.catalog_update, result.catalog_failure);
    encode_child_updates(payload, result.child_updates, result.child_loss, result.child_failure);
    return payload;
}

std::optional<Poll_observations_result> decode_poll_observations_result(
    const Json_value& payload,
    std::size_t       limit,
    Contract_failure* out_failure)
{
    if (!payload.is_object()) {
        report_event(out_failure, reason::k_frame_violation, "poll_observations", "$",
            Provider_event_problem::WRONG_TYPE, &payload, {}, "object");
        return std::nullopt;
    }
    const std::optional<std::string> unexpected =
        strict::unexpected_member(payload, {"observations", "resume_cursor", "more", "context_updates", "context_failure", "execution_context", "model_state", "child_state", "model_catalog"});
    if (unexpected.has_value()) {
        report_event(out_failure, reason::k_unexpected_member, "poll_observations", "$",
            Provider_event_problem::INVALID_SHAPE, &payload, {}, "closed_members");
        return std::nullopt;
    }
    const Json_value* observations = payload.find("observations");
    const Json_value* more         = strict::member_of_type(payload, "more", Json_type::BOOLEAN);
    if (observations == nullptr || more == nullptr) {
        const auto* field  = observations == nullptr ? "observations" : "more";
        const auto* actual = payload.find(field);
        report_event(out_failure, reason::k_frame_violation, "poll_observations", field,
            actual ? Provider_event_problem::WRONG_TYPE : Provider_event_problem::MISSING,
            actual, {}, observations == nullptr ? "array" : "boolean");
        return std::nullopt;
    }
    Poll_observations_result result;
    decode_context_updates(payload, result.context_updates, result.context_failure);
    decode_execution_updates(payload, result.execution_updates, result.execution_failure);
    decode_model_updates(payload, result.model_updates, result.model_failure);
    decode_model_catalog_update(payload, result.catalog_update, result.catalog_failure);
    decode_child_updates(payload, result.child_updates, result.child_loss, result.child_failure);
    std::optional<std::vector<Lifecycle_observation>> decoded =
        decode_observations(*observations, std::min(limit, limits::k_max_observations_per_result), out_failure, Lifecycle_observation_policy::CONTINUOUS);
    if (!decoded.has_value()) {
        if (out_failure) {
            auto diagnostic = decode_provider_event_diagnostic(out_failure->detail).value();
            diagnostic.event_name = "poll_observations";
            out_failure->detail = encode_provider_event_diagnostic(diagnostic);
        }
        return std::nullopt;
    }
    if (!read_cursor(payload, result.resume_cursor, out_failure, "poll_observations")) {
        return std::nullopt;
    }
    result.observations = std::move(*decoded);
    result.more         = more->as_boolean();
    return result;
}

} // namespace logonomic::cli_addon
