#include "sdk_test_support.h"

#include "logonomic_cli_addon/capability_ids.h"
#include "logonomic_cli_addon/managed_configuration.h"
#include "logonomic_cli_addon/transcript.h"

#include <cstdint>
#include <initializer_list>

namespace sdk_tests {

bool test_managed_configuration_contracts()
{
    namespace addon = logonomic::cli_addon;
    constexpr const char* test = "managed_configuration_contracts";
    addon::Contract_failure failure;
    const addon::Managed_configuration_declaration declaration{
        "EXAMPLE_HOME", "config.toml", ".example", {}, {}};
    const addon::Json_value encoded = addon::encode_managed_configuration_declaration(declaration);
    const auto decoded = addon::decode_managed_configuration_declaration(encoded, &failure);
    if (!check(decoded && decoded->environment_name == declaration.environment_name &&
            decoded->relative_path == declaration.relative_path &&
            decoded->default_home_relative_path == declaration.default_home_relative_path,
            test, "A bounded generic target selector must round trip")) return false;

    auto escaped = encoded;
    escaped.set("relative_path", addon::Json_value("../config.toml"));
    if (!check(!addon::decode_managed_configuration_declaration(escaped, &failure), test,
            "A declaration must not escape its resolved home")) return false;
    auto reserved = encoded;
    reserved.set("environment_name", addon::Json_value("LOGONOMIC_TARGET"));
    if (!check(!addon::decode_managed_configuration_declaration(reserved, &failure), test,
            "A declaration must not select a host-reserved environment name")) return false;

    auto raw = declaration;
    raw.selector_policy = "raw";
    raw.default_home_policy = "canonical-home";
    const auto raw_wire = addon::encode_managed_configuration_declaration(raw, 2);
    const auto raw_decoded = addon::decode_managed_configuration_declaration(raw_wire, &failure, 2);
    if (!check(raw_decoded && raw_decoded->version == 2 && raw_decoded->selector_policy == "raw", test,
            "The explicit raw policy belongs to managed-configuration@2")) return false;
    if (!check(!addon::decode_managed_configuration_declaration(raw_wire, &failure), test,
            "An old host cannot silently interpret a raw selector as trimmed")) return false;
    auto bad_policy = raw_wire;
    bad_policy.set("selector_policy", addon::Json_value("unknown"));
    if (!check(!addon::decode_managed_configuration_declaration(bad_policy, &failure, 2), test,
            "Unknown selector policy refuses locally")) return false;

    addon::Managed_configuration_declaration home;
    home.relative_path = ".example/config.json";
    home.version = addon::k_managed_configuration_home_version;
    const auto home_wire = addon::encode_managed_configuration_declaration(home, home.version);
    const auto home_decoded = addon::decode_managed_configuration_declaration(home_wire, &failure, home.version);
    if (!check(home_wire.as_object().size() == 1 && home_decoded &&
            home_decoded->relative_path == home.relative_path && home_decoded->version == home.version &&
            home_decoded->environment_name.empty() && home_decoded->default_home_relative_path.empty(), test,
            "The retained-preimage home authority declares only its relative target")) return false;
    if (!check(!addon::decode_managed_configuration_declaration(home_wire, &failure, 1) &&
            !addon::decode_managed_configuration_declaration(home_wire, &failure, 2), test,
            "Selector contracts cannot silently interpret home authority")) return false;
    for (const char* member : {"environment_name", "default_home_relative_path", "selector_policy", "default_home_policy"}) {
        auto mixed = home_wire;
        mixed.set(member, addon::Json_value("EXAMPLE_HOME"));
        if (!check(!addon::decode_managed_configuration_declaration(mixed, &failure, home.version), test,
                "A home declaration cannot acquire selector or fallback authority")) return false;
    }
    for (const char* path : {"../config.json", "/config.json", ".example/../config.json", ".example\\config.json"}) {
        auto escaped_home = home_wire;
        escaped_home.set("relative_path", addon::Json_value(path));
        if (!check(!addon::decode_managed_configuration_declaration(escaped_home, &failure, home.version), test,
                "Home authority stays within its declared relative target")) return false;
    }

    const addon::Account_profile_managed_configuration_declaration binding{
        addon::k_managed_configuration_version};
    const addon::Json_value binding_wire =
        addon::encode_account_profile_managed_configuration_declaration(binding);
    const auto decoded_binding =
        addon::decode_account_profile_managed_configuration_declaration(binding_wire, &failure);
    if (!check(decoded_binding && decoded_binding->managed_configuration_version ==
            addon::k_managed_configuration_version, test,
            "The static account-profile binding selects managed-configuration@1")) return false;
    auto selector_binding = binding_wire;
    selector_binding.set("managed_configuration_version",
        addon::Json_value((std::int64_t)addon::k_managed_configuration_selector_version));
    const auto decoded_selector_binding =
        addon::decode_account_profile_managed_configuration_declaration(selector_binding, &failure);
    if (!check(decoded_selector_binding && decoded_selector_binding->managed_configuration_version ==
            addon::k_managed_configuration_selector_version, test,
            "The static account-profile binding selects managed-configuration@2")) return false;
    auto malformed_binding = binding_wire;
    malformed_binding.set("unexpected", addon::Json_value(true));
    if (!check(!addon::decode_account_profile_managed_configuration_declaration(
            malformed_binding, &failure), test,
            "The static account-profile binding rejects an unexpected member")) return false;
    malformed_binding = binding_wire;
    malformed_binding.set("managed_configuration_version", addon::Json_value(3));
    if (!check(!addon::decode_account_profile_managed_configuration_declaration(
            malformed_binding, &failure), test,
            "The static account-profile binding rejects an unsupported managed version")) return false;
    for (const addon::Json_value& non_object : {
             addon::Json_value(),
             addon::Json_value::array(),
             addon::Json_value("managed-configuration"),
         })
    {
        if (!check(!addon::decode_account_profile_managed_configuration_declaration(
                non_object, &failure) && failure.reason_code == addon::reason::k_invalid_sidecar,
                test, "The static account-profile binding rejects a non-object envelope")) return false;
    }

    const std::string grant(32, 'a');
    const addon::Managed_configuration_transform_request request{
        grant, true, std::string("a\0b", 3)};
    const auto decoded_request = addon::decode_managed_configuration_transform_request(
        addon::encode_managed_configuration_transform_request(request), &failure);
    if (!check(decoded_request && decoded_request->grant == grant && decoded_request->present &&
            decoded_request->content == request.content, test,
            "The transform request must preserve raw snapshot bytes and its opaque grant")) return false;
    auto invalid_grant = addon::encode_managed_configuration_transform_request(request);
    invalid_grant.set("grant", addon::Json_value("not-a-grant"));
    if (!check(!addon::decode_managed_configuration_transform_request(invalid_grant, &failure), test,
            "The transform request must reject an invalid opaque grant")) return false;
    auto absent_content = addon::encode_managed_configuration_transform_request(request);
    absent_content.set("present", addon::Json_value(false));
    if (!check(!addon::decode_managed_configuration_transform_request(absent_content, &failure), test,
            "A missing configuration must not carry snapshot bytes")) return false;

    const addon::Managed_configuration_transform_result result{grant, "transformed"};
    addon::Transcript_recorder transcript;
    const auto request_frame = addon::encode_managed_configuration_transform_request(request);
    const auto result_frame = addon::encode_managed_configuration_transform_result(result);
    transcript.record_host_frame(request_frame);
    transcript.record_adapter_frame(result_frame);
    for (const auto& line : transcript.lines()) {
        if (!check(line.find(request_frame.find("content_base64")->as_string()) == std::string::npos &&
                line.find(result_frame.find("content_base64")->as_string()) == std::string::npos,
                test, "Configuration snapshot and result bytes must not persist in transcripts")) return false;
    }
    const auto decoded_result = addon::decode_managed_configuration_transform_result(
        addon::encode_managed_configuration_transform_result(result), &failure);
    if (!check(decoded_result && decoded_result->grant == grant &&
            decoded_result->content == result.content, test,
            "The result must echo the grant and preserve transformed bytes")) return false;
    const addon::Managed_configuration_transform_request oversized{
        grant, true, std::string(addon::k_max_managed_configuration_bytes + 1, 'x')};
    return check(!addon::decode_managed_configuration_transform_request(
            addon::encode_managed_configuration_transform_request(oversized), &failure), test,
        "An oversized snapshot must not reach a package");
}

} // namespace sdk_tests
