#include "sdk_test_support.h"

#include "logonomic_cli_addon/session_observation.h"
#include "logonomic_cli_addon/transcript.h"

namespace sdk_tests {

bool test_session_observation_contracts()
{
    namespace addon = logonomic::cli_addon;
    constexpr const char* test = "session_observation_contracts";
    addon::Contract_failure failure;
    const std::string session_id = "12c7cd11-0ac6-4e7d-9a6d-d9009b562661";
    const addon::Session_observation_registration registration{
        std::string(32, 'a'), {"native-instance", "rig", "seat", "occupant"},
        "example.native", std::filesystem::temp_directory_path().generic_string(), session_id};
    const auto encoded = addon::encode_session_observation_registration(registration);
    const auto decoded = addon::decode_session_observation_registration(encoded, &failure);
    if (!check(decoded && *decoded == registration, test,
            "Registration must preserve exact native identity and explicit replacement")) {
        return false;
    }
    for (const auto* field : {"session_id", "session_capability", "runtime_session_id", "activation_id"}) {
        auto forged = encoded;
        forged.set(field, addon::Json_value("caller-selected"));
        if (!check(!addon::decode_session_observation_registration(forged, &failure), test,
                "A registrant cannot supply host identity, authority or native process ownership")) {
            return false;
        }
    }
    auto invalid = encoded;
    invalid.set("working_directory", addon::Json_value("relative"));
    if (!check(!addon::decode_session_observation_registration(invalid, &failure), test,
            "An observed native working directory must be absolute")) {
        return false;
    }
    invalid = encoded;
    invalid.set("version", addon::Json_value(2));
    if (!check(!addon::decode_session_observation_registration(invalid, &failure), test,
            "Unsupported observation versions must remain capability local")) {
        return false;
    }
    invalid = encoded;
    invalid.set("replaces_session_id", addon::Json_value());
    if (!check(!addon::decode_session_observation_registration(invalid, &failure), test,
            "Replacement is absent or an exact durable Session ID")) {
        return false;
    }
    const addon::Session_observation_authority authority{
        session_id, std::string(64, 'b'), std::string(32, 'c')};
    const auto authority_payload = addon::encode_session_observation_authority(authority);
    const auto decoded_authority = addon::decode_session_observation_authority(authority_payload, &failure);
    if (!check(decoded_authority && *decoded_authority == authority, test,
            "Host authority must retain a launch activation distinct from public occupant identity")) {
        return false;
    }
    const auto retired = addon::decode_session_observation_retirement(
        addon::encode_session_observation_retirement({session_id, authority.activation_id}), &failure);
    if (!check(retired && retired->session_id == session_id &&
            retired->activation_id == authority.activation_id, test,
            "Retirement must name the exact host launch activation")) {
        return false;
    }
    addon::Transcript_recorder transcript;
    auto envelope = addon::Json_value::object();
    envelope.set("authority", authority_payload);
    transcript.record_host_frame(envelope);
    auto echo = addon::Json_value::object();
    echo.set("detail", addon::Json_value("echo=" + authority.session_capability));
    transcript.record_adapter_frame(echo);
    transcript.record_event("rejected", "session=" + authority.session_capability);
    if (!check(transcript.text().find(authority.session_capability) == std::string::npos &&
            transcript.text().find("<session-authority>") != std::string::npos &&
            transcript.text().find(authority.session_id) != std::string::npos, test,
            "Observation credentials and subsequent echoes must be redacted without hiding durable identity")) {
        return false;
    }
    transcript.clear();
    transcript.record_adapter_frame(echo);
    return check(transcript.text().find(authority.session_capability) == std::string::npos, test,
        "Clearing transcript lines must not forget known private authority");
}

} // namespace sdk_tests
