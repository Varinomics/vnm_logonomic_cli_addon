#include "logonomic_cli_addon/cooperative_description.h"

#include "logonomic_cli_addon/capability_ids.h"
#include "logonomic_cli_addon/limits.h"
#include "strict_json.h"

#include <algorithm>
#include <utility>

namespace logonomic::cli_addon {
namespace {

constexpr strict::Name_entry<Description_kind> k_kind_names[] = {
    {Description_kind::TITLE, "title"},
    {Description_kind::SUMMARY, "summary"},
    {Description_kind::SUMMARY_UNCHANGED, "summary_unchanged"},
};

bool text(std::string_view value, std::size_t maximum, bool multiline = false)
{
    return !value.empty() && value.size() <= maximum && is_valid_utf8(value) &&
        std::all_of(value.begin(), value.end(), [multiline](unsigned char ch) {
            return (ch >= 0x20 && ch != 0x7f) || (multiline && (ch == '\n' || ch == '\r' || ch == '\t'));
        });
}

void reject(Contract_failure* failure, const char* detail)
{
    if (failure) *failure = Contract_failure{reason::k_observation_invalid, detail};
}

} // namespace

const char* description_kind_name(Description_kind kind)
{
    return strict::name_of(k_kind_names, kind);
}

bool is_valid_description_submission(const Description_submission& submission)
{
    if (!text(submission.provider_session_id, limits::k_max_lifecycle_identity_bytes) ||
        !is_valid_transition_nonce_hex(submission.transition_nonce) ||
        !text(submission.request_id, limits::k_max_lifecycle_identity_bytes) ||
        (submission.title && !text(*submission.title, limits::k_max_cooperative_title_bytes)) ||
        (submission.summary && !text(*submission.summary, limits::k_max_cooperative_summary_bytes, true)))
    {
        return false;
    }
    switch (submission.kind) {
        case Description_kind::TITLE:
            if (!submission.title || submission.summary) return false;
            break;
        case Description_kind::SUMMARY:
            if (!submission.summary) return false;
            break;
        case Description_kind::SUMMARY_UNCHANGED:
            if (submission.summary) return false;
            break;
        default:
            return false;
    }
    return serialize_json(encode_description_submission(submission)).size() <=
        limits::k_max_cooperative_description_bytes;
}

Json_value encode_description_submission(const Description_submission& submission)
{
    Json_value result = Json_value::object();
    result.set("provider_session_id", Json_value(submission.provider_session_id));
    result.set("transition_nonce", Json_value(submission.transition_nonce));
    result.set("request_id", Json_value(submission.request_id));
    result.set("kind", Json_value(description_kind_name(submission.kind)));
    if (submission.title) result.set("title", Json_value(*submission.title));
    if (submission.summary) result.set("summary", Json_value(*submission.summary));
    return result;
}

std::optional<Description_submission> decode_description_submission(const Json_value& value, Contract_failure* failure)
{
    if (!value.is_object() || strict::unexpected_member(value,
            {"provider_session_id", "transition_nonce", "request_id", "kind", "title", "summary"}))
    {
        reject(failure, "description submission has unsupported members");
        return std::nullopt;
    }
    const auto* id = strict::string_member(value, "provider_session_id");
    const auto* nonce = strict::string_member(value, "transition_nonce");
    const auto* request = strict::string_member(value, "request_id");
    const auto* kind = strict::string_member(value, "kind");
    const auto* title = value.find("title");
    const auto* summary = value.find("summary");
    const auto parsed = kind ? strict::value_of(k_kind_names, *kind) : std::nullopt;
    if (!id || !nonce || !request || !parsed || (title && !title->is_string()) || (summary && !summary->is_string())) {
        reject(failure, "description submission is missing typed members");
        return std::nullopt;
    }
    Description_submission submission{*id, *nonce, *request, *parsed, std::nullopt, std::nullopt};
    if (title) submission.title = title->as_string();
    if (summary) submission.summary = summary->as_string();
    if (!is_valid_description_submission(submission)) {
        reject(failure, "description submission violates its text, identity or action bounds");
        return std::nullopt;
    }
    return submission;
}

Json_value encode_description_receipt(const Description_receipt& receipt)
{
    Json_value result = Json_value::object();
    result.set("version", Json_value(k_cooperative_description_version));
    result.set("request_id", Json_value(receipt.request_id));
    result.set("accepted", Json_value(receipt.accepted));
    result.set("reason", Json_value(receipt.reason));
    return result;
}

std::optional<Description_receipt> decode_description_receipt(const Json_value& value, Contract_failure* failure)
{
    if (!value.is_object() || strict::unexpected_member(value, {"version", "request_id", "accepted", "reason"}) ||
        !strict::integer_member(value, "version", k_cooperative_description_version, k_cooperative_description_version))
    {
        reject(failure, "description receipt is not a supported host result");
        return std::nullopt;
    }
    const auto* id = strict::string_member(value, "request_id");
    const auto* accepted = value.find("accepted");
    const auto* reason_code = strict::string_member(value, "reason");
    if (!id || !text(*id, limits::k_max_lifecycle_identity_bytes) || !accepted || !accepted->is_boolean() ||
        !reason_code || (reason_code->empty() ? !accepted->as_boolean() : !is_valid_reason_code(*reason_code)))
    {
        reject(failure, "description receipt lacks its request identity or disposition");
        return std::nullopt;
    }
    return Description_receipt{*id, accepted->as_boolean(), *reason_code};
}

void encode_cooperative_description_request(Json_value& payload, std::uint32_t version)
{
    if (version == 0) return;
    Json_value envelope = Json_value::object();
    envelope.set("version", Json_value(static_cast<std::uint64_t>(version)));
    payload.set("cooperative_description", std::move(envelope));
}

void decode_cooperative_description_request(
    const Json_value& payload, std::uint32_t& version, std::optional<Contract_failure>& failure)
{
    version = 0;
    failure.reset();
    const auto* envelope = payload.find("cooperative_description");
    if (!envelope) return;
    if (!envelope->is_object() || strict::unexpected_member(*envelope, {"version"}) ||
        !strict::integer_member(*envelope, "version", k_cooperative_description_version, k_cooperative_description_version))
    {
        failure = Contract_failure{reason::k_unsupported_version, "cooperative description request is unsupported"};
        return;
    }
    version = k_cooperative_description_version;
}

void encode_cooperative_description_result(
    Json_value& payload, const std::optional<Description_submission>& submission,
    const std::optional<Contract_failure>& failure)
{
    if (!submission && !failure) return;
    Json_value envelope = Json_value::object();
    envelope.set("version", Json_value(k_cooperative_description_version));
    if (failure) {
        Json_value encoded = Json_value::object();
        encoded.set("reason_code", Json_value(failure->reason_code));
        encoded.set("detail", Json_value(failure->detail));
        envelope.set("failure", std::move(encoded));
    }
    else {
        envelope.set("submission", encode_description_submission(*submission));
    }
    payload.set("cooperative_description", std::move(envelope));
}

void decode_cooperative_description_result(
    const Json_value& payload, std::optional<Description_submission>& submission,
    std::optional<Contract_failure>& failure)
{
    submission.reset();
    failure.reset();
    const auto* envelope = payload.find("cooperative_description");
    if (!envelope) return;
    const auto invalid = [&] {
        failure = Contract_failure{reason::k_observation_invalid, "cooperative description result is malformed"};
    };
    if (!envelope->is_object() || strict::unexpected_member(*envelope, {"version", "submission", "failure"}) ||
        !strict::integer_member(*envelope, "version", k_cooperative_description_version, k_cooperative_description_version))
    {
        invalid();
        return;
    }
    const auto* value = envelope->find("submission");
    if (const auto* reported = envelope->find("failure")) {
        const auto* code = strict::string_member(*reported, "reason_code");
        const auto* detail = strict::string_member(*reported, "detail");
        if (value || !reported->is_object() || strict::unexpected_member(*reported, {"reason_code", "detail"}) ||
            !code || !is_valid_reason_code(*code) || !detail || detail->size() > 1024 || !is_valid_utf8(*detail) ||
            !strict::has_no_control_characters(*detail))
        {
            invalid();
            return;
        }
        failure = Contract_failure{*code, *detail};
        return;
    }
    Contract_failure rejected;
    if (value) submission = decode_description_submission(*value, &rejected);
    if (!submission) invalid();
}

} // namespace logonomic::cli_addon
