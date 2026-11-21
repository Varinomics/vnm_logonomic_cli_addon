#include "sdk_test_support.h"

#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/provider_identity.h"

#include <string>

namespace sdk_tests {

namespace {

namespace addon = logonomic::cli_addon;

} // namespace

bool test_identifier_grammars()
{
    bool ok = true;
    const char* id = __func__;

    // Package IDs.
    ok &= check(addon::is_valid_package_id("org.example.unknown-fixture"),  id, "reverse-domain id");
    ok &= check(addon::is_valid_package_id("a.b"),                          id, "shortest id");
    ok &= check(addon::is_valid_package_id("io.x1.y-2.z"),                  id, "digits and interior hyphens");
    ok &= check(!addon::is_valid_package_id("ab"),                          id, "fewer than 3 bytes");
    ok &= check(!addon::is_valid_package_id("single"),                      id, "one segment");
    ok &= check(!addon::is_valid_package_id("Org.example"),                 id, "uppercase");
    ok &= check(!addon::is_valid_package_id("org..example"),                id, "empty segment");
    ok &= check(!addon::is_valid_package_id(".org.example"),                id, "leading dot");
    ok &= check(!addon::is_valid_package_id("org.example."),                id, "trailing dot");
    ok &= check(!addon::is_valid_package_id("org.-example"),                id, "leading hyphen in segment");
    ok &= check(!addon::is_valid_package_id("org.example-"),                id, "trailing hyphen in segment");
    ok &= check(!addon::is_valid_package_id("org.example.two--hyphens"),    id, "double hyphen in segment");
    ok &= check(!addon::is_valid_package_id("org.ex_ample"),                id, "underscore separator");
    ok &= check(!addon::is_valid_package_id("org.ex\xC3\xA9"),              id, "non-ascii");
    ok &= check(!addon::is_valid_package_id("con.example"),                 id, "Windows device package directory");
    ok &= check(!addon::is_valid_package_id("org.example.a" + std::string(63, 'b')), id, "segment longer than 63");
    ok &= check(addon::is_valid_package_id("org." + std::string(63, 'b')),  id, "segment of exactly 63");
    ok &= check(!addon::is_valid_package_id(std::string(64, 'a') + "." + std::string(63, 'b') + "." + std::string(63, 'c')), id,
        "segment of 64 rejected");
    {
        std::string long_id;
        while (long_id.size() < 128 - 4) {
            long_id += std::string(63, 'a') + ".";
        }
        long_id.resize(126);
        long_id += ".b";
        ok &= check(long_id.size() == 128 && addon::is_valid_package_id(long_id), id, "128-byte id accepted");
        ok &= check(!addon::is_valid_package_id(long_id + "c"), id, "129-byte id rejected");
    }

    // Capability IDs.
    ok &= check(addon::is_valid_capability_id("profile-storage"),        id, "hyphenated capability");
    ok &= check(addon::is_valid_capability_id("org.example.unadopted"),  id, "dotted capability");
    ok &= check(addon::is_valid_capability_id("org.example.source--events"), id, "interior double hyphen");
    ok &= check(addon::is_valid_capability_id("a"),                      id, "one-byte capability");
    ok &= check(!addon::is_valid_capability_id(""),                      id, "empty capability");
    ok &= check(!addon::is_valid_capability_id("-lead"),                 id, "leading hyphen");
    ok &= check(!addon::is_valid_capability_id("trail-"),                id, "trailing hyphen");
    ok &= check(!addon::is_valid_capability_id("a..b"),                  id, "double dot");
    ok &= check(!addon::is_valid_capability_id("a.-b"),                  id, "hyphen adjacent to dot");
    ok &= check(!addon::is_valid_capability_id("a-.b"),                  id, "hyphen before dot");
    ok &= check(!addon::is_valid_capability_id("Upper"),                 id, "uppercase capability");
    ok &= check(!addon::is_valid_capability_id("under_score"),           id, "underscore in capability");
    ok &= check(!addon::is_valid_capability_id(std::string(129, 'a')),   id, "129-byte capability");
    ok &= check(addon::is_valid_capability_id(std::string(128, 'a')),    id, "128-byte capability");

    // Operation names and reason codes.
    ok &= check(addon::is_valid_operation_name("acknowledge"),           id, "simple operation");
    ok &= check(addon::is_valid_operation_name("spawn_child2"),          id, "underscore and digit");
    ok &= check(!addon::is_valid_operation_name("2fast"),                id, "leading digit");
    ok &= check(!addon::is_valid_operation_name("_lead"),                id, "leading underscore");
    ok &= check(!addon::is_valid_operation_name("dash-ed"),              id, "hyphen in operation");
    ok &= check(!addon::is_valid_operation_name("Cap"),                  id, "uppercase operation");
    ok &= check(!addon::is_valid_operation_name(std::string(65, 'a')),   id, "65-byte operation");
    ok &= check(addon::is_valid_operation_name(std::string(64, 'a')),    id, "64-byte operation");
    ok &= check(addon::is_valid_reason_code("unknown_operation"),        id, "reason code");

    // Versions.
    ok &= check(addon::is_valid_version(0) && addon::is_valid_version(65535), id, "version bounds inclusive");
    ok &= check(!addon::is_valid_version(-1) && !addon::is_valid_version(65536), id, "version outside bounds");

    // Release versions.
    ok &= check(addon::is_valid_release_version("1.0.0+fixture"),        id, "release version");
    ok &= check(addon::is_valid_release_version("\xC3\xA9" "dition 2"),  id, "utf-8 release version");
    ok &= check(!addon::is_valid_release_version(""),                    id, "empty release version");
    ok &= check(!addon::is_valid_release_version("a\tb"),                id, "control character in release version");
    ok &= check(!addon::is_valid_release_version(std::string(65, 'v')),  id, "65-byte release version");

    // Platform IDs.
    ok &= check(addon::is_valid_platform_id("linux-x86_64"),             id, "linux platform");
    ok &= check(addon::is_valid_platform_id("windows-x86_64"),           id, "windows platform");
    ok &= check(addon::is_valid_platform_id("freebsd-arm64"),            id, "unknown platform still in grammar");
    ok &= check(!addon::is_valid_platform_id("linux"),                   id, "no arch");
    ok &= check(!addon::is_valid_platform_id("Linux-x86_64"),            id, "uppercase platform");
    ok &= check(!addon::is_valid_platform_id("-x86_64"),                 id, "empty os");
    ok &= check(!addon::is_valid_platform_id("linux-"),                  id, "empty arch");

    // Request IDs.
    ok &= check(addon::is_valid_request_id("r1") && addon::is_valid_request_id("r10") && addon::is_valid_request_id("r0"), id,
        "request ids");
    ok &= check(!addon::is_valid_request_id("r") && !addon::is_valid_request_id("r01") && !addon::is_valid_request_id("1"), id,
        "malformed request ids");
    ok &= check(!addon::is_valid_request_id("r" + std::string(64, '1')), id, "65-byte request id");
    ok &= check(addon::is_valid_request_id("r" + std::string(63, '1')),  id, "64-byte request id");

    // Environment names.
    ok &= check(addon::is_valid_environment_name("HOME") && addon::is_valid_environment_name("_x1"), id, "environment names");
    ok &= check(!addon::is_valid_environment_name("1X") && !addon::is_valid_environment_name("A-B") && !addon::is_valid_environment_name(""), id,
        "malformed environment names");
    ok &= check(!addon::is_valid_environment_name(std::string(129, 'A')), id, "129-byte environment name");
    ok &= check(
        addon::is_reserved_environment_name("LOGONOMIC_ANYTHING") &&
            addon::is_reserved_environment_name("Logonomic_Anything") &&
            !addon::is_reserved_environment_name("LOGONOMICS"), id,
        "reserved prefix");

    // Package-relative paths.
    ok &= check(addon::is_valid_package_relative_path("adapter/unknown-fixture-adapter"),  id, "adapter path");
    ok &= check(addon::is_valid_package_relative_path("capabilities/profile-storage@1.json"), id, "sidecar path");
    ok &= check(addon::is_valid_package_relative_path("assets/icons/dark/mark.svg"),       id, "nested asset path");
    ok &= check(!addon::is_valid_package_relative_path(""),                                id, "empty path");
    ok &= check(!addon::is_valid_package_relative_path("/adapter/x"),                      id, "leading slash");
    ok &= check(!addon::is_valid_package_relative_path("adapter/"),                        id, "trailing slash");
    ok &= check(!addon::is_valid_package_relative_path("adapter//x"),                      id, "empty segment");
    ok &= check(!addon::is_valid_package_relative_path("adapter/../x"),                    id, "parent segment");
    ok &= check(!addon::is_valid_package_relative_path("adapter/./x"),                     id, "dot segment");
    ok &= check(!addon::is_valid_package_relative_path("adapter\\x"),                      id, "backslash");
    ok &= check(!addon::is_valid_package_relative_path("adapter/x:y"),                     id, "colon");
    ok &= check(!addon::is_valid_package_relative_path("adapter/x?"),                      id, "question mark");
    ok &= check(!addon::is_valid_package_relative_path("adapter/x*"),                      id, "asterisk");
    ok &= check(!addon::is_valid_package_relative_path("adapter/x\"y"),                    id, "quote");
    ok &= check(!addon::is_valid_package_relative_path("adapter/x<y>"),                    id, "angle brackets");
    ok &= check(!addon::is_valid_package_relative_path("adapter/x|y"),                     id, "pipe");
    ok &= check(!addon::is_valid_package_relative_path("adapter/x\ty"),                    id, "control character");
    ok &= check(!addon::is_valid_package_relative_path("adapter/x."),                      id, "segment ending in dot");
    ok &= check(!addon::is_valid_package_relative_path("adapter/x "),                      id, "segment ending in space");
    ok &= check(!addon::is_valid_package_relative_path("assets/NUL"),                      id, "Windows device name");
    ok &= check(!addon::is_valid_package_relative_path("assets/com1.txt"),                 id, "Windows device name with extension");
    ok &= check(!addon::is_valid_package_relative_path("assets/LPT9.svg"),                 id, "Windows port device name");
    for (const char* prefix : {"COM", "com", "LPT", "lpt"}) {
        for (const char* digit : {"\xC2\xB9", "\xC2\xB2", "\xC2\xB3"}) {
            const std::string path = std::string("assets/") + prefix + digit;
            ok &= check(!addon::is_valid_package_relative_path(path), id, "superscript device alias");
            ok &= check(!addon::is_valid_package_relative_path(path + ".txt"), id, "device alias with extension");
            ok &= check(!addon::is_valid_package_relative_path(path + "/child.txt"), id, "device alias directory");
        }
    }
    for (const char* path : {"assets/com10.txt", "assets/COM0", "assets/COM\xC2\xB9" "extra.txt",
            "assets/prefixLPT\xC2\xB2.txt", "assets/\xC3\xA9.svg"}) {
        ok &= check(addon::is_valid_package_relative_path(path), id, "non-device Unicode and lookalike paths");
    }
    ok &= check(!addon::is_valid_package_relative_path("other/x"),                         id, "unknown first segment");
    ok &= check(!addon::is_valid_package_relative_path("logonomic-cli-addon.json"),        id, "bootstrap is not indexable");
    ok &= check(addon::is_valid_package_relative_path("assets/" + std::string(128, 'a')),  id, "128-byte segment");
    ok &= check(!addon::is_valid_package_relative_path("assets/" + std::string(129, 'a')), id, "129-byte segment");
    {
        std::string deep = "assets";
        for (int i = 0; i < 15; ++i) {
            deep += "/d";
        }
        ok &= check(addon::is_valid_package_relative_path(deep), id, "16 segments accepted");
        ok &= check(!addon::is_valid_package_relative_path(deep + "/d"), id, "17 segments rejected");
    }
    {
        const std::string long_path = "assets/" + std::string(128, 'a') + "/" + std::string(64, 'b');
        ok &= check(long_path.size() == 200 && addon::is_valid_package_relative_path(long_path), id, "200-byte path accepted");
        ok &= check(!addon::is_valid_package_relative_path(long_path + "c"), id, "201-byte path rejected");
    }
    ok &= check(addon::path_first_segment("assets/a/b") == "assets" && addon::path_first_segment("x") == "x", id, "first segment");
    ok &= check(addon::split_path_segments("a//b").size() == 3, id, "split keeps empty segments");
    ok &= check(addon::ascii_lowercase("MiXeD\xC3\x89") == "mixed\xC3\x89", id, "ascii lowercase leaves non-ascii alone");

    // Hex grammars.
    ok &= check(addon::is_valid_sha256_hex(std::string(64, 'a')) && !addon::is_valid_sha256_hex(std::string(64, 'A')), id,
        "sha256 hex is lowercase");
    ok &= check(!addon::is_valid_sha256_hex(std::string(63, 'a')), id, "sha256 hex length");
    ok &= check(addon::is_valid_generation_nonce_hex(std::string(32, '0')) && !addon::is_valid_generation_nonce_hex(std::string(31, '0')), id,
        "nonce hex length");
    ok &= check(addon::is_valid_transition_nonce_hex(std::string(32, 'f')) &&
        !addon::is_valid_transition_nonce_hex(std::string(32, 'F')) &&
        !addon::is_valid_transition_nonce_hex(std::string(33, 'f')), id, "transition nonce is 32 lowercase hex");
    {
        const std::string first  = addon::random_nonce_hex();
        const std::string second = addon::random_nonce_hex();
        ok &= check(addon::is_valid_generation_nonce_hex(first) && addon::is_valid_transition_nonce_hex(first), id,
            "the random nonce source produces the nonce grammar");
        ok &= check(first != second, id, "two minted nonces differ");
    }

    // Provider identifiers: the provenance grammar and the derived form.
    ok &= check(addon::is_valid_provider_id("fixture-legacy") && addon::is_valid_provider_id("p0") &&
        addon::is_valid_provider_id("a_b-c9") && addon::is_valid_provider_id(std::string(64, 'a')), id,
        "provider ids in grammar");
    ok &= check(!addon::is_valid_provider_id("") && !addon::is_valid_provider_id("Fixture") &&
        !addon::is_valid_provider_id("1abc") && !addon::is_valid_provider_id("_x") &&
        !addon::is_valid_provider_id("a.b") && !addon::is_valid_provider_id(std::string(65, 'a')), id,
        "provider ids outside the grammar");

    // The four vectors of the derived identity, computed independently with
    // sha256sum over decimal(len) ":" package_id.
    const struct
    {
        const char* package_id;
        const char* derived;
    }
    vectors[] = {
        {"org.example.unknown-fixture",                   "p1d191838f9eedd48f5a10b85f78d4cf274612b66874c6a8dca5f8180f799c74"},
        {"org.example.unknown-fixture-provider-surfaces", "p416982762f9bee403f3238228e96243df6c990096fc884fa608d1b7940bdae2"},
        {"com.example.vendor-build",                      "pde5d7cf33421cd3e8a31add208f629c95e1a2345aab3e0954511f7eaeae221d"},
        {"org.example.cli.tool",                          "p499bf4bcd3f3e38c1cc006a7ee86c6478b5bc078a29f53dd4758829a0930268"},
    };
    for (const auto& vector : vectors) {
        const std::string derived = addon::derived_provider_id(vector.package_id);
        ok &= check(derived == vector.derived, id,
            std::string("derived provider id of ") + vector.package_id + ": " + derived);
        ok &= check(derived.size() == 64 && addon::is_derived_provider_id(derived) &&
            addon::is_valid_provider_id(derived), id,
            "a derived id is 64 characters, recognized as derived, and within the provenance grammar");
    }
    ok &= check(!addon::is_derived_provider_id("q" + std::string(63, 'a')) &&
        !addon::is_derived_provider_id("p" + std::string(62, 'a')) &&
        !addon::is_derived_provider_id("p" + std::string(63, 'A')) &&
        !addon::is_derived_provider_id("fixture-legacy"), id,
        "non-derived identifiers are not recognized as derived");
    ok &= check(addon::derived_provider_id("a.b") != addon::derived_provider_id("a.bc"), id,
        "the length delimiter separates package ids that share a prefix");

    // Every reason code this SDK reports is in the reason-code grammar.
    for (const char* code : {
             addon::reason::k_purpose_unavailable, addon::reason::k_launcher_program_unavailable,
             addon::reason::k_launcher_version_cancelled,
             addon::reason::k_inspection_invalid, addon::reason::k_acknowledgment_mismatch,
             addon::reason::k_private_artifact_invalid, addon::reason::k_resume_record_invalid,
             addon::reason::k_observation_invalid, addon::reason::k_classification_invalid,
             addon::reason::k_dependency_capability_unavailable, addon::reason::k_provider_id_collision,
             addon::reason::k_invalid_helper_environment, addon::reason::k_initial_prompt_unavailable,
             addon::reason::k_initial_prompt_unacknowledged, addon::reason::k_initial_prompt_mismatch,
         })
    {
        ok &= check(addon::is_valid_reason_code(code), id, std::string("reason code in grammar: ") + code);
    }
    return ok;
}

} // namespace sdk_tests
