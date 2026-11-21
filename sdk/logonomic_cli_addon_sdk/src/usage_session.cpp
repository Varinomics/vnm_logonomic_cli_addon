#include "logonomic_cli_addon/usage_session.h"

#include "strict_json.h"

#include <set>
#include <utility>

namespace logonomic::cli_addon {
namespace {

bool fail(Contract_failure* out_failure, const char* detail)
{
    if (out_failure != nullptr) {
        *out_failure = {"usage_session_invalid", detail};
    }
    return false;
}

bool valid_key(const Json_value* value)
{
    return value && value->is_string() && !value->as_string().empty() &&
        value->as_string().size() <= 256 && value->as_string().find('\0') == std::string::npos &&
        is_valid_utf8(value->as_string());
}

Json_value encode_identity(const Usage_session_identity& identity)
{
    auto payload = Json_value::object();
    payload.set("record_id", Json_value(identity.record_id));
    payload.set("session_key", identity.session_key ? Json_value(*identity.session_key) : Json_value());
    return payload;
}

std::optional<Usage_session_identity> decode_identity(const Json_value& payload)
{
    const auto* record_id   = payload.find("record_id");
    const auto* session_key = payload.find("session_key");
    if (!valid_key(record_id) || !session_key || (!session_key->is_null() && !valid_key(session_key))) {
        return std::nullopt;
    }
    return Usage_session_identity{record_id->as_string(), session_key->is_null()
        ? std::nullopt : std::optional<std::string>(session_key->as_string())};
}

const Json_value* bounded_records(const Json_value& payload)
{
    const auto* records = payload.find("records");
    if (!payload.is_object() || strict::unexpected_member(payload, {"records"}) ||
        !records || !records->is_array() || records->as_array().empty() ||
        records->as_array().size() > k_max_usage_page_records)
    {
        return nullptr;
    }
    return records;
}

} // namespace

bool decode_usage_session_declaration(const Json_value& payload, Contract_failure* out_failure)
{
    if (!payload.is_object() || !payload.as_object().empty()) {
        return fail(out_failure, "usage session declaration must be an empty object");
    }
    return true;
}

Json_value encode_usage_session_fact(const Usage_session_fact& fact)
{
    auto payload = Json_value::object();
    payload.set("version", Json_value((int)k_usage_session_version));
    payload.set("provider_session_id", Json_value(fact.provider_session_id));
    payload.set("excludes_turn_in_flight", Json_value(fact.excludes_turn_in_flight));
    return payload;
}

std::optional<Usage_session_fact> decode_usage_session_fact(
    const Json_value& payload, Contract_failure* out_failure)
{
    const auto* provider_session_id = payload.find("provider_session_id");
    const auto* cutoff = payload.find("excludes_turn_in_flight");
    if (!payload.is_object() || strict::unexpected_member(payload,
            {"version", "provider_session_id", "excludes_turn_in_flight"}) ||
        strict::integer_member(payload, "version", 1, 1) != 1 ||
        !valid_key(provider_session_id) || !cutoff || !cutoff->is_boolean())
    {
        fail(out_failure, "usage session fact is invalid");
        return std::nullopt;
    }
    return Usage_session_fact{provider_session_id->as_string(), cutoff->as_boolean()};
}

Json_value encode_usage_session_request(const std::vector<Usage_session_identity>& records)
{
    auto payload = Json_value::object();
    auto encoded = Json_value::array();
    for (const auto& record : records) {
        encoded.push_back(encode_identity(record));
    }
    payload.set("records", std::move(encoded));
    return payload;
}

std::optional<std::vector<Usage_session_identity>> decode_usage_session_request(
    const Json_value& payload, Contract_failure* out_failure)
{
    const auto* records = bounded_records(payload);
    if (!records) {
        fail(out_failure, "usage session request exceeds its record bound");
        return std::nullopt;
    }
    std::vector<Usage_session_identity> result;
    std::set<std::string> seen;
    for (const auto& record : records->as_array()) {
        auto identity = decode_identity(record);
        if (!record.is_object() || strict::unexpected_member(record, {"record_id", "session_key"}) ||
            !identity || !seen.insert(identity->record_id).second)
        {
            fail(out_failure, "usage session request identity is invalid");
            return std::nullopt;
        }
        result.push_back(std::move(*identity));
    }
    return result;
}

Json_value encode_usage_session_result(const std::vector<Usage_session_description>& records)
{
    auto payload = Json_value::object();
    auto encoded = Json_value::array();
    for (const auto& record : records) {
        auto value = encode_identity(record.identity);
        value.set("session", record.session ? encode_usage_session_fact(*record.session) : Json_value());
        encoded.push_back(std::move(value));
    }
    payload.set("records", std::move(encoded));
    return payload;
}

std::optional<std::vector<Usage_session_description>> decode_usage_session_result(
    const Json_value& payload, Contract_failure* out_failure)
{
    const auto* records = bounded_records(payload);
    if (!records) {
        fail(out_failure, "usage session result exceeds its record bound");
        return std::nullopt;
    }
    std::vector<Usage_session_description> result;
    std::set<std::string> seen;
    for (const auto& record : records->as_array()) {
        auto identity = decode_identity(record);
        const auto* session = record.find("session");
        if (!record.is_object() || strict::unexpected_member(record, {"record_id", "session_key", "session"}) ||
            !identity || !session || !seen.insert(identity->record_id).second)
        {
            fail(out_failure, "usage session result identity is invalid");
            return std::nullopt;
        }
        std::optional<Usage_session_fact> fact;
        if (!session->is_null()) {
            fact = decode_usage_session_fact(*session, out_failure);
            if (!fact) {
                return std::nullopt;
            }
        }
        result.push_back({std::move(*identity), std::move(fact)});
    }
    return result;
}

} // namespace logonomic::cli_addon
