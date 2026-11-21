#include "logonomic_cli_addon/usage_owned_event_correction.h"

#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/limits.h"
#include "logonomic_cli_addon/package_source.h"
#include "strict_json.h"
#include "usage_repository_p.h"
#include "usage_temporal_p.h"

#include <algorithm>
#include <limits>
#include <string_view>
#include <utility>

namespace logonomic::cli_addon {
namespace {

constexpr std::int64_t k_max_exact_integer = 9007199254740991;

bool fail(Contract_failure* out_failure, const char* detail)
{
    if (out_failure) {
        *out_failure = {"usage_owned_event_correction_invalid", detail};
    }
    return false;
}

bool supported_version(const Json_value& payload, Contract_failure* out_failure)
{
    const auto version = strict::integer_member(payload, "version", 1, std::numeric_limits<std::int64_t>::max());
    if (!version) {
        return fail(out_failure, "owned event correction requires a positive integer version");
    }
    if (*version != k_usage_owned_event_correction_version) {
        if (out_failure) {
            *out_failure = {reason::k_unsupported_version, "owned event correction version is unavailable"};
        }
        return false;
    }
    return true;
}

bool valid_digest(const std::string& value)
{
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    });
}

bool valid_record_id(const Json_value& value)
{
    return value.is_string() && !value.as_string().empty() && value.as_string().size() <= 256 &&
        value.as_string().find('\0') == std::string::npos && is_valid_utf8(value.as_string());
}

bool absolute_root(const std::string& value)
{
    return strict::is_bounded_absolute_path(value) &&
        path_from_utf8(value) == path_from_utf8(value).lexically_normal();
}

bool descendant(const std::string& root, const std::string& path)
{
    if (!absolute_root(root) || !absolute_root(path)) {
        return false;
    }
    const auto relative = path_from_utf8(path).lexically_relative(path_from_utf8(root));
    return !relative.empty() && relative != "." && !relative.is_absolute() &&
        std::none_of(relative.begin(), relative.end(), [](const auto& part) { return part == ".."; });
}

bool within_payload_bound(const Json_value& payload, Contract_failure* out_failure)
{
    return serialize_json(payload).size() <= limits::k_max_exchange_payload_bytes ||
        fail(out_failure, "owned event correction exceeds the transport payload bound");
}

Json_value encode_counter(const Usage_owned_event_counter_removal& counter)
{
    auto payload = Json_value::object();
    payload.set("value", Json_value(counter.value));
    payload.set("covered_records", Json_value(counter.covered_records));
    return payload;
}

std::optional<Usage_owned_event_counter_removal> decode_counter(
    const Json_value& payload, std::uint64_t total_records, Contract_failure* out_failure)
{
    const auto value   = strict::integer_member(payload, "value", 0, k_max_exact_integer);
    const auto covered = strict::integer_member(payload, "covered_records", 0, k_max_exact_integer);
    if (!payload.is_object() || strict::unexpected_member(payload, {"value", "covered_records"}) ||
        !value || !covered || (std::uint64_t)*covered > total_records || (*covered == 0 && *value != 0))
    {
        fail(out_failure, "removed counter value or occurrence coverage is invalid");
        return std::nullopt;
    }
    return Usage_owned_event_counter_removal{(std::uint64_t)*value, (std::uint64_t)*covered};
}

Json_value encode_removal(const Usage_owned_event_removal& removal)
{
    auto payload = Json_value::object();
    payload.set("total_tokens", Json_value(removal.total_tokens));
    payload.set("total_records", Json_value(removal.total_records));
    payload.set("input", encode_counter(removal.input));
    payload.set("output", encode_counter(removal.output));
    payload.set("cache_read", encode_counter(removal.cache_read));
    payload.set("cache_creation", encode_counter(removal.cache_creation));
    payload.set("repository_usage", usage_repository_detail::encode(removal.repository_usage));
    auto temporal = Json_value();
    if (removal.temporal) {
        temporal = Json_value::object();
        temporal.set("portions", usage_temporal_detail::encode_portions(removal.temporal->portions));
        temporal.set("unavailable_tokens", Json_value(removal.temporal->unavailable_tokens));
    }
    payload.set("temporal", std::move(temporal));
    return payload;
}

std::optional<Usage_owned_event_removal> decode_removal(
    const Json_value& payload, Contract_failure* out_failure)
{
    const auto total   = strict::integer_member(payload, "total_tokens", 0, k_max_exact_integer);
    const auto records = strict::integer_member(payload, "total_records", 0, k_max_exact_integer);
    const auto* repository = payload.find("repository_usage");
    const auto* temporal   = payload.find("temporal");
    if (!payload.is_object() || strict::unexpected_member(payload,
            {"total_tokens", "total_records", "input", "output", "cache_read", "cache_creation",
             "repository_usage", "temporal"}) ||
        !total || !records || (*records == 0 && *total != 0) || !repository || !temporal)
    {
        fail(out_failure, "owned event removal requires its exact accounting vector");
        return std::nullopt;
    }
    Usage_owned_event_removal removal;
    removal.total_tokens  = (std::uint64_t)*total;
    removal.total_records = (std::uint64_t)*records;
    const struct
    {
        const char* name;
        Usage_owned_event_counter_removal Usage_owned_event_removal::* member;
    }
    counters[] = {
        {"input",          &Usage_owned_event_removal::input},
        {"output",         &Usage_owned_event_removal::output},
        {"cache_read",     &Usage_owned_event_removal::cache_read},
        {"cache_creation", &Usage_owned_event_removal::cache_creation},
    };
    for (const auto& counter : counters) {
        const auto* value = payload.find(counter.name);
        const auto decoded = value ? decode_counter(*value, removal.total_records, out_failure) : std::nullopt;
        if (!decoded) {
            fail(out_failure, "owned event removal requires valid counters and occurrence coverage");
            return std::nullopt;
        }
        removal.*(counter.member) = *decoded;
    }
    auto attribution = usage_repository_detail::decode(*repository, removal.total_tokens, out_failure);
    if (!attribution) {
        return std::nullopt;
    }
    removal.repository_usage = std::move(*attribution);
    if (!temporal->is_null()) {
        const auto* portions = temporal->find("portions");
        const auto unavailable = strict::integer_member(*temporal, "unavailable_tokens", 0, k_max_exact_integer);
        if (!temporal->is_object() || strict::unexpected_member(*temporal, {"portions", "unavailable_tokens"}) ||
            !portions || !unavailable || serialize_json(*temporal).size() > k_max_usage_temporal_bytes)
        {
            fail(out_failure, "removed temporal evidence must contain only its subtraction vector");
            return std::nullopt;
        }
        auto decoded = usage_temporal_detail::decode_portions(
            *portions, removal.total_tokens, (std::uint64_t)*unavailable, out_failure);
        if (!decoded) {
            return std::nullopt;
        }
        removal.temporal = Usage_owned_event_temporal_removal{std::move(*decoded), (std::uint64_t)*unavailable};
    }
    return removal;
}

} // namespace

bool decode_usage_owned_event_correction_declaration(
    const Json_value& payload, Contract_failure* out_failure)
{
    return (payload.is_object() && payload.as_object().empty()) ||
        fail(out_failure, "owned event correction declaration must be an empty object");
}

Json_value encode_usage_owned_event_correction_request(const Usage_owned_event_correction_request& request)
{
    auto payload      = Json_value::object();
    auto predecessors = Json_value::object();
    predecessors.set("path", Json_value(request.predecessors.path));
    predecessors.set("sha256_hex", Json_value(request.predecessors.sha256_hex));
    payload.set("scan", encode_usage_scan_request(request.scan, k_usage_scan_acceptance_version));
    payload.set("predecessors", std::move(predecessors));
    if (request.scan.context.account_context) {
        payload.set(k_account_context_member, encode_account_context(*request.scan.context.account_context));
    }
    if (request.scan.context.profile_storage) {
        payload.set(k_profile_storage_member, profile_storage_member(*request.scan.context.profile_storage));
    }
    return payload;
}

std::optional<Usage_owned_event_correction_request> decode_usage_owned_event_correction_request(
    const Json_value& payload, Contract_failure* out_failure)
{
    const auto* scan         = payload.find("scan");
    const auto* predecessors = payload.find("predecessors");
    if (!payload.is_object() || strict::unexpected_member(payload,
            {"scan", "predecessors", k_account_context_member, k_profile_storage_member}) ||
        !scan || !predecessors || !within_payload_bound(payload, out_failure))
    {
        fail(out_failure, "owned event correction request requires a scan and predecessors");
        return std::nullopt;
    }
    auto decoded = decode_usage_scan_request(*scan, out_failure, k_usage_scan_acceptance_version);
    if (!decoded) {
        return std::nullopt;
    }
    const auto* account = payload.find(k_account_context_member);
    const auto* storage = payload.find(k_profile_storage_member);
    if ((account != nullptr) != decoded->context.account_context.has_value() ||
        (storage != nullptr) != decoded->context.profile_storage.has_value())
    {
        fail(out_failure, "owned event correction transport context presence differs from its scan");
        return std::nullopt;
    }
    if (account) {
        const auto outer = decode_account_context(*account, out_failure);
        if (!outer) {
            return std::nullopt;
        }
        const auto& nested = *decoded->context.account_context;
        if (outer->version != nested.version || outer->adapter_account_key != nested.adapter_account_key ||
            outer->profile_path != nested.profile_path)
        {
            fail(out_failure, "owned event correction transport account differs from its scan");
            return std::nullopt;
        }
    }
    if (storage) {
        std::string detail;
        const auto outer = decode_profile_storage_context(*storage, &detail);
        const auto& nested = *decoded->context.profile_storage;
        if (!outer || outer->version != nested.version || outer->root != nested.root) {
            fail(out_failure, "owned event correction transport profile storage is invalid or differs from its scan");
            return std::nullopt;
        }
    }
    const auto* path   = strict::string_member(*predecessors, "path");
    const auto* digest = strict::string_member(*predecessors, "sha256_hex");
    if (!predecessors->is_object() || strict::unexpected_member(*predecessors, {"path", "sha256_hex"}) ||
        !path || !digest || !valid_digest(*digest) || !descendant(decoded->context.profile_storage->root, *path))
    {
        fail(out_failure, "owned event predecessors require a profile-scoped path and SHA256");
        return std::nullopt;
    }
    return Usage_owned_event_correction_request{std::move(*decoded), {*path, *digest}};
}

Json_value encode_usage_owned_event_predecessor_header(const Usage_owned_event_predecessor_header& header)
{
    auto payload = Json_value::object();
    payload.set("version", Json_value((int)k_usage_owned_event_correction_version));
    payload.set("provider_id", Json_value(header.provider_id));
    payload.set("account_context", header.account_context ? encode_account_context(*header.account_context) : Json_value());
    payload.set("profile_root", Json_value(header.profile_root));
    payload.set("accepted_cache", header.accepted_cache ? encode_usage_cache_reference(*header.accepted_cache) : Json_value());
    return payload;
}

std::optional<Usage_owned_event_predecessor_header> decode_usage_owned_event_predecessor_header(
    const Json_value& payload, Contract_failure* out_failure)
{
    if (!supported_version(payload, out_failure)) {
        return std::nullopt;
    }
    const auto* provider = strict::string_member(payload, "provider_id");
    const auto* root     = strict::string_member(payload, "profile_root");
    const auto* account  = payload.find("account_context");
    const auto* cache    = payload.find("accepted_cache");
    if (strict::unexpected_member(payload, {"version", "provider_id", "account_context", "profile_root", "accepted_cache"}) ||
        !provider || !is_valid_package_id(*provider) || !root || !absolute_root(*root) || !account || !cache ||
        !within_payload_bound(payload, out_failure))
    {
        fail(out_failure, "owned event predecessor header is invalid");
        return std::nullopt;
    }
    Usage_owned_event_predecessor_header header;
    header.provider_id  = *provider;
    header.profile_root = *root;
    if (!account->is_null()) {
        header.account_context = decode_account_context(*account, out_failure);
        if (!header.account_context) {
            return std::nullopt;
        }
    }
    if (!cache->is_null()) {
        header.accepted_cache = decode_usage_cache_reference(*cache, out_failure);
        if (!header.accepted_cache || !descendant(header.profile_root, header.accepted_cache->root)) {
            fail(out_failure, "owned event predecessor cache is outside its profile");
            return std::nullopt;
        }
    }
    return header;
}

Json_value encode_usage_owned_event_predecessor(const Usage_owned_event_predecessor& predecessor)
{
    auto payload = Json_value::object();
    payload.set("record", encode_usage_record(predecessor.record, k_usage_scan_acceptance_version));
    payload.set("predecessor_sha256_hex", Json_value(predecessor.predecessor_sha256_hex));
    payload.set("temporal_acceptance", predecessor.temporal_acceptance);
    return payload;
}

std::optional<Usage_owned_event_predecessor> decode_usage_owned_event_predecessor(
    const Json_value& payload, Contract_failure* out_failure)
{
    const auto* record     = payload.find("record");
    const auto* digest     = strict::string_member(payload, "predecessor_sha256_hex");
    const auto* acceptance = payload.find("temporal_acceptance");
    if (!payload.is_object() || strict::unexpected_member(payload,
            {"record", "predecessor_sha256_hex", "temporal_acceptance"}) ||
        !record || !digest || !valid_digest(*digest) || !acceptance ||
        (!acceptance->is_null() && !acceptance->is_object()) || !within_payload_bound(payload, out_failure))
    {
        fail(out_failure, "owned event predecessor requires its projection, digest and temporal acceptance");
        return std::nullopt;
    }
    auto decoded = decode_usage_record(*record, out_failure, k_usage_scan_acceptance_version);
    if (!decoded) {
        return std::nullopt;
    }
    return Usage_owned_event_predecessor{std::move(*decoded), *digest, *acceptance};
}

Json_value encode_usage_owned_event_correction(const Usage_owned_event_correction& correction)
{
    auto payload = Json_value::object();
    payload.set("version", Json_value((int)k_usage_owned_event_correction_version));
    payload.set("disposition", Json_value(correction.disposition));
    payload.set("predecessor_sha256_hex", Json_value(correction.predecessor_sha256_hex));
    if (correction.disposition == "correct") {
        payload.set("coverage_policy", Json_value(correction.coverage_policy));
        payload.set("removed", correction.removed ? encode_removal(*correction.removed) : Json_value());
        auto owners = Json_value::array();
        for (const auto& owner : correction.original_record_ids) {
            owners.push_back(Json_value(owner));
        }
        payload.set("original_record_ids", std::move(owners));
    }
    return payload;
}

std::optional<Usage_owned_event_correction> decode_usage_owned_event_correction(
    const Json_value& payload, Contract_failure* out_failure)
{
    if (!supported_version(payload, out_failure)) {
        return std::nullopt;
    }
    const auto* disposition = strict::string_member(payload, "disposition");
    const auto* digest      = strict::string_member(payload, "predecessor_sha256_hex");
    if (!disposition || (*disposition != "correct" && *disposition != "retain") ||
        !digest || !valid_digest(*digest) || !within_payload_bound(payload, out_failure))
    {
        fail(out_failure, "owned event correction disposition or predecessor digest is invalid");
        return std::nullopt;
    }
    Usage_owned_event_correction correction;
    correction.disposition            = *disposition;
    correction.predecessor_sha256_hex = *digest;
    if (*disposition == "retain") {
        if (strict::unexpected_member(payload, {"version", "disposition", "predecessor_sha256_hex"})) {
            fail(out_failure, "owned event retention cannot carry removal authority");
            return std::nullopt;
        }
        return correction;
    }
    const auto* policy  = strict::string_member(payload, "coverage_policy");
    const auto* removed = payload.find("removed");
    const auto* owners  = strict::member_of_type(payload, "original_record_ids", Json_type::ARRAY);
    if (strict::unexpected_member(payload,
            {"version", "disposition", "predecessor_sha256_hex", "coverage_policy", "removed", "original_record_ids"}) ||
        !policy || (*policy != "records" && *policy != "retained_unknown_sessions") ||
        !removed || !owners || owners->as_array().empty())
    {
        fail(out_failure, "owned event correction requires its removal policy and original owners");
        return std::nullopt;
    }
    correction.coverage_policy = *policy;
    correction.removed = decode_removal(*removed, out_failure);
    if (!correction.removed) {
        return std::nullopt;
    }
    for (const auto& owner : owners->as_array()) {
        if (!valid_record_id(owner) || (!correction.original_record_ids.empty() &&
                correction.original_record_ids.back() >= owner.as_string()))
        {
            fail(out_failure, "owned event original IDs must be sorted, unique and valid stable IDs");
            return std::nullopt;
        }
        correction.original_record_ids.push_back(owner.as_string());
    }
    return correction;
}

} // namespace logonomic::cli_addon
