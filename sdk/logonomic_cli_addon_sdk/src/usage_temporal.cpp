#include "logonomic_cli_addon/usage_temporal.h"

#include "strict_json.h"
#include "usage_temporal_p.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <set>
#include <string>
#include <string_view>

namespace logonomic::cli_addon {
namespace {

constexpr std::int64_t k_max_exact_integer = 9007199254740991;

bool fail(Contract_failure* out_failure, const char* detail)
{
    if (out_failure) {
        *out_failure = {"usage_temporal_invalid", detail};
    }
    return false;
}

bool valid_key(const std::string& value)
{
    return !value.empty() && value.size() <= 256 &&
        strict::has_no_control_characters(value) && is_valid_utf8(value);
}

int decimal(std::string_view text)
{
    int value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            return -1;
        }
        value = value * 10 + ch - '0';
    }
    return value;
}

bool valid_date(std::string_view text, bool allow_year_zero = false)
{
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') {
        return false;
    }
    const int year  = decimal(text.substr(0, 4));
    const int month = decimal(text.substr(5, 2));
    const int day   = decimal(text.substr(8, 2));
    return (year > 0 || (allow_year_zero && year == 0)) && month > 0 && day > 0 &&
        std::chrono::year_month_day{std::chrono::year(year),
            std::chrono::month(static_cast<unsigned>(month)), std::chrono::day(static_cast<unsigned>(day))}.ok();
}

bool valid_instant(std::string_view text)
{
    if (text.size() < 20 || !valid_date(text.substr(0, 10), true) ||
        (text[10] != 'T' && text[10] != 't') || text[13] != ':' || text[16] != ':')
    {
        return false;
    }
    const int hour   = decimal(text.substr(11, 2));
    const int minute = decimal(text.substr(14, 2));
    const int second = decimal(text.substr(17, 2));
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 60) {
        return false;
    }
    std::size_t offset = 19;
    if (text[offset] == '.') {
        const std::size_t start = ++offset;
        while (offset < text.size() && text[offset] >= '0' && text[offset] <= '9') {
            ++offset;
        }
        if (offset == start) {
            return false;
        }
    }
    int offset_minutes = 0;
    if (!(offset == text.size() - 1 && (text[offset] == 'Z' || text[offset] == 'z'))) {
        if (text.size() - offset != 6 || (text[offset] != '+' && text[offset] != '-') || text[offset + 3] != ':') {
            return false;
        }
        const int offset_hour   = decimal(text.substr(offset + 1, 2));
        const int offset_minute = decimal(text.substr(offset + 4, 2));
        // The capability requires a known offset; RFC 3339 -00:00 declares it unknown.
        if (offset_hour < 0 || offset_hour > 23 || offset_minute < 0 || offset_minute > 59 ||
            (text[offset] == '-' && offset_hour == 0 && offset_minute == 0))
        {
            return false;
        }
        offset_minutes = (offset_hour * 60 + offset_minute) * (text[offset] == '-' ? -1 : 1);
    }
    if (second != 60) return true;
    // Check the RFC 3339 leap-second position in UTC without changing the
    // original representation or freezing a table of future announcements.
    const std::chrono::year_month_day date{std::chrono::year(decimal(text.substr(0, 4))),
        std::chrono::month(static_cast<unsigned>(decimal(text.substr(5, 2)))),
        std::chrono::day(static_cast<unsigned>(decimal(text.substr(8, 2))))};
    const auto utc = std::chrono::sys_days{date} + std::chrono::hours(hour) + std::chrono::minutes(minute - offset_minutes);
    const auto utc_day = std::chrono::floor<std::chrono::days>(utc);
    const std::chrono::year_month_day utc_date{utc_day};
    const std::chrono::year_month_day_last month_end{utc_date.year(), std::chrono::month_day_last{utc_date.month()}};
    return utc - utc_day == std::chrono::hours(23) + std::chrono::minutes(59) && utc_date.day() == month_end.day();
}

bool valid_digest(const std::string& value)
{
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    });
}

} // namespace

namespace usage_temporal_detail {

Json_value encode_portions(const std::vector<Usage_temporal_portion>& values)
{
    Json_value portions = Json_value::array();
    for (const auto& portion : values) {
        Json_value row = Json_value::object();
        row.set("basis", Json_value(portion.basis));
        row.set("total_tokens", Json_value(portion.total_tokens));
        if (portion.basis == "instant") {
            row.set("instant", Json_value(portion.instant));
        }
        else {
            row.set("date", Json_value(portion.date));
            row.set("calendar", Json_value(portion.calendar));
        }
        portions.push_back(std::move(row));
    }
    return portions;
}

std::optional<std::vector<Usage_temporal_portion>> decode_portions(
    const Json_value& portions,
    std::uint64_t total_tokens,
    std::uint64_t unavailable_tokens,
    Contract_failure* out_failure)
{
    if (!portions.is_array() || portions.as_array().size() > k_max_usage_temporal_portions ||
        unavailable_tokens > total_tokens)
    {
        fail(out_failure, "usage temporal portions or unavailable remainder exceed their bounds");
        return std::nullopt;
    }
    std::vector<Usage_temporal_portion> result;
    std::uint64_t sum = unavailable_tokens;
    std::set<std::string> positions;
    for (const auto& row : portions.as_array()) {
        const auto* basis = strict::string_member(row, "basis");
        const auto tokens = strict::integer_member(row, "total_tokens", 1, k_max_exact_integer);
        const auto* instant = strict::string_member(row, "instant");
        const auto* date = strict::string_member(row, "date");
        const auto* calendar = strict::string_member(row, "calendar");
        if (!row.is_object() || !basis || !tokens || static_cast<std::uint64_t>(*tokens) > total_tokens - sum ||
            (*basis == "instant" ?
                strict::unexpected_member(row, {"basis", "instant", "total_tokens"}) || !instant || !valid_instant(*instant) :
                *basis != "source-calendar" || strict::unexpected_member(row, {"basis", "date", "calendar", "total_tokens"}) ||
                !date || !valid_date(*date) || !calendar || !valid_key(*calendar)))
        {
            fail(out_failure, "usage temporal portion is invalid or exceeds its parent amount");
            return std::nullopt;
        }
        const std::string position = *basis == "instant" ? "instant:" + *instant : "calendar:" + *calendar + ':' + *date;
        if (!positions.insert(position).second) {
            fail(out_failure, "usage temporal portions must combine repeated temporal positions");
            return std::nullopt;
        }
        result.push_back({*basis, instant ? *instant : "", date ? *date : "", calendar ? *calendar : "",
            static_cast<std::uint64_t>(*tokens)});
        sum += static_cast<std::uint64_t>(*tokens);
    }
    if (sum != total_tokens) {
        fail(out_failure, "usage temporal portions and unavailable remainder do not reconcile");
        return std::nullopt;
    }
    return result;
}

} // namespace usage_temporal_detail

std::optional<std::int64_t> parse_native_usage_timestamp(std::string_view iso)
{
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (iso.size() < 19 || std::sscanf(std::string(iso.substr(0, 19)).c_str(),
            "%4d-%2d-%2dT%2d:%2d:%2d", &year, &month, &day, &hour, &minute, &second) != 6)
    {
        return std::nullopt;
    }
    const std::chrono::year_month_day date{std::chrono::year{year},
        std::chrono::month{static_cast<unsigned>(month)}, std::chrono::day{static_cast<unsigned>(day)}};
    if (!date.ok() || year < 1 || year > 9999 || hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 || second < 0 || second > 59)
    {
        return std::nullopt;
    }
    std::size_t offset = 19;
    std::int64_t milliseconds = 0;
    if (offset < iso.size() && iso[offset] == '.') {
        ++offset;
        const auto begin = offset;
        while (offset < iso.size() && iso[offset] >= '0' && iso[offset] <= '9') {
            if (offset - begin < 3) {
                milliseconds = milliseconds * 10 + (iso[offset] - '0');
            }
            ++offset;
        }
        if (offset == begin) {
            return std::nullopt;
        }
        for (auto digits = offset - begin; digits < 3; ++digits) milliseconds *= 10;
    }
    if (offset == iso.size()) {
        std::tm local{};
        local.tm_year = year - 1900;
        local.tm_mon = month - 1;
        local.tm_mday = day;
        local.tm_hour = hour;
        local.tm_min = minute;
        local.tm_sec = second;
        local.tm_isdst = -1;
        const auto seconds = std::mktime(&local);
        if (seconds == static_cast<std::time_t>(-1)) return std::nullopt;
        return static_cast<std::int64_t>(seconds) * 1000 + milliseconds;
    }
    int zone_minutes = 0;
    if (offset < iso.size() && iso[offset] == 'Z' && offset + 1 == iso.size()) {
        ++offset;
    }
    else
    if (offset + 6 == iso.size() && (iso[offset] == '+' || iso[offset] == '-') && iso[offset + 3] == ':') {
        int zone_hour = 0, zone_minute = 0;
        if (std::sscanf(std::string(iso.substr(offset + 1)).c_str(), "%2d:%2d", &zone_hour, &zone_minute) != 2 ||
            zone_hour > 23 || zone_minute > 59)
        {
            return std::nullopt;
        }
        zone_minutes = (zone_hour * 60 + zone_minute) * (iso[offset] == '-' ? -1 : 1);
        offset = iso.size();
    }
    else {
        return std::nullopt;
    }
    if (offset != iso.size()) {
        return std::nullopt;
    }
    const std::int64_t days = std::chrono::sys_days(date).time_since_epoch().count();
    return ((days * 24 + hour) * 3600 + minute * 60 + second - zone_minutes * 60) * 1000 + milliseconds;
}

bool decode_usage_temporal_declaration(const Json_value& payload, Contract_failure* out_failure)
{
    return (payload.is_object() && payload.as_object().empty()) ||
        fail(out_failure, "usage temporal declaration must be an empty object");
}

Json_value encode_usage_temporal(const Usage_temporal& evidence)
{
    Json_value payload = Json_value::object();
    payload.set("version", Json_value(static_cast<int>(k_usage_temporal_version)));
    payload.set("metric", Json_value("total_tokens"));
    Json_value parent = Json_value::object();
    parent.set("record_id", Json_value(evidence.parent.record_id));
    parent.set("total_tokens", Json_value(evidence.parent.total_tokens));
    parent.set("snapshot_id", Json_value(evidence.parent.snapshot_id));
    parent.set("source_account_key", evidence.parent.source_account_key
        ? Json_value(*evidence.parent.source_account_key) : Json_value());
    payload.set("parent", std::move(parent));
    payload.set("portions", usage_temporal_detail::encode_portions(evidence.portions));
    payload.set("unavailable_tokens", Json_value(evidence.unavailable_tokens));
    payload.set("coverage_reason", Json_value(evidence.coverage_reason));
    if (evidence.retained_from) {
        Json_value retained = Json_value::object();
        retained.set("snapshot_id", Json_value(evidence.retained_from->snapshot_id));
        retained.set("manifest_sha256_hex", Json_value(evidence.retained_from->manifest_sha256_hex));
        payload.set("retained_from", std::move(retained));
    }
    return payload;
}

std::optional<Usage_temporal> decode_usage_temporal(const Json_value& payload, Contract_failure* out_failure)
{
    const auto version = strict::integer_member(payload, "version", 1, 1);
    const auto* metric = strict::string_member(payload, "metric");
    const auto* parent = strict::member_of_type(payload, "parent", Json_type::OBJECT);
    const auto* portions = strict::member_of_type(payload, "portions", Json_type::ARRAY);
    const auto remainder = strict::integer_member(payload, "unavailable_tokens", 0, k_max_exact_integer);
    const auto* reason = strict::string_member(payload, "coverage_reason");
    if (!payload.is_object() || strict::unexpected_member(payload,
            {"version", "metric", "parent", "portions", "unavailable_tokens", "coverage_reason", "retained_from"}) ||
        !version || !metric || *metric != "total_tokens" || !parent || !portions || !remainder || !reason ||
        (*reason != "complete" && *reason != "missing-evidence" && *reason != "capacity" && *reason != "unsupported") ||
        (*reason == "complete" && *remainder != 0) || (*reason == "unsupported" && !portions->as_array().empty()) ||
        portions->as_array().size() > k_max_usage_temporal_portions ||
        serialize_json(payload).size() > k_max_usage_temporal_bytes)
    {
        fail(out_failure, "usage temporal envelope or bounds are invalid");
        return std::nullopt;
    }
    const auto* id = strict::string_member(*parent, "record_id");
    const auto total = strict::integer_member(*parent, "total_tokens", 0, k_max_exact_integer);
    const auto* snapshot = strict::string_member(*parent, "snapshot_id");
    const auto* account = parent->find("source_account_key");
    if (strict::unexpected_member(*parent, {"record_id", "total_tokens", "snapshot_id", "source_account_key"}) ||
        !id || !valid_key(*id) || !total || !snapshot || !valid_key(*snapshot) || !account ||
        (!account->is_null() && (!account->is_string() || !valid_key(account->as_string()))) || *remainder > *total)
    {
        fail(out_failure, "usage temporal parent is invalid");
        return std::nullopt;
    }
    Usage_temporal evidence;
    evidence.parent = {*id, static_cast<std::uint64_t>(*total), *snapshot,
        account->is_null() ? std::nullopt : std::optional(account->as_string())};
    evidence.unavailable_tokens = static_cast<std::uint64_t>(*remainder);
    evidence.coverage_reason = *reason;
    auto decoded_portions = usage_temporal_detail::decode_portions(
        *portions, evidence.parent.total_tokens, evidence.unavailable_tokens, out_failure);
    if (!decoded_portions) {
        return std::nullopt;
    }
    evidence.portions = std::move(*decoded_portions);
    if (const auto* retained = payload.find("retained_from")) {
        const auto* retained_snapshot = strict::string_member(*retained, "snapshot_id");
        const auto* digest = strict::string_member(*retained, "manifest_sha256_hex");
        if (!retained->is_object() || strict::unexpected_member(*retained, {"snapshot_id", "manifest_sha256_hex"}) ||
            !retained_snapshot || !valid_key(*retained_snapshot) || !digest || !valid_digest(*digest))
        {
            fail(out_failure, "usage temporal retained parent is invalid");
            return std::nullopt;
        }
        evidence.retained_from = Usage_temporal_retained_from{*retained_snapshot, *digest};
    }
    return evidence;
}

} // namespace logonomic::cli_addon
