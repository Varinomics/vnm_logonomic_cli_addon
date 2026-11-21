#include "sdk_test_support.h"

#include "logonomic_cli_addon/account_pet.h"
#include "logonomic_cli_addon/package_source.h"

#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>

namespace sdk_tests {

bool test_account_pet_contracts()
{
    namespace addon = logonomic::cli_addon;
    namespace fs    = std::filesystem;

    // Oracle: account-pet@1's adopted optional result, account acknowledgment,
    // local path, grid and animation bounds in docs/account_pet_1.md.
    addon::Contract_failure failure;
    const auto root = fs::temp_directory_path() / "logonomic-pet-contracts";
    const std::string profile = addon::path_to_utf8(root / "profile");
    const std::string home    = addon::path_to_utf8(root / "home");
    const std::string cwd     = addon::path_to_utf8(root / "project");
    const std::string image   = addon::path_to_utf8(root / "sprite.webp");
    const std::string config  = addon::path_to_utf8(root / "config.toml");
    const addon::Account_context context{2, "Primary", profile};

    SDK_CHECK(addon::decode_account_pet_declaration(addon::Json_value::object(), &failure),
        "an empty declaration adopts the optional capability");
    auto declaration = addon::Json_value::object();
    declaration.set("theme", addon::Json_value("forest"));
    SDK_CHECK(!addon::decode_account_pet_declaration(declaration, &failure),
        "account-pet declarations use the closed empty payload");

    const auto request = addon::encode_account_pet_request(context, cwd, home);
    const auto decoded_request = addon::decode_account_pet_request(request, &failure);
    SDK_CHECK(decoded_request && decoded_request->normalization_directory == cwd &&
        decoded_request->home_directory == home && decoded_request->account_context.profile_path == profile,
        "inspection preserves the launcher's frozen HOME, directory and selected profile");
    SDK_CHECK(!addon::account_context_acknowledgment_failure(context, request),
        "the request carries the complete selected account context");
    const auto opaque = addon::decode_account_pet_request(
        addon::encode_account_pet_request({2, "Primary", std::nullopt}), &failure);
    SDK_CHECK(opaque && !opaque->account_context.profile_path &&
        opaque->normalization_directory.empty() && opaque->home_directory.empty(),
        "an account without a profile locator needs no invented path or process environment");
    auto invalid = request;
    invalid.set("home_directory", addon::Json_value("relative/home"));
    SDK_CHECK(!addon::decode_account_pet_request(invalid, &failure),
        "a supplied HOME must be absolute rather than resolved in the adapter's environment");
    invalid = addon::encode_account_pet_request({1, "Primary"});
    SDK_CHECK(!addon::decode_account_pet_request(invalid, &failure),
        "pet inspection requires the accounts@2 context");

    addon::Account_pet_inspect_result absent{context, std::nullopt, {config}, {}, {}};
    const auto absent_payload = addon::encode_account_pet_inspect_result(absent);
    const auto no_selection = addon::decode_account_pet_inspect_result(absent_payload, &failure);
    SDK_CHECK(no_selection && !no_selection->pet && no_selection->unavailable_reason.empty() &&
        no_selection->watch_paths == absent.watch_paths && absent_payload.find("pet")->is_null(),
        "no selected pet remains observable without being reported as unreadable");
    absent.unavailable_reason = "The selected sprite sheet could not be read.";
    const auto unavailable_payload = addon::encode_account_pet_inspect_result(absent);
    const auto unavailable = addon::decode_account_pet_inspect_result(unavailable_payload, &failure);
    SDK_CHECK(unavailable && !unavailable->pet && unavailable->unavailable_reason == absent.unavailable_reason &&
        unavailable->watch_paths == absent.watch_paths,
        "an unavailable configured pet retains its reason and repair observation paths");

    auto discovery = absent;
    discovery.watch_discoveries = {{addon::path_to_utf8(root / "cache" / "tui-pets"),
        "*/assets", "forest-spritesheet-", {".webp", ".png"}}};
    const auto discovery_payload = addon::encode_account_pet_inspect_result(discovery);
    const auto discovery_result = addon::decode_account_pet_inspect_result(discovery_payload, &failure);
    SDK_CHECK(discovery_result && discovery_result->watch_discoveries.size() == 1 &&
        discovery_result->watch_discoveries.front().directory == discovery.watch_discoveries.front().directory &&
        discovery_result->watch_discoveries.front().relative_directory_pattern == "*/assets" &&
        discovery_result->watch_discoveries.front().name_prefix == "forest-spritesheet-" &&
        discovery_result->watch_discoveries.front().suffixes == discovery.watch_discoveries.front().suffixes,
        "an unavailable selected pet retains bounded filename discovery across future cache directories");
    invalid = discovery_payload;
    invalid.erase("watch_discoveries");
    const auto exact_paths_only = addon::decode_account_pet_inspect_result(invalid, &failure);
    SDK_CHECK(exact_paths_only && exact_paths_only->watch_discoveries.empty(),
        "absence of the optional discovery record leaves exact-file observation usable");
    const struct
    {
        const char* member;
        addon::Json_value value;
    }
    invalid_discovery_members[] = {
        {"directory",                  addon::Json_value("relative/cache")},
        {"relative_directory_pattern", addon::Json_value("../assets")},
        {"relative_directory_pattern", addon::Json_value("*/../assets")},
        {"relative_directory_pattern", addon::Json_value("*/*/*/*/assets")},
        {"relative_directory_pattern", addon::Json_value("*/assets/")},
        {"relative_directory_pattern", addon::Json_value("*\\assets")},
        {"name_prefix",                addon::Json_value("forest/spritesheet-")},
        {"name_prefix",                addon::Json_value(std::string(257, 'f'))},
        {"suffixes",                   addon::Json_value::array()},
        {"suffixes",                   addon::Json_value(addon::Json_value::Array{addon::Json_value("")})},
        {"suffixes",                   addon::Json_value(addon::Json_value::Array{addon::Json_value("/.png")})},
        {"suffixes",                   addon::Json_value(addon::Json_value::Array{
            addon::Json_value(std::string(33, 'p'))})},
    };
    for (const auto& entry : invalid_discovery_members) {
        invalid = discovery_payload;
        invalid.find("watch_discoveries")->as_array().front().set(entry.member, entry.value);
        SDK_CHECK(!addon::decode_account_pet_inspect_result(invalid, &failure),
            std::string("rejects out-of-contract discovery member: ") + entry.member);
    }
    auto bounded_discovery = discovery.watch_discoveries.front();
    bounded_discovery.relative_directory_pattern = "*/*/*/assets";
    bounded_discovery.name_prefix = std::string(256, 'f');
    bounded_discovery.suffixes.assign(8, std::string(32, 'p'));
    discovery.watch_discoveries.assign(8, bounded_discovery);
    SDK_CHECK(addon::decode_account_pet_inspect_result(
        addon::encode_account_pet_inspect_result(discovery), &failure).has_value(),
        "the documented discovery count, depth, prefix and suffix bounds are usable");
    discovery.watch_discoveries.push_back(bounded_discovery);
    SDK_CHECK(!addon::decode_account_pet_inspect_result(
        addon::encode_account_pet_inspect_result(discovery), &failure),
        "a provider cannot exceed the discovery record bound");
    discovery.watch_discoveries.assign(1, bounded_discovery);
    discovery.watch_discoveries.front().suffixes.push_back(".png");
    SDK_CHECK(!addon::decode_account_pet_inspect_result(
        addon::encode_account_pet_inspect_result(discovery), &failure),
        "a discovery cannot exceed the suffix count bound");

    addon::Account_pet pet{image, 192, 208, 8, 9, {0, 1, 2, 3, 4, 5}, {1680, 660, 660, 840, 840, 1920}};
    const addon::Account_pet_inspect_result ready{context, pet, {config, image}, {}, {}};
    const auto ready_payload = addon::encode_account_pet_inspect_result(ready);
    const auto decoded = addon::decode_account_pet_inspect_result(ready_payload, &failure);
    SDK_CHECK(decoded && decoded->pet && decoded->pet->spritesheet_path == image &&
        decoded->pet->frame_width == 192 && decoded->pet->frame_height == 208 &&
        decoded->pet->columns == 8 && decoded->pet->rows == 9 &&
        decoded->pet->frames == pet.frames && decoded->pet->frame_durations_ms == pet.frame_durations_ms,
        "inspection retains the original sheet geometry, first still frame and complete idle timing");
    SDK_CHECK(!addon::account_context_acknowledgment_failure(context, ready_payload),
        "a ready result acknowledges the exact selected account");
    invalid = ready_payload;
    invalid.set(addon::k_account_context_member, addon::encode_account_context({2, "primary", profile}));
    SDK_CHECK(addon::account_context_acknowledgment_failure(context, invalid).has_value(),
        "a different case-sensitive account cannot supply this badge");
    invalid.set(addon::k_account_context_member, addon::encode_account_context({2, "Primary", home}));
    SDK_CHECK(addon::account_context_acknowledgment_failure(context, invalid).has_value(),
        "the same opaque account key cannot acknowledge another profile");
    invalid = ready_payload;
    invalid.set("unavailable_reason", addon::Json_value(absent.unavailable_reason));
    SDK_CHECK(!addon::decode_account_pet_inspect_result(invalid, &failure),
        "a result cannot describe a sprite as both ready and unavailable");

    const struct
    {
        const char* member;
        addon::Json_value value;
    }
    invalid_pet_members[] = {
        {"spritesheet_path", addon::Json_value("sprite.webp")},
        {"frame_width",      addon::Json_value(0)},
        {"frame_height",     addon::Json_value(4097)},
        {"columns",          addon::Json_value(257)},
        {"rows",             addon::Json_value(33)},
        {"frames",           addon::Json_value::array()},
        {"frame_durations_ms", addon::Json_value::array()},
    };
    for (const auto& entry : invalid_pet_members) {
        invalid = ready_payload;
        invalid.find("pet")->set(entry.member, entry.value);
        SDK_CHECK(!addon::decode_account_pet_inspect_result(invalid, &failure),
            std::string("rejects out-of-contract pet member: ") + entry.member);
    }
    for (const auto duration : {std::int64_t(16), std::int64_t(std::numeric_limits<int>::max()) + 1}) {
        invalid = ready_payload;
        invalid.find("pet")->find("frame_durations_ms")->as_array()[0] = addon::Json_value(duration);
        SDK_CHECK(!addon::decode_account_pet_inspect_result(invalid, &failure),
            "frame timings must be representable by the bounded animation timer");
    }
    invalid = ready_payload;
    invalid.find("pet")->find("frames")->as_array()[0] = addon::Json_value(72);
    SDK_CHECK(!addon::decode_account_pet_inspect_result(invalid, &failure),
        "a row-major frame index must fit the declared grid");

    auto maximum = ready;
    maximum.pet = addon::Account_pet{image, 4096, 4096, 1, 1, {0}, {17}};
    SDK_CHECK(addon::decode_account_pet_inspect_result(
        addon::encode_account_pet_inspect_result(maximum), &failure).has_value(),
        "the documented maximum pixel area and minimum timer interval are accepted");
    maximum.pet->columns = 2;
    SDK_CHECK(!addon::decode_account_pet_inspect_result(
        addon::encode_account_pet_inspect_result(maximum), &failure),
        "a sheet exceeding the pixel budget is rejected before image loading");
    maximum.pet = addon::Account_pet{image, 1, 1, 256, 1, {}, {}};
    for (std::uint32_t index = 0; index < 256; ++index) {
        maximum.pet->frames.push_back(index);
        maximum.pet->frame_durations_ms.push_back(std::numeric_limits<int>::max());
    }
    maximum.watch_paths.assign(32, config);
    SDK_CHECK(addon::decode_account_pet_inspect_result(
        addon::encode_account_pet_inspect_result(maximum), &failure).has_value(),
        "the full frame, animation and observation bounds remain usable");
    maximum.watch_paths.push_back(image);
    SDK_CHECK(!addon::decode_account_pet_inspect_result(
        addon::encode_account_pet_inspect_result(maximum), &failure),
        "a provider cannot exceed the observation path bound");
    return true;
}

} // namespace sdk_tests
