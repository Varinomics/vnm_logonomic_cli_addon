#include "logonomic_cli_addon/helper_ingress.h"
#include "logonomic_cli_addon/capability_ids.h"
#include "logonomic_cli_addon/limits.h"
#include "logonomic_cli_addon/provider_event_diagnostic.h"
#include "logonomic_cli_addon/sha256.h"
#include "strict_json.h"

#include <algorithm>
#include <cstdint>
#include <unordered_set>

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

bool has_nul(std::string_view text) { return text.find('\0') != std::string_view::npos; }

bool report_event_failure(
    Contract_failure* out_failure, const char* reason_code, const char* field,
    const Json_value* actual, Provider_event_problem problem, const char* rule,
    std::optional<std::uint64_t> bound = {})
{
    if (out_failure) {
        out_failure->reason_code = reason_code;
        out_failure->detail = encode_provider_event_diagnostic(make_provider_event_diagnostic(
            "classify_event", field, actual ? problem : Provider_event_problem::MISSING, actual, bound, rule));
    }
    return false;
}

bool report_identity_failure(Contract_failure* failure, const char* field, const Json_value* actual)
{
    const auto problem = !actual || !actual->is_string() ? Provider_event_problem::WRONG_TYPE
        : actual->as_string().empty() ? Provider_event_problem::EMPTY
        : has_nul(actual->as_string()) ? Provider_event_problem::CONTROL_CHARACTER
        : actual->as_string().size() > limits::k_max_lifecycle_identity_bytes ? Provider_event_problem::TOO_LARGE
        : Provider_event_problem::INVALID_VALUE;
    return report_event_failure(failure, reason::k_classification_invalid, field, actual, problem,
        "nonempty_nul_free_identity", limits::k_max_lifecycle_identity_bytes);
}

constexpr Name_entry<Transition_kind> k_transition_kind_names[] = {
    {Transition_kind::START,  "start"},
    {Transition_kind::RESUME, "resume"},
};

constexpr Name_entry<Classification> k_classification_names[] = {
    {Classification::TRANSITION, "transition"},
    {Classification::EVENT,      "event"},
    {Classification::IGNORED,    "ignore"},
};

Json_value encode_provider_event(const Provider_event& event)
{
    Json_value object = Json_value::object();
    object.set("name", Json_value(event.name));
    object.set("body", event.body);
    return object;
}

bool decode_provider_event(const Json_value& value, Provider_event& out_event, Contract_failure* out_failure)
{
    if (!value.is_object()) {
        return report_event_failure(out_failure, reason::k_frame_violation, "event", &value,
            Provider_event_problem::WRONG_TYPE, "object_required");
    }
    const std::optional<std::string> unexpected = strict::unexpected_member(value, {"name", "body"});
    if (unexpected.has_value()) {
        return report_event_failure(out_failure, reason::k_unexpected_member, "event", &value,
            Provider_event_problem::INVALID_SHAPE, "closed_members");
    }
    const std::string* name = strict::string_member(value, "name");
    const Json_value*  body = strict::member_of_type(value, "body", Json_type::OBJECT);
    if (!name || name->empty()) {
        return report_event_failure(out_failure, reason::k_frame_violation, "event.name", value.find("name"),
            name ? Provider_event_problem::EMPTY : Provider_event_problem::WRONG_TYPE, "nonempty_string");
    }
    if (!body) {
        return report_event_failure(out_failure, reason::k_frame_violation, "event.body", value.find("body"),
            Provider_event_problem::WRONG_TYPE, "object_required");
    }
    if (name->size() > limits::k_max_provider_event_name_bytes || has_nul(*name)) {
        return report_event_failure(out_failure, reason::k_payload_too_large, "event.name", value.find("name"),
            has_nul(*name) ? Provider_event_problem::CONTROL_CHARACTER : Provider_event_problem::TOO_LARGE,
            "provider_event_name", limits::k_max_provider_event_name_bytes);
    }
    out_event.name = *name;
    out_event.body = *body;
    return true;
}

Json_value encode_ledger(const Ledger_view& ledger)
{
    Json_value object = Json_value::object();
    object.set("current_epoch", Json_value(ledger.current_epoch));
    if (ledger.current_transition.has_value()) {
        Json_value transition = Json_value::object();
        transition.set("kind",                Json_value(transition_kind_name(ledger.current_transition->kind)));
        transition.set("provider_session_id", Json_value(ledger.current_transition->provider_session_id));
        transition.set("nonce",               Json_value(ledger.current_transition->nonce));
        object.set("current_transition", std::move(transition));
    }
    else {
        object.set("current_transition", Json_value());
    }
    return object;
}

bool decode_ledger(const Json_value& value, Ledger_view& out_ledger, Contract_failure* out_failure)
{
    if (!value.is_object()) {
        return report_event_failure(out_failure, reason::k_frame_violation, "ledger", &value,
            Provider_event_problem::WRONG_TYPE, "object_required");
    }
    const std::optional<std::string> unexpected =
        strict::unexpected_member(value, {"current_epoch", "current_transition"});
    if (unexpected.has_value()) {
        return report_event_failure(out_failure, reason::k_unexpected_member, "ledger", &value,
            Provider_event_problem::INVALID_SHAPE, "closed_members");
    }
    const std::optional<std::int64_t> epoch = strict::integer_member(value, "current_epoch", 0, INT64_MAX);
    const Json_value* transition = value.find("current_transition");
    if (!epoch) {
        return report_event_failure(out_failure, reason::k_frame_violation, "ledger.current_epoch",
            value.find("current_epoch"), Provider_event_problem::INVALID_VALUE, "nonnegative_integer");
    }
    if (!transition) {
        return report_event_failure(out_failure, reason::k_frame_violation, "ledger.current_transition", nullptr,
            Provider_event_problem::MISSING, "required_member");
    }
    out_ledger.current_epoch = (std::uint64_t)*epoch;
    if (transition->is_null()) {
        if (out_ledger.current_epoch != 0) {
            return report_event_failure(out_failure, reason::k_frame_violation, "ledger.current_transition", transition,
                Provider_event_problem::INVALID_VALUE, "transition_required_after_epoch_zero");
        }
        return true;
    }
    if (!transition->is_object() ||
        strict::unexpected_member(*transition, {"kind", "provider_session_id", "nonce"}).has_value())
    {
        return report_event_failure(out_failure, reason::k_frame_violation, "ledger.current_transition", transition,
            Provider_event_problem::INVALID_SHAPE, "closed_transition_object");
    }
    if (out_ledger.current_epoch == 0) {
        return report_event_failure(out_failure, reason::k_frame_violation, "ledger.current_transition", transition,
            Provider_event_problem::INVALID_VALUE, "no_transition_at_epoch_zero");
    }
    const std::string* kind  = strict::string_member(*transition, "kind");
    const std::string* id    = strict::string_member(*transition, "provider_session_id");
    const std::string* nonce = strict::string_member(*transition, "nonce");
    const std::optional<Transition_kind> parsed_kind = kind == nullptr ? std::nullopt : parse_transition_kind(*kind);
    if (!parsed_kind) {
        return report_event_failure(out_failure, reason::k_frame_violation, "ledger.current_transition.kind",
            transition->find("kind"), Provider_event_problem::INVALID_VALUE, "transition_kind");
    }
    if (!id) {
        return report_event_failure(out_failure, reason::k_frame_violation, "ledger.current_transition.provider_session_id",
            transition->find("provider_session_id"), Provider_event_problem::WRONG_TYPE, "string_required");
    }
    if (!nonce || !is_valid_transition_nonce_hex(*nonce)) {
        return report_event_failure(out_failure, reason::k_frame_violation, "ledger.current_transition.nonce",
            transition->find("nonce"), Provider_event_problem::INVALID_VALUE, "transition_nonce");
    }
    if (id->size() > limits::k_max_lifecycle_identity_bytes || has_nul(*id)) {
        return report_event_failure(out_failure, reason::k_payload_too_large, "ledger.current_transition.provider_session_id",
            transition->find("provider_session_id"),
            has_nul(*id) ? Provider_event_problem::CONTROL_CHARACTER : Provider_event_problem::TOO_LARGE,
            "lifecycle_identity", limits::k_max_lifecycle_identity_bytes);
    }
    Ledger_transition current;
    current.kind                = *parsed_kind;
    current.provider_session_id = *id;
    current.nonce               = *nonce;
    out_ledger.current_transition = std::move(current);
    return true;
}

} // namespace

bool validate_helper_ingress_dependency(const std::vector<Capability_pair>& live_pairs, Contract_failure* out_failure)
{
    const bool lifecycle_available = std::any_of(
        live_pairs.begin(), live_pairs.end(), [](const Capability_pair& pair) {
            return pair.capability_id == k_lifecycle_events_capability_id &&
                pair.version == k_lifecycle_events_version;
        });
    if (!lifecycle_available) {
        return report(out_failure, reason::k_dependency_capability_unavailable,
            "helper ingress requires lifecycle-events@2 live in the same generation");
    }
    return true;
}

std::string provider_event_evidence_digest_hex(const Provider_event& event)
{
    return to_hex(Sha256::digest(serialize_json_canonical(encode_provider_event(event))));
}

const char* transition_kind_name(Transition_kind kind)            { return name_of(k_transition_kind_names, kind); }
const char* classification_name(Classification classification)   { return name_of(k_classification_names, classification); }

std::optional<Transition_kind> parse_transition_kind(std::string_view name) { return value_of(k_transition_kind_names, name); }
std::optional<Classification> parse_classification(std::string_view name)  { return value_of(k_classification_names, name); }

Json_value encode_classify_event_request(const Classify_event_request& request)
{
    Json_value payload = Json_value::object();
    payload.set("event",            encode_provider_event(request.event));
    payload.set("ledger",           encode_ledger(request.ledger));
    payload.set("transition_nonce", Json_value(request.transition_nonce));
    encode_context_state_request(payload, request.context_state_version);
    encode_execution_context_request(payload, request.execution_context_version);
    encode_model_state_request(payload, request.model_state_version);
    encode_model_catalog_request(payload, request.model_catalog_version);
    encode_child_state_request(payload, request.child_state_version);
    encode_cooperative_description_request(payload, request.cooperative_description_version);
    if (request.context_snapshot_revision) {
        payload.set("context_snapshot_revision", Json_value(*request.context_snapshot_revision));
    }
    if (request.terminal_identity) {
        payload.set(k_terminal_identity_member, encode_terminal_identity_context(*request.terminal_identity));
    }
    return payload;
}

std::optional<Classify_event_request> decode_classify_event_request(
    const Json_value& payload, Contract_failure* out_failure)
{
    if (!payload.is_object()) {
        report_event_failure(out_failure, reason::k_frame_violation, "payload", &payload,
            Provider_event_problem::WRONG_TYPE, "object_required");
        return std::nullopt;
    }
    const std::optional<std::string> unexpected =
        strict::unexpected_member(payload,
            {"event", "ledger", "transition_nonce", "context_state_version", "context_snapshot_revision", "execution_context", "model_state", "child_state", "model_catalog",
                "cooperative_description", k_terminal_identity_member});
    if (unexpected.has_value()) {
        report_event_failure(out_failure, reason::k_unexpected_member, "payload", &payload,
            Provider_event_problem::INVALID_SHAPE, "closed_members");
        return std::nullopt;
    }
    const Json_value* event  = payload.find("event");
    const Json_value* ledger = payload.find("ledger");
    const std::string* nonce = strict::string_member(payload, "transition_nonce");
    if (!event || !ledger) {
        report_event_failure(out_failure, reason::k_frame_violation, event ? "ledger" : "event", nullptr,
            Provider_event_problem::MISSING, "required_member");
        return std::nullopt;
    }
    if (!nonce || !is_valid_transition_nonce_hex(*nonce)) {
        report_event_failure(out_failure, reason::k_frame_violation, "transition_nonce", payload.find("transition_nonce"),
            Provider_event_problem::INVALID_VALUE, "transition_nonce");
        return std::nullopt;
    }
    Classify_event_request request;
    decode_context_state_request(payload, request.context_state_version, request.context_failure);
    decode_execution_context_request(payload, request.execution_context_version, request.execution_failure);
    decode_model_state_request(payload, request.model_state_version, request.model_failure);
    decode_model_catalog_request(payload, request.model_catalog_version, request.catalog_failure);
    decode_child_state_request(payload, request.child_state_version, request.child_failure);
    decode_cooperative_description_request(payload, request.cooperative_description_version, request.description_failure);
    if (const auto* identity = payload.find(k_terminal_identity_member)) {
        request.terminal_identity = decode_terminal_identity_context(*identity, out_failure);
        if (!request.terminal_identity) {
            return std::nullopt;
        }
    }
    if (payload.find("context_snapshot_revision")) {
        const auto revision = strict::integer_member(payload, "context_snapshot_revision", 1, 9007199254740991);
        if (request.context_state_version != k_context_state_capability_version || !revision) {
            request.context_state_version = 0;
            if (!request.context_failure) {
                request.context_failure = Contract_failure{reason::k_observation_invalid,
                    "context_snapshot_revision requires context-state@2 and an exact positive integer"};
            }
        }
        else {
            request.context_snapshot_revision = static_cast<std::uint64_t>(*revision);
        }
    }
    if (!decode_provider_event(*event, request.event, out_failure) ||
        !decode_ledger(*ledger, request.ledger, out_failure))
    {
        return std::nullopt;
    }
    request.transition_nonce = *nonce;
    return request;
}

Json_value encode_classify_event_result(const Classify_event_result& result)
{
    Json_value payload = Json_value::object();
    payload.set("classification", Json_value(classification_name(result.classification)));
    if (result.transition.has_value()) {
        Json_value transition = Json_value::object();
        transition.set("kind", Json_value(transition_kind_name(*result.transition)));
        payload.set("transition", std::move(transition));
    }
    payload.set("observations", encode_observations(result.observations));
    encode_context_updates(payload, result.context_updates, result.context_failure);
    encode_execution_updates(payload, result.execution_updates, result.execution_failure);
    encode_model_updates(payload, result.model_updates, result.model_failure);
    encode_model_catalog_update(payload, result.catalog_update, result.catalog_failure);
    encode_child_updates(payload, result.child_updates, result.child_loss, result.child_failure);
    encode_cooperative_description_result(payload, result.description_submission, result.description_failure);
    if (result.helper_response.has_value()) {
        payload.set("helper_response", *result.helper_response);
    }
    if (result.lifecycle_degradation) {
        Json_value degradation = Json_value::object();
        degradation.set("version", Json_value((std::int64_t)result.lifecycle_degradation->version));
        if (result.lifecycle_degradation->provider_session_id) {
            degradation.set("provider_session_id", Json_value(*result.lifecycle_degradation->provider_session_id));
        }
        payload.set("lifecycle_degradation", std::move(degradation));
    }
    if (result.subagent_evidence) {
        const Subagent_evidence& evidence = *result.subagent_evidence;
        Json_value encoded = Json_value::object();
        encoded.set("version", Json_value((std::int64_t)evidence.version));
        encoded.set(
            "kind",
            Json_value(evidence.kind == Subagent_evidence_kind::BATCH
                    ? "batch"
                    : evidence.kind == Subagent_evidence_kind::LOSS
                        ? "loss"
                        : "reconcile"));
        encoded.set("provider_session_id", Json_value(evidence.provider_session_id));
        if (evidence.kind == Subagent_evidence_kind::BATCH) {
            encoded.set("batch_id", Json_value(evidence.batch_id));
            Json_value child_ids = Json_value::array();
            for (const std::string& child_id : evidence.child_ids) {
                child_ids.push_back(Json_value(child_id));
            }
            encoded.set("child_ids", std::move(child_ids));
        }
        else if (evidence.kind == Subagent_evidence_kind::LOSS) {
            encoded.set("reason_code", Json_value(evidence.reason_code));
        }
        else {
            encoded.set("provider_event_id", Json_value(evidence.provider_event_id));
        }
        payload.set("subagent_evidence", std::move(encoded));
    }
    return payload;
}

std::optional<Classify_event_result> decode_classify_event_result(
    const Json_value& payload, Contract_failure* out_failure, std::uint16_t version)
{
    if (version != k_helper_ingress_degradation_version &&
        version != k_helper_ingress_subagent_reconciliation_version)
    {
        const Json_value actual(version);
        report_event_failure(out_failure, reason::k_unsupported_version, "helper_ingress.version", &actual,
            Provider_event_problem::INVALID_VALUE, "supported_helper_version");
        return std::nullopt;
    }
    if (!payload.is_object()) {
        report_event_failure(out_failure, reason::k_classification_invalid, "payload", &payload,
            Provider_event_problem::WRONG_TYPE, "object_required");
        return std::nullopt;
    }
    const std::optional<std::string> unexpected =
        strict::unexpected_member(payload,
            {"classification", "transition", "observations", "helper_response", "lifecycle_degradation",
             "subagent_evidence", "context_updates", "context_failure", "execution_context", "model_state", "child_state", "model_catalog",
             "cooperative_description"});
    if (unexpected.has_value()) {
        report_event_failure(out_failure, reason::k_classification_invalid, "payload", &payload,
            Provider_event_problem::INVALID_SHAPE, "closed_members");
        return std::nullopt;
    }
    const std::string* classification = strict::string_member(payload, "classification");
    const std::optional<Classification> parsed =
        classification == nullptr ? std::nullopt : parse_classification(*classification);
    const Json_value* observations = payload.find("observations");
    if (!parsed) {
        report_event_failure(out_failure, reason::k_classification_invalid, "classification", payload.find("classification"),
            Provider_event_problem::INVALID_VALUE, "classification_vocabulary");
        return std::nullopt;
    }
    if (!observations) {
        report_event_failure(out_failure, reason::k_classification_invalid, "observations", nullptr,
            Provider_event_problem::MISSING, "required_member");
        return std::nullopt;
    }
    Classify_event_result result;
    result.classification = *parsed;
    decode_context_updates(payload, result.context_updates, result.context_failure);
    decode_execution_updates(payload, result.execution_updates, result.execution_failure);
    decode_model_updates(payload, result.model_updates, result.model_failure);
    decode_model_catalog_update(payload, result.catalog_update, result.catalog_failure);
    decode_child_updates(payload, result.child_updates, result.child_loss, result.child_failure);
    decode_cooperative_description_result(payload, result.description_submission, result.description_failure);
    if (result.description_submission && result.classification != Classification::IGNORED) {
        result.description_submission.reset();
        result.description_failure = Contract_failure{reason::k_observation_invalid,
            "description submission requires an independent ignored carrier"};
    }

    const std::size_t observation_limit =
        version == k_helper_ingress_subagent_reconciliation_version
            ? k_max_subagent_evidence_observations
            : limits::k_max_observations_per_result;
    std::optional<std::vector<Lifecycle_observation>> decoded =
        decode_observations(*observations, observation_limit, out_failure, Lifecycle_observation_policy::HELPER_INGRESS);
    if (!decoded.has_value()) {
        return std::nullopt;
    }
    result.observations = std::move(*decoded);
    const std::size_t started = (std::size_t)std::count_if(
        result.observations.begin(), result.observations.end(), [](const Lifecycle_observation& observation) {
            return observation.session_transition == Session_transition::STARTED;
        });

    const Json_value* transition = payload.find("transition");
    if (result.classification == Classification::TRANSITION) {
        if (transition == nullptr || !transition->is_object() ||
            strict::unexpected_member(*transition, {"kind"}).has_value())
        {
            report_event_failure(out_failure, reason::k_classification_invalid, "transition", transition,
                Provider_event_problem::INVALID_SHAPE, "transition_kind_object");
            return std::nullopt;
        }
        const std::string* kind = strict::string_member(*transition, "kind");
        result.transition = kind == nullptr ? std::nullopt : parse_transition_kind(*kind);
        if (!result.transition.has_value()) {
            report_event_failure(out_failure, reason::k_classification_invalid, "transition.kind", transition->find("kind"),
                Provider_event_problem::INVALID_VALUE, "transition_kind");
            return std::nullopt;
        }
        if (started != 1) {
            report_event_failure(out_failure, reason::k_classification_invalid, "observations", observations,
                Provider_event_problem::INVALID_SHAPE, "exactly_one_started_observation");
            return std::nullopt;
        }
    }
    else {
        if (transition != nullptr) {
            report_event_failure(out_failure, reason::k_classification_invalid, "transition", transition,
                Provider_event_problem::INVALID_VALUE, "transition_classification_required");
            return std::nullopt;
        }
        if (result.classification == Classification::EVENT && (result.observations.empty() || started != 0)) {
            report_event_failure(out_failure, reason::k_classification_invalid, "observations", observations,
                Provider_event_problem::INVALID_SHAPE, "nonempty_nonstarted_observations");
            return std::nullopt;
        }
        if (result.classification == Classification::IGNORED && !result.observations.empty()) {
            report_event_failure(out_failure, reason::k_classification_invalid, "observations", observations,
                Provider_event_problem::INVALID_SHAPE, "ignored_has_no_observations");
            return std::nullopt;
        }
    }

    if (const Json_value* response = payload.find("helper_response"); response != nullptr) {
        result.helper_response = *response;
    }
    if (const Json_value* degradation = payload.find("lifecycle_degradation"); degradation != nullptr) {
        if (version != k_helper_ingress_degradation_version ||
            result.classification != Classification::IGNORED || !degradation->is_object() ||
            strict::unexpected_member(*degradation, {"version", "provider_session_id"}) ||
            !strict::integer_member(*degradation, "version", 1, 1))
        {
            report_event_failure(out_failure, reason::k_classification_invalid, "lifecycle_degradation", degradation,
                Provider_event_problem::INVALID_SHAPE, "supported_degradation_record");
            return std::nullopt;
        }
        Lifecycle_degradation fact;
        if (degradation->find("provider_session_id") != nullptr) {
            const std::string* session = strict::string_member(*degradation, "provider_session_id");
            if (session == nullptr || session->empty() ||
                session->size() > limits::k_max_lifecycle_identity_bytes || has_nul(*session))
            {
                report_identity_failure(out_failure, "lifecycle_degradation.provider_session_id",
                    degradation->find("provider_session_id"));
                return std::nullopt;
            }
            fact.provider_session_id = *session;
        }
        result.lifecycle_degradation = std::move(fact);
    }
    if (const Json_value* encoded = payload.find("subagent_evidence");
        encoded != nullptr)
    {
        if (version != k_helper_ingress_subagent_reconciliation_version ||
            !encoded->is_object() ||
            !strict::integer_member(*encoded, "version", 1, 1))
        {
            report_event_failure(out_failure, reason::k_classification_invalid, "subagent_evidence", encoded,
                Provider_event_problem::INVALID_SHAPE, "supported_subagent_record");
            return std::nullopt;
        }
        const std::string* kind = strict::string_member(*encoded, "kind");
        const std::string* session = strict::string_member(
            *encoded, "provider_session_id");
        if (!kind) {
            report_event_failure(out_failure, reason::k_classification_invalid, "subagent_evidence.kind",
                encoded->find("kind"), Provider_event_problem::WRONG_TYPE, "string_required");
            return std::nullopt;
        }
        if (session == nullptr || session->empty() ||
            session->size() > limits::k_max_lifecycle_identity_bytes ||
            has_nul(*session))
        {
            report_identity_failure(out_failure, "subagent_evidence.provider_session_id",
                encoded->find("provider_session_id"));
            return std::nullopt;
        }
        Subagent_evidence evidence;
        evidence.provider_session_id = *session;
        if (*kind == "batch") {
            if ((result.classification != Classification::TRANSITION &&
                 result.classification != Classification::EVENT) ||
                strict::unexpected_member(
                    *encoded,
                    {"version", "kind", "provider_session_id", "batch_id", "child_ids"})
                        .has_value())
            {
                report_event_failure(out_failure, reason::k_classification_invalid, "subagent_evidence", encoded,
                    Provider_event_problem::INVALID_SHAPE, "batch_record_shape_and_classification");
                return std::nullopt;
            }
            const std::string* batch_id = strict::string_member(*encoded, "batch_id");
            const Json_value* child_ids = strict::member_of_type(
                *encoded, "child_ids", Json_type::ARRAY);
            if (batch_id == nullptr || batch_id->empty() ||
                batch_id->size() > limits::k_max_lifecycle_identity_bytes ||
                has_nul(*batch_id))
            {
                report_identity_failure(out_failure, "subagent_evidence.batch_id", encoded->find("batch_id"));
                return std::nullopt;
            }
            if (!child_ids || child_ids->as_array().empty() ||
                child_ids->as_array().size() > k_max_subagent_evidence_observations)
            {
                report_event_failure(out_failure, reason::k_classification_invalid, "subagent_evidence.child_ids",
                    encoded->find("child_ids"),
                    child_ids ? Provider_event_problem::INVALID_SHAPE : Provider_event_problem::WRONG_TYPE,
                    "nonempty_bounded_children", k_max_subagent_evidence_observations);
                return std::nullopt;
            }
            std::unordered_set<std::string> unique_child_ids;
            for (const Json_value& child_id : child_ids->as_array()) {
                if (!child_id.is_string() || child_id.as_string().empty() ||
                    child_id.as_string().size() > limits::k_max_lifecycle_identity_bytes ||
                    has_nul(child_id.as_string()) ||
                    !unique_child_ids.insert(child_id.as_string()).second)
                {
                    const std::string field =
                        "subagent_evidence.child_ids[" + std::to_string(evidence.child_ids.size()) + "]";
                    if (!child_id.is_string() || child_id.as_string().empty() ||
                        child_id.as_string().size() > limits::k_max_lifecycle_identity_bytes ||
                        has_nul(child_id.as_string()))
                    {
                        report_identity_failure(out_failure, field.c_str(), &child_id);
                    }
                    else {
                        report_event_failure(out_failure, reason::k_classification_invalid, field.c_str(), &child_id,
                            Provider_event_problem::DUPLICATE, "unique_child_identity");
                    }
                    return std::nullopt;
                }
                evidence.child_ids.push_back(child_id.as_string());
            }
            evidence.kind = Subagent_evidence_kind::BATCH;
            evidence.batch_id = *batch_id;
        }
        else if (*kind == "loss") {
            if ((result.classification != Classification::TRANSITION &&
                 result.classification != Classification::EVENT) ||
                strict::unexpected_member(
                    *encoded,
                    {"version", "kind", "provider_session_id", "reason_code"})
                        .has_value())
            {
                report_event_failure(out_failure, reason::k_classification_invalid, "subagent_evidence", encoded,
                    Provider_event_problem::INVALID_SHAPE, "loss_record_shape_and_classification");
                return std::nullopt;
            }
            const std::string* reason_code = strict::string_member(*encoded, "reason_code");
            if (reason_code == nullptr || reason_code->empty() ||
                reason_code->size() > limits::k_max_lifecycle_identity_bytes ||
                has_nul(*reason_code))
            {
                report_identity_failure(out_failure, "subagent_evidence.reason_code", encoded->find("reason_code"));
                return std::nullopt;
            }
            evidence.kind = Subagent_evidence_kind::LOSS;
            evidence.reason_code = *reason_code;
        }
        else if (*kind == "reconcile") {
            if ((result.classification != Classification::TRANSITION &&
                 result.classification != Classification::EVENT) ||
                strict::unexpected_member(
                    *encoded,
                    {"version", "kind", "provider_session_id", "provider_event_id"})
                        .has_value())
            {
                report_event_failure(out_failure, reason::k_classification_invalid, "subagent_evidence", encoded,
                    Provider_event_problem::INVALID_SHAPE, "reconcile_record_shape_and_classification");
                return std::nullopt;
            }
            const std::string* event = strict::string_member(*encoded, "provider_event_id");
            if (event == nullptr || event->empty() ||
                event->size() > limits::k_max_lifecycle_identity_bytes ||
                has_nul(*event))
            {
                report_identity_failure(out_failure, "subagent_evidence.provider_event_id",
                    encoded->find("provider_event_id"));
                return std::nullopt;
            }
            evidence.kind = Subagent_evidence_kind::RECONCILE;
            evidence.provider_event_id = *event;
        }
        else {
            report_event_failure(out_failure, reason::k_classification_invalid, "subagent_evidence.kind", encoded->find("kind"),
                Provider_event_problem::INVALID_VALUE, "subagent_evidence_kind");
            return std::nullopt;
        }
        result.subagent_evidence = std::move(evidence);
    }
    if (result.observations.size() > limits::k_max_observations_per_result &&
        (!result.subagent_evidence ||
         result.subagent_evidence->kind != Subagent_evidence_kind::BATCH))
    {
        report_event_failure(out_failure, reason::k_classification_invalid, "observations", observations,
            Provider_event_problem::TOO_LARGE, "ordinary_observations_without_batch", limits::k_max_observations_per_result);
        return std::nullopt;
    }
    return result;
}

Json_value encode_terminal_identity_classification_result(const Terminal_identity_classification_result& result)
{
    auto payload = encode_classify_event_result(result.base);
    payload.set(k_terminal_identity_member, result.terminal_identity
        ? encode_terminal_identity_observation(*result.terminal_identity) : Json_value());
    return payload;
}

std::optional<Terminal_identity_classification_result> decode_terminal_identity_classification_result(
    const Json_value& payload, Contract_failure* out_failure, std::uint16_t helper_version)
{
    if (!payload.is_object()) {
        report_event_failure(out_failure, reason::k_classification_invalid, "payload", &payload,
            Provider_event_problem::WRONG_TYPE, "object_required");
        return std::nullopt;
    }
    Json_value common = payload;
    common.erase(k_terminal_identity_member);
    const auto base = decode_classify_event_result(common, out_failure, helper_version);
    if (!base) return std::nullopt;
    Terminal_identity_classification_result result{*base, std::nullopt};
    const auto* identity = payload.find(k_terminal_identity_member);
    if (identity == nullptr) {
        report_event_failure(out_failure, reason::k_classification_invalid, k_terminal_identity_member, nullptr,
            Provider_event_problem::MISSING, "required_member");
        return std::nullopt;
    }
    if (!identity->is_null()) {
        result.terminal_identity = decode_terminal_identity_observation(*identity, out_failure);
        if (!result.terminal_identity) {
            return std::nullopt;
        }
    }
    if (base->classification == Classification::EVENT ||
        (base->classification == Classification::TRANSITION &&
            (!result.terminal_identity || base->observations.size() != 1 ||
                base->observations.front().provider_session_id != result.terminal_identity->provider_session_id ||
                !base->observations.front().resume_record)))
    {
        report_event_failure(out_failure, reason::k_classification_invalid, k_terminal_identity_member, identity,
            Provider_event_problem::MISMATCH, "exact_identity_transition_proof");
        return std::nullopt;
    }
    return result;
}

Json_value encode_attention_classification_request(const Attention_classification_request& request)
{
    auto payload = encode_classify_event_request(request.base);
    if (request.pending_conversational_request_id) {
        payload.set("pending_conversational_request_id", Json_value(*request.pending_conversational_request_id));
    }
    return payload;
}

std::optional<Attention_classification_request> decode_attention_classification_request(
    const Json_value& payload, Contract_failure* out_failure)
{
    if (!payload.is_object()) {
        report_event_failure(out_failure, reason::k_classification_invalid, "payload", &payload,
            Provider_event_problem::WRONG_TYPE, "object_required");
        return std::nullopt;
    }
    Json_value common = payload;
    common.erase("pending_conversational_request_id");
    const auto base = decode_classify_event_request(common, out_failure);
    if (!base) return std::nullopt;
    Attention_classification_request request{*base, std::nullopt};
    if (const auto* pending = payload.find("pending_conversational_request_id")) {
        if (!pending->is_string() || pending->as_string().empty() ||
            pending->as_string().size() > limits::k_max_lifecycle_identity_bytes ||
            pending->as_string().find('\0') != std::string::npos)
        {
            report_identity_failure(out_failure, "pending_conversational_request_id", pending);
            return std::nullopt;
        }
        request.pending_conversational_request_id = pending->as_string();
    }
    return request;
}

Json_value encode_attention_classification_result(const Attention_classification_result& result)
{
    auto payload = encode_classify_event_result(result.base);
    if (result.attention_kind) {
        auto kind = Json_value::object();
        kind.set("version", Json_value(1));
        kind.set("observation_index", Json_value(static_cast<std::int64_t>(result.attention_kind->observation_index)));
        kind.set("kind", Json_value(result.attention_kind->kind));
        payload.set("attention_kind", std::move(kind));
    }
    return payload;
}

std::optional<Attention_classification_result> decode_attention_classification_result(
    const Json_value& payload, Contract_failure* out_failure, std::uint16_t helper_version)
{
    if (!payload.is_object()) {
        report_event_failure(out_failure, reason::k_classification_invalid, "payload", &payload,
            Provider_event_problem::WRONG_TYPE, "object_required");
        return std::nullopt;
    }
    Json_value common = payload;
    common.erase("attention_kind");
    const auto base = decode_classify_event_result(common, out_failure, helper_version);
    if (!base) return std::nullopt;
    Attention_classification_result result{*base, std::nullopt};
    const auto* kind = payload.find("attention_kind");
    if (!kind || !kind->is_object()) return result;
    const auto version = strict::integer_member(*kind, "version", 1, limits::k_max_version);
    const auto index = strict::integer_member(*kind, "observation_index", 0, limits::k_max_observations_per_result - 1);
    const auto* name = strict::string_member(*kind, "kind");
    // Unknown or malformed optional metadata cannot disable base observations.
    if (!version || *version != 1 || !index || !name ||
        strict::unexpected_member(*kind, {"version", "observation_index", "kind"}) ||
        (*name != "conversational_hint" && *name != "interactive_question" &&
            *name != "interactive_question_candidate" && *name != "permission_candidate" &&
            *name != "permission") ||
        static_cast<std::size_t>(*index) >= base->observations.size())
    {
        return result;
    }
    const auto& observation = base->observations[static_cast<std::size_t>(*index)];
    const bool inferred_identity = *name == "conversational_hint" ||
        *name == "interactive_question_candidate" || *name == "permission_candidate";
    if (observation.attention_transition == Attention_transition::NONE ||
        (inferred_identity &&
            (observation.attention_evidence != Evidence::INFERRED ||
            observation.attention_request_id.empty())))
    {
        return result;
    }
    result.attention_kind = Attention_kind{static_cast<std::size_t>(*index), *name};
    return result;
}

Json_value encode_async_question_classification_result(const Async_question_classification_result& result)
{
    auto payload = encode_attention_classification_result(result.base);
    if (result.async_questions) {
        const auto& update = *result.async_questions;
        auto record = Json_value::object();
        record.set("version", Json_value(1));
        record.set("observation_index", Json_value(static_cast<std::int64_t>(update.observation_index)));
        const char* kind = update.kind == Async_question_update_kind::STARTED ? "started"
            : update.kind == Async_question_update_kind::ACCEPTED ? "accepted"
            : update.kind == Async_question_update_kind::ANSWERED ? "answered" : "reset";
        record.set("kind", Json_value(kind));
        auto ids = Json_value::array();
        for (const auto& id : update.question_ids) {
            ids.push_back(Json_value(id));
        }
        record.set("question_ids", std::move(ids));
        payload.set("async_questions", std::move(record));
    }
    return payload;
}

std::optional<Async_question_classification_result> decode_async_question_classification_result(
    const Json_value& payload, Contract_failure* out_failure, std::uint16_t helper_version)
{
    if (!payload.is_object()) {
        report_event_failure(out_failure, reason::k_classification_invalid, "payload", &payload,
            Provider_event_problem::WRONG_TYPE, "object_required");
        return std::nullopt;
    }
    auto common = payload;
    common.erase("async_questions");
    const auto base = decode_attention_classification_result(common, out_failure, helper_version);
    if (!base) {
        return std::nullopt;
    }
    Async_question_classification_result result{*base, std::nullopt};
    const auto* record = payload.find("async_questions");
    if (!record || !record->is_object()) {
        return result;
    }
    const auto version = strict::integer_member(*record, "version", 1, limits::k_max_version);
    const auto index = strict::integer_member(*record, "observation_index", 0, limits::k_max_observations_per_result - 1);
    const auto* kind = strict::string_member(*record, "kind");
    const auto* ids = strict::member_of_type(*record, "question_ids", Json_type::ARRAY);
    // Unsupported optional correlation leaves the ordinary lifecycle usable.
    if (!version || *version != 1 || !index || !kind || !ids ||
        strict::unexpected_member(*record, {"version", "observation_index", "kind", "question_ids"}) ||
        (*kind != "started" && *kind != "accepted" && *kind != "answered" && *kind != "reset") ||
        (ids->as_array().empty() != (*kind == "reset")) ||
        ids->as_array().size() > k_max_async_question_ids ||
        static_cast<std::size_t>(*index) >= base->base.observations.size())
    {
        return result;
    }
    Async_question_update update;
    update.observation_index = static_cast<std::size_t>(*index);
    update.kind = *kind == "started" ? Async_question_update_kind::STARTED
        : *kind == "accepted" ? Async_question_update_kind::ACCEPTED
        : *kind == "answered" ? Async_question_update_kind::ANSWERED : Async_question_update_kind::RESET;
    std::unordered_set<std::string> unique_ids;
    for (const auto& id : ids->as_array()) {
        if (!id.is_string() || id.as_string().empty() ||
            id.as_string().size() > k_max_async_question_id_bytes || has_nul(id.as_string()) ||
            !unique_ids.insert(id.as_string()).second)
        {
            return result;
        }
        update.question_ids.push_back(id.as_string());
    }
    const auto& observation = base->base.observations[update.observation_index];
    const bool request = update.kind == Async_question_update_kind::STARTED ||
        update.kind == Async_question_update_kind::ACCEPTED;
    const bool reset = update.kind == Async_question_update_kind::RESET;
    if (observation.provider_session_id.empty() || (!reset && observation.provider_turn_id.empty()) ||
        (request && (observation.attention_transition != Attention_transition::REQUESTED ||
            !base->attention_kind || base->attention_kind->observation_index != update.observation_index ||
            base->attention_kind->kind != "interactive_question_candidate")) ||
        (!request && !reset && observation.phase_transition != Phase_transition::WORKING) ||
        (reset && observation.phase_transition != Phase_transition::AWAITING_INPUT &&
            observation.session_transition != Session_transition::ENDED))
    {
        return result;
    }
    result.async_questions = std::move(update);
    return result;
}

} // namespace logonomic::cli_addon
