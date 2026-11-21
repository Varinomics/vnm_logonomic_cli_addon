#include "logonomic_cli_addon/accounts.h"
#include "logonomic_cli_addon/limits.h"
#include "logonomic_cli_addon/package_source.h"
#include "logonomic_cli_addon/presentation_record.h"
#include "logonomic_cli_addon/sha256.h"
#include "strict_json.h"

#include <filesystem>
#include <iterator>
#include <set>
#include <stdexcept>
#include <utility>

namespace logonomic::cli_addon {

namespace {

constexpr strict::Name_entry<Account_status> k_status_names[] = {
    {Account_status::UNKNOWN,     "unknown"},
    {Account_status::AVAILABLE,   "available"},
    {Account_status::UNAVAILABLE, "unavailable"},
};

void fail(Contract_failure* out_failure, const char* detail)
{
    if (out_failure != nullptr) {
        out_failure->reason_code = "account_record_invalid";
        out_failure->detail      = detail;
    }
}

std::optional<Account_record> decode_account(const Json_value& value, Contract_failure* out_failure)
{
    if (!value.is_object() || strict::unexpected_member(value, {
            "adapter_account_key", "display_name", "status", "profile_path"}).has_value())
    {
        fail(out_failure, "account must contain only its declared members");
        return std::nullopt;
    }
    const auto* key     = strict::string_member(value, "adapter_account_key");
    const auto* display = strict::string_member(value, "display_name");
    const auto* status  = strict::string_member(value, "status");
    if (key == nullptr || !is_valid_adapter_account_key(*key) || display == nullptr ||
        !is_valid_utf8(*display) || status == nullptr) {
        fail(out_failure, "account key, display name or status is missing or invalid");
        return std::nullopt;
    }
    std::string name_detail;
    const auto name           = normalized_display_name(*display, &name_detail);
    const auto account_status = strict::value_of(k_status_names, *status);
    if (!name || !account_status) {
        fail(out_failure, "account display name or status is outside its grammar");
        return std::nullopt;
    }
    Account_record account{*key, *name, *account_status, std::nullopt};
    if (const Json_value* path = value.find("profile_path"); path != nullptr) {
        if (!path->is_string() || !strict::is_bounded_absolute_path(path->as_string())) {
            fail(out_failure, "profile_path must be an absolute nonempty UTF-8 path within its bound");
            return std::nullopt;
        }
        account.profile_path = path->as_string();
    }
    return account;
}

} // namespace

Json_value encode_accounts_launcher_request(const Accounts_launcher_request& request)
{
    Json_value payload = Json_value::object();
    payload.set("launcher", encode_launcher_request(request.launcher));
    payload.set("launch_environment", encode_launch_environment(request.launch_environment));
    payload.set("default_home_fallback", Json_value(request.default_home_fallback));
    payload.set("normalization_directory", Json_value(request.normalization_directory));
    return payload;
}

std::optional<Accounts_launcher_request> decode_accounts_launcher_request(
    const Json_value& payload, Contract_failure* out_failure)
{
    const auto reject = [&]() -> std::optional<Accounts_launcher_request> {
        if (out_failure) {
            out_failure->reason_code = reason::k_frame_violation;
            out_failure->detail = "accounts launcher input is invalid";
        }
        return std::nullopt;
    };
    if (!payload.is_object() || strict::unexpected_member(payload,
            {"launcher", "launch_environment", "default_home_fallback", "normalization_directory"}))
    {
        return reject();
    }
    const auto* launcher = payload.find("launcher");
    const auto* environment = payload.find("launch_environment");
    const auto* fallback = strict::string_member(payload, "default_home_fallback");
    const auto* directory = strict::string_member(payload, "normalization_directory");
    if (!launcher || !environment || !fallback || !directory ||
        !strict::is_bounded_absolute_path(*directory) ||
        fallback->size() > limits::k_max_environment_value_bytes ||
        fallback->find('\0') != std::string::npos || !is_valid_utf8(*fallback))
    {
        return reject();
    }
    auto parsed_launcher = decode_launcher_request(*launcher, out_failure);
    auto parsed_environment = decode_launch_environment(*environment, out_failure);
    if (!parsed_launcher || !parsed_environment) return std::nullopt;
    return Accounts_launcher_request{
        std::move(*parsed_launcher), std::move(*parsed_environment), *fallback, *directory};
}

bool is_valid_adapter_account_key(std::string_view key)
{
    return !key.empty() && key.size() <= limits::k_max_adapter_account_key_bytes &&
           key.find('\0') == std::string_view::npos && is_valid_utf8(key);
}

const char* account_status_name(Account_status status)
{
    return strict::name_of(k_status_names, status);
}

std::string account_identity(std::string_view package_id, std::string_view adapter_account_key)
{
    Sha256 digest;
    for (const std::string_view member : {package_id, std::string_view("adapter-account"), adapter_account_key}) {
        digest.update(std::to_string(member.size()));
        digest.update(":");
        digest.update(member);
    }
    return "a" + to_hex(digest.finish());
}

std::string account_display_name(std::string_view profile_path)
{
    auto folder = path_from_utf8(profile_path).lexically_normal();
    if (!folder.has_filename()) folder = folder.parent_path();
    // A UNC share, \\server\share, is a root although the filesystem library
    // parses \\server as the root name and the share as a relative component.
    const auto root_name = folder.root_name().native();
    const auto relative  = folder.relative_path();
    const bool share_root =
        root_name.size() > 2                          &&
        (root_name[0] == '\\' || root_name[0] == '/') &&
        (root_name[1] == '\\' || root_name[1] == '/') &&
        std::distance(relative.begin(), relative.end()) == 1;
    auto name = share_root ? std::nullopt : fitted_display_name(path_to_utf8(folder.filename()));
    // A root, or a folder whose name is only white space, has no usable name.
    if (!name) name = fitted_display_name(path_to_utf8(folder));
    if (!name) throw std::invalid_argument("account_display_name: the profile path names no folder");
    return *name;
}

Json_value encode_accounts(const std::vector<Account_record>& accounts)
{
    Json_value values = Json_value::array();
    for (const auto& account : accounts) {
        Json_value value = Json_value::object();
        value.set("adapter_account_key", Json_value(account.adapter_account_key));
        value.set("display_name",        Json_value(account.display_name));
        value.set("status",              Json_value(account_status_name(account.status)));
        if (account.profile_path) {
            value.set("profile_path", Json_value(*account.profile_path));
        }
        values.push_back(std::move(value));
    }
    Json_value payload = Json_value::object();
    payload.set("accounts", std::move(values));
    return payload;
}

std::optional<std::vector<Account_record>> decode_accounts_result(
    const Json_value& payload, Contract_failure* out_failure)
{
    const Json_value* values = strict::member_of_type(payload, "accounts", Json_type::ARRAY);
    if (values == nullptr || strict::unexpected_member(payload, {"accounts"}).has_value() ||
        values->as_array().size() > limits::k_max_discovered_accounts)
    {
        fail(out_failure, "accounts result must contain a bounded accounts array");
        return std::nullopt;
    }
    std::vector<Account_record> accounts;
    std::set<std::string> keys;
    for (const auto& value : values->as_array()) {
        auto account = decode_account(value, out_failure);
        if (!account) {
            return std::nullopt;
        }
        if (!keys.insert(account->adapter_account_key).second) {
            fail(out_failure, "accounts result repeats an account key");
            return std::nullopt;
        }
        accounts.push_back(std::move(*account));
    }
    return accounts;
}

Json_value encode_account_resolution(const Account_resolution& resolution)
{
    Json_value payload = Json_value::object();
    payload.set("adapter_account_key", resolution.adapter_account_key
        ? Json_value(*resolution.adapter_account_key) : Json_value());
    return payload;
}

std::optional<Account_resolution> decode_account_resolution_result(
    const Json_value& payload, Contract_failure* out_failure)
{
    const Json_value* key = payload.find("adapter_account_key");
    if (key == nullptr || strict::unexpected_member(payload, {"adapter_account_key"}).has_value() ||
        (!key->is_null() && (!key->is_string() || !is_valid_adapter_account_key(key->as_string()))))
    {
        fail(out_failure, "account resolution must contain a valid account key or null");
        return std::nullopt;
    }
    Account_resolution result;
    if (!key->is_null()) {
        result.adapter_account_key = key->as_string();
    }
    return result;
}

Json_value encode_account_context(const Account_context& context)
{
    Json_value value = Json_value::object();
    value.set("version",             Json_value(static_cast<int>(context.version)));
    value.set("adapter_account_key", Json_value(context.adapter_account_key));
    if (context.version == 2) {
        value.set("profile_path", context.profile_path ? Json_value(*context.profile_path) : Json_value());
    }
    return value;
}

std::optional<Account_context> decode_account_context(const Json_value& value, Contract_failure* out_failure)
{
    const auto version = strict::integer_member(value, "version", 1, limits::k_max_version);
    const auto* key    = strict::string_member(value, "adapter_account_key");
    if (!version || (*version != 1 && *version != 2) || key == nullptr || !is_valid_adapter_account_key(*key) ||
        strict::unexpected_member(value, {"version", "adapter_account_key", "profile_path"}).has_value() ||
        (*version == 1 && value.find("profile_path")))
    {
        fail(out_failure, "account context version or key is unsupported or invalid");
        return std::nullopt;
    }
    Account_context context{static_cast<std::uint16_t>(*version), *key};
    if (*version == 2) {
        const auto* path = value.find("profile_path");
        if (!path || (!path->is_null() && (!path->is_string() ||
                !strict::is_bounded_absolute_path(path->as_string()))))
        {
            fail(out_failure, "account profile locator is missing or invalid");
            return std::nullopt;
        }
        if (!path->is_null()) context.profile_path = path->as_string();
    }
    return context;
}

std::optional<std::string> account_context_acknowledgment_failure(
    const Account_context& context, const Json_value& result_payload)
{
    const Json_value* value = result_payload.find(k_account_context_member);
    if (value == nullptr) {
        return "account_context_unacknowledged";
    }
    const auto decoded = decode_account_context(*value, nullptr);
    if (!decoded || decoded->version != context.version ||
        decoded->adapter_account_key != context.adapter_account_key || decoded->profile_path != context.profile_path)
    {
        return "account_context_mismatch";
    }
    return std::nullopt;
}

} // namespace logonomic::cli_addon
