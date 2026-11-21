#include "sdk_test_support.h"

#include "logonomic_cli_addon/startup_observer.h"
#include "logonomic_cli_addon/transcript.h"

namespace sdk_tests {

bool test_startup_observer_contracts()
{
    namespace addon = logonomic::cli_addon;
    constexpr const char* test = "startup_observer_contracts";
    addon::Contract_failure failure;
    addon::Resume_declaration declaration;
    declaration.environment_names = {"EXAMPLE_PROFILE"};
    declaration.startup_observer = addon::Startup_observer_declaration{
        1, addon::k_startup_observer_mechanism, "prepared-identity"};
    const auto declared = addon::encode_resume_declaration(declaration, 2);
    if (!check(bool(addon::decode_resume_declaration(declared, &failure, 2)), test,
            "Version 2 must admit independently declared startup observation") ||
        !check(!addon::decode_resume_declaration(declared, &failure, 1), test,
            "Version 1 must not silently adopt an observer") ||
        !check(bool(addon::decode_resume_declaration(declared, &failure, 3)), test,
            "Inspection retains the version 2 declaration contract") ||
        !check(bool(addon::decode_resume_declaration(declared, &failure, 4)), test,
            "Content inspection retains the version 2 declaration contract") ||
        !check(addon::encode_resume_declaration(declaration, 4) == declared, test,
            "Content inspection does not change startup observation authority") ||
        !check(!addon::decode_resume_declaration(declared, &failure, 5), test,
            "An unknown resume version must remain unsupported")) return false;

    const std::string secret(64, 'a');
    addon::Startup_observer_offer offer{1, addon::k_startup_observer_mechanism,
        "http://127.0.0.1:32123/events", "Bearer " + secret, "prepared-identity"};
    const auto encoded = addon::encode_startup_observer_offer(offer);
    if (!check(bool(addon::decode_startup_observer_offer(encoded, &failure)), test,
            "A host-owned offer must round trip")) return false;
    for (const auto* endpoint : {"http://remote:32123/events", "http://127.0.0.1:0/events",
             "http://127.0.0.1:65536/events", "http://127.0.0.1:1/events?token=x"})
    {
        auto invalid = encoded;
        invalid.set("endpoint", addon::Json_value(endpoint));
        if (!check(!addon::decode_startup_observer_offer(invalid, &failure), test,
                "Offer authority must remain on the exact private loopback endpoint")) return false;
    }
    auto unsupported = encoded;
    unsupported.set("confirmation", addon::Json_value("assume-ready"));
    if (!check(!addon::decode_startup_observer_offer(unsupported, &failure), test,
            "Unknown identity proof must fail closed")) return false;

    const auto request = addon::encode_startup_observation_request({std::string("a\0b", 3)});
    const auto decoded = addon::decode_startup_observation_request(request, &failure);
    if (!check(decoded && decoded->body == std::string("a\0b", 3), test,
            "Opaque authenticated body must preserve bytes") ||
        !check(!addon::decode_startup_observation_request(
            addon::encode_startup_observation_request({std::string(addon::k_max_startup_observation_bytes + 1, 'x')}),
            &failure), test, "Oversized authenticated body must not reach a package")) return false;
    if (!check(!addon::decode_startup_observation_result(addon::encode_startup_observation_result(
            {{"exact-a", "exact-a"}}), &failure), test, "Duplicate identities must be rejected") ||
        !check(bool(addon::decode_startup_observation_result(addon::encode_startup_observation_result(
            {{"exact-a", "different-b"}}), &failure)), test,
            "Distinct observations must remain available for the host's exact identity comparison")) return false;

    addon::Transcript_recorder transcript;
    transcript.record_host_frame(encoded);
    auto response = addon::Json_value::object();
    response.set("environment", addon::Json_value("Authorization=Bearer%20" + secret));
    response.set("startup_observer", encoded);
    transcript.record_adapter_frame(response);
    if (!check(transcript.text().find(secret) == std::string::npos, test,
            "Offer, echoed acknowledgment and encoded exporter environment must not expose authority")) return false;
    transcript.clear();
    transcript.record_adapter_frame(response);
    return check(transcript.text().find(secret) == std::string::npos, test,
        "Clearing recorded lines must not forget a live observer's private authority");
}

} // namespace sdk_tests
