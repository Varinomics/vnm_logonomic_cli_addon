#include "logonomic_cli_addon/session_observation.h"

#include "strict_json.h"

namespace logonomic::cli_addon {
namespace {

bool reject(Contract_failure* failure, const char* detail)
{
    if (failure) {
        *failure = {reason::k_frame_violation, detail};
    }
    return false;
}

bool version_one(const Json_value& payload)
{
    return strict::integer_member(payload, "version", 1, 1).has_value();
}

bool bounded_identity(const std::string* identity)
{
    return identity && !identity->empty() && identity->size() <= 1024 &&
        is_valid_utf8(*identity) && strict::has_no_control_characters(*identity);
}

bool durable_session_id(const std::string* id)
{
    if (!id || id->size() != 36) {
        return false;
    }
    for (std::size_t index = 0; index < id->size(); ++index) {
        const char value = (*id)[index];
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (value != '-') {
                return false;
            }
        }
        else
        if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'))) {
            return false;
        }
    }
    return true;
}

} // namespace

Json_value encode_session_observation_registration(const Session_observation_registration& registration)
{
    auto identity = Json_value::object();
    identity.set("instance_id", Json_value(registration.identity.instance_id));
    identity.set("group_id", Json_value(registration.identity.group_id));
    identity.set("seat_id", Json_value(registration.identity.seat_id));
    identity.set("occupant_id", Json_value(registration.identity.occupant_id));
    auto payload = Json_value::object();
    payload.set("version", Json_value(1));
    payload.set("request_nonce", Json_value(registration.request_nonce));
    payload.set("identity", std::move(identity));
    payload.set("native_package_id", Json_value(registration.native_package_id));
    payload.set("working_directory", Json_value(registration.working_directory));
    if (registration.replaces_session_id) {
        payload.set("replaces_session_id", Json_value(*registration.replaces_session_id));
    }
    return payload;
}

std::optional<Session_observation_registration> decode_session_observation_registration(
    const Json_value& payload, Contract_failure* failure)
{
    const auto* nonce = strict::string_member(payload, "request_nonce");
    const auto* identity = strict::member_of_type(payload, "identity", Json_type::OBJECT);
    const auto* package = strict::string_member(payload, "native_package_id");
    const auto* directory = strict::string_member(payload, "working_directory");
    const auto* replaces = strict::string_member(payload, "replaces_session_id");
    if (!payload.is_object() || !version_one(payload) ||
        strict::unexpected_member(payload, {"version", "request_nonce", "identity",
            "native_package_id", "working_directory", "replaces_session_id"}) ||
        !nonce || !is_valid_generation_nonce_hex(*nonce) || !identity ||
        !package || !is_valid_package_id(*package) ||
        !directory || !strict::is_bounded_absolute_path(*directory) ||
        (payload.find("replaces_session_id") && !durable_session_id(replaces)) ||
        strict::unexpected_member(*identity, {"instance_id", "group_id", "seat_id", "occupant_id"}))
    {
        reject(failure, "Invalid Session observation registration");
        return std::nullopt;
    }
    const auto* instance = strict::string_member(*identity, "instance_id");
    const auto* group = strict::string_member(*identity, "group_id");
    const auto* seat = strict::string_member(*identity, "seat_id");
    const auto* occupant = strict::string_member(*identity, "occupant_id");
    if (!bounded_identity(instance) || !bounded_identity(group) ||
        !bounded_identity(seat) || !bounded_identity(occupant))
    {
        reject(failure, "Invalid public Session observation identity");
        return std::nullopt;
    }
    return Session_observation_registration{*nonce, {*instance, *group, *seat, *occupant},
        *package, *directory, replaces ? std::optional<std::string>(*replaces) : std::nullopt};
}

Json_value encode_session_observation_authority(const Session_observation_authority& authority)
{
    auto payload = Json_value::object();
    payload.set("version", Json_value(1));
    payload.set("session_id", Json_value(authority.session_id));
    payload.set("session_capability", Json_value(authority.session_capability));
    payload.set("activation_id", Json_value(authority.activation_id));
    return payload;
}

std::optional<Session_observation_authority> decode_session_observation_authority(
    const Json_value& payload, Contract_failure* failure)
{
    const auto* session = strict::string_member(payload, "session_id");
    const auto* capability = strict::string_member(payload, "session_capability");
    const auto* activation = strict::string_member(payload, "activation_id");
    if (!payload.is_object() || !version_one(payload) ||
        strict::unexpected_member(payload, {"version", "session_id", "session_capability", "activation_id"}) ||
        !durable_session_id(session) || !capability || !is_valid_sha256_hex(*capability) ||
        !activation || !is_valid_generation_nonce_hex(*activation))
    {
        reject(failure, "Invalid Session observation authority");
        return std::nullopt;
    }
    return Session_observation_authority{*session, *capability, *activation};
}

Json_value encode_session_observation_retirement(const Session_observation_retirement& retirement)
{
    auto payload = Json_value::object();
    payload.set("version", Json_value(1));
    payload.set("session_id", Json_value(retirement.session_id));
    payload.set("activation_id", Json_value(retirement.activation_id));
    return payload;
}

std::optional<Session_observation_retirement> decode_session_observation_retirement(
    const Json_value& payload, Contract_failure* failure)
{
    const auto* session = strict::string_member(payload, "session_id");
    const auto* activation = strict::string_member(payload, "activation_id");
    if (!payload.is_object() || !version_one(payload) ||
        strict::unexpected_member(payload, {"version", "session_id", "activation_id"}) ||
        !durable_session_id(session) || !activation || !is_valid_generation_nonce_hex(*activation))
    {
        reject(failure, "Invalid Session observation retirement");
        return std::nullopt;
    }
    return Session_observation_retirement{*session, *activation};
}

} // namespace logonomic::cli_addon
