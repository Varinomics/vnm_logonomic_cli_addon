#include "logonomic_cli_addon/usage_scan.h"
#include "logonomic_cli_addon/stored_usage_accounting.h"
#include "logonomic_cli_addon/package_source.h"
#include "strict_json.h"
#include "usage_cache_p.h"
#include "usage_repository_p.h"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>

namespace logonomic::cli_addon {

namespace {

constexpr std::int64_t k_max_exact_integer = 9007199254740991;
constexpr std::size_t  k_max_key_bytes     = 256;
constexpr std::size_t  k_max_repository_allocations = 4096;
constexpr strict::Name_entry<Usage_support> k_support_names[] = {
    {Usage_support::UNKNOWN, "unknown"},
    {Usage_support::SUPPORTED, "supported"},
    {Usage_support::UNSUPPORTED, "unsupported"},
};

constexpr strict::Name_entry<Usage_counter_semantics> k_semantics_names[] = {
    {Usage_counter_semantics::CUMULATIVE_FINAL, "cumulative-final"},
    {Usage_counter_semantics::DELTA_EVENT,      "delta-event"},
};

bool fail(Contract_failure* out_failure, const char* detail)
{
    if (out_failure != nullptr) {
        out_failure->reason_code = "usage_scan_invalid";
        out_failure->detail      = detail;
    }
    return false;
}

bool valid_key(const Json_value& value)
{
    return value.is_string() && !value.as_string().empty() && value.as_string().size() <= k_max_key_bytes &&
        value.as_string().find('\0') == std::string::npos && is_valid_utf8(value.as_string());
}

bool valid_digest(const std::string& value)
{
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

bool absolute_root(const std::string& value)
{
    return strict::is_bounded_absolute_path(value) &&
        path_from_utf8(value) == path_from_utf8(value).lexically_normal();
}

bool cache_file_name(const std::string& path)
{
    if (path.empty() || path.size() > 128 || path == "." || path == ".." || path.back() == '.' ||
        path == "cache-manifest.json" || path == "cache-manifest.pending" ||
        !std::all_of(path.begin(), path.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        }))
    {
        return false;
    }
    const auto base = path.substr(0, path.find('.'));
    return base != "con" && base != "prn" && base != "aux" && base != "nul" &&
        !(base.size() == 4 && (base.starts_with("com") || base.starts_with("lpt")) &&
            base.back() >= '1' && base.back() <= '9');
}

bool descendant(const std::string& root, const std::string& child)
{
    if (!absolute_root(root) || !absolute_root(child)) {
        return false;
    }
    const auto relative = path_from_utf8(child).lexically_relative(path_from_utf8(root));
    return !relative.empty() && relative != "." && !relative.is_absolute() &&
        std::none_of(relative.begin(), relative.end(), [](const auto& component) { return component == ".."; });
}

bool read_nullable_key(
    const Json_value& object, const char* member, std::optional<std::string>& output, Contract_failure* out_failure)
{
    const Json_value* value = object.find(member);
    if (value == nullptr || (!value->is_null() && !valid_key(*value))) {
        return fail(out_failure, "usage snapshot/cursor must be a bounded key or explicit null");
    }
    if (!value->is_null()) {
        output = value->as_string();
    }
    return true;
}

bool valid_date(std::string_view text)
{
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') {
        return false;
    }
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (index != 4 && index != 7 && (text[index] < '0' || text[index] > '9')) {
            return false;
        }
    }
    const int year  = (text[0] - '0') * 1000 + (text[1] - '0') * 100 + (text[2] - '0') * 10 + text[3] - '0';
    const int month = (text[5] - '0') * 10 + text[6] - '0';
    const int day   = (text[8] - '0') * 10 + text[9] - '0';
    if (year == 0 || month < 1 || month > 12) {
        return false;
    }
    constexpr int k_month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    const int last = k_month_days[month - 1] + (month == 2 && leap ? 1 : 0);
    return day >= 1 && day <= last;
}

// Persisted accounting and public observations are distinct current records.
// Only observations carry aggregate evidence and independently versioned proofs.
enum class Usage_record_contract
{
    STORED_ACCOUNTING,
    SCAN_OBSERVATION,
};

Json_value encode_amount(const Usage_amount& amount)
{
    Json_value value = Json_value::object();
    value.set("applicable", Json_value(amount.applicable));
    if (amount.coverage) {
        Json_value coverage = Json_value::object();
        coverage.set("covered_records", Json_value(amount.coverage->covered_records));
        coverage.set("total_records", Json_value(amount.coverage->total_records));
        coverage.set("overflow", Json_value(amount.coverage->overflow));
        coverage.set("support", Json_value(strict::name_of(k_support_names, amount.coverage->support)));
        value.set("coverage", std::move(coverage));
    }
    if (amount.value) {
        value.set("value", Json_value(*amount.value));
    }
    return value;
}

std::optional<Usage_amount> decode_amount(
    const Json_value& value, Contract_failure* out_failure, Usage_record_contract contract)
{
    const auto* applicable = strict::member_of_type(value, "applicable", Json_type::BOOLEAN);
    if (!value.is_object() || strict::unexpected_member(value, {"applicable", "value", "coverage"}) || applicable == nullptr) {
        fail(out_failure, "usage dimension requires explicit applicability");
        return std::nullopt;
    }
    Usage_amount amount;
    amount.applicable = applicable->as_boolean();
    const auto* coverage = value.find("coverage");
    if (!coverage || !coverage->is_object() || strict::unexpected_member(*coverage,
            {"covered_records", "total_records", "overflow", "support"}))
    {
        fail(out_failure, "usage token dimension requires its coverage record");
        return std::nullopt;
    }
    const auto covered = strict::integer_member(*coverage, "covered_records", 0, k_max_exact_integer);
    const auto total = strict::integer_member(*coverage, "total_records", 0, k_max_exact_integer);
    const auto* overflow = strict::member_of_type(*coverage, "overflow", Json_type::BOOLEAN);
    const auto* support = strict::string_member(*coverage, "support");
    const auto parsed = support ? strict::value_of(k_support_names, *support) : std::nullopt;
    const bool unknown_amount_absent = contract == Usage_record_contract::SCAN_OBSERVATION &&
        parsed == Usage_support::UNKNOWN &&
        value.find("value") == nullptr;
    if (!covered || !total || *covered > *total || !overflow || !parsed ||
        amount.applicable != (*parsed != Usage_support::UNSUPPORTED) ||
        (unknown_amount_absent && *covered != 0) ||
        (*parsed == Usage_support::UNSUPPORTED && (*covered != 0 || overflow->as_boolean())))
    {
        fail(out_failure, "usage coverage support or counts are inconsistent");
        return std::nullopt;
    }
    amount.coverage = Usage_field_coverage{static_cast<std::uint64_t>(*covered),
        static_cast<std::uint64_t>(*total), overflow->as_boolean(), *parsed};
    if (amount.coverage->overflow) {
        if (value.find("value")) {
            fail(out_failure, "overflowed usage cannot claim a known token sum");
            return std::nullopt;
        }
        return amount;
    }
    if (unknown_amount_absent) {
        return amount;
    }
    if (amount.applicable) {
        const auto integer = strict::integer_member(value, "value", 0, k_max_exact_integer);
        if (!integer) {
            fail(out_failure, "applicable usage requires an exact bounded integer value");
            return std::nullopt;
        }
        amount.value = static_cast<std::uint64_t>(*integer);
    }
    else if (value.find("value") != nullptr) {
        fail(out_failure, "inapplicable usage must omit its value");
        return std::nullopt;
    }
    return amount;
}

Json_value encode_record(const Usage_record& record, Usage_record_contract contract)
{
    Json_value value = Json_value::object();
    value.set("record_id",      Json_value(record.record_id));
    value.set("completed_date", Json_value(record.completed_date));
    value.set("total_tokens",   Json_value(record.total_tokens));
    value.set("semantics",      Json_value(usage_counter_semantics_name(record.semantics)));
    value.set("input",          encode_amount(record.input));
    value.set("output",         encode_amount(record.output));
    value.set("cache_read",     encode_amount(record.cache_read));
    value.set("cache_creation", encode_amount(record.cache_creation));
    if (record.session_key) {
        value.set("session_key", Json_value(*record.session_key));
    }
    if (record.source_path) {
        value.set("source_path", Json_value(*record.source_path));
    }
    if (contract == Usage_record_contract::SCAN_OBSERVATION && record.aggregate_evidence) {
        value.set("aggregate_evidence", Json_value(*record.aggregate_evidence));
    }
    if (contract == Usage_record_contract::SCAN_OBSERVATION) {
        Json_value capabilities = record.capabilities;
        if (record.cross_source_event) {
            capabilities.set(k_usage_cross_source_event_capability_id,
                encode_usage_cross_source_event(*record.cross_source_event));
        }
        if (record.temporal) {
            capabilities.set(k_usage_temporal_capability_id, encode_usage_temporal(*record.temporal));
        }
        if (!capabilities.as_object().empty()) {
            value.set("capabilities", std::move(capabilities));
        }
    }
    value.set("coverage_unit", Json_value(record.coverage_unit == Usage_coverage_unit::RECORDS ? "records" : "sessions"));
    if (record.model) value.set("model", Json_value(*record.model));
    if (record.counter_field) value.set("counter_field", Json_value(*record.counter_field));
    if (record.source_counter_semantics) value.set("source_counter_semantics", Json_value(*record.source_counter_semantics));
    if (record.repository_usage) {
        value.set("repository_usage", usage_repository_detail::encode(*record.repository_usage));
    }
    if (record.repository_path) {
        value.set("repository_path", Json_value(*record.repository_path));
    }
    return value;
}

std::optional<Usage_record> decode_record(
    const Json_value& value, Contract_failure* out_failure, Usage_record_contract contract)
{
    const auto* id        = value.find("record_id");
    const auto* date      = strict::string_member(value, "completed_date");
    const auto* semantics = strict::string_member(value, "semantics");
    const auto total      = strict::integer_member(value, "total_tokens", 0, k_max_exact_integer);
    const auto parsed_semantics = semantics != nullptr ? strict::value_of(k_semantics_names, *semantics) : std::nullopt;
    if (!value.is_object() || strict::unexpected_member(value, {
            "record_id", "session_key", "completed_date", "total_tokens", "source_path", "repository_path",
            "semantics", "input", "output", "cache_read", "cache_creation",
            "coverage_unit", "repository_usage", "model", "counter_field", "source_counter_semantics",
            "aggregate_evidence", "capabilities"}) ||
        (contract == Usage_record_contract::STORED_ACCOUNTING &&
            (value.find("aggregate_evidence") || value.find("capabilities"))) ||
        id == nullptr || !valid_key(*id) || date == nullptr || !valid_date(*date) || !total || !parsed_semantics)
    {
        fail(out_failure, "usage record identity, date, total or semantics is invalid");
        return std::nullopt;
    }
    Usage_record record;
    record.record_id      = id->as_string();
    record.completed_date = *date;
    record.total_tokens   = static_cast<std::uint64_t>(*total);
    record.semantics      = *parsed_semantics;
    if (contract == Usage_record_contract::SCAN_OBSERVATION) {
        const auto* evidence = strict::string_member(value, "aggregate_evidence");
        if (!evidence || (*evidence != "complete" && *evidence != "partial")) {
            fail(out_failure, "usage3 requires explicit aggregate evidence");
            return std::nullopt;
        }
        record.aggregate_evidence = *evidence;
        if (const Json_value* capabilities = value.find("capabilities")) {
            if (!capabilities->is_object()) {
                fail(out_failure, "usage record capabilities must be an object");
                return std::nullopt;
            }
            record.capabilities = *capabilities;
            if (const Json_value* proof = capabilities->find(k_usage_cross_source_event_capability_id)) {
                record.cross_source_event = decode_usage_cross_source_event(*proof, nullptr);
            }
            if (const Json_value* temporal = capabilities->find(k_usage_temporal_capability_id)) {
                const Json_value* temporal_version = temporal->find("version");
                if (!temporal->is_object() || !temporal_version || !temporal_version->is_integer() ||
                    temporal_version->as_integer() < 1)
                {
                    fail(out_failure, "usage temporal envelope requires its positive integer version");
                    return std::nullopt;
                }
                if (temporal_version->as_integer() == k_usage_temporal_version) {
                    record.temporal = decode_usage_temporal(*temporal, out_failure);
                    if (!record.temporal || record.temporal->parent.record_id != record.record_id ||
                        record.temporal->parent.total_tokens != record.total_tokens)
                    {
                        fail(out_failure, "usage temporal evidence does not bind its accounting record");
                        return std::nullopt;
                    }
                }
            }
        }
    }
    const auto* unit = strict::string_member(value, "coverage_unit");
    if (!unit || (*unit != "records" && *unit != "sessions")) {
        fail(out_failure, "usage record requires its native coverage unit");
        return std::nullopt;
    }
    record.coverage_unit = *unit == "records" ? Usage_coverage_unit::RECORDS : Usage_coverage_unit::SESSIONS;
    for (const char* member : {"model", "counter_field", "source_counter_semantics"}) {
        if (const auto* field = value.find(member); field != nullptr) {
            if (!field->is_string() || field->as_string().size() > k_max_key_bytes ||
                !is_valid_utf8(field->as_string()) || !strict::has_no_control_characters(field->as_string()))
            {
                fail(out_failure, "usage source metadata is outside its string bound");
                return std::nullopt;
            }
            if (std::string_view(member) == "model") record.model = field->as_string();
            if (std::string_view(member) == "counter_field") record.counter_field = field->as_string();
            if (std::string_view(member) == "source_counter_semantics") {
                if (field->as_string() != "cumulative_final" && field->as_string() != "per_event_sum" &&
                    !(contract == Usage_record_contract::SCAN_OBSERVATION && field->as_string() == "cumulative_turn_delta"))
                {
                    fail(out_failure, "usage source counter semantics is unknown");
                    return std::nullopt;
                }
                record.source_counter_semantics = field->as_string();
            }
        }
    }
    if (const auto* repository = value.find("repository_usage"); repository != nullptr) {
        if (value.find("repository_path")) {
            fail(out_failure, "usage repository allocations are malformed or exceed their bound");
            return std::nullopt;
        }
        record.repository_usage = usage_repository_detail::decode(*repository, record.total_tokens, out_failure);
        if (!record.repository_usage) {
            return std::nullopt;
        }
    }
    if (const auto* session = value.find("session_key"); session != nullptr) {
        if (!valid_key(*session)) {
            fail(out_failure, "usage session key is invalid");
            return std::nullopt;
        }
        record.session_key = session->as_string();
    }
    for (const char* name : {"source_path", "repository_path"}) {
        const Json_value* path = value.find(name);
        if (path == nullptr) {
            continue;
        }
        if (!path->is_string() || !strict::is_bounded_absolute_path(path->as_string())) {
            fail(out_failure, "usage paths must be real absolute paths when present");
            return std::nullopt;
        }
        (std::string_view(name) == "source_path" ? record.source_path : record.repository_path) = path->as_string();
    }
    const struct
    {
        const char* name;
        Usage_amount Usage_record::* member;
    }
    dimensions[] = {
        {"input",          &Usage_record::input},
        {"output",         &Usage_record::output},
        {"cache_read",     &Usage_record::cache_read},
        {"cache_creation", &Usage_record::cache_creation},
    };
    for (const auto& dimension : dimensions) {
        const Json_value* amount = value.find(dimension.name);
        if (amount == nullptr) {
            fail(out_failure, "usage record omits dimension applicability");
            return std::nullopt;
        }
        const auto decoded = decode_amount(*amount, out_failure, contract);
        if (!decoded) {
            return std::nullopt;
        }
        record.*(dimension.member) = *decoded;
    }
    return record;
}

bool byte_less(const std::string& left, const std::string& right)
{
    return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end(),
        [](unsigned char a, unsigned char b) { return a < b; });
}

Json_value encode_page_envelope(const Usage_scan_page& page, std::uint16_t version)
{
    if (version != 3) {
        throw std::invalid_argument("unsupported usage page version");
    }
    Json_value payload = Json_value::object();
    payload.set("scope",                          encode_observation_scope(page.scope));
    payload.set("snapshot_id",                    Json_value(page.snapshot_id));
    payload.set("next_cursor",                    page.next_cursor ? Json_value(*page.next_cursor) : Json_value());
    payload.set("complete",                       Json_value(page.complete));
    payload.set("incomplete_historical_coverage", Json_value(page.incomplete_historical_coverage));
    payload.set("records",                        Json_value::array());
    if (page.cache_candidate) {
        payload.set("cache_candidate", encode_usage_cache_reference(*page.cache_candidate));
    }
    if (page.account_context) {
        payload.set(k_account_context_member, encode_account_context(*page.account_context));
    }
    if (page.profile_storage) {
        payload.set(k_profile_storage_member, profile_storage_member(*page.profile_storage));
    }
    return payload;
}

} // namespace

namespace usage_repository_detail {

Json_value encode(const Usage_repository& value)
{
    Json_value repository = Json_value::object();
    Json_value allocations = Json_value::array();
    for (const auto& source : value.allocations) {
        Json_value allocation = Json_value::object();
        allocation.set("repository_path", Json_value(source.repository_path));
        allocation.set("total_tokens", Json_value(source.total_tokens));
        allocations.push_back(std::move(allocation));
    }
    repository.set("allocations", std::move(allocations));
    repository.set("unattributed_tokens", Json_value(value.unattributed_tokens));
    return repository;
}

std::optional<Usage_repository> decode(
    const Json_value& payload, std::uint64_t total_tokens, Contract_failure* out_failure)
{
    const auto* allocations = strict::member_of_type(payload, "allocations", Json_type::ARRAY);
    const auto unattributed = strict::integer_member(payload, "unattributed_tokens", 0, k_max_exact_integer);
    if (!payload.is_object() || strict::unexpected_member(payload, {"allocations", "unattributed_tokens"}) ||
        !allocations || !unattributed ||
        allocations->as_array().size() > k_max_repository_allocations)
    {
        fail(out_failure, "usage repository allocations are malformed or exceed their bound");
        return std::nullopt;
    }
    Usage_repository attribution;
    attribution.unattributed_tokens = static_cast<std::uint64_t>(*unattributed);
    std::uint64_t sum = attribution.unattributed_tokens;
    std::set<std::string> paths;
    for (const auto& allocation : allocations->as_array()) {
        const auto* path = strict::string_member(allocation, "repository_path");
        const auto tokens = strict::integer_member(allocation, "total_tokens", 0, k_max_exact_integer);
        if (!allocation.is_object() || strict::unexpected_member(allocation, {"repository_path", "total_tokens"}) ||
            !path || !strict::is_bounded_absolute_path(*path) || !paths.insert(*path).second || !tokens ||
            static_cast<std::uint64_t>(*tokens) > total_tokens - std::min(sum, total_tokens))
        {
            fail(out_failure, "usage repository allocation identity or token total is invalid");
            return std::nullopt;
        }
        attribution.allocations.push_back({*path, static_cast<std::uint64_t>(*tokens)});
        sum += static_cast<std::uint64_t>(*tokens);
    }
    if (sum != total_tokens) {
        fail(out_failure, "usage repository allocations do not reconcile to the record total");
        return std::nullopt;
    }
    return attribution;
}

} // namespace usage_repository_detail

Json_value encode_stored_usage_accounting(const Usage_record& record)
{
    return encode_record(record, Usage_record_contract::STORED_ACCOUNTING);
}

std::optional<Usage_record> decode_stored_usage_accounting(
    const Json_value& payload, Contract_failure* out_failure)
{
    return decode_record(payload, out_failure, Usage_record_contract::STORED_ACCOUNTING);
}

Json_value encode_usage_record(const Usage_record& record, std::uint16_t version)
{
    if (version != 3) {
        throw std::invalid_argument("unsupported usage record version");
    }
    return encode_record(record, Usage_record_contract::SCAN_OBSERVATION);
}

std::optional<Usage_record> decode_usage_record(
    const Json_value& payload, Contract_failure* out_failure, std::uint16_t version)
{
    if (version != 3) {
        fail(out_failure, "unsupported usage record version");
        return std::nullopt;
    }
    return decode_record(payload, out_failure, Usage_record_contract::SCAN_OBSERVATION);
}

bool append_usage_page_record(Usage_scan_page& page, const Usage_record& record,
    std::size_t& encoded_records_bytes, std::uint16_t version, std::size_t reserved_payload_bytes)
{
    if (page.records.size() >= k_max_usage_page_records || reserved_payload_bytes > limits::k_max_exchange_payload_bytes) {
        return false;
    }
    const auto envelope_bytes = serialize_json(encode_page_envelope(page, version)).size();
    const auto record_bytes   = serialize_json(encode_record(record, Usage_record_contract::SCAN_OBSERVATION)).size();
    if (envelope_bytes + encoded_records_bytes + record_bytes >
        limits::k_max_exchange_payload_bytes - reserved_payload_bytes)
    {
        return false;
    }
    page.records.push_back(record);
    encoded_records_bytes += record_bytes + 1;
    return true;
}

const char* usage_counter_semantics_name(Usage_counter_semantics semantics)
{
    return strict::name_of(k_semantics_names, semantics);
}

std::optional<Launcher_discovery_declaration> decode_usage_scan_declaration(
    const Json_value& payload, Contract_failure* out_failure)
{
    if (payload.is_object() && payload.as_object().empty()) {
        return Launcher_discovery_declaration{};
    }
    return decode_launcher_discovery_declaration(payload, out_failure);
}

Json_value encode_usage_cache_reference(const Usage_cache_reference& reference)
{
    Json_value value = Json_value::object();
    value.set("root", Json_value(reference.root));
    value.set("snapshot_id", Json_value(reference.snapshot_id));
    value.set("manifest_sha256_hex", Json_value(reference.manifest_sha256_hex));
    return value;
}

std::optional<Usage_cache_reference> decode_usage_cache_reference(
    const Json_value& payload, Contract_failure* out_failure)
{
    const auto* root = strict::string_member(payload, "root");
    const auto* snapshot = payload.find("snapshot_id");
    const auto* hash = strict::string_member(payload, "manifest_sha256_hex");
    if (!payload.is_object() || strict::unexpected_member(payload, {"root", "snapshot_id", "manifest_sha256_hex"}) ||
        !root || !absolute_root(*root) || !snapshot || !valid_key(*snapshot) || !hash || !valid_digest(*hash))
    {
        fail(out_failure, "usage cache reference is invalid");
        return std::nullopt;
    }
    return Usage_cache_reference{*root, snapshot->as_string(), *hash};
}

bool validate_usage_cache_selection(const Usage_scan_request& request, Contract_failure* out_failure)
{
    if (!request.cache_selection || !request.context.profile_storage ||
        !descendant(request.context.profile_storage->root, request.cache_selection->candidate_root))
    {
        return fail(out_failure, "usage cache allocation is outside the acknowledged profile");
    }
    const auto& selected = request.cache_selection->accepted;
    if (selected && (!descendant(request.context.profile_storage->root, selected->root) ||
        selected->root == request.cache_selection->candidate_root ||
        descendant(selected->root, request.cache_selection->candidate_root) ||
        descendant(request.cache_selection->candidate_root, selected->root)))
    {
        return fail(out_failure, "usage cache allocations must be disjoint within the profile");
    }
    if (request.mode == "reset" && selected) {
        return fail(out_failure, "usage3 reset reconstructs from sources without accepted cache");
    }
    return true;
}

Json_value encode_usage_cache_file(const Usage_cache_file& entry)
{
    Json_value file = Json_value::object();
    file.set("path", Json_value(entry.path));
    file.set("bytes", Json_value(entry.bytes));
    file.set("sha256_hex", Json_value(entry.sha256_hex));
    return file;
}

std::optional<Usage_cache_file> decode_usage_cache_file(
    const Json_value& payload, Contract_failure* out_failure)
{
    const auto* path = strict::string_member(payload, "path");
    const auto* hash = strict::string_member(payload, "sha256_hex");
    const auto bytes = strict::integer_member(payload, "bytes", 0, std::numeric_limits<std::int64_t>::max());
    if (!payload.is_object() || strict::unexpected_member(payload, {"path", "bytes", "sha256_hex"}) ||
        !path || !cache_file_name(*path) || !hash || !valid_digest(*hash) || !bytes)
    {
        fail(out_failure, "usage cache inventory entry is invalid");
        return std::nullopt;
    }
    return Usage_cache_file{*path, (std::uint64_t)*bytes, *hash};
}

Json_value encode_usage_cache_manifest(const Usage_cache_manifest& manifest)
{
    Json_value value = Json_value::object();
    value.set("version", Json_value(1));
    value.set("account_context", manifest.account_context ? encode_account_context(*manifest.account_context) : Json_value());
    value.set("snapshot_id", Json_value(manifest.snapshot_id));
    value.set("format", Json_value(manifest.format));
    Json_value files = Json_value::array();
    for (const auto& entry : manifest.files) {
        files.push_back(encode_usage_cache_file(entry));
    }
    value.set("files", std::move(files));
    return value;
}

std::optional<Usage_cache_manifest> decode_usage_cache_manifest(
    const Json_value& payload, Contract_failure* out_failure)
{
    const auto* account = payload.find("account_context");
    const auto* snapshot = payload.find("snapshot_id");
    const auto* format = payload.find("format");
    const auto* files = strict::member_of_type(payload, "files", Json_type::ARRAY);
    if (!payload.is_object() || strict::unexpected_member(payload,
            {"version", "account_context", "snapshot_id", "format", "files"}) ||
        strict::integer_member(payload, "version", 1, 1) != 1 || !account || !snapshot || !valid_key(*snapshot) ||
        !format || !valid_key(*format) || !files)
    {
        fail(out_failure, "usage cache manifest is invalid");
        return std::nullopt;
    }
    Usage_cache_manifest manifest;
    manifest.snapshot_id = snapshot->as_string();
    manifest.format = format->as_string();
    if (!account->is_null()) {
        manifest.account_context = decode_account_context(*account, out_failure);
        if (!manifest.account_context) {
            return std::nullopt;
        }
    }
    std::uint64_t total = 0;
    for (const auto& file : files->as_array()) {
        auto entry = decode_usage_cache_file(file, out_failure);
        if (!entry || (!manifest.files.empty() && manifest.files.back().path >= entry->path) ||
            entry->bytes > std::numeric_limits<std::uint64_t>::max() - total)
        {
            fail(out_failure, "usage cache inventory is invalid or exceeds its bound");
            return std::nullopt;
        }
        total += entry->bytes;
        manifest.files.push_back(std::move(*entry));
    }
    return manifest;
}

Json_value encode_usage_scan_request(const Usage_scan_request& request, std::uint16_t version)
{
    if (version != 3) {
        throw std::invalid_argument("unsupported usage scan request version");
    }
    Json_value payload = encode_probe_prepare_request({request.context}, 2);
    payload.set("mode",        Json_value(request.mode));
    payload.set("snapshot_id", request.snapshot_id ? Json_value(*request.snapshot_id) : Json_value());
    payload.set("cursor",      request.cursor ? Json_value(*request.cursor) : Json_value());
    if (request.cache_selection) {
        Json_value selection = Json_value::object();
        selection.set("version", Json_value(1));
        selection.set("accepted", request.cache_selection->accepted
            ? encode_usage_cache_reference(*request.cache_selection->accepted) : Json_value());
        selection.set("candidate_root", Json_value(request.cache_selection->candidate_root));
        payload.set("cache_selection", std::move(selection));
    }
    return payload;
}

std::optional<Usage_scan_request> decode_usage_scan_request(const Json_value& payload, Contract_failure* out_failure, std::uint16_t version)
{
    if (!payload.is_object() || version != 3) {
        fail(out_failure, "usage scan request must be an object");
        return std::nullopt;
    }
    Usage_scan_request request;
    const auto* mode = strict::string_member(payload, "mode");
    if (mode == nullptr || (*mode != "incremental" && *mode != "repair" && *mode != "reset") ||
        !read_nullable_key(payload, "snapshot_id", request.snapshot_id, out_failure) ||
        !read_nullable_key(payload, "cursor", request.cursor, out_failure) ||
        request.snapshot_id.has_value() != request.cursor.has_value())
    {
        fail(out_failure, "usage scan mode or snapshot/cursor pair is invalid");
        return std::nullopt;
    }
    Json_value context = payload;
    context.erase("mode");
    context.erase("snapshot_id");
    context.erase("cursor");
    const auto* selection = context.find("cache_selection");
    const auto* accepted = selection ? selection->find("accepted") : nullptr;
    const auto* root = selection ? strict::string_member(*selection, "candidate_root") : nullptr;
    if (!selection || !selection->is_object() ||
        strict::unexpected_member(*selection, {"version", "accepted", "candidate_root"}) ||
        strict::integer_member(*selection, "version", 1, 1) != 1 || !accepted || !root)
    {
        fail(out_failure, "usage3 requires a cache selection");
        return std::nullopt;
    }
    request.cache_selection = Usage_cache_selection{std::nullopt, *root};
    if (!accepted->is_null()) {
        request.cache_selection->accepted = decode_usage_cache_reference(*accepted, out_failure);
        if (!request.cache_selection->accepted) {
            return std::nullopt;
        }
    }
    context.erase("cache_selection");
    auto decoded = decode_probe_prepare_request(context, out_failure, 2);
    if (!decoded) {
        return std::nullopt;
    }
    request.mode    = *mode;
    request.context = std::move(decoded->context);
    if (!validate_usage_cache_selection(request, out_failure)) {
        return std::nullopt;
    }
    return request;
}

Json_value encode_usage_scan_result(const Usage_scan_page& page, std::uint16_t version)
{
    Json_value payload = encode_page_envelope(page, version);
    auto* records = payload.find("records");
    for (const auto& record : page.records) {
        records->push_back(encode_record(record, Usage_record_contract::SCAN_OBSERVATION));
    }
    return payload;
}

std::optional<Usage_scan_page> decode_usage_scan_result(const Json_value& payload, Contract_failure* out_failure, std::uint16_t version)
{
    if (version != 3) {
        fail(out_failure, "unsupported usage page version");
        return std::nullopt;
    }
    const auto* scope      = payload.find("scope");
    const auto* snapshot   = payload.find("snapshot_id");
    const auto* complete   = strict::member_of_type(payload, "complete", Json_type::BOOLEAN);
    const auto* incomplete = strict::member_of_type(payload, "incomplete_historical_coverage", Json_type::BOOLEAN);
    const auto* records    = strict::member_of_type(payload, "records", Json_type::ARRAY);
    if (!payload.is_object() || strict::unexpected_member(payload, {
            "scope", "snapshot_id", "next_cursor", "complete", "incomplete_historical_coverage", "records",
            "cache_candidate", k_account_context_member, k_profile_storage_member}) ||
        scope == nullptr || snapshot == nullptr || !valid_key(*snapshot) || complete == nullptr || incomplete == nullptr ||
        records == nullptr || records->as_array().size() > k_max_usage_page_records)
    {
        fail(out_failure, "usage page is missing required fields or exceeds its record bound");
        return std::nullopt;
    }
    Usage_scan_page page;
    const auto decoded_scope = decode_observation_scope(*scope, out_failure);
    if (!decoded_scope || !read_nullable_key(payload, "next_cursor", page.next_cursor, out_failure) ||
        complete->as_boolean() == page.next_cursor.has_value())
    {
        fail(out_failure, "usage page scope or completion cursor is invalid");
        return std::nullopt;
    }
    page.scope                          = *decoded_scope;
    page.snapshot_id                    = snapshot->as_string();
    const auto* storage = payload.find(k_profile_storage_member);
    std::string storage_detail;
    page.profile_storage = storage ? decode_profile_storage_context(*storage, &storage_detail) : std::nullopt;
    if (!page.profile_storage || page.profile_storage->version != k_profile_storage_version) {
        fail(out_failure, "usage3 page requires a profile-storage acknowledgment");
        return std::nullopt;
    }
    if (const auto* account = payload.find(k_account_context_member)) {
        page.account_context = decode_account_context(*account, out_failure);
        if (!page.account_context) {
            return std::nullopt;
        }
    }
    const auto account_key = page.account_context
        ? std::optional<std::string>(page.account_context->adapter_account_key) : std::nullopt;
    if (page.scope.source_account_key != account_key) {
        fail(out_failure, "usage3 page account acknowledgment must match observation scope");
        return std::nullopt;
    }
    const auto* candidate = payload.find("cache_candidate");
    page.cache_candidate = candidate ? decode_usage_cache_reference(*candidate, out_failure) : std::nullopt;
    if (!page.cache_candidate || page.cache_candidate->snapshot_id != page.snapshot_id) {
        fail(out_failure, "usage3 candidate must bind the page snapshot");
        return std::nullopt;
    }
    page.complete                       = complete->as_boolean();
    page.incomplete_historical_coverage = incomplete->as_boolean();
    for (const auto& value : records->as_array()) {
        auto record = decode_record(value, out_failure, Usage_record_contract::SCAN_OBSERVATION);
        if (!record) {
            return std::nullopt;
        }
        if (record->temporal && (record->temporal->parent.snapshot_id != page.snapshot_id ||
                record->temporal->parent.source_account_key != page.scope.source_account_key))
        {
            fail(out_failure, "usage temporal parent does not bind its page snapshot and account");
            return std::nullopt;
        }
        if (!page.records.empty() && !byte_less(page.records.back().record_id, record->record_id)) {
            fail(out_failure, "usage page record IDs must be strictly ascending by UTF-8 bytes");
            return std::nullopt;
        }
        page.records.push_back(std::move(*record));
    }
    return page;
}

} // namespace logonomic::cli_addon
