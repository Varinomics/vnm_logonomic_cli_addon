#include "logonomic_cli_addon/startup_observer.h"

#include "logonomic_cli_addon/launcher_discovery.h"
#include "logonomic_cli_addon/session_contracts.h"
#include "strict_json.h"

#include <algorithm>
#include <charconv>
#include <stdexcept>

namespace logonomic::cli_addon {
namespace {

bool reject(Contract_failure* failure, const char* detail)
{
    if (failure) *failure = {reason::k_frame_violation, detail};
    return false;
}

bool valid_mechanism(const Json_value& payload)
{
    const auto* version = payload.find("version");
    const auto* mechanism = strict::string_member(payload, "mechanism");
    const auto* confirmation = strict::string_member(payload, "confirmation");
    return version && version->is_integer() && version->as_integer() == 1 &&
        mechanism && *mechanism == k_startup_observer_mechanism && confirmation &&
        (*confirmation == "observed-identity" || *confirmation == "prepared-identity");
}

bool valid_endpoint(std::string_view endpoint)
{
    constexpr std::string_view prefix = "http://127.0.0.1:";
    constexpr std::string_view suffix = "/events";
    if (!endpoint.starts_with(prefix) || !endpoint.ends_with(suffix)) return false;
    const auto port = endpoint.substr(prefix.size(), endpoint.size() - prefix.size() - suffix.size());
    unsigned value = 0;
    const auto parsed = std::from_chars(port.data(), port.data() + port.size(), value);
    return !port.empty() && parsed.ec == std::errc{} && parsed.ptr == port.data() + port.size() &&
        value > 0 && value <= 65535;
}

std::size_t utf16_units(std::string_view text)
{
    std::size_t result = 0;
    for (const unsigned char byte : text) {
        if ((byte & 0xc0) != 0x80) result += byte >= 0xf0 ? 2 : 1;
    }
    return result;
}

} // namespace

Json_value encode_resume_declaration(const Resume_declaration& declaration, std::uint16_t version)
{
    if (version != 2 && version != 3 && version != 4) {
        throw std::invalid_argument("unsupported interactive resume version");
    }
    auto result = encode_launcher_discovery_declaration({declaration.environment_names});
    if (declaration.startup_observer) {
        auto observer = Json_value::object();
        observer.set("version", Json_value(int(declaration.startup_observer->version)));
        observer.set("mechanism", Json_value(declaration.startup_observer->mechanism));
        observer.set("confirmation", Json_value(declaration.startup_observer->confirmation));
        result.set("startup_observer", std::move(observer));
    }
    return result;
}

std::optional<Resume_declaration> decode_resume_declaration(
    const Json_value& payload, Contract_failure* failure, std::uint16_t version)
{
    if (!payload.is_object() || (version != 2 && version != 3 && version != 4) ||
        strict::unexpected_member(payload, {"environment_names", "startup_observer"}))
    {
        reject(failure, "Invalid interactive resume declaration");
        return std::nullopt;
    }
    Resume_declaration result;
    auto environment = payload;
    environment.erase("startup_observer");
    const auto decoded = decode_launcher_discovery_declaration(environment, failure);
    if (!decoded) return std::nullopt;
    result.environment_names = decoded->environment_names;
    if (const auto* observer = payload.find("startup_observer")) {
        if (!observer->is_object() ||
            strict::unexpected_member(*observer, {"version", "mechanism", "confirmation"}) || !valid_mechanism(*observer))
        {
            reject(failure, "Unsupported startup observer declaration");
            return std::nullopt;
        }
        result.startup_observer.emplace();
        result.startup_observer->confirmation = *strict::string_member(*observer, "confirmation");
    }
    return result;
}

Json_value encode_startup_observer_offer(const Startup_observer_offer& offer)
{
    auto result = Json_value::object();
    result.set("version", Json_value(int(offer.version)));
    result.set("mechanism", Json_value(offer.mechanism));
    result.set("endpoint", Json_value(offer.endpoint));
    result.set("authorization", Json_value(offer.authorization));
    result.set("confirmation", Json_value(offer.confirmation));
    return result;
}

std::optional<Startup_observer_offer> decode_startup_observer_offer(
    const Json_value& payload, Contract_failure* failure)
{
    const auto* endpoint = strict::string_member(payload, "endpoint");
    const auto* authorization = strict::string_member(payload, "authorization");
    if (!payload.is_object() ||
        strict::unexpected_member(payload, {"version", "mechanism", "endpoint", "authorization", "confirmation"}) ||
        !valid_mechanism(payload) || !endpoint || !valid_endpoint(*endpoint) ||
        !authorization || authorization->size() != 71 || !authorization->starts_with("Bearer ") ||
        !std::all_of(authorization->begin() + 7, authorization->end(), [](char byte) {
            return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
        }))
    {
        reject(failure, "Invalid private startup observer offer");
        return std::nullopt;
    }
    return Startup_observer_offer{1, k_startup_observer_mechanism, *endpoint, *authorization,
        *strict::string_member(payload, "confirmation")};
}

Json_value encode_startup_observation_request(const Startup_observation_request& request)
{
    auto result = Json_value::object();
    result.set("body_base64", Json_value(encode_base64(request.body)));
    return result;
}

std::optional<Startup_observation_request> decode_startup_observation_request(
    const Json_value& payload, Contract_failure* failure)
{
    const auto* body = strict::string_member(payload, "body_base64");
    if (!payload.is_object() || strict::unexpected_member(payload, {"body_base64"}) ||
        !body || body->size() > ((k_max_startup_observation_bytes + 2) / 3) * 4)
    {
        reject(failure, "Invalid bounded startup observation");
        return std::nullopt;
    }
    auto decoded = decode_base64(*body);
    if (!decoded || decoded->size() > k_max_startup_observation_bytes) {
        reject(failure, "Invalid startup observation encoding");
        return std::nullopt;
    }
    return Startup_observation_request{std::move(*decoded)};
}

Json_value encode_startup_observation_result(const Startup_observation_result& result)
{
    auto ids = Json_value::array();
    for (const auto& id : result.provider_session_ids) ids.push_back(Json_value(id));
    auto payload = Json_value::object();
    payload.set("provider_session_ids", std::move(ids));
    return payload;
}

std::optional<Startup_observation_result> decode_startup_observation_result(
    const Json_value& payload, Contract_failure* failure)
{
    const auto* ids = strict::member_of_type(payload, "provider_session_ids", Json_type::ARRAY);
    if (!payload.is_object() || strict::unexpected_member(payload, {"provider_session_ids"}) ||
        !ids || ids->as_array().size() > k_max_startup_observation_ids)
    {
        reject(failure, "Invalid startup observation result");
        return std::nullopt;
    }
    Startup_observation_result result;
    for (const auto& id : ids->as_array()) {
        if (!id.is_string() || id.as_string().empty() || id.as_string().size() > 1024 ||
            !is_valid_utf8(id.as_string()) || utf16_units(id.as_string()) > 256 ||
            id.as_string().find('\0') != std::string::npos ||
            std::find(result.provider_session_ids.begin(), result.provider_session_ids.end(),
                id.as_string()) != result.provider_session_ids.end())
        {
            reject(failure, "Invalid or duplicate startup session identity");
            return std::nullopt;
        }
        result.provider_session_ids.push_back(id.as_string());
    }
    return result;
}

} // namespace logonomic::cli_addon
