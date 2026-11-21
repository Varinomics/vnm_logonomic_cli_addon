#include "logonomic_cli_addon/usage_saved_daily_tokens.h"

#include "strict_json.h"

#include <algorithm>
#include <set>
#include <string_view>
#include <utility>

namespace logonomic::cli_addon {
namespace {

constexpr std::int64_t k_max_exact_integer = 9007199254740991;
constexpr std::size_t k_max_model_bytes = 256;
constexpr std::size_t k_max_source_kind_bytes = 64;

bool fail(Contract_failure* out_failure, const char* detail)
{
    if (out_failure != nullptr) {
        *out_failure = {"usage_saved_daily_tokens_invalid", detail};
    }
    return false;
}

bool valid_source_kind(std::string_view value)
{
    return !value.empty() && value.size() <= k_max_source_kind_bytes &&
        std::all_of(value.begin(), value.end(), [](char ch) {
            return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-';
        });
}

bool valid_model(std::string_view value)
{
    return !value.empty() && value.size() <= k_max_model_bytes &&
        strict::has_no_control_characters(value) && is_valid_utf8(value);
}

bool valid_digest(std::string_view value)
{
    return value.size() == 64 &&
        std::all_of(value.begin(), value.end(), [](char ch) {
            return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        });
}

bool valid_date(std::string_view text)
{
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') return false;
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (index != 4 && index != 7 && (text[index] < '0' || text[index] > '9')) return false;
    }
    const int year = (text[0] - '0') * 1000 + (text[1] - '0') * 100 + (text[2] - '0') * 10 + text[3] - '0';
    const int month = (text[5] - '0') * 10 + text[6] - '0';
    const int day = (text[8] - '0') * 10 + text[9] - '0';
    if (year == 0 || month < 1 || month > 12) return false;
    constexpr int k_month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    return day >= 1 && day <= k_month_days[month - 1] + (month == 2 && leap ? 1 : 0);
}

bool valid_source_profile(const Account_context& account)
{
    return account.version == k_accounts_launcher_version && account.profile_path.has_value();
}

bool checked_add(std::uint64_t value, std::uint64_t* total)
{
    if (value > static_cast<std::uint64_t>(k_max_exact_integer) - *total) return false;
    *total += value;
    return true;
}

} // namespace

bool decode_usage_saved_daily_tokens_declaration(
    const Json_value& payload, Contract_failure* out_failure)
{
    if (!payload.is_object() || !payload.as_object().empty()) {
        return fail(out_failure, "usage saved daily tokens declaration must be an empty object");
    }
    return true;
}

Json_value encode_usage_saved_daily_tokens_request(const Usage_saved_daily_tokens_request& request)
{
    Json_value payload = Json_value::object();
    payload.set(k_account_context_member, encode_account_context(request.account_context));
    return payload;
}

std::optional<Usage_saved_daily_tokens_request> decode_usage_saved_daily_tokens_request(
    const Json_value& payload, Contract_failure* out_failure)
{
    if (!payload.is_object() || strict::unexpected_member(payload, {k_account_context_member})) {
        fail(out_failure, "usage saved daily tokens request has unexpected members");
        return std::nullopt;
    }
    const Json_value* account_value = payload.find(k_account_context_member);
    const auto account = account_value ? decode_account_context(*account_value, out_failure) : std::nullopt;
    if (!account || !valid_source_profile(*account)) {
        fail(out_failure, "usage saved daily tokens requires an admitted accounts@2 source profile");
        return std::nullopt;
    }
    return Usage_saved_daily_tokens_request{*account};
}

Json_value encode_usage_saved_daily_tokens_result(const Usage_saved_daily_tokens& summary)
{
    Json_value payload = Json_value::object();
    payload.set(k_account_context_member, encode_account_context(summary.account_context));
    payload.set("source_kind", Json_value(summary.source_kind));
    payload.set("source_format_version", summary.source_format_version
        ? Json_value(*summary.source_format_version) : Json_value());
    payload.set("daily_series_format_version", summary.daily_series_format_version
        ? Json_value(*summary.daily_series_format_version) : Json_value());
    payload.set("source_content_digest", Json_value(summary.source_content_digest));
    payload.set("cache_stated_date", summary.cache_stated_date
        ? Json_value(*summary.cache_stated_date) : Json_value());
    payload.set("historical_principal", Json_value(summary.historical_principal));
    Json_value days = Json_value::array();
    for (const Usage_saved_daily_day& day : summary.days) {
        Json_value row = Json_value::object();
        row.set("date", Json_value(day.date));
        Json_value models = Json_value::array();
        for (const Usage_saved_daily_model& model : day.models) {
            Json_value item = Json_value::object();
            item.set("model", Json_value(model.model));
            item.set("tokens", Json_value(model.tokens));
            models.push_back(std::move(item));
        }
        row.set("models", std::move(models));
        days.push_back(std::move(row));
    }
    payload.set("days", std::move(days));
    return payload;
}

std::optional<Usage_saved_daily_tokens> decode_usage_saved_daily_tokens_result(
    const Json_value& payload, Contract_failure* out_failure)
{
    if (!payload.is_object() || strict::unexpected_member(payload, {
            k_account_context_member, "source_kind", "source_format_version", "daily_series_format_version",
            "source_content_digest", "cache_stated_date", "historical_principal", "days"}))
    {
        fail(out_failure, "usage saved daily tokens result has unexpected members");
        return std::nullopt;
    }
    const Json_value* account_value = payload.find(k_account_context_member);
    const auto account = account_value ? decode_account_context(*account_value, out_failure) : std::nullopt;
    const std::string* source_kind = strict::string_member(payload, "source_kind");
    const std::string* digest = strict::string_member(payload, "source_content_digest");
    const std::string* principal = strict::string_member(payload, "historical_principal");
    const Json_value* version = payload.find("source_format_version");
    const Json_value* series_version = payload.find("daily_series_format_version");
    const Json_value* date = payload.find("cache_stated_date");
    const Json_value* days = strict::member_of_type(payload, "days", Json_type::ARRAY);
    const auto valid_version = [](const Json_value* value) {
        return value && (value->is_null() || (value->is_integer() &&
            value->as_integer() >= 0 && value->as_integer() <= k_max_exact_integer));
    };
    if (!account || !valid_source_profile(*account) || !source_kind || !valid_source_kind(*source_kind) ||
        !digest || !valid_digest(*digest) || !principal || *principal != "unknown" ||
        !valid_version(version) || !valid_version(series_version) ||
        !date || (!date->is_null() && (!date->is_string() || !valid_date(date->as_string()))) ||
        !days || days->as_array().size() > k_max_usage_saved_daily_days)
    {
        fail(out_failure, "usage saved daily tokens metadata or source profile is invalid");
        return std::nullopt;
    }
    Usage_saved_daily_tokens summary;
    summary.account_context = *account;
    summary.source_kind = *source_kind;
    if (!version->is_null()) summary.source_format_version = version->as_integer();
    if (!series_version->is_null()) summary.daily_series_format_version = series_version->as_integer();
    summary.source_content_digest = *digest;
    if (!date->is_null()) summary.cache_stated_date = date->as_string();
    std::set<std::string> unique_days;
    std::size_t model_rows = 0;
    std::uint64_t total = 0;
    for (const Json_value& row : days->as_array()) {
        const std::string* day = strict::string_member(row, "date");
        const Json_value* models = strict::member_of_type(row, "models", Json_type::ARRAY);
        if (!row.is_object() || strict::unexpected_member(row, {"date", "models"}) ||
            !day || !valid_date(*day) || !unique_days.insert(*day).second || !models ||
            models->as_array().size() > k_max_usage_saved_daily_models_per_day ||
            models->as_array().size() > k_max_usage_saved_daily_model_rows - model_rows)
        {
            fail(out_failure, "usage saved daily tokens day is invalid");
            return std::nullopt;
        }
        Usage_saved_daily_day parsed_day;
        parsed_day.date = *day;
        std::set<std::string> unique_models;
        for (const Json_value& item : models->as_array()) {
            const std::string* model = strict::string_member(item, "model");
            const auto tokens = strict::integer_member(item, "tokens", 0, k_max_exact_integer);
            if (!item.is_object() || strict::unexpected_member(item, {"model", "tokens"}) ||
                !model || !valid_model(*model) || !unique_models.insert(*model).second || !tokens ||
                !checked_add(static_cast<std::uint64_t>(*tokens), &total))
            {
                fail(out_failure, "usage saved daily tokens model or token total is invalid");
                return std::nullopt;
            }
            parsed_day.models.push_back({*model, static_cast<std::uint64_t>(*tokens)});
        }
        model_rows += models->as_array().size();
        summary.days.push_back(std::move(parsed_day));
    }
    return summary;
}

} // namespace logonomic::cli_addon
