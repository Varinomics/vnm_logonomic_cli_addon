#include "sdk_test_support.h"

#include "logonomic_cli_addon/environment_bootstrap.h"
#include "logonomic_cli_addon/launcher_discovery.h"
#include "logonomic_cli_addon/package_source.h"

namespace sdk_tests {

bool test_environment_bootstrap_contracts()
{
    namespace addon = logonomic::cli_addon;
    constexpr const char* test = "environment_bootstrap_contracts";
    addon::Environment_bootstrap_declaration declaration{
        {"EXAMPLE_HOME", "EXAMPLE_RUNTIME"}, "EXAMPLE_HOME", ".example", ".env"};
    addon::Contract_failure failure;
    const auto decoded = addon::decode_environment_bootstrap_declaration(
        addon::encode_environment_bootstrap_declaration(declaration), &failure);
    if (!check(decoded && decoded->environment_names == declaration.environment_names &&
            decoded->home_environment_name == declaration.home_environment_name,
            test, "A bounded bootstrap declaration must round trip")) return false;
    addon::Launch_environment legacy{"linux-x86_64", addon::Json_value::object(), std::nullopt};
    const auto old_host = addon::decode_launch_environment(addon::encode_launch_environment(legacy), &failure);
    if (!check(old_host && !old_host->bootstrap, test,
            "An older host cannot imply successful bootstrap")) return false;
    legacy.bootstrap = addon::Environment_bootstrap_evidence{addon::path_to_utf8(std::filesystem::current_path())};
    auto frozen = addon::encode_launch_environment(legacy);
    const auto current = addon::decode_launch_environment(frozen, &failure);
    if (!check(current && current->bootstrap, test, "Known bootstrap evidence survives projection")) return false;
    addon::Json_value missing_home = addon::Json_value::object();
    missing_home.set("version", addon::Json_value(1));
    auto incomplete = frozen;
    incomplete.set("bootstrap", missing_home);
    if (!check(!addon::decode_launch_environment(incomplete, &failure), test,
            "A version alone cannot prove a resolved profile")) return false;
    addon::Json_value unknown = addon::Json_value::object();
    unknown.set("version", addon::Json_value(2));
    frozen.set("bootstrap", unknown);
    if (!check(!addon::decode_launch_environment(frozen, &failure), test,
            "Unknown bootstrap evidence cannot claim resolved input")) return false;
    auto invalid = addon::encode_environment_bootstrap_declaration(declaration);
    invalid.set("home_environment_name", addon::Json_value("NOT_DECLARED"));
    return check(!addon::decode_environment_bootstrap_declaration(invalid, &failure), test,
        "The home selector must be one declared key");
}

} // namespace sdk_tests
