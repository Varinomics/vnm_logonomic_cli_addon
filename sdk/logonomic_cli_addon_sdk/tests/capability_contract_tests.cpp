#include "sdk_test_support.h"

#include "logonomic_cli_addon/accounts.h"
#include "logonomic_cli_addon/capability_ids.h"
#include "logonomic_cli_addon/helper_client.h"
#include "logonomic_cli_addon/helper_ingress.h"
#include "logonomic_cli_addon/launch_preparation.h"
#include "logonomic_cli_addon/launcher_discovery.h"
#include "logonomic_cli_addon/lifecycle_events.h"
#include "logonomic_cli_addon/limits.h"
#include "logonomic_cli_addon/managed_assets.h"
#include "logonomic_cli_addon/package_source.h"
#include "logonomic_cli_addon/presentation_record.h"
#include "logonomic_cli_addon/process_proposal.h"
#include "logonomic_cli_addon/profile_storage.h"
#include "logonomic_cli_addon/provider_event_diagnostic.h"
#include "logonomic_cli_addon/session_contracts.h"
#include "logonomic_cli_addon/sha256.h"
#include "logonomic_cli_addon/svg_subset.h"
#include "native_io_path.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sdk_tests {

namespace {

namespace addon  = logonomic::cli_addon;
namespace limits = logonomic::cli_addon::limits;
namespace fs     = std::filesystem;

// Regression fixture from the manager's complete production title instruction.
// The desktop integration test exercises the actual producer; the standalone
// SDK must also admit these bytes without depending on the desktop or Qt.
constexpr std::string_view k_production_title_instruction =
    "For this first substantive task, invoke the executable path in "
    "LOGONOMIC_VNMCTL_PATH once with arguments `set-title` and a "
    "concise 3-7 word title that omits the repository name. Only the root "
    "agent may set it, and a user title takes precedence. Do not inspect "
    "title state or retry; continue the task if it fails. Also keep a "
    "working-state summary: pipe it to that executable with argument "
    "`set-summary`. Reconsider it when asked or after a material change of "
    "objective or a milestone: rerun it with a revision, or with "
    "`--unchanged` if still accurate, adding `--title TITLE` if the subject "
    "materially changed. Before changing another git worktree, report it with "
    "argument `report-worktree PATH`.";

void append_be32(std::string& bytes, std::uint32_t value)
{
    bytes.push_back(static_cast<char>(value >> 24));
    bytes.push_back(static_cast<char>(value >> 16));
    bytes.push_back(static_cast<char>(value >> 8));
    bytes.push_back(static_cast<char>(value));
}

std::uint32_t crc32(std::string_view bytes)
{
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const unsigned char byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

void append_png_chunk(std::string& bytes, std::string_view type, std::string_view data)
{
    append_be32(bytes, static_cast<std::uint32_t>(data.size()));
    const std::size_t crc_begin = bytes.size();
    bytes.append(type);
    bytes.append(data);
    append_be32(bytes, crc32(std::string_view(bytes).substr(crc_begin)));
}

std::string valid_png(
    std::uint32_t width  = 64,
    std::uint32_t height = 64)
{
    std::string bytes("\x89PNG\r\n\x1A\n", 8);
    std::string ihdr;
    append_be32(ihdr, width);
    append_be32(ihdr, height);
    ihdr.append("\x08\x06\x00\x00\x00", 5);
    append_png_chunk(bytes, "IHDR", ihdr);
    append_png_chunk(bytes, "IDAT", "");
    append_png_chunk(bytes, "IEND", "");
    return bytes;
}

addon::Json_value string_array(std::initializer_list<const char*> values)
{
    addon::Json_value array = addon::Json_value::array();
    for (const char* value : values) {
        array.push_back(addon::Json_value(value));
    }
    return array;
}

} // namespace

bool test_launcher_capability_contracts()
{
    {
        // Oracle: launch-preparation@3 replaces verified argv and can only
        // restrict the package declaration's per-launch feature admission.
        addon::Contract_failure failure;
        addon::Launch_patch patch;
        bool missing_disposition_rejected = false;
        try { (void)addon::encode_launch_patch(patch, 3); }
        catch (const std::invalid_argument&) { missing_disposition_rejected = true; }
        SDK_CHECK(missing_disposition_rejected,
            "version 3 producer cannot serialize a success without feature admission");
        patch.argv = std::vector<std::string>{"/tools/provider", "--hooks", "{{asset:assets/hooks.json}}"};
        patch.disposition = addon::Launch_disposition{};
        patch.apply_managed_configuration = true;
        const auto encoded = addon::encode_launch_patch(patch, 3);
        const auto decoded = addon::decode_launch_patch(encoded, &failure, 3);
        SDK_CHECK(decoded && decoded->apply_managed_configuration && decoded->argv == patch.argv &&
            addon::validate_launch_patch(*decoded, {"assets/hooks.json"}, {"assets/hooks.json"}, &failure),
            "verified argv replacement preserves a declared indexed hook asset");
        SDK_CHECK(!addon::decode_launch_patch(encoded, &failure, 4),
            "unknown preparation versions cannot borrow an older patch grammar");
        SDK_CHECK(!addon::validate_launch_patch(*decoded, {}, {}, &failure),
            "argv asset references must belong to the immutable package index");
        patch.argv.reset();
        patch.disposition = addon::Launch_disposition{false, false, false};
        SDK_CHECK(!addon::validate_launch_patch(patch, {}, {}, &failure),
            "an unmanaged launch cannot request a configuration write");
        patch.apply_managed_configuration = false;
        SDK_CHECK(addon::validate_launch_patch(patch, {}, {}, &failure),
            "unmanaged launch remains a successful unchanged launch");
        patch.set.emplace_back("PROVIDER_HOOKS", "enabled");
        SDK_CHECK(!addon::validate_launch_patch(patch, {}, {}, &failure),
            "unmanaged disposition cannot silently modify launch input");
    }

    addon::Contract_failure failure;
    addon::Json_value sidecar = addon::Json_value::object();
    sidecar.set("environment_names", string_array({"HTTP_PROXY", "HTTP_PROXY"}));
    SDK_CHECK(!addon::decode_launcher_discovery_declaration(sidecar, &failure).has_value(),
        "identical environment declarations must be rejected");

    sidecar.set("environment_names", string_array({"HTTP_PROXY", "http_proxy"}));
    const auto proxy_declaration = addon::decode_launcher_discovery_declaration(sidecar, &failure);
    SDK_CHECK(proxy_declaration.has_value(),
        "case-distinct environment declarations must preserve Linux names");
    const addon::Json_value linux_proxies = addon::ambient_view(
        {{"HTTP_PROXY", "upper"}, {"http_proxy", "lower"}},
        *proxy_declaration, addon::k_platform_linux_x86_64);
    SDK_CHECK(linux_proxies.find("HTTP_PROXY") != nullptr &&
        linux_proxies.find("HTTP_PROXY")->as_string() == "upper" &&
        linux_proxies.find("http_proxy") != nullptr &&
        linux_proxies.find("http_proxy")->as_string() == "lower",
        "Linux must preserve distinct values for case-distinct names");
    const addon::Json_value linux_lower_proxy = addon::ambient_view(
        {{"http_proxy", "lower"}}, *proxy_declaration, addon::k_platform_linux_x86_64);
    SDK_CHECK(linux_lower_proxy.find("HTTP_PROXY") == nullptr &&
        linux_lower_proxy.find("http_proxy") != nullptr &&
        linux_lower_proxy.find("http_proxy")->as_string() == "lower",
        "Linux must forward a lowercase-only declared proxy without renaming it");
    const addon::Json_value windows_proxy = addon::ambient_view(
        {{"http_proxy", "one"}}, *proxy_declaration, addon::k_platform_windows_x86_64);
    SDK_CHECK(windows_proxy.as_object().size() == 1 &&
        windows_proxy.find("HTTP_PROXY") != nullptr &&
        windows_proxy.find("HTTP_PROXY")->as_string() == "one",
        "Windows must match proxy names case-insensitively and emit one declared spelling");

    sidecar.set("environment_names", string_array({"logonomic_addon_token"}));
    SDK_CHECK(!addon::decode_launcher_discovery_declaration(sidecar, &failure).has_value() &&
        failure.reason_code == addon::reason::k_reserved_environment_variable,
        "the reserved prefix must be case-insensitive");

    addon::Launcher_discovery_declaration declaration{{"EXTRA", "Path"}};
    const std::string invalid_utf8("\xC3\x28", 2);
    const addon::Json_value linux_view = addon::ambient_view(
        {{"HOME", invalid_utf8}, {"EXTRA", "ok"}}, declaration, addon::k_platform_linux_x86_64);
    SDK_CHECK(linux_view.find("HOME") == nullptr && linux_view.find("EXTRA") != nullptr,
        "invalid UTF-8 ambient values must be omitted before JSON construction");
    const addon::Json_value windows_view = addon::ambient_view(
        {{"Path", "one"}, {"EXTRA", "two"}}, declaration, addon::k_platform_windows_x86_64);
    SDK_CHECK(windows_view.find("PATH") != nullptr && windows_view.find("Path") == nullptr,
        "a Windows baseline spelling must win case-insensitive final dedupe");

    const addon::Launcher_request launcher{
        "example-cli --chat",
        addon::k_runner_direct,
        "MODE=chat",
    };
    const addon::Json_value recognize_payload = addon::encode_recognize_request(
        launcher, addon::Json_value::object(), addon::k_platform_windows_x86_64);
    const std::optional<addon::Recognize_request> recognize =
        addon::decode_recognize_request(recognize_payload, &failure);
    SDK_CHECK(recognize.has_value() && recognize->launcher.script_body == launcher.script_body,
        "recognize must round-trip the typed launcher request");

    addon::Json_value oversized_discover = addon::encode_discover_request(
        addon::Json_value::object(),
        std::string(limits::k_max_working_directory_bytes + 1, 'x'),
        addon::k_platform_linux_x86_64);
    SDK_CHECK(!addon::decode_discover_request(oversized_discover, &failure).has_value(),
        "the discovery default-home fallback must enforce its path bound");
    addon::Json_value invalid_home = addon::encode_discover_request(
        addon::Json_value::object(), invalid_utf8, addon::k_platform_linux_x86_64);
    SDK_CHECK(!addon::decode_discover_request(invalid_home, &failure).has_value(),
        "the discovery default-home fallback must reject invalid UTF-8");

    addon::Prepare_request prepare;
    prepare.launcher          = launcher;
    prepare.launcher_argv     = {"example-cli", "--chat"};
    prepare.launch_environment = addon::Launch_environment{addon::k_platform_windows_x86_64,
        addon::Json_value::object(), std::nullopt};
    prepare.working_directory = "C:\\repo";
    prepare.title_instruction = k_production_title_instruction;
    prepare.declared_assets   = {"assets/config.json"};
    prepare.profile_storage   = addon::Profile_storage_context{1, "C:\\profile\\package\\default"};
    const std::optional<addon::Prepare_request> decoded_prepare =
        addon::decode_prepare_request(addon::encode_prepare_request(prepare), &failure);
    SDK_CHECK(decoded_prepare.has_value() &&
        decoded_prepare->launcher.environment_text == launcher.environment_text &&
        decoded_prepare->title_instruction == k_production_title_instruction,
        "prepare must preserve the complete production title instruction and launcher");
    {
        auto request = prepare;
        addon::Launch_environment environment;
        environment.platform_id = addon::k_platform_windows_x86_64;
        environment.effective_environment.set("OPENCODE_CONFIG", addon::Json_value("C:/profile/opencode.json"));
        request.launch_environment = environment;
        const auto encoded = addon::encode_prepare_request(request, 3);
        const auto decoded = addon::decode_prepare_request(encoded, &failure, 3);
        SDK_CHECK(decoded && decoded->launch_environment &&
            decoded->launch_environment->platform_id == environment.platform_id &&
            decoded->launch_environment->effective_environment == environment.effective_environment,
            "launch preparation receives the frozen effective launcher environment");
        auto malformed = encoded;
        malformed.erase("launch_environment");
        SDK_CHECK(!addon::decode_prepare_request(malformed, &failure, 3),
            "launch preparation cannot infer a missing effective environment");
        malformed = encoded;
        malformed.find("launch_environment")->find("effective_environment")->set(
            "opencode_config", addon::Json_value("C:/other/config.json"));
        SDK_CHECK(!addon::decode_prepare_request(malformed, &failure, 3),
            "Windows preparation rejects case aliases in the effective environment");
        malformed = encoded;
        malformed.find("launch_environment")->find("effective_environment")->set(
            "OPENCODE_CONFIG", addon::Json_value(std::string(limits::k_max_environment_value_bytes + 1, 'x')));
        SDK_CHECK(!addon::decode_prepare_request(malformed, &failure, 3),
            "effective environment values retain the process environment byte bound");
        malformed = encoded;
        malformed.find("launch_environment")->set("unrelated", addon::Json_value(true));
        SDK_CHECK(!addon::decode_prepare_request(malformed, &failure, 3),
            "the effective environment record has a closed member set");
    }
    for (const std::size_t size : {
            limits::k_max_title_instruction_bytes - 1,
            limits::k_max_title_instruction_bytes})
    {
        prepare.title_instruction = std::string(size, 't');
        const auto decoded = addon::decode_prepare_request(
            addon::encode_prepare_request(prepare), &failure);
        SDK_CHECK(decoded.has_value() && decoded->title_instruction == prepare.title_instruction,
            "prepare must preserve title instructions below and at the byte bound");
    }
    prepare.title_instruction.push_back('t');
    SDK_CHECK(!addon::decode_prepare_request(addon::encode_prepare_request(prepare), &failure).has_value() &&
        failure.reason_code == addon::reason::k_payload_too_large,
        "prepare must reject a title instruction one byte above the bound");
    prepare.title_instruction = k_production_title_instruction;
    prepare.working_directory = invalid_utf8;
    SDK_CHECK(!addon::decode_prepare_request(addon::encode_prepare_request(prepare), &failure).has_value(),
        "prepare must reject an invalid UTF-8 working directory before path conversion");

    addon::Launch_patch patch;
    patch.set   = {{"Token", "one"}};
    patch.unset = {"TOKEN"};
    SDK_CHECK(!addon::validate_launch_patch(patch, {}, {}, &failure),
        "case-equivalent set/unset names must be rejected for Windows safety");
    patch.set   = {{"logonomic_secret", "one"}};
    patch.unset = {};
    SDK_CHECK(!addon::validate_launch_patch(patch, {}, {}, &failure) &&
        failure.reason_code == addon::reason::k_reserved_environment_variable,
        "a lowercase reserved launch-patch name must be rejected");

    const std::string token = "{{asset:assets/config.json}}";
    const std::optional<std::vector<addon::Asset_token>> tokens = addon::scan_asset_tokens(token, &failure);
    SDK_CHECK(tokens.has_value(), "a declared asset token must parse");
    patch.set = {{"EXAMPLE_CONFIG", token}};
    SDK_CHECK(!addon::validate_launch_patch(
            patch,
            {"assets/config.json"},
            {},
            &failure) &&
        failure.reason_code == addon::reason::k_asset_reference_invalid,
        "a declared asset absent from the custody index must be rejected");
    SDK_CHECK(addon::validate_launch_patch(
            patch,
            {"assets/config.json"},
            {"assets/config.json"},
            &failure),
        "a declared and indexed asset token must be admitted");
    SDK_CHECK(!addon::substitute_asset_tokens(
        token,
        *tokens,
        {{"assets/config.json", std::string(limits::k_max_environment_value_bytes + 1, 'x')}}).has_value(),
        "materialization must enforce the final expanded value bound");
    SDK_CHECK(!addon::scan_asset_tokens("{{asset:../escape}}", &failure).has_value(),
        "asset tokens must stay in the canonical assets subtree");
    SDK_CHECK(!addon::scan_asset_tokens("{{future:assets/config.json}}", &failure).has_value(),
        "unknown double-brace token forms must fail closed");

    addon::Launcher_candidate unsupported;
    unsupported.candidate_id     = "windows_default";
    unsupported.display_name     = "Windows";
    unsupported.script_body      = "example-cli";
    unsupported.runner_key       = addon::k_runner_cmd;
    unsupported.environment_text = "";
    unsupported.is_default       = true;
    addon::Launcher_candidate supported = unsupported;
    supported.candidate_id = "linux_default";
    supported.display_name = "Linux";
    supported.runner_key   = addon::k_runner_bash;
    const std::optional<std::vector<addon::Launcher_candidate>> candidates = addon::decode_discover_result(
        addon::encode_candidates({unsupported, supported}), addon::k_platform_linux_x86_64, &failure);
    SDK_CHECK(candidates.has_value() && candidates->size() == 1 && candidates->front().is_default,
        "a dropped unsupported default must not block the supported default");
    return true;
}

bool test_presentation_media_contracts()
{
    addon::Contract_failure failure;
    const std::string png = valid_png();
    SDK_CHECK(addon::read_png_header(png, &failure).has_value(),
        "a complete structurally valid PNG must be accepted");

    std::string bad_crc = png;
    bad_crc[29] ^= 1;
    SDK_CHECK(!addon::read_png_header(bad_crc, &failure).has_value(),
        "a bad PNG chunk CRC must be rejected");
    std::string bad_idat_crc = png;
    bad_idat_crc[41] ^= 1;   // first byte of the empty IDAT chunk's CRC
    SDK_CHECK(!addon::read_png_header(bad_idat_crc, &failure).has_value(),
        "a bad non-IHDR PNG chunk CRC must be rejected");
    SDK_CHECK(!addon::read_png_header(std::string_view(png).substr(0, png.size() - 12), &failure).has_value(),
        "a PNG without IEND must be rejected");
    SDK_CHECK(!addon::read_png_header(png + "trailing", &failure).has_value(),
        "bytes after IEND must be rejected");

    std::string animated("\x89PNG\r\n\x1A\n", 8);
    std::string ihdr;
    append_be32(ihdr, 64);
    append_be32(ihdr, 64);
    ihdr.append("\x08\x06\x00\x00\x00", 5);
    append_png_chunk(animated, "IHDR", ihdr);
    append_png_chunk(animated, "acTL", std::string(8, '\0'));
    append_png_chunk(animated, "IDAT", "");
    append_png_chunk(animated, "IEND", "");
    SDK_CHECK(!addon::read_png_header(animated, &failure).has_value(),
        "APNG control chunks must be rejected even when their CRC is valid");

    std::string unknown_critical("\x89PNG\r\n\x1A\n", 8);
    append_png_chunk(unknown_critical, "IHDR", ihdr);
    append_png_chunk(unknown_critical, "ABCD", "");
    append_png_chunk(unknown_critical, "IDAT", "");
    append_png_chunk(unknown_critical, "IEND", "");
    SDK_CHECK(!addon::read_png_header(unknown_critical, &failure).has_value(),
        "an unknown critical PNG chunk must fail closed");

    std::string split_idat("\x89PNG\r\n\x1A\n", 8);
    append_png_chunk(split_idat, "IHDR", ihdr);
    append_png_chunk(split_idat, "IDAT", "");
    append_png_chunk(split_idat, "tEXt", "x");
    append_png_chunk(split_idat, "IDAT", "");
    append_png_chunk(split_idat, "IEND", "");
    SDK_CHECK(!addon::read_png_header(split_idat, &failure).has_value(),
        "IDAT chunks must be consecutive across the complete PNG stream");

    SDK_CHECK(addon::inspect_icon(png, &failure).has_value(),
        "inspect_icon must accept a structurally valid in-bounds PNG");
    SDK_CHECK(!addon::inspect_icon(
        std::string(limits::k_max_icon_source_bytes + 1, 'x'),
        &failure).has_value() &&
        failure.reason_code == addon::reason::k_icon_too_large,
        "inspect_icon must enforce the source-byte bound before media parsing");
    for (const std::string& invalid_dimensions : {
             valid_png(limits::k_min_icon_pixels - 1, limits::k_min_icon_pixels),
             valid_png(limits::k_max_icon_pixels + 1, limits::k_max_icon_pixels),
             valid_png(
                 limits::k_min_icon_pixels *
                     (limits::k_max_icon_aspect_ratio + 1),
                 limits::k_min_icon_pixels),
         })
    {
        SDK_CHECK(!addon::inspect_icon(invalid_dimensions, &failure).has_value() &&
            failure.reason_code == addon::reason::k_icon_dimensions_invalid,
            "inspect_icon must enforce PNG side and aspect bounds directly");
    }

    const std::string path_svg =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 32 32\">"
        "<path d=\"M1 1L2 2H3V4C4 4 5 5 6 6S7 7 8 8Q9 9 10 10T11 11A1 2 0 0 1 12 12z\"/>"
        "</svg>";
    SDK_CHECK(addon::sanitize_svg_subset(path_svg, addon::Svg_subset_limits{}, "#010203", &failure).has_value(),
        "the complete bounded path command grammar must be accepted");
    const std::string decimal_path_svg =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 32 32\">"
        "<path d=\"M+.5 -.25L1. 2e+1\"/></svg>";
    SDK_CHECK(addon::sanitize_svg_subset(decimal_path_svg, addon::Svg_subset_limits{}, "#010203", &failure).has_value(),
        "SVG numbers accept signs, decimal points and exponents");
    const std::string overflow_path_svg =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 32 32\">"
        "<path d=\"M0 0L1e400 1\"/></svg>";
    SDK_CHECK(!addon::sanitize_svg_subset(overflow_path_svg, addon::Svg_subset_limits{}, "#010203", &failure).has_value(),
        "SVG numbers outside the double range are rejected");
    const std::string canonical_source =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" color=\"#ABCDEF\" viewBox=\"0 0 32 32\">"
        "<rect height=\"8\" fill=\"currentColor\" width=\"8\"/></svg>";
    const std::optional<addon::Svg_canonical> canonical =
        addon::sanitize_svg_subset(
            canonical_source,
            addon::Svg_subset_limits{},
            "#010203",
            &failure);
    SDK_CHECK(canonical.has_value() &&
        canonical->canonical_bytes.find("color=\"#010203\"") !=
            std::string::npos &&
        canonical->canonical_bytes.find("#ABCDEF") == std::string::npos,
        "canonical SVG must replace the root colour with the selected variant");
    const std::optional<addon::Svg_canonical> canonical_again =
        canonical
            ? addon::sanitize_svg_subset(
                  canonical->canonical_bytes,
                  addon::Svg_subset_limits{},
                  "#010203",
                  &failure)
            : std::nullopt;
    const std::optional<addon::Svg_canonical> dark_canonical =
        addon::sanitize_svg_subset(
            canonical_source,
            addon::Svg_subset_limits{},
            "#F2F2F2",
            &failure);
    SDK_CHECK(canonical_again.has_value() && canonical.has_value() &&
        canonical_again->canonical_bytes == canonical->canonical_bytes,
        "canonical SVG output must be byte-idempotent");
    SDK_CHECK(dark_canonical.has_value() && canonical.has_value() &&
        dark_canonical->canonical_bytes != canonical->canonical_bytes,
        "currentColor variants must have distinct canonical bytes");

    std::vector<std::string> xml_attacks = {
        "\xEF\xBB\xBF<svg xmlns=\"http://www.w3.org/2000/svg\"/>",
        "<!DOCTYPE svg [<!ENTITY x SYSTEM \"file:///etc/passwd\">]>"
            "<svg xmlns=\"http://www.w3.org/2000/svg\"><text>&x;</text></svg>",
        "<?target data?><svg xmlns=\"http://www.w3.org/2000/svg\"/>",
        "<svg xmlns=\"http://www.w3.org/2000/svg\"><![CDATA[text]]></svg>",
        "<svg xmlns=\"http://www.w3.org/2000/svg\"><!--comment--></svg>",
    };
    std::string nul_attack =
        "<svg xmlns=\"http://www.w3.org/2000/svg\">text</svg>";
    nul_attack.insert(nul_attack.find("text"), 1, '\0');
    xml_attacks.push_back(std::move(nul_attack));
    for (const std::string& attack : xml_attacks) {
        SDK_CHECK(!addon::sanitize_svg_subset(
            attack,
            addon::Svg_subset_limits{},
            "#010203",
            &failure).has_value() &&
            failure.reason_code == addon::reason::k_svg_subset_violation,
            "BOM, entity, processing-instruction, CDATA, comment and NUL XML attacks must fail closed");
    }
    const std::string invalid_aspect_svg =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 80 16\"/>";
    SDK_CHECK(!addon::inspect_icon(invalid_aspect_svg, &failure).has_value() &&
        failure.reason_code == addon::reason::k_icon_dimensions_invalid,
        "inspect_icon must enforce the SVG viewBox aspect bound directly");
    const std::string malformed_path =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 32 32\"><path d=\"M1 1 A1 1 0 2 0 3 3\"/></svg>";
    SDK_CHECK(!addon::sanitize_svg_subset(malformed_path, addon::Svg_subset_limits{}, "#010203", &failure).has_value(),
        "an arc flag outside 0/1 must be rejected");
    const std::string malformed_points =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 32 32\"><polygon points=\"0,0 1\"/></svg>";
    SDK_CHECK(!addon::sanitize_svg_subset(malformed_points, addon::Svg_subset_limits{}, "#010203", &failure).has_value(),
        "an incomplete points coordinate pair must be rejected");
    const std::string missing_use_target =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 32 32\"><use/></svg>";
    SDK_CHECK(!addon::sanitize_svg_subset(missing_use_target, addon::Svg_subset_limits{}, "#010203", &failure).has_value(),
        "every use occurrence must name a bounded local target");
    const std::string external_use =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 32 32\"><use href=\"https://example.test/a.svg#x\"/></svg>";
    SDK_CHECK(!addon::sanitize_svg_subset(external_use, addon::Svg_subset_limits{}, "#010203", &failure).has_value(),
        "external SVG references must be rejected");

    const char* const forbidden_svg[] = {
        "<svg xmlns=\"http://www.w3.org/2000/svg\"><script/></svg>",
        "<svg xmlns=\"http://www.w3.org/2000/svg\"><rect onload=\"run()\"/></svg>",
        "<svg xmlns=\"http://www.w3.org/2000/svg\"><rect style=\"fill:red\"/></svg>",
        "<svg xmlns=\"http://www.w3.org/2000/svg\"><image href=\"data:image/png;base64,AA==\"/></svg>",
        "<svg xmlns=\"http://www.w3.org/2000/svg\"><filter/></svg>",
        "<svg xmlns=\"http://www.w3.org/2000/svg\"><mask/></svg>",
        "<svg xmlns=\"http://www.w3.org/2000/svg\"><animate/></svg>",
        "<svg xmlns=\"http://www.w3.org/2000/svg\"><use href=\"javascript:run()\"/></svg>",
    };
    for (const char* svg : forbidden_svg) {
        SDK_CHECK(!addon::sanitize_svg_subset(svg, addon::Svg_subset_limits{}, "#010203", &failure).has_value() &&
            failure.reason_code == addon::reason::k_svg_subset_violation,
            "scripts, events, styles, images, filters, masks, animation and javascript hrefs must fail closed");
    }

    const std::string use_svg =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 32 32\">"
        "<defs><g id=\"tile\"><rect width=\"1\" height=\"1\"/></g></defs>"
        "<use href=\"#tile\"/><use href=\"#tile\"/>"
        "</svg>";
    addon::Svg_subset_limits tight;
    tight.max_expanded_elements = 6;
    SDK_CHECK(!addon::sanitize_svg_subset(use_svg, tight, "#010203", &failure).has_value(),
        "use expansion must be bounded after reference multiplication");
    tight.max_expanded_elements = limits::k_max_svg_expanded_elements;
    SDK_CHECK(addon::sanitize_svg_subset(use_svg, tight, "#010203", &failure).has_value(),
        "bounded local use expansion must remain supported");
    return true;
}

bool test_assets_import_and_storage_contracts()
{
    addon::Integrity_index index;
    index.index_version = 1;
    index.files.push_back(addon::Index_entry{
        "assets/config.json", 16, std::string(64, '0'),
        addon::Resource_kind::ASSET, addon::Resource_origin::PACKAGE, {}, {}});
    addon::Managed_assets_declaration assets{{"assets/config.json"}};
    addon::Contract_failure failure;
    const std::optional<addon::Managed_assets_declaration> decoded_assets =
        addon::decode_managed_assets_declaration(addon::encode_managed_assets_declaration(assets), index, &failure);
    SDK_CHECK(decoded_assets.has_value(), "a bounded indexed managed asset must round-trip");
    addon::Managed_assets_declaration duplicate_assets{{"assets/config.json", "assets/config.json"}};
    SDK_CHECK(!addon::decode_managed_assets_declaration(
        addon::encode_managed_assets_declaration(duplicate_assets), index, &failure).has_value(),
        "a managed-asset declaration must reject duplicate paths");
    addon::Integrity_index oversized_index = index;
    oversized_index.files.front().bytes = limits::k_max_managed_asset_bytes + 1;
    SDK_CHECK(!addon::decode_managed_assets_declaration(
        addon::encode_managed_assets_declaration(assets), oversized_index, &failure).has_value() &&
        failure.reason_code == addon::reason::k_asset_too_large,
        "a managed asset above its individual byte bound must be rejected");

    const fs::path base = make_temp_directory("profile-storage");
    SDK_CHECK(!base.empty(), "a temporary profile root must be available");
    std::string error;
    const auto expected = addon::expected_profile_storage_root(base, "org.example.package", &error);
    SDK_CHECK(expected.has_value() && !fs::exists(addon::path_from_utf8(*expected)),
        "resume matching resolves the package root without allocating it (A3.4)");
    const auto absent_base = base / "unallocated";
    SDK_CHECK(addon::expected_profile_storage_root(absent_base, "org.example.package", &error).has_value() &&
        !fs::exists(absent_base), "the expected-root query never creates its base");
    const std::optional<std::string> root =
        addon::allocate_profile_storage_root(base, "org.example.package", &error);
    SDK_CHECK(root == expected, "allocation echoes the exact root used for resume candidate matching");
    SDK_CHECK(addon::expected_profile_storage_root(base, "org.example.package", &error) == root,
        "the expected root is stable after allocation");
    const std::string scope = addon::profile_storage_provenance_account_id("org.example.package");
    SDK_CHECK(scope == addon::profile_storage_provenance_account_id("org.example.package") &&
        scope != addon::profile_storage_provenance_account_id("org.example.other-package"),
        "the host profile identity is deterministic and package-scoped (A3.4)");
    SDK_CHECK(root.has_value() && fs::is_directory(addon::path_from_utf8(*root)),
        "profile storage must allocate the package-confined default directory");
    const addon::Profile_storage_context context{addon::k_profile_storage_version, *root};
    std::string detail;
    const std::optional<addon::Profile_storage_context> decoded_context =
        addon::decode_profile_storage_context(addon::profile_storage_member(context), &detail);
    SDK_CHECK(decoded_context.has_value() && decoded_context->root == context.root,
        "the extracted profile-storage codec must round-trip independently of the simulator");
    const fs::path outside = base.parent_path() / (base.filename().string() + "-escape");
    const std::string escaping_id = "../" + outside.filename().string();
    SDK_CHECK(!addon::allocate_profile_storage_root(base, escaping_id, &error).has_value(),
        "profile storage allocation must reject a package ID that could escape the base");
    SDK_CHECK(!fs::exists(outside),
        "a rejected package ID must create nothing outside the profile-storage root");
    SDK_CHECK(!addon::expected_profile_storage_root(base, escaping_id, &error).has_value(),
        "resume queries enforce allocation's package containment");

#ifdef _WIN32
    const std::string package_id = "org.example." + std::string(57, 'x') + "." + std::string(58, 'x');
    auto nested_base = fs::canonical(base) / "profile-storage";
    while ((nested_base / package_id / "default").native().size() < 250) {
        nested_base /= "profile-storage";
    }
    auto deep_base = nested_base;
    while (deep_base.native().size() < 260) {
        deep_base /= "profile-storage";
    }
    for (const auto& location : {nested_base, deep_base, deep_base / "new-profile"}) {
        const auto scope_path = location / package_id / "default";
        const auto spelling   = addon::path_to_utf8(scope_path);
        SDK_CHECK(addon::expected_profile_storage_root(location, package_id, &error) == spelling &&
            !fs::exists(addon::native_io_path(location)), "a long expected root is resolved without creating its base");
        SDK_CHECK(addon::allocate_profile_storage_root(location, package_id, &error) == spelling,
            "allocating a long Windows profile root must preserve its canonical spelling: " + error);
        SDK_CHECK(fs::is_directory(addon::native_io_path(scope_path)), "the long package scope must exist on disk");

        const auto extended = addon::native_io_path(location);
        SDK_CHECK(addon::expected_profile_storage_root(extended, package_id, &error) == spelling &&
            addon::allocate_profile_storage_root(extended, package_id, &error) == spelling,
            "an extended spelling must reopen the same profile scope");
    }
#endif

    std::error_code remove_error;
    fs::remove_all(addon::native_io_path(base), remove_error);
    return true;
}

} // namespace sdk_tests

// ---------------------------------------------------------------------------
// Session, lifecycle, ingress and helper-client contracts

namespace sdk_tests {

namespace {

addon::Session_launch_context fixture_launch_context()
{
    addon::Session_launch_context context;
    context.launcher          = addon::Launcher_request{"unknown-fixture-cli", addon::k_runner_direct, ""};
    context.launcher_argv     = {"unknown-fixture-cli"};
    context.launch_environment = addon::Launch_environment{addon::k_platform_linux_x86_64,
        addon::Json_value::object(), std::nullopt};
    context.working_directory = "/repo";
    context.declared_assets   = {"assets/config.json"};
    context.profile_storage   = addon::Profile_storage_context{1, "/profile/org.example.unknown-fixture/default"};
    return context;
}

addon::Process_proposal fixture_structured_process()
{
    addon::Process_proposal process;
    process.executable        = addon::Process_executable::launcher_program();
    process.argv              = {"--review", "--prompt-stdin"};
    process.stdin_behavior    = addon::Stdin_behavior::PAYLOAD;
    process.stdin_payload     = "Review this.";
    process.environment_names = {"PATH", "HOME"};
    process.environment       = {{"EXAMPLE_CLI_MODE", "review"}};
    return process;
}

addon::Lifecycle_observation fixture_started_observation(const std::string& session_id)
{
    addon::Lifecycle_observation observation;
    observation.provider_event_name = "SessionStart";
    observation.session_transition  = addon::Session_transition::STARTED;
    observation.provider_session_id = session_id;
    observation.session_source      = "startup";
    observation.provider_event_id   = "evt-" + session_id;
    observation.provider_sequence   = "1";
    addon::Json_value record = addon::Json_value::object();
    record.set("provider_session_id", addon::Json_value(session_id));
    observation.resume_record = record;
    return observation;
}

addon::Lifecycle_observation fixture_phase_observation(const std::string& event_id)
{
    addon::Lifecycle_observation observation;
    observation.provider_event_name = "Stop";
    observation.phase_transition    = addon::Phase_transition::AWAITING_INPUT;
    observation.phase_evidence      = addon::Evidence::CONFIRMED;
    observation.provider_session_id = "A";
    observation.provider_event_id   = event_id;
    return observation;
}

std::string reason_of(const addon::Contract_failure& failure) { return failure.reason_code + " " + failure.detail; }

/// The reason a decoder reported, or empty when it decoded.
template <typename T>
std::string rejection_of(const std::optional<T>& decoded, const addon::Contract_failure& failure)
{
    return decoded.has_value() ? std::string() : failure.reason_code;
}

addon::Json_value object_with(std::initializer_list<std::pair<const char*, addon::Json_value>> members)
{
    addon::Json_value object = addon::Json_value::object();
    for (const auto& [name, value] : members) {
        object.set(name, value);
    }
    return object;
}

std::string read_all_stdin()
{
    std::string bytes;
    char buffer[4096];
    std::size_t count = 0;
    while ((count = std::fread(buffer, 1, sizeof(buffer), stdin)) > 0) {
        bytes.append(buffer, count);
    }
    return bytes;
}

int answer(const std::string& stdout_bytes, int exit_code)
{
    std::fwrite(stdout_bytes.data(), 1, stdout_bytes.size(), stdout);
    std::fflush(stdout);
    return exit_code;
}

int fake_provider_event_deliver(int argc, char** argv)
{
    if (argc == 4 && std::strcmp(argv[1], "provider-event") == 0 &&
        std::strcmp(argv[2], "acknowledge") == 0)
    {
        return addon::k_helper_exit_committed;
    }
    if (argc != 4 || std::strcmp(argv[2], "deliver") != 0 || std::strcmp(argv[3], "--args-stdin") != 0) {
        std::fprintf(stderr, "fake vnmctl: provider-event deliver --args-stdin expected\n");
        return addon::k_helper_exit_invalid_arguments;
    }
    addon::Json_parse_error parse_error;
    const std::optional<addon::Json_value> arguments =
        addon::parse_json(read_all_stdin(), addon::Json_parse_limits{}, &parse_error);
    const addon::Json_value* key   = arguments ? arguments->find("idempotency_key") : nullptr;
    const addon::Json_value* nonce = arguments ? arguments->find("transition_nonce") : nullptr;
    const addon::Json_value* event = arguments ? arguments->find("event") : nullptr;
    const addon::Json_value* name  = event != nullptr && event->is_object() ? event->find("name") : nullptr;
    if (!arguments || !arguments->is_object() || arguments->as_object().size() != 3 ||
        key == nullptr || !key->is_string() || key->as_string().empty() ||
        key->as_string().size() > limits::k_max_idempotency_key_bytes ||
        nonce == nullptr || !nonce->is_string() || !addon::is_valid_transition_nonce_hex(nonce->as_string()) ||
        name == nullptr || !name->is_string() || event->find("body") == nullptr || !event->find("body")->is_object())
    {
        std::fprintf(stderr, "fake vnmctl: malformed deliver arguments\n");
        return addon::k_helper_exit_invalid_arguments;
    }
    const std::string& scenario = name->as_string();
    addon::Json_value received = addon::Json_value::object();
    received.set("received", *arguments);
    if (scenario == "Committed") {
        addon::Json_value outcome = object_with({
            {"accepted", addon::Json_value(true)},
            {"epoch",    addon::Json_value(3)},
        });
        outcome.set("helper_response", received);
        return answer(addon::serialize_json(outcome), addon::k_helper_exit_committed);
    }
    if (scenario == "Rejected") {
        addon::Json_value outcome = object_with({
            {"accepted", addon::Json_value(false)},
            {"reason",   addon::Json_value("stale_epoch")},
            {"epoch",    addon::Json_value(2)},
        });
        outcome.set("helper_response", received);
        return answer(addon::serialize_json(outcome), addon::k_helper_exit_committed);
    }
    if (scenario == "HostSized") {
        return answer(R"({"accepted":true,"epoch":4})", addon::k_helper_exit_committed);
    }
    if (scenario == "Transport") {
        return addon::k_helper_exit_transport_failure;
    }
    if (scenario == "Pending") {
        return answer(R"({"status":"pending"})", addon::k_helper_exit_pending);
    }
    if (scenario == "Garbage") {
        return answer("this is not a committed outcome", addon::k_helper_exit_committed);
    }
    if (scenario == "Oversize") {
        addon::Json_value outcome = object_with({{"accepted", addon::Json_value(true)}});
        outcome.set("helper_response", addon::Json_value(std::string(limits::k_max_helper_client_stdout_bytes, 'x')));
        return answer(addon::serialize_json(outcome), addon::k_helper_exit_committed);
    }
    std::fprintf(stderr, "fake vnmctl: unknown scenario %s\n", scenario.c_str());
    return addon::k_helper_exit_invalid_arguments;
}

} // namespace

bool is_fake_vnmctl_subcommand(std::string_view argument)
{
    return argument == "provider-event" || argument == "set-title" ||
        argument == "request-title-instruction" || argument == "call";
}

int run_fake_vnmctl(int argc, char** argv)
{
    const std::string_view subcommand = argv[1];
    if (subcommand == "provider-event") {
        return fake_provider_event_deliver(argc, argv);
    }
    if (subcommand == "set-title") {
        if (argc != 3) {
            return addon::k_helper_exit_invalid_arguments;
        }
        return std::strcmp(argv[2], "unreachable") == 0
            ? addon::k_helper_exit_transport_failure
            : addon::k_helper_exit_committed;
    }
    if (subcommand == "request-title-instruction") {
        if (argc != 3 || std::strcmp(argv[2], "--args-stdin") != 0) {
            return addon::k_helper_exit_invalid_arguments;
        }
        addon::Json_parse_error parse_error;
        const std::optional<addon::Json_value> request = addon::parse_json(
            read_all_stdin(), addon::Json_parse_limits{}, &parse_error);
        const addon::Json_value* source = request ? request->find("source") : nullptr;
        const addon::Json_value* event = request ? request->find("hook_event_name") : nullptr;
        const addon::Json_value* changed = request
            ? request->find("changed_files_requested")
            : nullptr;
        const addon::Json_value* delivery = request ? request->find("delivery") : nullptr;
        if (!request || !request->is_object() || request->as_object().size() != (delivery ? 4 : 3) ||
            source == nullptr || !source->is_string() || event == nullptr ||
            !event->is_string() || changed == nullptr || !changed->is_boolean())
        {
            return addon::k_helper_exit_invalid_arguments;
        }
        if (delivery) {
            const auto* version = delivery->find("version");
            const auto* kind = delivery->find("kind");
            if (!delivery->is_object() || !version || !version->is_integer() ||
                !kind || !kind->is_string() || kind->as_string() != "model-context" ||
                event->as_string() != "PreToolUse")
            {
                return addon::k_helper_exit_invalid_arguments;
            }
            if (version->as_integer() == 1) {
                if (delivery->as_object().size() != 2) return addon::k_helper_exit_invalid_arguments;
            }
            else if (version->as_integer() == 2) {
                const auto* activation = delivery->find("activation");
                const auto* id = activation ? activation->find("provider_session_id") : nullptr;
                const auto* nonce = activation ? activation->find("transition_nonce") : nullptr;
                if (delivery->as_object().size() != 3 || !activation || !activation->is_object() ||
                    activation->as_object().size() != 2 || !id || !id->is_string() ||
                    id->as_string() != "captured-a" || !nonce || !nonce->is_string() ||
                    nonce->as_string() != "0123456789abcdef0123456789abcdef")
                {
                    return addon::k_helper_exit_invalid_arguments;
                }
            }
            else {
                return addon::k_helper_exit_invalid_arguments;
            }
        }
        if (source->as_string() == "refused") {
            return answer(R"({"accepted":false,"reason":"unknown_session"})",
                addon::k_helper_exit_committed);
        }
        return answer(
            R"({"accepted":true,"instruction_required":true,"instruction":"host instruction"})",
            addon::k_helper_exit_committed);
    }
    // call logonomic.diff show_changed_files -- <paths...> | call logonomic.diff show --manifest-stdin
    if (argc >= 5 && std::strcmp(argv[2], "logonomic.diff") == 0 && std::strcmp(argv[3], "show_changed_files") == 0 &&
        std::strcmp(argv[4], "--") == 0)
    {
        return argc > 5 ? addon::k_helper_exit_committed : addon::k_helper_exit_invalid_arguments;
    }
    if (argc == 5 && std::strcmp(argv[2], "logonomic.diff") == 0 && std::strcmp(argv[3], "show") == 0 &&
        std::strcmp(argv[4], "--manifest-stdin") == 0)
    {
        addon::Json_parse_error parse_error;
        const std::optional<addon::Json_value> manifest =
            addon::parse_json(read_all_stdin(), addon::Json_parse_limits{}, &parse_error);
        return manifest && manifest->is_object()
            ? addon::k_helper_exit_committed
            : addon::k_helper_exit_invalid_arguments;
    }
    return addon::k_helper_exit_invalid_arguments;
}

bool test_resume_inspection_contracts()
{
    addon::Contract_failure failure;
    addon::Resume_inspect_request request;
    request.context = fixture_launch_context();
    request.context.declared_assets.clear();
    request.context.launch_environment = addon::Launch_environment{
        addon::k_platform_linux_x86_64, object_with({{"HOME", addon::Json_value("/home/alex")}})};
    request.resume_record = object_with({{"provider_session_id", addon::Json_value("conversation-a")}});
    const auto payload = addon::encode_resume_inspect_request(request);
    const auto decoded = addon::decode_resume_inspect_request(payload, &failure);
    SDK_CHECK(decoded && addon::encode_resume_inspect_request(*decoded) == payload,
        "inspection preserves the exact opaque record and resolved read-only context");
    SDK_CHECK(payload.find("resume")->find("version")->as_integer() == 1,
        "inspection does not change the stored resume envelope version");

    for (const char* member : {"resume", "launch_environment", "launcher", "launcher_argv"}) {
        auto missing = payload;
        missing.erase(member);
        SDK_CHECK(!addon::decode_resume_inspect_request(missing, &failure),
            std::string("inspection requires ") + member);
    }
    for (const char* member : {"title_instruction", "startup_observer", "session_wake", "codex_invocation"}) {
        auto unexpected = payload;
        unexpected.set(member, addon::Json_value::object());
        SDK_CHECK(!addon::decode_resume_inspect_request(unexpected, &failure),
            std::string("inspection rejects preparation input ") + member);
    }
    auto assets = payload;
    assets.set("declared_assets", string_array({"assets/config.json"}));
    SDK_CHECK(!addon::decode_resume_inspect_request(assets, &failure),
        "read-only inspection cannot prepare managed assets");
    auto future_envelope = payload;
    future_envelope.find("resume")->set("version", addon::Json_value(2));
    SDK_CHECK(!addon::decode_resume_inspect_request(future_envelope, &failure),
        "unknown stored-envelope versions stay capability-local failures");
    auto oversized = request;
    oversized.resume_record.set("padding", addon::Json_value(std::string(limits::k_max_resume_record_bytes, 'x')));
    SDK_CHECK(!addon::decode_resume_inspect_request(addon::encode_resume_inspect_request(oversized), &failure) &&
        failure.reason_code == addon::reason::k_payload_too_large,
        "inspection enforces the existing opaque-record byte limit");

    const auto digest = addon::resume_inspect_request_digest_hex(request);
    for (const auto availability : {addon::Resume_availability::AVAILABLE,
             addon::Resume_availability::ABSENT, addon::Resume_availability::UNKNOWN})
    {
        const addon::Resume_inspect_result result{availability, digest};
        const auto decoded_result = addon::decode_resume_inspect_result(
            addon::encode_resume_inspect_result(result), &failure);
        SDK_CHECK(decoded_result && decoded_result->availability == availability &&
            addon::validate_resume_inspection(request, *decoded_result, &failure),
            "each inspection disposition acknowledges the complete request");
    }
    const addon::Resume_inspect_result acknowledged{addon::Resume_availability::ABSENT, digest};
    const std::vector<std::function<void(addon::Resume_inspect_request&)>> changes{
        [](auto& value) { value.resume_record.set("revision", addon::Json_value("changed")); },
        [](auto& value) { value.context.working_directory = "/other-repo"; },
        [](auto& value) { value.context.launcher_argv.push_back("--different-profile"); },
        [](auto& value) { value.context.launcher.environment_text = "EXAMPLE_PROFILE=other"; },
        [](auto& value) { value.context.profile_storage->root += "-other"; },
        [](auto& value) { value.context.account_context = addon::Account_context{1, "account-b"}; },
        [](auto& value) {
            value.context.launch_environment->effective_environment.set("HOME", addon::Json_value("/home/blair"));
        },
    };
    for (const auto& change : changes) {
        auto other = request;
        change(other);
        SDK_CHECK(!addon::validate_resume_inspection(other, acknowledged, &failure) &&
            failure.reason_code == addon::reason::k_acknowledgment_mismatch,
            "an answer for another record, profile, environment or launcher must not hide this conversation");
    }
    auto result = addon::encode_resume_inspect_result(acknowledged);
    for (const char* availability : {"missing", "ABSENT", "", "unavailable"}) {
        auto invalid = result;
        invalid.set("availability", addon::Json_value(availability));
        SDK_CHECK(!addon::decode_resume_inspect_result(invalid, &failure),
            "only the three defined availability dispositions are accepted");
    }
    for (const auto& digest_value : {addon::Json_value(true), addon::Json_value(std::string(63, 'a')),
             addon::Json_value(std::string(64, 'A'))})
    {
        auto invalid = result;
        invalid.set("request_digest", digest_value);
        SDK_CHECK(!addon::decode_resume_inspect_result(invalid, &failure),
            "inspection acknowledgment is exactly lowercase SHA-256 hex");
    }
    for (const char* member : {"availability", "request_digest"}) {
        auto invalid = result;
        invalid.erase(member);
        SDK_CHECK(!addon::decode_resume_inspect_result(invalid, &failure),
            "inspection results require both disposition and exact request acknowledgment");
    }
    result.set("process", addon::Json_value::object());
    SDK_CHECK(!addon::decode_resume_inspect_result(result, &failure),
        "inspection cannot return a process proposal or undeclared fields");

    // @4 adds content evidence without changing native availability or the
    // bytes an installed @3 host receives from an upgraded provider.
    for (const auto availability : {addon::Resume_availability::AVAILABLE,
             addon::Resume_availability::ABSENT, addon::Resume_availability::UNKNOWN})
    {
        for (const auto content : {addon::Resume_content::UNKNOWN,
                 addon::Resume_content::EMPTY, addon::Resume_content::NONEMPTY})
        {
            const addon::Resume_inspect_result inspected{availability, digest, content};
            const auto availability_payload = addon::encode_resume_inspect_result(inspected, 3);
            const auto availability_result = addon::decode_resume_inspect_result(availability_payload, &failure, 3);
            SDK_CHECK(!availability_payload.find("content") && availability_result &&
                availability_result->availability == availability &&
                availability_result->content == addon::Resume_content::UNKNOWN,
                "@3 retains its closed result vocabulary and does not claim content evidence");
            const auto content_payload = addon::encode_resume_inspect_result(inspected, 4);
            const auto content_result = addon::decode_resume_inspect_result(content_payload, &failure, 4);
            SDK_CHECK(content_result && content_result->availability == availability &&
                content_result->content == content &&
                addon::validate_resume_inspection(request, *content_result, &failure),
                "@4 content and availability are independent and acknowledge the same exact request");
            SDK_CHECK(!addon::decode_resume_inspect_result(content_payload, &failure, 3),
                "content results require negotiated @4 support");
        }
    }
    auto content_payload = addon::encode_resume_inspect_result(
        {addon::Resume_availability::AVAILABLE, digest, addon::Resume_content::EMPTY}, 4);
    for (const char* content : {"unknown", "future-content-state", "EMPTY", ""}) {
        content_payload.set("content", addon::Json_value(content));
        const auto unknown = addon::decode_resume_inspect_result(content_payload, &failure, 4);
        SDK_CHECK(unknown && unknown->availability == addon::Resume_availability::AVAILABLE &&
            unknown->content == addon::Resume_content::UNKNOWN,
            "unrecognized content never becomes empty or discards native availability");
    }
    content_payload.erase("content");
    const auto omitted = addon::decode_resume_inspect_result(content_payload, &failure, 4);
    SDK_CHECK(omitted && omitted->content == addon::Resume_content::UNKNOWN,
        "omitted optional content defaults to unknown");
    for (const auto& content : {addon::Json_value(), addon::Json_value(true),
             addon::Json_value::object(), addon::Json_value::array()})
    {
        content_payload.set("content", content);
        SDK_CHECK(!addon::decode_resume_inspect_result(content_payload, &failure, 4),
            "malformed optional content cannot certify emptiness");
    }
    return true;
}

bool test_session_contracts()
{
    addon::Contract_failure failure;
    using Prepare_request = addon::Structured_prepare_request;
    using Inspect_request = addon::Structured_inspect_request;
    using Inspect_result  = addon::Structured_inspect_result;

    {
        Inspect_request request;
        request.purpose = addon::Structured_purpose::IMPLEMENTATION;
        auto request_wire = addon::encode_structured_inspect_request(request);
        const auto ordinary_request = addon::decode_structured_inspect_request(request_wire, &failure);
        SDK_CHECK(!request_wire.find("failure_diagnostics") && ordinary_request &&
            !ordinary_request->report_failure_diagnostics,
            "an ordinary inspection keeps the closed wire grammar used by peers without the extension");
        request.report_failure_diagnostics = true;
        request_wire = addon::encode_structured_inspect_request(request);
        const auto opted_in = addon::decode_structured_inspect_request(request_wire, &failure);
        SDK_CHECK(opted_in && opted_in->report_failure_diagnostics,
            "supported failure diagnostics require an explicit inspection request");
        for (const auto& marker : {addon::Json_value(2), addon::Json_value("unsupported")}) {
            auto unsupported = request_wire;
            unsupported.find("failure_diagnostics")->set("version", marker);
            const auto decoded = addon::decode_structured_inspect_request(unsupported, &failure);
            SDK_CHECK(decoded && !decoded->report_failure_diagnostics,
                "an unsupported optional request leaves the core inspection available without opting in");
        }
        auto malformed_request = request_wire;
        malformed_request.set("failure_diagnostics", addon::Json_value("malformed"));
        const auto declined = addon::decode_structured_inspect_request(malformed_request, &failure);
        SDK_CHECK(declined && !declined->report_failure_diagnostics,
            "a malformed optional marker does not reject the ordinary inspection request");

        Inspect_result failed;
        failed.complete = true;
        failed.failure_reason = "implementation_failed";
        failed.failure_diagnostics = addon::Structured_failure_diagnostics{
            "native_error", "The requested model is unavailable.\nPlease select another model.", false};
        const auto ordinary_reply = addon::encode_structured_inspect_result(failed);
        SDK_CHECK(!ordinary_reply.find("failure_diagnostics"),
            "a producer omits diagnostics when an older host did not request them");
        auto wire = addon::encode_structured_inspect_result(failed, true);
        const auto diagnostic = wire.find("failure_diagnostics");
        SDK_CHECK(diagnostic, "an opted-in failed inspection carries diagnostic evidence");
        const addon::Json_value valid_diagnostic = *diagnostic;
        auto decoded = addon::decode_structured_inspect_result(wire, true, &failure);
        SDK_CHECK(decoded && decoded->failure_reason == failed.failure_reason && decoded->failure_diagnostics &&
            decoded->failure_diagnostics->source == "native_error" &&
            decoded->failure_diagnostics->message == failed.failure_diagnostics->message &&
            !decoded->failure_diagnostics->truncated,
            "diagnostic text and its source accompany the unchanged terminal failure");

        std::vector<addon::Json_value> invalid_diagnostics{
            addon::Json_value(), addon::Json_value("malformed"), addon::Json_value::array()};
        const auto invalid_field = [
                &invalid_diagnostics,
                &valid_diagnostic
            ](
                const char* member,
                addon::Json_value value
            )
            {
                auto invalid = valid_diagnostic;
                invalid.set(member, std::move(value));
                invalid_diagnostics.push_back(std::move(invalid));
            };
        invalid_field("version", addon::Json_value(2));
        invalid_field("source", addon::Json_value("unrecognized_source"));
        invalid_field("message", addon::Json_value(""));
        invalid_field("message", addon::Json_value(std::string("native\0message", 14)));
        invalid_field("message", addon::Json_value(std::string("\xc3", 1)));
        invalid_field("message", addon::Json_value(
            std::string(limits::k_max_structured_failure_message_bytes + 1, 'x')));
        invalid_field("truncated", addon::Json_value("false"));
        invalid_field("unexpected", addon::Json_value(true));
        auto missing = valid_diagnostic;
        missing.erase("truncated");
        invalid_diagnostics.push_back(std::move(missing));
        for (const auto& invalid : invalid_diagnostics) {
            wire.set("failure_diagnostics", invalid);
            decoded = addon::decode_structured_inspect_result(wire, true, &failure);
            SDK_CHECK(decoded && decoded->complete && decoded->failure_reason == failed.failure_reason &&
                !decoded->failure_diagnostics,
                "malformed or unsupported diagnostic evidence cannot discard a valid core failure");
        }

        failed.failure_diagnostics->message.assign(limits::k_max_structured_failure_message_bytes, 'x');
        failed.failure_diagnostics->truncated = true;
        decoded = addon::decode_structured_inspect_result(
            addon::encode_structured_inspect_result(failed, true), true, &failure);
        SDK_CHECK(decoded && decoded->failure_diagnostics && decoded->failure_diagnostics->truncated &&
            decoded->failure_diagnostics->message.size() == limits::k_max_structured_failure_message_bytes,
            "the stated diagnostic byte bound is accepted with its truncation flag");

        Inspect_result successful;
        successful.complete = true;
        successful.result_bytes = "validated result";
        auto successful_wire = addon::encode_structured_inspect_result(successful);
        successful_wire.set("failure_diagnostics", valid_diagnostic);
        decoded = addon::decode_structured_inspect_result(successful_wire, true, &failure);
        SDK_CHECK(decoded && decoded->result_bytes == successful.result_bytes && !decoded->failure_diagnostics,
            "evidence on a successful reply does not invent a failure");
        Inspect_result incomplete;
        auto incomplete_wire = addon::encode_structured_inspect_result(incomplete);
        incomplete_wire.set("failure_diagnostics", valid_diagnostic);
        decoded = addon::decode_structured_inspect_result(incomplete_wire, false, &failure);
        SDK_CHECK(decoded && !decoded->complete && !decoded->failure_diagnostics,
            "failure evidence cannot turn an incomplete reply into a completed failure");
        wire.set("failure_diagnostics", valid_diagnostic);
        wire.set("failure_reason", addon::Json_value(3));
        SDK_CHECK(!addon::decode_structured_inspect_result(wire, true, &failure),
            "valid optional evidence never relaxes the core failure grammar");
    }

    // The ACP exchange contract preserves the native 2 MiB UTF-16 continuation
    // bound while each JSON string and the complete RPC remain transport-bounded.
    {
        Inspect_request request;
        request.purpose                   = addon::Structured_purpose::IMPLEMENTATION;
        request.inspection.protocol_state = std::string(1536 * 1024, 's');
        request.inspection.stdout_bytes   = std::string(128 * 1024, '\0');
        request.inspection.stderr_bytes   = "diagnostic";
        const auto encoded = addon::encode_structured_inspect_request(request);
        const auto wire = addon::parse_json(addon::serialize_json(encoded), {}, nullptr);
        SDK_CHECK(wire.has_value(), "chunked ACP continuation parses under ordinary JSON string bounds");
        const auto decoded = addon::decode_structured_inspect_request(*wire, &failure);
        SDK_CHECK(decoded && decoded->inspection.protocol_state == request.inspection.protocol_state &&
            decoded->inspection.stdout_bytes == request.inspection.stdout_bytes &&
            decoded->inspection.stderr_bytes == request.inspection.stderr_bytes,
            "chunked inspection preserves continuation and binary stream bytes");

        auto malformed = encoded;
        malformed.find("inspection")->find("stdout_base64_chunks")->as_array()[0] = addon::Json_value("AA=A");
        SDK_CHECK(!addon::decode_structured_inspect_request(malformed, &failure),
            "invalid base64 cannot enter the inspected stdout");
        malformed = encoded;
        malformed.find("inspection")->set("stdout_base64", addon::Json_value(""));
        SDK_CHECK(!addon::decode_structured_inspect_request(malformed, &failure),
            "inspection member sets remain closed");

        request.inspection.protocol_state = std::string(limits::k_max_exchange_protocol_state_code_units, 's');
        request.inspection.stdout_bytes   = std::string(limits::k_max_structured_stream_bytes, 'o');
        SDK_CHECK(!addon::decode_structured_inspect_request(
                addon::encode_structured_inspect_request(request), &failure) &&
            failure.reason_code == addon::reason::k_payload_too_large,
            "individually bounded streams cannot exceed the aggregate RPC allowance");
    }
    {
        const std::string retained(limits::k_max_exchange_protocol_state_code_units - 2, 's');
        const std::string state = addon::serialize_json(addon::Json_value(retained));
        SDK_CHECK(!addon::parse_json(state, {}, nullptr),
            "the continuation fixture exceeds ordinary JSON string bounds");
        const auto parsed = addon::parse_exchange_protocol_state(state);
        SDK_CHECK(parsed && parsed->is_string() && parsed->as_string() == retained,
            "JSON continuation at the exchange code-unit bound preserves its full string");
        SDK_CHECK(!addon::parse_exchange_protocol_state(state + " "),
            "continuation parsing enforces the code-unit bound including JSON syntax");

        std::string unicode_state = "\"";
        for (std::size_t i = 0; i < limits::k_max_exchange_protocol_state_code_units - 2; ++i) {
            unicode_state += "\xe0\xa0\x80";
        }
        unicode_state += '"';
        const auto unicode = addon::parse_exchange_protocol_state(unicode_state);
        SDK_CHECK(unicode && unicode->is_string() &&
            unicode->as_string() == unicode_state.substr(1, unicode_state.size() - 2),
            "three-byte UTF-8 continuation uses the shared derived byte allowance");
        SDK_CHECK(!addon::parse_exchange_protocol_state(unicode_state + " "),
            "UTF-8 byte allowance does not replace the continuation code-unit bound");
        const auto array = addon::parse_exchange_protocol_state("[]");
        SDK_CHECK(array && array->is_array(), "the caller owns the continuation root type");
        SDK_CHECK(!addon::parse_exchange_protocol_state("{") &&
            !addon::parse_exchange_protocol_state("\"\xf0\x80\x80\x80\"") &&
            !addon::parse_exchange_protocol_state(std::string("\"a") + '\0' + "\""),
            "continuation parsing rejects malformed JSON, invalid UTF-8 and raw NUL");
        const auto depth = limits::k_max_json_depth + 1;
        SDK_CHECK(!addon::parse_exchange_protocol_state(std::string(depth, '[') + "0" + std::string(depth, ']')),
            "continuation parsing retains the ordinary JSON depth bound");
        auto members = addon::Json_value::object();
        for (std::size_t i = 0; i <= limits::k_max_json_members; ++i) {
            members.set(std::to_string(i), addon::Json_value(0));
        }
        SDK_CHECK(!addon::parse_exchange_protocol_state(addon::serialize_json(members)),
            "continuation parsing retains the ordinary JSON member bound");
    }
    {
        std::string unicode_state;
        unicode_state.reserve(2 * limits::k_max_exchange_protocol_state_code_units);
        for (std::size_t i = 0; i < limits::k_max_exchange_protocol_state_code_units; ++i) {
            unicode_state += "\xc3\xa9";
        }
        SDK_CHECK(addon::exchange_protocol_state_valid(unicode_state),
            "continuation bound counts UTF-16 code units rather than UTF-8 bytes");
        unicode_state += "a";
        SDK_CHECK(!addon::exchange_protocol_state_valid(unicode_state), "continuation code-unit limit is enforced");
        SDK_CHECK(!addon::exchange_protocol_state_valid("\xf0\x80\x80\x80"),
            "invalid UTF-8 cannot be counted as continuation state");
    }
    {
        Inspect_result result;
        result.protocol_state = std::string(128 * 1024, 's');
        result.stdin_bytes    = std::string(256 * 1024, 'i');
        auto decoded = addon::decode_structured_inspect_result(
            addon::encode_structured_inspect_result(result), false, &failure);
        SDK_CHECK(decoded && !decoded->close_stdin && decoded->protocol_state == result.protocol_state &&
            decoded->stdin_bytes == result.stdin_bytes, "partial exchange retains state and incremental stdin without EOF");
        result.complete       = true;
        result.protocol_state = {};
        result.stdin_bytes.reset();
        result.result_bytes = std::string(limits::k_max_structured_stream_bytes, 'r');
        const auto wire = addon::parse_json(
            addon::serialize_json(addon::encode_structured_inspect_result(result)), {}, nullptr);
        SDK_CHECK(wire.has_value(), "full ACP result parses under ordinary JSON string bounds");
        decoded = addon::decode_structured_inspect_result(*wire, true, &failure);
        SDK_CHECK(decoded && decoded->result_bytes == result.result_bytes,
            "complete exchange preserves the native result bound");
        result.protocol_state = "unused";
        SDK_CHECK(!addon::decode_structured_inspect_result(
            addon::encode_structured_inspect_result(result), true, &failure),
            "completed exchange must release continuation state");
    }
    {
        Inspect_result result;
        result.protocol_state = "await native EOF";
        result.stdin_bytes = "last input\n";
        result.close_stdin = true;
        auto wire = addon::encode_structured_inspect_result(result);
        auto decoded = addon::decode_structured_inspect_result(wire, false, &failure);
        SDK_CHECK(decoded && decoded->close_stdin && decoded->stdin_bytes == result.stdin_bytes &&
            decoded->protocol_state == result.protocol_state,
            "EOF is requested after input while retaining continuation");
        wire.set("close_stdin", addon::Json_value("true"));
        SDK_CHECK(!addon::decode_structured_inspect_result(wire, false, &failure),
            "EOF control must be Boolean");
        wire.set("close_stdin", addon::Json_value(false));
        decoded = addon::decode_structured_inspect_result(wire, false, &failure);
        SDK_CHECK(decoded && !decoded->close_stdin, "false EOF control has no effect");
        result.close_stdin = false;
        result.stdin_bytes.reset();
        result.complete = true;
        result.protocol_state.clear();
        result.result_bytes = "validated after native EOF";
        wire = addon::encode_structured_inspect_result(result);
        decoded = addon::decode_structured_inspect_result(wire, true, &failure);
        SDK_CHECK(decoded && decoded->complete && decoded->result_bytes == result.result_bytes,
            "final inspection preserves the terminal result grammar");
        wire.set("close_stdin", addon::Json_value(false));
        SDK_CHECK(!addon::decode_structured_inspect_result(wire, true, &failure),
            "completed replies cannot carry EOF control even when false");
        result.close_stdin = true;
        bool rejected = false;
        try {
            (void)addon::encode_structured_inspect_result(result);
        }
        catch (const std::invalid_argument&) {
            rejected = true;
        }
        SDK_CHECK(rejected, "completed replies cannot request EOF on encoding");
    }
    const auto purposes_rejection = [&failure](const addon::Purposes_declaration& declaration) {
        return rejection_of(
            addon::decode_purposes_declaration(addon::encode_purposes_declaration(declaration), &failure), failure);
    };
    const auto proposal_rejection = [&failure](const addon::Json_value& object) {
        return rejection_of(addon::decode_process_proposal(object, &failure), failure);
    };
    const auto prepare_payload_rejection = [&failure](const addon::Json_value& payload) {
        return rejection_of(addon::decode_structured_prepare_request(payload, &failure), failure);
    };
    const auto prepare_rejection = [&prepare_payload_rejection](const Prepare_request& request) {
        return prepare_payload_rejection(addon::encode_structured_prepare_request(request));
    };
    const auto prepare_result_rejection = [&failure](const addon::Structured_prepare_result& result) {
        const addon::Json_value payload = addon::encode_structured_prepare_result(result);
        return rejection_of(addon::decode_structured_prepare_result(payload, &failure), failure);
    };
    const auto inspect_request_rejection = [&failure](const Inspect_request& request) {
        const addon::Json_value payload = addon::encode_structured_inspect_request(request);
        return rejection_of(addon::decode_structured_inspect_request(payload, &failure), failure);
    };
    const auto inspect_result_rejection = [&failure](const Inspect_result& result, bool is_final) {
        const addon::Json_value payload = addon::encode_structured_inspect_result(result);
        return rejection_of(addon::decode_structured_inspect_result(payload, is_final, &failure), failure);
    };
    const auto delegated_rejection = [&failure](const addon::Delegated_prepare_request& request) {
        const addon::Json_value payload = addon::encode_delegated_prepare_request(request);
        return rejection_of(addon::decode_delegated_prepare_request(payload, &failure), failure);
    };
    const auto delegated_result_rejection = [&failure](const addon::Delegated_prepare_result& result) {
        return rejection_of(
            addon::decode_delegated_prepare_result(addon::encode_delegated_prepare_result(result), &failure), failure);
    };
    const auto resume_request_rejection = [&failure](const addon::Resume_prepare_request& request) {
        return rejection_of(
            addon::decode_resume_prepare_request(addon::encode_resume_prepare_request(request), &failure), failure);
    };
    const auto resume_record_rejection = [&failure](const addon::Json_value& record) {
        return rejection_of(addon::decode_resume_record(addon::encode_resume_record(record), &failure), failure);
    };
    // base64: the *_base64 member grammar.
    SDK_CHECK(addon::encode_base64("")    == ""     && addon::encode_base64("A")   == "QQ==" &&
        addon::encode_base64("AB")        == "QUI=" && addon::encode_base64("ABC") == "QUJD",
        "base64 encodes with padding");
    const std::string binary("\x00\xFF\x10\x80", 4);
    SDK_CHECK(addon::decode_base64(addon::encode_base64(binary)) == binary && addon::decode_base64("") == std::string(),
        "base64 round-trips arbitrary bytes");
    SDK_CHECK(!addon::decode_base64("QQ=").has_value() && !addon::decode_base64("Q Q==").has_value() &&
        !addon::decode_base64("QR==").has_value() && !addon::decode_base64("QQ==QQ==").has_value() &&
        !addon::decode_base64("QUJD\n").has_value(),
        "base64 rejects bad length, whitespace, non-canonical padding bits, interior padding and line breaks");

    // Purposes declaration.
    const addon::Purposes_declaration both{
        {addon::Structured_purpose::READ_ONLY_REVIEW, addon::Structured_purpose::IMPLEMENTATION}, {}};
    const std::optional<addon::Purposes_declaration> decoded_purposes =
        addon::decode_purposes_declaration(addon::encode_purposes_declaration(both), &failure);
    SDK_CHECK(decoded_purposes.has_value() && decoded_purposes->purposes == both.purposes,
        "purposes declaration round-trips");
    const addon::Purposes_declaration repeated{
        {addon::Structured_purpose::IMPLEMENTATION, addon::Structured_purpose::IMPLEMENTATION}, {}};
    SDK_CHECK(purposes_rejection(repeated) == addon::reason::k_invalid_sidecar,
        "a repeated purpose is invalid_sidecar");
    SDK_CHECK(!addon::decode_purposes_declaration(
            object_with({{"purposes", addon::Json_value::array()}}), &failure).has_value(),
        "an empty purposes list is rejected");
    addon::Json_value quota_purposes = addon::Json_value::array();
    quota_purposes.push_back(addon::Json_value("quota"));
    SDK_CHECK(!addon::decode_purposes_declaration(object_with({{"purposes", quota_purposes}}), &failure).has_value() &&
        failure.reason_code == addon::reason::k_invalid_sidecar,
        "a purpose outside the current vocabulary is invalid_sidecar");
    addon::Json_value three = addon::encode_purposes_declaration(both);
    three.find("purposes")->push_back(addon::Json_value("read_only_review"));
    SDK_CHECK(!addon::decode_purposes_declaration(three, &failure).has_value(),
        "more than k_max_structured_purposes entries are rejected");
    addon::Json_value extra_member = addon::encode_purposes_declaration(both);
    extra_member.set("deadline_ms", addon::Json_value(1));
    SDK_CHECK(!addon::decode_purposes_declaration(extra_member, &failure).has_value() &&
        failure.reason_code == addon::reason::k_invalid_sidecar, "an unexpected declaration member is invalid_sidecar");

    const addon::Purposes_declaration review_only{{addon::Structured_purpose::READ_ONLY_REVIEW}, {}};
    SDK_CHECK(!addon::validate_delegated_session_dependency(review_only, nullptr, &failure) &&
        failure.reason_code == addon::reason::k_dependency_capability_unavailable,
        "delegated-session@3 without structured-session@3 is dependency_capability_unavailable");
    SDK_CHECK(!addon::validate_delegated_session_dependency(both, &review_only, &failure) &&
        failure.reason_code == addon::reason::k_dependency_capability_unavailable,
        "a delegated purpose the structured declaration lacks is dependency_capability_unavailable");
    SDK_CHECK(addon::validate_delegated_session_dependency(review_only, &both, &failure),
        "delegated purposes covered by the structured declaration satisfy the dependency");

    // The launcher-program reference and the proposal policy.
    addon::Process_proposal process = fixture_structured_process();
    const addon::Json_value encoded_process = addon::encode_process_proposal(process);
    const addon::Json_value* executable = encoded_process.find("executable");
    SDK_CHECK(executable != nullptr && executable->is_object() && executable->find("launcher_program") != nullptr,
        "the launcher program encodes as {\"launcher_program\": true}");
    const std::optional<addon::Process_proposal> decoded_process =
        addon::decode_process_proposal(encoded_process, &failure);
    SDK_CHECK(decoded_process.has_value() &&
        decoded_process->executable == addon::Process_executable::launcher_program() &&
        decoded_process->executable.kind == addon::Process_executable::Kind::LAUNCHER_PROGRAM,
        "the launcher program round-trips");
    for (const addon::Json_value& bad_executable : {
             object_with({{"launcher_program", addon::Json_value(false)}}),
             object_with({{"launcher_program", addon::Json_value(true)}, {"path", addon::Json_value("x")}}),
             addon::Json_value(1),
         })
    {
        addon::Json_value object = encoded_process;
        object.set("executable", bad_executable);
        SDK_CHECK(proposal_rejection(object) == addon::reason::k_frame_violation,
            "any other object or type in executable is frame_violation");
    }
    SDK_CHECK(!addon::validate_process_proposal(process, addon::Process_proposal_policy{}, &failure) &&
        failure.reason_code == addon::reason::k_path_escape, "the launcher program is refused under the probe policy");
    addon::Process_proposal_policy sessions;
    sessions.launcher_program_allowed = true;
    SDK_CHECK(addon::validate_process_proposal(process, sessions, &failure),
        "the launcher program is admitted under the session policy");
    SDK_CHECK(addon::validate_structured_process_proposal(process, &failure),
        "a payload-stdin structured proposal validates");
    addon::Process_proposal inherits = process;
    inherits.stdin_behavior = addon::Stdin_behavior::INHERIT;
    inherits.stdin_payload.clear();
    SDK_CHECK(!addon::validate_structured_process_proposal(inherits, &failure) &&
        failure.reason_code == addon::reason::k_launch_patch_invalid,
        "a structured proposal inheriting stdin is launch_patch_invalid");
    addon::Process_proposal detached = process;
    detached.start = addon::Start_intent::DETACHED;
    SDK_CHECK(!addon::validate_structured_process_proposal(detached, &failure) &&
        failure.reason_code == addon::reason::k_detached_helper_proposal,
        "a detached structured proposal is detached_helper_proposal");
    addon::Process_proposal resume_process;
    resume_process.executable     = addon::Process_executable::launcher_program();
    resume_process.argv           = {"--resume", "A"};
    resume_process.stdin_behavior = addon::Stdin_behavior::INHERIT;
    resume_process.cleanup        = addon::Cleanup_intent::RETAIN_SCRATCH;
    SDK_CHECK(addon::validate_resume_process_proposal(resume_process, &failure),
        "a session-workspace inherit-stdin resume proposal validates");
    addon::Process_proposal wrong_directory = resume_process;
    wrong_directory.working_directory = addon::Working_directory_intent::PACKAGE_ROOT;
    SDK_CHECK(!addon::validate_resume_process_proposal(wrong_directory, &failure) &&
        failure.reason_code == addon::reason::k_launch_patch_invalid,
        "a resume outside the session workspace is launch_patch_invalid");
    addon::Process_proposal closed_stdin = resume_process;
    closed_stdin.stdin_behavior = addon::Stdin_behavior::CLOSED;
    SDK_CHECK(!addon::validate_resume_process_proposal(closed_stdin, &failure) &&
        failure.reason_code == addon::reason::k_launch_patch_invalid,
        "a resume without the terminal's stdin is launch_patch_invalid");
    // Oracle: the terminal consumes the resume launch patch, never the
    // process proposal's environment or asset request channels.
    auto inherited_environment = resume_process;
    inherited_environment.environment_names = {"PATH"};
    auto assigned_environment = resume_process;
    assigned_environment.environment = {{"EXAMPLE_PROFILE", "/profiles/selected"}};
    auto process_assets = resume_process;
    process_assets.assets = {"assets/resume.json"};
    for (const auto& invalid : {inherited_environment, assigned_environment, process_assets}) {
        SDK_CHECK(!addon::validate_resume_process_proposal(invalid, &failure) &&
            failure.reason_code == addon::reason::k_launch_patch_invalid,
            "resume process environment and assets have no terminal consumer");
        SDK_CHECK(addon::validate_process_proposal(invalid, sessions, &failure),
            "process environment and assets remain valid for owned child processes");
    }

    // Current structured prepare.
    addon::Structured_prepare_request prepare;
    prepare.purpose = addon::Structured_purpose::READ_ONLY_REVIEW;
    prepare.context = fixture_launch_context();
    prepare.prompt  = std::string(limits::k_max_structured_prompt_bytes, 'p');
    {
        auto exchange_prepare = prepare;
        addon::Launch_environment environment;
        environment.platform_id = addon::k_platform_windows_x86_64;
        environment.effective_environment.set("OPENCODE_CONFIG", addon::Json_value("C:/profile/opencode.json"));
        exchange_prepare.context.launch_environment = environment;
        auto payload = addon::encode_structured_prepare_request(exchange_prepare);
        const auto decoded = addon::decode_structured_prepare_request(payload, &failure);
        SDK_CHECK(decoded && decoded->context.launch_environment &&
            decoded->context.launch_environment->effective_environment == environment.effective_environment,
            "preparation uses the frozen effective launcher environment");
        payload.erase("launch_environment");
        SDK_CHECK(!addon::decode_structured_prepare_request(payload, &failure),
            "preparation requires the frozen environment");
    }
    const std::optional<addon::Structured_prepare_request> decoded_prepare =
        addon::decode_structured_prepare_request(addon::encode_structured_prepare_request(prepare), &failure);
    SDK_CHECK(decoded_prepare.has_value() && decoded_prepare->purpose == prepare.purpose &&
        decoded_prepare->context.launcher_argv == prepare.context.launcher_argv &&
        decoded_prepare->context.declared_assets == prepare.context.declared_assets &&
        decoded_prepare->context.profile_storage.has_value() &&
        decoded_prepare->context.profile_storage->root == prepare.context.profile_storage->root &&
        decoded_prepare->prompt == prepare.prompt,
        "a structured prepare request with a prompt at k_max_structured_prompt_bytes round-trips: " +
            reason_of(failure));
    prepare.prompt.push_back('p');
    SDK_CHECK(prepare_rejection(prepare) == addon::reason::k_payload_too_large,
        "a prompt one byte above the bound is payload_too_large");
    prepare.prompt = "Review this.";
    addon::Json_value prepare_payload = addon::encode_structured_prepare_request(prepare);
    prepare_payload.set("shell", addon::Json_value("bash"));
    SDK_CHECK(prepare_payload_rejection(prepare_payload) == addon::reason::k_unexpected_member,
        "an unexpected prepare member is unexpected_member");
    prepare_payload = addon::encode_structured_prepare_request(prepare);
    prepare_payload.set("purpose", addon::Json_value("quota"));
    SDK_CHECK(prepare_payload_rejection(prepare_payload) == addon::reason::k_frame_violation,
        "a purpose outside the vocabulary is frame_violation");
    prepare_payload = addon::encode_structured_prepare_request(prepare);
    prepare_payload.set("prompt_base64", addon::Json_value("not base64!"));
    SDK_CHECK(prepare_payload_rejection(prepare_payload) == addon::reason::k_frame_violation,
        "a prompt that is not base64 is frame_violation");
    addon::Structured_prepare_request bounds = prepare;
    bounds.context.launcher_argv.assign(limits::k_max_argv_entries + 1, "x");
    SDK_CHECK(prepare_rejection(bounds) == addon::reason::k_payload_too_large,
        "launcher_argv above k_max_argv_entries is payload_too_large");
    bounds = prepare;
    bounds.context.launcher_argv.clear();
    SDK_CHECK(prepare_rejection(bounds) == addon::reason::k_frame_violation,
        "an empty launcher_argv is frame_violation");
    bounds = prepare;
    bounds.context.working_directory = std::string(limits::k_max_working_directory_bytes + 1, 'w');
    SDK_CHECK(prepare_rejection(bounds) == addon::reason::k_payload_too_large,
        "working_directory above its bound is payload_too_large");
    bounds = prepare;
    bounds.context.declared_assets = {"helpers/tool"};
    SDK_CHECK(prepare_rejection(bounds) == addon::reason::k_frame_violation,
        "a declared asset outside assets/ is frame_violation");

    addon::Structured_prepare_result prepare_result;
    prepare_result.process                   = process;
    prepare_result.inspection.incremental    = true;
    prepare_result.inspection.protocol_state = std::string(limits::k_max_exchange_protocol_state_code_units, 's');
    prepare_result.profile_storage           = prepare.context.profile_storage;
    {
        auto exchange_result = prepare_result;
        exchange_result.inspection.keep_stdin_open = true;
        exchange_result.inspection.protocol_state = std::string(128 * 1024, 's');
        const auto encoded = addon::encode_structured_prepare_result(exchange_result);
        const auto decoded = addon::decode_structured_prepare_result(encoded, &failure);
        SDK_CHECK(decoded && decoded->inspection.keep_stdin_open && decoded->inspection.incremental &&
            decoded->inspection.protocol_state == exchange_result.inspection.protocol_state,
            "preparation retains persistent stdin and expanded continuation");
        exchange_result.inspection.incremental = false;
        SDK_CHECK(!addon::decode_structured_prepare_result(
            addon::encode_structured_prepare_result(exchange_result), &failure),
            "persistent stdin requires incremental inspection");
        exchange_result.inspection.incremental = true;
        exchange_result.process.stdin_behavior = addon::Stdin_behavior::INHERIT;
        SDK_CHECK(!addon::decode_structured_prepare_result(
            addon::encode_structured_prepare_result(exchange_result), &failure),
            "a structured stdin pipe cannot inherit interactive stdin");
    }
    const std::optional<addon::Structured_prepare_result> decoded_prepare_result =
        addon::decode_structured_prepare_result(addon::encode_structured_prepare_result(prepare_result), &failure);
    SDK_CHECK(decoded_prepare_result.has_value() && decoded_prepare_result->process.executable == process.executable &&
        decoded_prepare_result->process.argv == process.argv && decoded_prepare_result->inspection.incremental &&
        decoded_prepare_result->inspection.protocol_state == prepare_result.inspection.protocol_state &&
        decoded_prepare_result->profile_storage.has_value(),
        "a structured prepare result with a protocol state at the current code-unit bound round-trips: " +
            reason_of(failure));
    prepare_result.inspection.protocol_state.push_back('s');
    SDK_CHECK(prepare_result_rejection(prepare_result) == addon::reason::k_payload_too_large,
        "a protocol state above the bound is payload_too_large");
    addon::Json_value result_payload = addon::encode_structured_prepare_result(prepare_result);
    result_payload.erase("inspection");
    SDK_CHECK(!addon::decode_structured_prepare_result(result_payload, &failure).has_value() &&
        failure.reason_code == addon::reason::k_frame_violation,
        "a prepare result without inspection is frame_violation");

    // Current chunked structured inspection.
    addon::Structured_inspect_request inspect;
    inspect.purpose                   = addon::Structured_purpose::IMPLEMENTATION;
    inspect.inspection.protocol_state = "state-1";
    inspect.inspection.stdout_bytes   = std::string(limits::k_max_structured_stream_bytes, 'o');
    inspect.inspection.stderr_bytes   = binary;
    inspect.inspection.final          = true;
    inspect.inspection.timed_out      = true;
    inspect.inspection.exit_code      = -7;
    const addon::Json_value encoded_inspect = addon::encode_structured_inspect_request(inspect);
    const std::optional<addon::Json_value> inspect_wire = addon::parse_json(
        addon::serialize_json(encoded_inspect), addon::Json_parse_limits{}, nullptr);
    const std::optional<addon::Structured_inspect_request> decoded_inspect = inspect_wire
        ? addon::decode_structured_inspect_request(*inspect_wire, &failure)
        : std::nullopt;
    SDK_CHECK(decoded_inspect.has_value() && decoded_inspect->purpose == inspect.purpose &&
        decoded_inspect->inspection.stdout_bytes == inspect.inspection.stdout_bytes &&
        decoded_inspect->inspection.stderr_bytes == binary && decoded_inspect->inspection.final &&
        decoded_inspect->inspection.timed_out && decoded_inspect->inspection.exit_code == -7,
        "a chunked inspect request with stdout at the current stream bound round-trips: " + reason_of(failure));
    inspect.inspection.stdout_bytes.push_back('o');
    SDK_CHECK(inspect_request_rejection(inspect) == addon::reason::k_payload_too_large,
        "stdout above the current stream bound is payload_too_large");
    inspect.inspection.stdout_bytes.clear();
    inspect.inspection.stderr_bytes = std::string(limits::k_max_structured_stderr_bytes + 1, 'e');
    SDK_CHECK(inspect_request_rejection(inspect) == addon::reason::k_payload_too_large,
        "stderr above its bound is payload_too_large");
    inspect.inspection.stderr_bytes.clear();
    addon::Json_value inspect_payload = addon::encode_structured_inspect_request(inspect);
    inspect_payload.find("inspection")->set("exit_code", addon::Json_value((std::int64_t)INT32_MAX + 1));
    SDK_CHECK(!addon::decode_structured_inspect_request(inspect_payload, &failure).has_value() &&
        failure.reason_code == addon::reason::k_frame_violation, "an exit code outside 32 bits is frame_violation");

    addon::Structured_inspect_result incomplete;
    incomplete.complete       = false;
    incomplete.protocol_state = "state-2";
    incomplete.stdin_bytes    = std::string("y\n");
    const std::optional<addon::Structured_inspect_result> decoded_incomplete =
        addon::decode_structured_inspect_result(addon::encode_structured_inspect_result(incomplete), false, &failure);
    SDK_CHECK(decoded_incomplete.has_value() && !decoded_incomplete->complete &&
        decoded_incomplete->stdin_bytes == std::string("y\n") &&
        decoded_incomplete->presentation_state == addon::Presentation_state::UNAVAILABLE,
        "an incomplete inspection writing stdin round-trips without presentation");
    incomplete.stdin_bytes = std::string(limits::k_max_structured_stream_bytes, 'i');
    SDK_CHECK(inspect_result_rejection(incomplete, false).empty(),
        "stdin at the current chunked stream bound decodes");
    incomplete.stdin_bytes->push_back('i');
    SDK_CHECK(inspect_result_rejection(incomplete, false) == addon::reason::k_payload_too_large,
        "stdin above the current stream bound is payload_too_large");
    incomplete.stdin_bytes = std::string("y\n");
    SDK_CHECK(inspect_result_rejection(incomplete, true) == addon::reason::k_inspection_invalid,
        "a final inspection answered incomplete is inspection_invalid");
    addon::Structured_inspect_result complete;
    complete.complete           = true;
    complete.protocol_state     = "";
    complete.result_bytes       = std::string(limits::k_max_structured_stream_bytes, 'r');
    complete.presentation_state = addon::Presentation_state::AVAILABLE;
    complete.presentation       = {
        {addon::Presentation_event_kind::REVIEW_INITIALIZED, 1},
        {addon::Presentation_event_kind::TOOL_RESPONSE,      3},
    };
    const addon::Json_value encoded_complete = addon::encode_structured_inspect_result(complete);
    const std::optional<addon::Json_value> complete_wire = addon::parse_json(
        addon::serialize_json(encoded_complete), addon::Json_parse_limits{}, nullptr);
    const std::optional<addon::Structured_inspect_result> decoded_complete = complete_wire
        ? addon::decode_structured_inspect_result(*complete_wire, true, &failure)
        : std::nullopt;
    SDK_CHECK(decoded_complete.has_value() && decoded_complete->complete &&
        decoded_complete->result_bytes == complete.result_bytes &&
        decoded_complete->presentation_state == addon::Presentation_state::AVAILABLE &&
        decoded_complete->presentation.size() == 2 &&
        decoded_complete->presentation[1].kind == addon::Presentation_event_kind::TOOL_RESPONSE &&
        decoded_complete->presentation[1].count == 3,
        "a complete inspection with a result at the current chunked stream bound and presentation round-trips: " + reason_of(failure));
    complete.result_bytes->push_back('r');
    SDK_CHECK(inspect_result_rejection(complete, true) == addon::reason::k_payload_too_large,
        "result above the current stream bound is payload_too_large");
    complete.result_bytes->pop_back();
    complete.failure_reason = std::string("startup_failed");
    SDK_CHECK(inspect_result_rejection(complete, true) == addon::reason::k_inspection_invalid,
        "result and failure_reason together are inspection_invalid");
    complete.result_bytes.reset();
    SDK_CHECK(inspect_result_rejection(complete, true).empty(),
        "a complete inspection with a failure reason alone decodes");
    complete.failure_reason = std::string("Vendor-Down");
    SDK_CHECK(inspect_result_rejection(complete, true) == addon::reason::k_frame_violation,
        "a failure reason outside the reason-code grammar is frame_violation");
    complete.failure_reason.reset();
    SDK_CHECK(inspect_result_rejection(complete, true) == addon::reason::k_inspection_invalid,
        "complete without result or failure_reason is inspection_invalid");
    complete.result_bytes = std::string("done");
    complete.stdin_bytes  = std::string("more");
    SDK_CHECK(inspect_result_rejection(complete, true) == addon::reason::k_inspection_invalid,
        "complete with pending stdin is inspection_invalid");
    complete.stdin_bytes.reset();
    incomplete.result_bytes = std::string("early");
    SDK_CHECK(inspect_result_rejection(incomplete, false) == addon::reason::k_inspection_invalid,
        "incomplete with a result is inspection_invalid");
    incomplete.result_bytes.reset();
    for (const addon::Json_value& bad_presentation : {
             addon::Json_value("records"),
             object_with({{"version", addon::Json_value(2)}, {"records", addon::Json_value::array()}}),
             object_with({{"version", addon::Json_value(1)}, {"records", addon::Json_value::array()}}),
         })
    {
        addon::Json_value payload = addon::encode_structured_inspect_result(complete);
        payload.set("presentation", bad_presentation);
        const std::optional<addon::Structured_inspect_result> tolerated =
            addon::decode_structured_inspect_result(payload, true, &failure);
        SDK_CHECK(tolerated.has_value() && tolerated->presentation_state == addon::Presentation_state::INVALID &&
            !tolerated->presentation_error.empty() && tolerated->complete,
            "a rejected presentation member leaves the inspection standing as INVALID");
    }
    {
        addon::Structured_inspect_result many = complete;
        many.presentation_state = addon::Presentation_state::AVAILABLE;
        many.presentation.assign(
            limits::k_max_presentation_records_per_inspection + 1, {addon::Presentation_event_kind::REASONING, 1});
        const std::optional<addon::Structured_inspect_result> tolerated =
            addon::decode_structured_inspect_result(addon::encode_structured_inspect_result(many), true, &failure);
        SDK_CHECK(tolerated.has_value() && tolerated->presentation_state == addon::Presentation_state::INVALID,
            "more than k_max_presentation_records_per_inspection records is INVALID, not a failed inspection");
        many.presentation.pop_back();
        const std::optional<addon::Structured_inspect_result> at_bound =
            addon::decode_structured_inspect_result(addon::encode_structured_inspect_result(many), true, &failure);
        SDK_CHECK(at_bound.has_value() && at_bound->presentation_state == addon::Presentation_state::AVAILABLE &&
            at_bound->presentation.size() == limits::k_max_presentation_records_per_inspection,
            "exactly k_max_presentation_records_per_inspection records are AVAILABLE");
        addon::Structured_inspect_result invalid_state = complete;
        invalid_state.presentation_state = addon::Presentation_state::INVALID;
        SDK_CHECK(addon::encode_structured_inspect_result(invalid_state).find("presentation") == nullptr,
            "an INVALID presentation state encodes without the member");
    }

    // delegated-session@3.
    addon::Delegated_prepare_request delegated;
    delegated.purpose = addon::Structured_purpose::IMPLEMENTATION;
    delegated.context = fixture_launch_context();
    delegated.prompt  = "Implement this.";
    delegated.control.task_id               = "task-7f3a";
    delegated.control.private_artifact_root = "/repo/.logonomic/delegation/task-7f3a";
    addon::Delegated_task_server coordination;
    coordination.server_id   = "coordination";
    coordination.program     = "/opt/logonomic/logonomic_coordination_mcp";
    coordination.actions     = {"status", "notice"};
    coordination.environment = {{"VNM_CONTROL_ENDPOINT", "vnm-control-example"}, {"VNM_CONTROL_TOKEN", "secret"}};
    addon::Delegated_task_server delegation = coordination;
    delegation.server_id = "delegation";
    delegation.program   = "C:\\Program Files\\Logonomic\\logonomic_delegation_mcp.exe";
    delegated.control.servers = {coordination, delegation};
    const std::optional<addon::Delegated_prepare_request> decoded_delegated =
        addon::decode_delegated_prepare_request(addon::encode_delegated_prepare_request(delegated), &failure);
    SDK_CHECK(decoded_delegated.has_value() && decoded_delegated->control.task_id == "task-7f3a" &&
        decoded_delegated->control.servers.size() == 2 &&
        decoded_delegated->control.servers[1].program == delegation.program &&
        decoded_delegated->control.servers[0].environment == coordination.environment &&
        decoded_delegated->control.servers[0].actions == coordination.actions &&
        decoded_delegated->control.private_artifact_root == delegated.control.private_artifact_root,
        "a delegated prepare request with two servers round-trips: " + reason_of(failure));
    addon::Delegated_prepare_request delegated_bounds = delegated;
    delegated_bounds.control.servers.clear();
    for (std::size_t index = 0; index <= limits::k_max_delegated_task_servers; ++index) {
        addon::Delegated_task_server server = coordination;
        server.server_id = "server-" + std::to_string(index);
        delegated_bounds.control.servers.push_back(server);
    }
    SDK_CHECK(delegated_rejection(delegated_bounds) == addon::reason::k_payload_too_large,
        "more than k_max_delegated_task_servers is payload_too_large");
    delegated_bounds = delegated;
    delegated_bounds.control.task_id = std::string(limits::k_max_delegated_task_id_bytes + 1, 't');
    SDK_CHECK(delegated_rejection(delegated_bounds) == addon::reason::k_payload_too_large,
        "a task id above k_max_delegated_task_id_bytes is payload_too_large");
    delegated_bounds = delegated;
    delegated_bounds.control.servers[1].server_id = "coordination";
    SDK_CHECK(delegated_rejection(delegated_bounds) == addon::reason::k_frame_violation,
        "a repeated server_id is frame_violation");
    delegated_bounds = delegated;
    delegated_bounds.control.servers[0].program = "bin/mcp";
    SDK_CHECK(delegated_rejection(delegated_bounds) == addon::reason::k_frame_violation,
        "a relative server program is frame_violation");
    delegated_bounds = delegated;
    delegated_bounds.control.servers[0].environment.clear();
    for (std::size_t index = 0; index <= limits::k_max_capability_environment_entries; ++index) {
        delegated_bounds.control.servers[0].environment.emplace_back("V" + std::to_string(index), "x");
    }
    SDK_CHECK(delegated_rejection(delegated_bounds) == addon::reason::k_payload_too_large,
        "a server environment above the capability bound is payload_too_large");
    addon::Json_value delegated_payload = addon::encode_delegated_prepare_request(delegated);
    delegated_payload.find("control")->set("version", addon::Json_value(2));
    SDK_CHECK(!addon::decode_delegated_prepare_request(delegated_payload, &failure).has_value() &&
        failure.reason_code == addon::reason::k_frame_violation, "a control version other than 1 is frame_violation");

    addon::Delegated_prepare_result delegated_result;
    delegated_result.process                   = process;
    delegated_result.acknowledgment.server_ids = {"delegation", "coordination"};
    delegated_result.private_artifacts         = {
        {"mcp/servers.json", std::string(limits::k_max_delegated_private_artifact_bytes, 'a')},
        {"notes.txt", binary},
    };
    delegated_result.inspection.incremental = true;
    const std::optional<addon::Delegated_prepare_result> decoded_delegated_result =
        addon::decode_delegated_prepare_result(addon::encode_delegated_prepare_result(delegated_result), &failure);
    SDK_CHECK(decoded_delegated_result.has_value() && decoded_delegated_result->private_artifacts.size() == 2 &&
        decoded_delegated_result->private_artifacts[0].content == delegated_result.private_artifacts[0].content &&
        decoded_delegated_result->private_artifacts[1].content == binary &&
        decoded_delegated_result->acknowledgment.server_ids == delegated_result.acknowledgment.server_ids,
        "a delegated prepare result with an artifact at k_max_delegated_private_artifact_bytes round-trips: " +
            reason_of(failure));
    SDK_CHECK(addon::validate_delegated_acknowledgment(
            delegated.control, decoded_delegated_result->acknowledgment, &failure),
        "a set-equal acknowledgment validates regardless of order");
    addon::Delegated_acknowledgment partial{{"coordination"}};
    {
        addon::Delegated_prepare_request connected = delegated;
        connected.control.connection = addon::Delegated_connection{
            "/opt/logonomic/logonomic_task_server_relay",
            "/repo/.logonomic/delegation/task-7f3a/.logonomic/task-servers.json"};
        addon::Json_value payload = addon::encode_delegated_prepare_request(connected);
        const auto decoded = addon::decode_delegated_prepare_request(payload, &failure);
        SDK_CHECK(decoded && decoded->control.connection &&
            decoded->control.connection->relay_program == connected.control.connection->relay_program &&
            decoded->control.connection->descriptor_path == connected.control.connection->descriptor_path &&
            decoded->control.servers[0].environment.empty() && decoded->control.servers[1].environment.empty() &&
            connected.control.servers[0].environment == coordination.environment,
            "connection metadata round-trips without serializing or changing private host environments");
        addon::Delegated_prepare_result connected_result = delegated_result;
        SDK_CHECK(!addon::validate_delegated_acknowledgment(
            connected.control, connected_result.acknowledgment, &failure),
            "server IDs alone do not acknowledge a connection-bearing request");
        connected_result.acknowledgment.connection_version = addon::k_delegated_connection_version;
        const auto decoded_result = addon::decode_delegated_prepare_result(
            addon::encode_delegated_prepare_result(connected_result), &failure);
        SDK_CHECK(decoded_result && addon::validate_delegated_acknowledgment(
            connected.control, decoded_result->acknowledgment, &failure) &&
            !addon::validate_delegated_acknowledgment(delegated.control, decoded_result->acknowledgment, &failure),
            "explicit connection acknowledgment validates only when requested");
        connected_result.acknowledgment.connection_version = std::uint16_t{2};
        SDK_CHECK(!addon::validate_delegated_acknowledgment(
            connected.control, connected_result.acknowledgment, &failure) &&
            !addon::decode_delegated_prepare_result(
                addon::encode_delegated_prepare_result(connected_result), &failure),
            "unknown connection acknowledgment cannot admit delegated preparation");
        payload.find("control")->find("connection")->set("version", addon::Json_value(2));
        SDK_CHECK(!addon::decode_delegated_prepare_request(payload, &failure) &&
            failure.reason_code == addon::reason::k_unsupported_version,
            "unknown connection version refuses only this delegated request");
        for (const char* field : {"relay_program", "descriptor_path"}) {
            payload = addon::encode_delegated_prepare_request(connected);
            payload.find("control")->find("connection")->set(field, addon::Json_value("relative/path"));
            SDK_CHECK(!addon::decode_delegated_prepare_request(payload, &failure),
                "connection paths must be absolute without requiring prepare-time file existence");
        }
        payload = addon::encode_delegated_prepare_request(connected);
        addon::Json_value credential = addon::Json_value::object();
        credential.set("VNM_CONTROL_TOKEN", addon::Json_value("secret"));
        payload.find("control")->find("servers")->as_array()[0].set("environment", std::move(credential));
        SDK_CHECK(!addon::decode_delegated_prepare_request(payload, &failure),
            "connection-bearing wire requests must not carry private server environments");
    }
    addon::Delegated_acknowledgment extra{{"coordination", "delegation", "vendor"}};
    SDK_CHECK(!addon::validate_delegated_acknowledgment(delegated.control, partial, &failure) &&
        failure.reason_code == addon::reason::k_acknowledgment_mismatch &&
        !addon::validate_delegated_acknowledgment(delegated.control, extra, &failure) &&
        failure.reason_code == addon::reason::k_acknowledgment_mismatch,
        "a missing or extra server id is acknowledgment_mismatch");
    addon::Delegated_prepare_result artifact_bounds = delegated_result;
    artifact_bounds.private_artifacts[0].content.push_back('a');
    SDK_CHECK(delegated_result_rejection(artifact_bounds) == addon::reason::k_private_artifact_invalid,
        "an artifact above its byte bound is private_artifact_invalid");
    artifact_bounds = delegated_result;
    artifact_bounds.private_artifacts.assign(limits::k_max_delegated_private_artifacts + 1, {"a.txt", "x"});
    for (std::size_t index = 0; index < artifact_bounds.private_artifacts.size(); ++index) {
        artifact_bounds.private_artifacts[index].relative_path = "a" + std::to_string(index) + ".txt";
    }
    SDK_CHECK(delegated_result_rejection(artifact_bounds) == addon::reason::k_private_artifact_invalid,
        "more than k_max_delegated_private_artifacts is private_artifact_invalid");
    for (const char* bad_path : {"../escape.json", "/abs.json", "mcp//servers.json", "mcp\\servers.json", "./x", ""}) {
        artifact_bounds = delegated_result;
        artifact_bounds.private_artifacts[1].relative_path = bad_path;
        SDK_CHECK(delegated_result_rejection(artifact_bounds) == addon::reason::k_private_artifact_invalid,
            std::string("an artifact path outside the relative-path rule is private_artifact_invalid: ") + bad_path);
    }
    artifact_bounds = delegated_result;
    artifact_bounds.private_artifacts[1].relative_path = "mcp/servers.json";
    SDK_CHECK(delegated_result_rejection(artifact_bounds) == addon::reason::k_private_artifact_invalid,
        "two artifacts at one path are private_artifact_invalid");

    // interactive-resume@2.
    addon::Json_value record = object_with({{"provider_session_id", addon::Json_value("A")}});
    addon::Resume_prepare_request resume;
    resume.context           = fixture_launch_context();
    resume.title_instruction = k_production_title_instruction;
    resume.resume_record     = record;
    const std::optional<addon::Resume_prepare_request> decoded_resume =
        addon::decode_resume_prepare_request(addon::encode_resume_prepare_request(resume), &failure);
    SDK_CHECK(decoded_resume.has_value() && decoded_resume->resume_record == record &&
        decoded_resume->title_instruction == resume.title_instruction &&
        decoded_resume->context.launcher_argv == resume.context.launcher_argv,
        "prepare_resume must preserve the complete production title instruction: " + reason_of(failure));
    auto frozen_resume = resume;
    frozen_resume.context.launch_environment =
        addon::Launch_environment{"linux-x86_64", addon::Json_value::object(), std::nullopt};
    frozen_resume.context.launch_environment->effective_environment.set(
        "PROFILE_HOME", addon::Json_value("/profiles/work"));
    const auto frozen_resume_payload = addon::encode_resume_prepare_request(frozen_resume, 2);
    const auto decoded_frozen_resume = addon::decode_resume_prepare_request(frozen_resume_payload, &failure, 2);
    SDK_CHECK(decoded_frozen_resume && decoded_frozen_resume->context.launch_environment &&
        decoded_frozen_resume->context.launch_environment->effective_environment ==
            frozen_resume.context.launch_environment->effective_environment,
        "resume2 preserves the frozen environment values");
    auto invalid_resume_environment = frozen_resume_payload;
    invalid_resume_environment.erase("launch_environment");
    SDK_CHECK(!addon::decode_resume_prepare_request(invalid_resume_environment, &failure, 2),
        "resume2 requires the frozen environment");
    invalid_resume_environment.set("launch_environment", addon::Json_value(true));
    SDK_CHECK(!addon::decode_resume_prepare_request(invalid_resume_environment, &failure, 2),
        "resume2 rejects a malformed environment");
    addon::Json_value resume_payload = addon::encode_resume_prepare_request(resume);
    resume_payload.find("resume")->set("version", addon::Json_value(2));
    SDK_CHECK(!addon::decode_resume_prepare_request(resume_payload, &failure).has_value() &&
        failure.reason_code == addon::reason::k_frame_violation,
        "a resume envelope version other than 1 is frame_violation");
    addon::Json_value large_record =
        object_with({{"blob", addon::Json_value(std::string(limits::k_max_resume_record_bytes - 11, 'b'))}});
    SDK_CHECK(addon::serialize_json_canonical(large_record).size() == limits::k_max_resume_record_bytes,
        "the large record fixture sits exactly at k_max_resume_record_bytes");
    SDK_CHECK(resume_record_rejection(large_record).empty(),
        "a resume record at k_max_resume_record_bytes decodes");
    large_record.set("blob", addon::Json_value(std::string(limits::k_max_resume_record_bytes - 10, 'b')));
    SDK_CHECK(resume_record_rejection(large_record) == addon::reason::k_payload_too_large,
        "a resume record one byte above the bound is payload_too_large");
    for (const std::size_t size : {
            limits::k_max_title_instruction_bytes - 1,
            limits::k_max_title_instruction_bytes})
    {
        resume.title_instruction = std::string(size, 't');
        const auto decoded = addon::decode_resume_prepare_request(
            addon::encode_resume_prepare_request(resume), &failure);
        SDK_CHECK(decoded.has_value() && decoded->title_instruction == resume.title_instruction,
            "prepare_resume must preserve title instructions below and at the byte bound");
    }
    resume.title_instruction.push_back('t');
    SDK_CHECK(resume_request_rejection(resume) == addon::reason::k_payload_too_large,
        "a title instruction above its bound is payload_too_large");

    addon::Resume_prepare_result resume_result;
    resume_result.process                        = resume_process;
    resume_result.patch.set                      = {{"EXAMPLE_CLI_RESUME", "1"}};
    resume_result.patch.unset                    = {"EXAMPLE_CLI_OLD"};
    resume_result.patch.profile_storage          = fixture_launch_context().profile_storage;
    resume_result.acknowledgment.record_digest_hex = addon::resume_record_digest_hex(record);
    const std::optional<addon::Resume_prepare_result> decoded_resume_result =
        addon::decode_resume_prepare_result(addon::encode_resume_prepare_result(resume_result), &failure);
    SDK_CHECK(decoded_resume_result.has_value() && decoded_resume_result->patch.set == resume_result.patch.set &&
        decoded_resume_result->patch.unset == resume_result.patch.unset &&
        decoded_resume_result->patch.profile_storage.has_value() &&
        decoded_resume_result->patch.working_directory == addon::Working_directory_patch::INHERIT &&
        decoded_resume_result->process.argv == resume_process.argv &&
        decoded_resume_result->acknowledgment.record_digest_hex == resume_result.acknowledgment.record_digest_hex,
        "a prepare_resume result preserves the shared current patch fields: " + reason_of(failure));
    const auto resume_wire = addon::encode_resume_prepare_result(resume_result);
    SDK_CHECK(!resume_wire.find("disposition") && !decoded_resume_result->patch.disposition,
        "resume does not invent a launch-preparation disposition");
    SDK_CHECK(addon::validate_resume_process_proposal(decoded_resume_result->process, &failure),
        "a round-tripped resume with environment in its patch satisfies terminal process policy");
    SDK_CHECK(addon::validate_launch_patch(decoded_resume_result->patch, {}, {}, &failure),
        "the resume environment follows the launch-patch rules");
    SDK_CHECK(addon::resume_record_digest_hex(record) ==
            addon::to_hex(addon::Sha256::digest(addon::serialize_json_canonical(record))),
        "the record digest is SHA-256 over the canonical record bytes");
    SDK_CHECK(addon::validate_resume_acknowledgment(record, decoded_resume_result->acknowledgment, &failure),
        "an acknowledgment naming the sent record validates");
    addon::Json_value other_record = object_with({{"provider_session_id", addon::Json_value("B")}});
    SDK_CHECK(!addon::validate_resume_acknowledgment(other_record, decoded_resume_result->acknowledgment, &failure) &&
        failure.reason_code == addon::reason::k_acknowledgment_mismatch,
        "a digest of another record is acknowledgment_mismatch");
    addon::Json_value resume_result_payload = addon::encode_resume_prepare_result(resume_result);
    resume_result_payload.erase("environment");
    SDK_CHECK(!addon::decode_resume_prepare_result(resume_result_payload, &failure).has_value() &&
        failure.reason_code == addon::reason::k_launch_patch_invalid,
        "a resume result without environment is launch_patch_invalid");
    resume_result_payload = addon::encode_resume_prepare_result(resume_result);
    resume_result_payload.find("acknowledgment")->set("record_digest", addon::Json_value("abc"));
    SDK_CHECK(!addon::decode_resume_prepare_result(resume_result_payload, &failure).has_value() &&
        failure.reason_code == addon::reason::k_frame_violation,
        "a record digest outside the hex grammar is frame_violation");
    addon::Resume_prepare_result reserved_patch = resume_result;
    reserved_patch.patch.set = {{"LOGONOMIC_SESSION_CAPABILITY", "forged"}};
    const std::optional<addon::Resume_prepare_result> decoded_reserved =
        addon::decode_resume_prepare_result(addon::encode_resume_prepare_result(reserved_patch), &failure);
    SDK_CHECK(decoded_reserved.has_value() &&
        !addon::validate_launch_patch(decoded_reserved->patch, {}, {}, &failure) &&
        failure.reason_code == addon::reason::k_reserved_environment_variable,
        "a reserved name in the resume environment is reserved_environment_variable under the patch rules");

    return true;
}

bool test_lifecycle_and_ingress_contracts()
{
    {
        addon::Async_question_classification_result result;
        result.base.base.classification = addon::Classification::EVENT;
        addon::Lifecycle_observation observation;
        observation.provider_event_name = "PostToolUse";
        observation.provider_session_id = "session";
        observation.provider_turn_id = "turn";
        observation.attention_transition = addon::Attention_transition::REQUESTED;
        observation.attention_evidence = addon::Evidence::INFERRED;
        observation.attention_request_id = "async-call";
        result.base.base.observations.push_back(observation);
        result.base.attention_kind = addon::Attention_kind{0, "interactive_question_candidate"};
        result.async_questions = addon::Async_question_update{0, addon::Async_question_update_kind::ACCEPTED,
            {R"(["request_user_input_async","call",0])", R"(["request_user_input_async","call",1])"}};
        const auto wire = addon::encode_async_question_classification_result(result);
        auto decoded = addon::decode_async_question_classification_result(wire, nullptr);
        SDK_CHECK(decoded && decoded->async_questions && decoded->async_questions->question_ids.size() == 2 &&
            decoded->base.base.observations.front().attention_transition == addon::Attention_transition::REQUESTED,
            "native async acceptance retains pending identities rather than resolving attention");
        SDK_CHECK(!addon::decode_attention_classification_result(wire, nullptr),
            "the existing attention-kind envelope remains strict and is never sent the optional companion");
        const auto old_wire = addon::encode_attention_classification_result(result.base);
        decoded = addon::decode_async_question_classification_result(old_wire, nullptr);
        SDK_CHECK(decoded && !decoded->async_questions && decoded->base.attention_kind,
            "absent async metadata preserves existing attention behavior");
        for (const auto kind : {"future", "accepted"}) {
            auto unsupported = wire;
            unsupported.find("async_questions")->set("kind", addon::Json_value(kind));
            unsupported.find("async_questions")->set("version", addon::Json_value(std::string_view(kind) == "future" ? 1 : 2));
            decoded = addon::decode_async_question_classification_result(unsupported, nullptr);
            SDK_CHECK(decoded && !decoded->async_questions && decoded->base.base.observations.size() == 1,
                "an unknown optional kind/version cannot discard valid base observations");
        }
        auto duplicate = wire;
        duplicate.find("async_questions")->find("question_ids")->push_back(
            addon::Json_value(result.async_questions->question_ids.front()));
        decoded = addon::decode_async_question_classification_result(duplicate, nullptr);
        SDK_CHECK(decoded && !decoded->async_questions, "duplicate correlation identities are not retained");
        auto oversized = wire;
        oversized.find("async_questions")->find("question_ids")->as_array().front() =
            addon::Json_value(std::string(addon::k_max_async_question_id_bytes + 1, 'x'));
        decoded = addon::decode_async_question_classification_result(oversized, nullptr);
        SDK_CHECK(decoded && !decoded->async_questions && decoded->base.attention_kind,
            "oversized optional correlation stays local to the async feature");
        result.async_questions->kind = addon::Async_question_update_kind::ANSWERED;
        result.async_questions->question_ids.resize(1);
        auto& answer = result.base.base.observations.front();
        answer.provider_event_name = "UserPromptSubmit";
        answer.attention_transition = addon::Attention_transition::NONE;
        answer.attention_evidence = addon::Evidence::NONE;
        answer.attention_request_id.clear();
        answer.phase_transition = addon::Phase_transition::WORKING;
        answer.phase_evidence = addon::Evidence::CONFIRMED;
        result.base.attention_kind.reset();
        decoded = addon::decode_async_question_classification_result(
            addon::encode_async_question_classification_result(result), nullptr);
        SDK_CHECK(decoded && decoded->async_questions && decoded->async_questions->question_ids.size() == 1 &&
            decoded->base.base.observations.front().phase_transition == addon::Phase_transition::WORKING,
            "a partial native reply carries exact identities alongside ordinary Working");
    }
    {
        addon::Attention_classification_result result;
        result.base.classification = addon::Classification::EVENT;
        addon::Lifecycle_observation observation;
        observation.provider_event_name = "Stop";
        observation.attention_transition = addon::Attention_transition::REQUESTED;
        observation.attention_evidence = addon::Evidence::INFERRED;
        observation.attention_request_id = "conversation-1";
        result.base.observations.push_back(observation);
        result.attention_kind = addon::Attention_kind{0, "conversational_hint"};
        auto wire = addon::encode_attention_classification_result(result);
        addon::Contract_failure failure;
        SDK_CHECK(!addon::decode_attention_classification_request(addon::Json_value(), &failure) &&
            !addon::decode_attention_classification_result(addon::Json_value(), &failure),
            "non-object companion envelopes are refused without throwing");
        auto decoded = addon::decode_attention_classification_result(wire, &failure);
        SDK_CHECK(decoded && decoded->attention_kind && decoded->base.observations.front().provider_event_name == "Stop",
            "companion kind preserves native provenance");
        SDK_CHECK(!addon::decode_classify_event_result(wire, &failure), "old peer remains strict");
        SDK_CHECK(addon::decode_classify_event_result(addon::encode_classify_event_result(result.base), &failure).has_value(),
            "ordinary helper result remains compatible");
        wire.find("attention_kind")->set("version", addon::Json_value(2));
        decoded = addon::decode_attention_classification_result(wire, &failure);
        SDK_CHECK(decoded && !decoded->attention_kind && decoded->base.observations.size() == 1,
            "unknown extension version preserves observations");
        wire.find("attention_kind")->set("version", addon::Json_value(1));
        wire.find("attention_kind")->set("kind", addon::Json_value("future"));
        decoded = addon::decode_attention_classification_result(wire, &failure);
        SDK_CHECK(decoded && !decoded->attention_kind, "unknown kind is unspecified");
        wire.find("attention_kind")->set("kind", addon::Json_value("interactive_question_candidate"));
        decoded = addon::decode_attention_classification_result(wire, &failure);
        SDK_CHECK(decoded && decoded->attention_kind &&
            decoded->attention_kind->kind == "interactive_question_candidate" &&
            decoded->base.observations.front().attention_request_id == "conversation-1" &&
            decoded->base.observations.front().provider_event_name == "Stop",
            "an inferred candidate preserves request identity and original native evidence");
        wire.find("attention_kind")->set("kind", addon::Json_value("permission_candidate"));
        decoded = addon::decode_attention_classification_result(wire, &failure);
        SDK_CHECK(decoded && decoded->attention_kind && decoded->attention_kind->kind == "permission_candidate" &&
            decoded->base.observations.front().attention_request_id == "conversation-1",
            "permission candidates retain their inferred identity without changing the base grammar");
        auto anonymous_candidate = result;
        anonymous_candidate.attention_kind = addon::Attention_kind{0, "interactive_question_candidate"};
        anonymous_candidate.base.observations.front().attention_request_id.clear();
        decoded = addon::decode_attention_classification_result(
            addon::encode_attention_classification_result(anonymous_candidate), &failure);
        SDK_CHECK(decoded && !decoded->attention_kind &&
            decoded->base.observations.front().attention_request_id.empty(),
            "anonymous inferred attention remains usable without candidate correlation metadata");
        anonymous_candidate.attention_kind = addon::Attention_kind{0, "permission_candidate"};
        decoded = addon::decode_attention_classification_result(
            addon::encode_attention_classification_result(anonymous_candidate), &failure);
        SDK_CHECK(decoded && !decoded->attention_kind &&
            decoded->base.observations.front().attention_request_id.empty(),
            "anonymous permission attention remains visible without a candidate identity");
        auto confirmed_candidate = result;
        confirmed_candidate.attention_kind = addon::Attention_kind{0, "interactive_question_candidate"};
        confirmed_candidate.base.observations.front().attention_evidence = addon::Evidence::CONFIRMED;
        decoded = addon::decode_attention_classification_result(
            addon::encode_attention_classification_result(confirmed_candidate), &failure, 2);
        SDK_CHECK(decoded && !decoded->attention_kind &&
            decoded->base.observations.front().attention_evidence == addon::Evidence::CONFIRMED,
            "invalid candidate metadata leaves confirmed base observations usable, not promoted");
        confirmed_candidate.attention_kind = addon::Attention_kind{0, "permission_candidate"};
        decoded = addon::decode_attention_classification_result(
            addon::encode_attention_classification_result(confirmed_candidate), &failure, 2);
        SDK_CHECK(decoded && !decoded->attention_kind &&
            decoded->base.observations.front().attention_evidence == addon::Evidence::CONFIRMED,
            "permission candidate metadata cannot hide confirmed base attention");
        confirmed_candidate.attention_kind = addon::Attention_kind{0, "interactive_question"};
        confirmed_candidate.base.observations.front().attention_evidence = addon::Evidence::INFERRED;
        decoded = addon::decode_attention_classification_result(
            addon::encode_attention_classification_result(confirmed_candidate), &failure, 2);
        SDK_CHECK(decoded && decoded->attention_kind && decoded->attention_kind->kind == "interactive_question",
            "ordinary inferred questions do not become hidden candidates");
        wire.find("attention_kind")->set("kind", addon::Json_value("conversational_hint"));
        wire.find("attention_kind")->set("observation_index", addon::Json_value(9));
        decoded = addon::decode_attention_classification_result(wire, &failure);
        SDK_CHECK(decoded && !decoded->attention_kind, "invalid optional index does not discard observations");
        wire.erase("attention_kind");
        SDK_CHECK(addon::decode_attention_classification_result(wire, &failure).has_value(), "absent extension preserves base result");
    }
    // Markdown fences retain their scope across paragraphs and long lines.
    SDK_CHECK(!addon::looks_like_reply_request("```text\n\n" + std::string(4096, 'x') + "?"),
        "a long fenced question must retain its opening context");
    SDK_CHECK(addon::looks_like_reply_request("Please provide " + std::string(4096, 'x') + "."),
        "a long prose request retains the final paragraph's prefix");
    SDK_CHECK(addon::looks_like_reply_request("Done.\n\nWhich directory?  \n"), "question ending is a hint");
    SDK_CHECK(addon::looks_like_reply_request("Please provide the filename."), "reply request without question mark");
    SDK_CHECK(addon::looks_like_reply_request("どちらですか？"), "full-width question ending");
    SDK_CHECK(!addon::looks_like_reply_request(""), "empty output is not a hint");
    SDK_CHECK(!addon::looks_like_reply_request("Which directory? The configured one."), "answered text does not end in a request");
    SDK_CHECK(!addon::looks_like_reply_request("> Which directory?"), "quoted ending is not a hint");
    SDK_CHECK(!addon::looks_like_reply_request("```\nWhich directory?\n```"), "code ending is not a hint");
    SDK_CHECK(addon::looks_like_reply_request("The build is configured.\n\n**Which target should I build?**"),
        "a bold closing question is a hint");
    SDK_CHECK(addon::looks_like_reply_request("_Which directory?_"), "an emphasised closing question is a hint");
    SDK_CHECK(addon::looks_like_reply_request("**Please provide the filename.**"),
        "an emphasised reply request is a hint");
    SDK_CHECK(!addon::looks_like_reply_request("Which directory? **The configured one.**"),
        "emphasis does not turn an answered question into a hint");
    {
        // The oracle is the existing punctuation/prefix grammar applied to the
        // final prose paragraph. Markdown context is established before bounds.
        const std::string long_prose = std::string(4096, 'x');
        std::string unicode;
        for (int i = 0; i < 1000; ++i) unicode += "界";
        const std::pair<std::string, bool> cases[] = {
            {long_prose + ".\n\nWhich target?", true},
            {long_prose + " Which target?", true},
            {"Please tell me " + long_prose + ".", true},
            {"**Please provide " + unicode + ".**", true},
            {unicode + "选择哪个目标？", true},
            {"Which target?" + std::string(4096, ' '), true},
            {"Earlier?\r\n \t\r\nThe changes are complete.", false},
            {"> " + long_prose + "\nWhich target?", false},
            {"Prose\n> Quoted text\nWhich target?", false},
            {"> Quoted text\n\nWhich target?", true},
            {"\"" + long_prose + " Which target?", false},
            {"    " + long_prose + " Which target?", false},
            {"\tWhich target?", false},
            {"```text\n" + long_prose + "\n\nWhich target?", false},
            {"~~~text\n" + long_prose + "\n\nWhich target?\n~~~", false},
            {"````text\n" + long_prose + "\n```\nWhich target?", false},
            {"  ```text\r\n" + long_prose + "\r\n  ````\r\nWhich target?", true},
            {"~~~text\n" + long_prose + "\n~~~ trailing text\nWhich target?", false},
            {"```text\n" + long_prose + "\n```\n\n**Which target?**\n\n", true},
            {"```text\n" + long_prose + "\n```\n\nComplete.", false},
            {"Which target?\n\n    sample code?", false},
            {"Which target?\n\n> sample quote?", false},
            {"\xffWhich target?", false},
            {std::string("private\0Which target?", 21), false},
        };
        for (const auto& [text, expected] : cases) {
            const auto evidence = addon::assistant_reply_evidence(text);
            SDK_CHECK(addon::looks_like_reply_request_evidence(evidence) == expected &&
                addon::looks_like_reply_request(text) == expected,
                "full-context evidence preserves the final paragraph's prose/code/quote meaning");
            if (!evidence.is_null()) {
                for (const auto* edge : {"prefix", "suffix"}) {
                    const auto& value = evidence.find(edge)->as_string();
                    SDK_CHECK(value.size() <= addon::k_assistant_reply_edge_bytes && addon::is_valid_utf8(value),
                        "paragraph edges end at UTF-8 scalar boundaries within their byte bound");
                }
            }
        }
        const auto evidence = addon::assistant_reply_evidence("Private earlier paragraph.\n\nWhich target?");
        SDK_CHECK(evidence.find("scope")->as_string() == "final_paragraph_edges" &&
            evidence.find("context")->as_string() == "prose" &&
            evidence.find("prefix")->as_string() == "Which target?" &&
            evidence.find("suffix")->as_string() == "Which target?" &&
            addon::serialize_json(evidence).find("Private earlier") == std::string::npos,
            "bounded evidence describes its scope without retaining earlier assistant content");
        for (const auto* key : {"prefix", "suffix"}) {
            for (const auto& value : {addon::Json_value(), addon::Json_value(""),
                addon::Json_value(std::string(129, '?')), addon::Json_value("\xff"),
                addon::Json_value(std::string("?\0", 2))})
            {
                auto invalid = evidence;
                invalid.set(key, value);
                SDK_CHECK(!addon::looks_like_reply_request_evidence(invalid), "invalid evidence edge is rejected");
            }
        }
        for (const auto* key : {"scope", "context"}) {
            auto invalid = evidence;
            invalid.set(key, addon::Json_value("unknown"));
            SDK_CHECK(!addon::looks_like_reply_request_evidence(invalid), "unknown evidence semantics are rejected");
        }
        auto extra = evidence;
        extra.set("complete_message", addon::Json_value("Which target?"));
        SDK_CHECK(!addon::looks_like_reply_request_evidence(extra), "edges cannot masquerade as a complete message");
    }
    addon::Contract_failure failure;
    using Observation = addon::Lifecycle_observation;
    using Result      = addon::Classify_event_result;
    {
        // The gate the host applies to every observation before its state machine.
        using Features = addon::Lifecycle_features;
        const auto admits = [](const Features& features, const Observation& observation) {
            return addon::lifecycle_profile_admits(features, observation);
        };
        Features none;
        Features all;
        all.root_turn_start        = addon::Feature_support::SUPPORTED;
        all.root_turn_completion   = addon::Feature_support::SUPPORTED;
        all.confirmed_phase_status = addon::Feature_support::SUPPORTED;
        all.attention              = addon::Feature_support::SUPPORTED;
        all.paired_attention       = addon::Feature_support::SUPPORTED;
        all.subagents              = addon::Feature_support::SUPPORTED;
        const auto without = [&all](addon::Feature_support Features::* feature) {
            auto features = all;
            features.*feature = addon::Feature_support::UNSUPPORTED;
            return features;
        };
        Observation session;
        session.provider_event_name = "SessionStart";
        session.session_transition  = addon::Session_transition::STARTED;
        session.provider_session_id = "session-1";
        SDK_CHECK(admits(none, session), "a session transition needs no feature");

        Observation stop = session;
        stop.provider_event_name = "Stop";
        stop.session_transition  = addon::Session_transition::NONE;
        stop.provider_turn_id    = "turn-1";
        stop.phase_transition    = addon::Phase_transition::AWAITING_INPUT;
        stop.phase_evidence      = addon::Evidence::CONFIRMED;
        SDK_CHECK(admits(all, stop), "a declared confirmed completion is admitted");
        SDK_CHECK(!admits(without(&Features::confirmed_phase_status), stop),
            "confirmed phase evidence needs confirmed_phase_status");
        SDK_CHECK(!admits(without(&Features::root_turn_completion), stop),
            "awaiting_input needs root_turn_completion");

        Observation interrupted = stop;
        interrupted.phase_transition   = addon::Phase_transition::NONE;
        interrupted.phase_evidence     = addon::Evidence::NONE;
        interrupted.outcome_transition = addon::Outcome_transition::INTERRUPTED;
        interrupted.outcome_evidence   = addon::Evidence::CONFIRMED;
        SDK_CHECK(!admits(without(&Features::confirmed_phase_status), interrupted),
            "confirmed outcome evidence needs confirmed_phase_status");

        Observation submit = stop;
        submit.phase_transition = addon::Phase_transition::WORKING;
        submit.phase_evidence   = addon::Evidence::INFERRED;
        SDK_CHECK(admits(without(&Features::confirmed_phase_status), submit),
            "inferred phase evidence needs no confirmed_phase_status");
        SDK_CHECK(!admits(without(&Features::root_turn_start), submit),
            "working needs root_turn_start");

        Observation hint = stop;
        hint.phase_evidence       = addon::Evidence::INFERRED;
        hint.attention_transition = addon::Attention_transition::REQUESTED;
        hint.attention_evidence   = addon::Evidence::INFERRED;
        hint.attention_request_id = "conversation-1";
        SDK_CHECK(admits(all, hint), "a declared identified attention is admitted");
        SDK_CHECK(!admits(without(&Features::paired_attention), hint),
            "an identified attention needs paired_attention, and its phase is refused with it");
        SDK_CHECK(!admits(without(&Features::attention), hint),
            "any attention needs attention");

        Observation permission = hint;
        permission.phase_transition = addon::Phase_transition::NONE;
        permission.phase_evidence   = addon::Evidence::NONE;
        permission.attention_request_id.clear();
        SDK_CHECK(admits(without(&Features::paired_attention), permission),
            "an anonymous inferred request needs no paired_attention");
        permission.attention_transition = addon::Attention_transition::RESOLVED;
        SDK_CHECK(!admits(all, permission), "an anonymous resolution is never admitted");

        Observation child = session;
        child.session_transition  = addon::Session_transition::NONE;
        child.subagent_transition = addon::Subagent_transition::STARTED;
        child.agent_id            = "agent-1";
        SDK_CHECK(admits(all, child), "a declared subagent transition is admitted");
        SDK_CHECK(!admits(without(&Features::subagents), child),
            "a subagent transition needs subagents");
    }
    const auto declaration_rejection = [&failure](const addon::Json_value& payload) {
        return rejection_of(addon::decode_lifecycle_declaration(payload, &failure), failure);
    };
    const auto observation_rejection = [&failure](const addon::Json_value& payload) {
        return rejection_of(addon::decode_lifecycle_observation(payload, &failure, addon::Lifecycle_observation_policy::HELPER_INGRESS), failure);
    };
    const auto observation_value_rejection = [&observation_rejection](const Observation& observation) {
        return observation_rejection(addon::encode_lifecycle_observation(observation));
    };
    const auto observe_payload_rejection = [&failure](const addon::Json_value& payload) {
        return rejection_of(addon::decode_observe_request(payload, &failure), failure);
    };
    const auto observe_rejection = [&observe_payload_rejection](const addon::Observe_request& request) {
        return observe_payload_rejection(addon::encode_observe_request(request));
    };
    const auto poll_request_rejection = [&failure](const addon::Poll_observations_request& request) {
        const addon::Json_value payload = addon::encode_poll_observations_request(request);
        return rejection_of(addon::decode_poll_observations_request(payload, &failure), failure);
    };
    const auto classify_request_rejection = [&failure](const addon::Json_value& payload) {
        return rejection_of(addon::decode_classify_event_request(payload, &failure), failure);
    };
    const auto classify_result_rejection = [&failure](const addon::Json_value& payload) {
        return rejection_of(addon::decode_classify_event_result(payload, &failure), failure);
    };
    const auto classify_result_value_rejection = [&classify_result_rejection](const Result& result) {
        return classify_result_rejection(addon::encode_classify_event_result(result));
    };

    // lifecycle-events@2 declaration.
    addon::Lifecycle_declaration declaration;
    declaration.profile_id                    = "unknown-fixture-events";
    declaration.features.root_turn_start      = addon::Feature_support::SUPPORTED;
    declaration.features.root_turn_completion = addon::Feature_support::SUPPORTED;
    declaration.features.confirmed_phase_status = addon::Feature_support::SUPPORTED;
    declaration.features.attention            = addon::Feature_support::SUPPORTED;
    declaration.observation                   = addon::Observation_mode::CONTINUOUS;
    const addon::Json_value declaration_payload = addon::encode_lifecycle_declaration(declaration);
    const std::optional<addon::Lifecycle_declaration> decoded_declaration =
        addon::decode_lifecycle_declaration(declaration_payload, &failure);
    SDK_CHECK(decoded_declaration.has_value() && decoded_declaration->profile_id == declaration.profile_id &&
        decoded_declaration->features.attention == addon::Feature_support::SUPPORTED &&
        decoded_declaration->features.subagents == addon::Feature_support::UNSUPPORTED &&
        decoded_declaration->turn_origin_correlation == addon::Feature_support::UNSUPPORTED &&
        decoded_declaration->observation == addon::Observation_mode::CONTINUOUS,
        "a lifecycle declaration round-trips: " + reason_of(failure));
    SDK_CHECK(declaration_payload.find("features")->as_object().size() == 8,
        "all eight feature fields are on the wire");
    for (const auto& [member, value] : std::initializer_list<std::pair<const char*, addon::Json_value>>{
             {"profile_id", addon::Json_value("Unknown_Fixture")},
             {"profile_id", addon::Json_value(std::string(limits::k_max_lifecycle_profile_id_bytes + 1, 'p'))},
             {"observation", addon::Json_value("polling")},
             {"turn_origin_correlation", addon::Json_value("exact")},
             {"extra", addon::Json_value(true)},
         })
    {
        addon::Json_value payload = declaration_payload;
        payload.set(member, value);
        SDK_CHECK(declaration_rejection(payload) == addon::reason::k_invalid_sidecar,
            std::string("a defective lifecycle declaration member is invalid_sidecar: ") + member);
    }
    {
        addon::Json_value payload = declaration_payload;
        payload.find("features")->erase("subagents");
        SDK_CHECK(declaration_rejection(payload) == addon::reason::k_invalid_sidecar,
            "a missing feature field is invalid_sidecar");
        payload = declaration_payload;
        payload.find("features")->set("attention", addon::Json_value("maybe"));
        SDK_CHECK(!addon::decode_lifecycle_declaration(payload, &failure).has_value(),
            "a feature value outside supported/unsupported is invalid_sidecar");
        payload = declaration_payload;
        payload.find("features")->set("telemetry", addon::Json_value("supported"));
        SDK_CHECK(!addon::decode_lifecycle_declaration(payload, &failure).has_value(),
            "an unknown feature field is invalid_sidecar");
    }

    // coordination-delivery@1.
    addon::Coordination_delivery_declaration delivery;
    delivery.startup_instruction_delivery = addon::Feature_support::SUPPORTED;
    const std::optional<addon::Coordination_delivery_declaration> decoded_delivery =
        addon::decode_coordination_delivery_declaration(
            addon::encode_coordination_delivery_declaration(delivery), &failure);
    SDK_CHECK(decoded_delivery.has_value() &&
        decoded_delivery->startup_instruction_delivery == addon::Feature_support::SUPPORTED &&
        decoded_delivery->turn_context_delivery == addon::Feature_support::UNSUPPORTED,
        "a coordination-delivery declaration round-trips");
    SDK_CHECK(!addon::decode_coordination_delivery_declaration(
        object_with({{"startup_instruction_delivery", addon::Json_value("supported")}}), &failure).has_value() &&
        failure.reason_code == addon::reason::k_invalid_sidecar, "both delivery members are required");

    // Observations.
    const addon::Lifecycle_observation started = fixture_started_observation("A");
    const addon::Json_value started_payload    = addon::encode_lifecycle_observation(started);
    SDK_CHECK(started_payload.as_object().size() == 19,
        "every member of a started observation with a resume record is on the wire");
    const std::optional<addon::Lifecycle_observation> decoded_started =
        addon::decode_lifecycle_observation(started_payload, &failure, addon::Lifecycle_observation_policy::HELPER_INGRESS);
    SDK_CHECK(decoded_started.has_value() &&
        decoded_started->session_transition == addon::Session_transition::STARTED &&
        decoded_started->provider_session_id == "A" && decoded_started->resume_record.has_value() &&
        *decoded_started->resume_record == *started.resume_record && decoded_started->provider_sequence == "1",
        "a started observation with a resume record round-trips: " + reason_of(failure));
    SDK_CHECK(addon::encode_lifecycle_observation(*decoded_started) == started_payload,
        "the observation encoding is stable");
    {
        using Problem = addon::Provider_event_problem;
        using Type    = addon::Json_type;
        struct Field_defect
        {
            const char* field;
            addon::Json_value value;
            Problem problem;
            const char* reason;
            const char* rule;
        };
        const std::vector<Field_defect> defects = {
            {"provider_event_name", addon::Json_value(), Problem::WRONG_TYPE,
                addon::reason::k_frame_violation, "nonempty_string"},
            {"provider_event_name", addon::Json_value(""), Problem::EMPTY,
                addon::reason::k_frame_violation, "nonempty_string"},
            {"phase_transition", addon::Json_value(true), Problem::WRONG_TYPE,
                addon::reason::k_frame_violation, "known_enum_value"},
            {"phase_evidence", addon::Json_value("private-metadata-value"), Problem::INVALID_VALUE,
                addon::reason::k_frame_violation, "known_enum_value"},
            {"provider_sequence", addon::Json_value("001"), Problem::INVALID_VALUE,
                addon::reason::k_observation_invalid, "empty_or_canonical_uint64"},
            {"provider_session_id", addon::Json_value(std::string(limits::k_max_lifecycle_identity_bytes + 1, 'x')),
                Problem::TOO_LARGE, addon::reason::k_payload_too_large, "bounded_nul_free_string"},
            {"session_source", addon::Json_value(std::string("private\0metadata", 16)), Problem::CONTROL_CHARACTER,
                addon::reason::k_payload_too_large, "bounded_nul_free_string"},
            {"resume_record", addon::Json_value("private-metadata-value"), Problem::WRONG_TYPE,
                addon::reason::k_frame_violation, "object"},
        };
        for (const auto& defect : defects) {
            auto payload = started_payload;
            payload.set(defect.field, defect.value);
            SDK_CHECK(observation_rejection(payload) == defect.reason,
                "observation diagnostics preserve the existing rejection reason");
            const auto diagnostic = addon::decode_provider_event_diagnostic(failure.detail);
            SDK_CHECK(diagnostic && diagnostic->event_name == "lifecycle_observation" &&
                diagnostic->field == "observation." + std::string(defect.field) &&
                diagnostic->problem == defect.problem && diagnostic->actual_type == defect.value.type() &&
                diagnostic->rule == defect.rule,
                "an observation rejection names its exact source field, type and validation rule");
            SDK_CHECK(failure.detail.find("private-metadata-value") == std::string::npos &&
                failure.detail.find("private\\u0000metadata") == std::string::npos,
                "observation diagnostic messages never retain malformed private values");
            if (defect.problem == Problem::TOO_LARGE) {
                SDK_CHECK(diagnostic->actual_size == limits::k_max_lifecycle_identity_bytes + 1 &&
                    diagnostic->limit == limits::k_max_lifecycle_identity_bytes,
                    "an oversized observation identity reports byte size and the enforced limit");
            }
        }
        auto missing = started_payload;
        missing.erase("provider_event_name");
        SDK_CHECK(observation_rejection(missing) == addon::reason::k_frame_violation,
            "an omitted provider event name retains its rejection");
        const auto missing_diagnostic = addon::decode_provider_event_diagnostic(failure.detail);
        SDK_CHECK(missing_diagnostic && missing_diagnostic->field == "observation.provider_event_name" &&
            missing_diagnostic->problem == Problem::MISSING && !missing_diagnostic->actual_type,
            "a missing observation field is distinguished from explicit null");

        auto continuous = started_payload;
        continuous.set("agent_label", addon::Json_value(std::string(257, 'a')));
        SDK_CHECK(!addon::decode_lifecycle_observation(
                continuous, &failure, addon::Lifecycle_observation_policy::CONTINUOUS) &&
            failure.reason_code == addon::reason::k_payload_too_large,
            "continuous labels still enforce the existing UTF-16 unit bound");
        const auto label_diagnostic = addon::decode_provider_event_diagnostic(failure.detail);
        SDK_CHECK(label_diagnostic && label_diagnostic->field == "observation.agent_label" &&
            label_diagnostic->problem == Problem::TOO_LARGE && label_diagnostic->actual_type == Type::STRING &&
            label_diagnostic->actual_size == 257 && label_diagnostic->limit == 256 &&
            label_diagnostic->rule == "utf16_units",
            "continuous label diagnostics state the failed measurement and limit");
    }
    {
        addon::Json_value payload = started_payload;
        payload.erase("agent_label");
        SDK_CHECK(observation_rejection(payload) == addon::reason::k_frame_violation,
            "every string member of an observation is required");
        payload = started_payload;
        payload.set("vendor_extra", addon::Json_value(1));
        SDK_CHECK(observation_rejection(payload) == addon::reason::k_unexpected_member,
            "an unexpected observation member is unexpected_member");
        payload = started_payload;
        payload.set("provider_event_name", addon::Json_value(""));
        SDK_CHECK(observation_rejection(payload) == addon::reason::k_frame_violation,
            "an empty provider_event_name is frame_violation");
        payload = started_payload;
        payload.set("provider_session_id",
            addon::Json_value(std::string(limits::k_max_lifecycle_identity_bytes + 1, 'i')));
        SDK_CHECK(observation_rejection(payload) == addon::reason::k_payload_too_large,
            "an identity above k_max_lifecycle_identity_bytes is payload_too_large");
        payload = started_payload;
        payload.set("session_source", addon::Json_value(std::string(limits::k_max_lifecycle_label_bytes + 1, 'l')));
        SDK_CHECK(observation_rejection(payload) == addon::reason::k_payload_too_large,
            "a label above k_max_lifecycle_label_bytes is payload_too_large");
        payload = started_payload;
        payload.set("session_source", addon::Json_value(std::string(limits::k_max_lifecycle_label_bytes, 'l')));
        payload.set("agent_id", addon::Json_value(std::string(limits::k_max_lifecycle_identity_bytes, 'i')));
        SDK_CHECK(addon::decode_lifecycle_observation(payload, &failure, addon::Lifecycle_observation_policy::HELPER_INGRESS).has_value(),
            "identities and labels at their bounds decode");
        payload = started_payload;
        payload.set("resume_record", addon::Json_value("A"));
        SDK_CHECK(observation_rejection(payload) == addon::reason::k_frame_violation,
            "a resume record that is not an object is frame_violation");
        payload = started_payload;
        payload.set("resume_record",
            object_with({{"blob", addon::Json_value(std::string(limits::k_max_resume_record_bytes, 'b'))}}));
        SDK_CHECK(observation_rejection(payload) == addon::reason::k_payload_too_large,
            "a resume record above k_max_resume_record_bytes is payload_too_large");
    }
    struct Invalid_case
    {
        const char*                       name;
        std::function<void(Observation&)> mutate;
    };
    const std::vector<Invalid_case> invalid_cases = {
        {"phase without evidence",        [](Observation& o) {
            o.phase_transition = addon::Phase_transition::WORKING;
        }},
        {"evidence without phase",        [](Observation& o) { o.phase_evidence = addon::Evidence::INFERRED; }},
        {"outcome without turn",          [](Observation& o) {
            o.outcome_transition = addon::Outcome_transition::SUCCEEDED;
            o.outcome_evidence   = addon::Evidence::CONFIRMED;
        }},
        {"started without session",       [](Observation& o) { o.provider_session_id.clear(); }},
        {"subagent without agent",        [](Observation& o) {
            o.subagent_transition = addon::Subagent_transition::STARTED;
        }},
        {"sequence with a leading zero",  [](Observation& o) { o.provider_sequence = "007"; }},
        {"sequence beyond 64 bits",       [](Observation& o) { o.provider_sequence = "18446744073709551616"; }},
        {"sequence with letters",         [](Observation& o) { o.provider_sequence = "1a"; }},
        {"anonymous confirmed attention", [](Observation& o) {
            o.attention_transition = addon::Attention_transition::REQUESTED;
            o.attention_evidence   = addon::Evidence::CONFIRMED;
        }},
        {"anonymous resolved attention",  [](Observation& o) {
            o.attention_transition = addon::Attention_transition::RESOLVED;
            o.attention_evidence   = addon::Evidence::INFERRED;
        }},
        {"resume record on a non-start",  [](Observation& o) {
            o.session_transition = addon::Session_transition::ENDED;
        }},
    };
    for (const Invalid_case& invalid : invalid_cases) {
        addon::Lifecycle_observation observation = started;
        invalid.mutate(observation);
        SDK_CHECK(observation_value_rejection(observation) == addon::reason::k_observation_invalid,
            std::string("observation_invalid: ") + invalid.name);
    }
    {
        addon::Lifecycle_observation observation = started;
        observation.provider_sequence = "18446744073709551615";
        SDK_CHECK(observation_value_rejection(observation).empty(),
            "the largest 64-bit sequence decodes");
        observation = started;
        observation.attention_transition = addon::Attention_transition::REQUESTED;
        observation.attention_evidence   = addon::Evidence::INFERRED;
        SDK_CHECK(observation_value_rejection(observation).empty(),
            "an anonymous requested+inferred attention decodes");
        observation.attention_evidence   = addon::Evidence::CONFIRMED;
        observation.attention_request_id = "attn-1";
        SDK_CHECK(observation_value_rejection(observation).empty(),
            "a confirmed attention naming its request decodes");
    }
    std::vector<addon::Lifecycle_observation> many;
    for (std::size_t index = 0; index <= limits::k_max_observations_per_result; ++index) {
        many.push_back(fixture_phase_observation("evt-" + std::to_string(index)));
    }
    SDK_CHECK(!addon::decode_observations(
            addon::encode_observations(many), limits::k_max_observations_per_result, &failure, addon::Lifecycle_observation_policy::HELPER_INGRESS).has_value() &&
        failure.reason_code == addon::reason::k_payload_too_large,
        "more than k_max_observations_per_result observations is payload_too_large");
    many.pop_back();
    const std::optional<std::vector<addon::Lifecycle_observation>> decoded_many =
        addon::decode_observations(addon::encode_observations(many), limits::k_max_observations_per_result, &failure, addon::Lifecycle_observation_policy::HELPER_INGRESS);
    SDK_CHECK(decoded_many.has_value() && decoded_many->size() == limits::k_max_observations_per_result,
        "exactly the bound decodes");

    // Continuous mode.
    addon::Observe_request observe;
    observe.session.working_directory   = "/repo";
    observe.session.launcher_argv       = {"unknown-fixture-cli"};
    observe.session.provider_session_id = "";
    observe.resume_cursor               = "";
    observe.window_ms                   = limits::k_observe_window_ms;
    const std::optional<addon::Observe_request> decoded_observe =
        addon::decode_observe_request(addon::encode_observe_request(observe), &failure);
    SDK_CHECK(decoded_observe.has_value() && decoded_observe->window_ms == limits::k_observe_window_ms &&
        decoded_observe->session.launcher_argv == observe.session.launcher_argv &&
        decoded_observe->resume_cursor.empty(),
        "an observe request round-trips: " + reason_of(failure));
    {
        const auto valid = addon::encode_observe_request(observe);
        auto malformed = valid;
        malformed.find("session")->set("launcher_argv",
            object_with({{"private-field-name", addon::Json_value("private-metadata-value")}}));
        SDK_CHECK(!addon::decode_observe_request(malformed, &failure) &&
            failure.reason_code == addon::reason::k_frame_violation,
            "a mistyped observe argument array retains its rejection");
        const auto argv_diagnostic = addon::decode_provider_event_diagnostic(failure.detail);
        SDK_CHECK(argv_diagnostic && argv_diagnostic->event_name == "observe" &&
            argv_diagnostic->field == "session.launcher_argv" &&
            argv_diagnostic->problem == addon::Provider_event_problem::WRONG_TYPE &&
            argv_diagnostic->actual_type == addon::Json_type::OBJECT &&
            failure.detail.find("private-field-name") == std::string::npos &&
            failure.detail.find("private-metadata-value") == std::string::npos,
            "request diagnostics describe the expected argument shape without private content");
        malformed = valid;
        malformed.find("session")->find("launcher_argv")->push_back(addon::Json_value(true));
        SDK_CHECK(!addon::decode_observe_request(malformed, &failure) &&
            failure.reason_code == addon::reason::k_frame_violation,
            "a mistyped argument entry retains its rejection");
        const auto entry_diagnostic = addon::decode_provider_event_diagnostic(failure.detail);
        SDK_CHECK(entry_diagnostic && entry_diagnostic->field == "session.launcher_argv[1]" &&
            entry_diagnostic->problem == addon::Provider_event_problem::WRONG_TYPE &&
            entry_diagnostic->actual_type == addon::Json_type::BOOLEAN,
            "an invalid argument reports its array index");
        malformed = valid;
        malformed.set("correlation", object_with({
            {"version", addon::Json_value(1)},
            {"transition_nonce", addon::Json_value("private-metadata-value")},
        }));
        SDK_CHECK(!addon::decode_observe_request(malformed, &failure) &&
            failure.reason_code == addon::reason::k_frame_violation,
            "invalid correlation identities remain rejected");
        const auto correlation_diagnostic = addon::decode_provider_event_diagnostic(failure.detail);
        SDK_CHECK(correlation_diagnostic && correlation_diagnostic->field == "correlation.transition_nonce" &&
            correlation_diagnostic->problem == addon::Provider_event_problem::INVALID_VALUE &&
            correlation_diagnostic->rule == "transition_nonce_hex" &&
            failure.detail.find("private-metadata-value") == std::string::npos,
            "a correlation rejection names its exact rule without retaining the identity");
    }
    observe.resume_cursor = std::string(limits::k_max_observe_cursor_bytes + 1, 'c');
    SDK_CHECK(observe_rejection(observe) == addon::reason::k_payload_too_large,
        "a cursor above k_max_observe_cursor_bytes is payload_too_large");
    observe.resume_cursor = std::string(limits::k_max_observe_cursor_bytes, 'c');
    observe.window_ms     = 0;
    SDK_CHECK(observe_rejection(observe) == addon::reason::k_frame_violation,
        "a non-positive window is frame_violation");
    observe.window_ms = 1;
    addon::Json_value observe_payload = addon::encode_observe_request(observe);
    observe_payload.set("poll", addon::Json_value(true));
    SDK_CHECK(observe_payload_rejection(observe_payload) == addon::reason::k_unexpected_member,
        "an unexpected observe member is unexpected_member");
    const std::optional<addon::Observe_result> decoded_observe_result =
        addon::decode_observe_result(addon::encode_observe_result(addon::Observe_result{"cursor-9"}), &failure);
    SDK_CHECK(decoded_observe_result.has_value() && decoded_observe_result->resume_cursor == "cursor-9",
        "an observe result round-trips");
    SDK_CHECK(addon::observe_progress_note(3) == R"({"available":3})" &&
        addon::observe_progress_note(3).size() <= limits::k_max_progress_note_bytes,
        "the progress note is {\"available\": n}");

    addon::Poll_observations_request poll{"cursor-9", limits::k_max_observations_per_result, std::nullopt};
    const std::optional<addon::Poll_observations_request> decoded_poll =
        addon::decode_poll_observations_request(addon::encode_poll_observations_request(poll), &failure);
    SDK_CHECK(decoded_poll.has_value() && decoded_poll->limit == limits::k_max_observations_per_result &&
        decoded_poll->resume_cursor == "cursor-9",
        "a poll request round-trips");
    poll.limit = limits::k_max_observations_per_result + 1;
    SDK_CHECK(poll_request_rejection(poll) == addon::reason::k_frame_violation,
        "a limit above k_max_observations_per_result is frame_violation");
    poll.limit = 0;
    SDK_CHECK(!poll_request_rejection(poll).empty(),
        "a limit of zero is rejected");
    const auto limit_diagnostic = addon::decode_provider_event_diagnostic(failure.detail);
    SDK_CHECK(limit_diagnostic && limit_diagnostic->event_name == "poll_observations" &&
        limit_diagnostic->field == "limit" &&
        limit_diagnostic->problem == addon::Provider_event_problem::INVALID_VALUE &&
        limit_diagnostic->actual_type == addon::Json_type::INTEGER &&
        limit_diagnostic->limit == limits::k_max_observations_per_result &&
        limit_diagnostic->rule == "positive_bounded_limit",
        "an invalid poll request bound preserves its field and validation condition");
    addon::Poll_observations_result poll_result;
    poll_result.observations  = {started, fixture_phase_observation("evt-2")};
    poll_result.resume_cursor = "cursor-11";
    poll_result.more          = true;
    const std::optional<addon::Poll_observations_result> decoded_poll_result =
        addon::decode_poll_observations_result(addon::encode_poll_observations_result(poll_result), 2, &failure);
    SDK_CHECK(decoded_poll_result.has_value() && decoded_poll_result->observations.size() == 2 &&
        decoded_poll_result->more &&
        decoded_poll_result->resume_cursor == "cursor-11", "a poll result round-trips: " + reason_of(failure));
    SDK_CHECK(!addon::decode_poll_observations_result(
            addon::encode_poll_observations_result(poll_result), 1, &failure).has_value() &&
        failure.reason_code == addon::reason::k_payload_too_large,
        "more observations than the request's limit is payload_too_large");

    // Wire admission is not owner Session admission. Preserve foreign identity
    // (and an empty optional turn) in either order.
    {
        addon::Lifecycle_observation candidate;
        candidate.provider_event_name = "UserPromptSubmit";
        candidate.provider_session_id = "native-claude";
        candidate.provider_turn_id = "candidate-turn";
        candidate.phase_transition = addon::Phase_transition::WORKING;
        candidate.phase_evidence = addon::Evidence::CONFIRMED;
        addon::Lifecycle_observation foreign;
        foreign.provider_event_name = "PreToolUse";
        foreign.provider_session_id = "foreign-session";
        foreign.phase_transition = addon::Phase_transition::WORKING;
        foreign.phase_evidence = addon::Evidence::CONFIRMED;
        for (const bool foreign_first : {false, true}) {
            addon::Poll_observations_result mixed;
            mixed.observations = foreign_first
                ? std::vector<addon::Lifecycle_observation>{foreign, candidate}
                : std::vector<addon::Lifecycle_observation>{candidate, foreign};
            mixed.resume_cursor = "safety-batch";
            const auto payload = addon::encode_poll_observations_result(mixed);
            addon::Json_parse_error parse_error;
            const auto parsed = addon::parse_json(addon::serialize_json(payload), {}, &parse_error);
            SDK_CHECK(parsed.has_value(), "the mixed lifecycle page must survive JSON round trip");
            addon::Contract_failure batch_failure;
            const auto decoded = addon::decode_poll_observations_result(*parsed, 2, &batch_failure);
            SDK_CHECK(decoded && decoded->observations.size() == 2 &&
                decoded->resume_cursor == mixed.resume_cursor && !decoded->more,
                "well-formed foreign batch reaches owner validation");
            for (std::size_t index = 0; index < mixed.observations.size(); ++index) {
                SDK_CHECK(addon::encode_lifecycle_observation(decoded->observations[index]) ==
                    addon::encode_lifecycle_observation(mixed.observations[index]),
                    "wire admission preserves observation order, identity and lifecycle facts");
            }
        }
        addon::Poll_observations_result mixed{{candidate, foreign}, "safety-batch", false};
        auto missing_name = addon::encode_poll_observations_result(mixed);
        missing_name.find("observations")->as_array().back().erase("provider_event_name");
        // Empty turn identity is allowed, but the JSON member is required.
        auto missing_turn = addon::encode_poll_observations_result(mixed);
        missing_turn.find("observations")->as_array().back().erase("provider_turn_id");
        mixed.observations.back().phase_evidence = addon::Evidence::NONE;
        const auto invalid_evidence = addon::encode_poll_observations_result(mixed);
        addon::Contract_failure missing_failure;
        SDK_CHECK(!addon::decode_poll_observations_result(missing_name, 2, &missing_failure) &&
            missing_failure.reason_code == addon::reason::k_frame_violation,
            "a missing required name rejects the whole page before owner reduction");
        const auto missing_diagnostic = addon::decode_provider_event_diagnostic(missing_failure.detail);
        SDK_CHECK(missing_diagnostic && missing_diagnostic->event_name == "poll_observations" &&
            missing_diagnostic->field == "observations[1].provider_event_name" &&
            missing_diagnostic->problem == addon::Provider_event_problem::MISSING,
            "poll rejection diagnostics preserve the exact failing observation index");
        addon::Contract_failure turn_failure;
        SDK_CHECK(!addon::decode_poll_observations_result(missing_turn, 2, &turn_failure) &&
            turn_failure.reason_code == addon::reason::k_frame_violation,
            "an optional turn identity is an empty string, not an omitted wire member");
        addon::Contract_failure evidence_failure;
        SDK_CHECK(!addon::decode_poll_observations_result(invalid_evidence, 2, &evidence_failure) &&
            evidence_failure.reason_code == addon::reason::k_observation_invalid,
            "inconsistent transition/evidence rejects the whole page before owner reduction");
        const auto evidence_diagnostic = addon::decode_provider_event_diagnostic(evidence_failure.detail);
        SDK_CHECK(evidence_diagnostic && evidence_diagnostic->field == "observations[1].phase_evidence" &&
            evidence_diagnostic->problem == addon::Provider_event_problem::MISMATCH &&
            evidence_diagnostic->rule == "phase_transition_evidence_pair",
            "semantic observation failures identify the mismatched field and relationship");
    }
    {
        using Problem = addon::Provider_event_problem;
        const auto valid = addon::encode_poll_observations_result(poll_result);
        auto unknown = valid;
        unknown.set("private-field-name", addon::Json_value("private-metadata-value"));
        SDK_CHECK(!addon::decode_poll_observations_result(unknown, 2, &failure) &&
            failure.reason_code == addon::reason::k_unexpected_member,
            "unknown poll result members retain their hard rejection");
        const auto unknown_diagnostic = addon::decode_provider_event_diagnostic(failure.detail);
        SDK_CHECK(unknown_diagnostic && unknown_diagnostic->event_name == "poll_observations" &&
            unknown_diagnostic->field == "$" && unknown_diagnostic->problem == Problem::INVALID_SHAPE &&
            unknown_diagnostic->rule == "closed_members" &&
            failure.detail.find("private-field-name") == std::string::npos &&
            failure.detail.find("private-metadata-value") == std::string::npos,
            "unexpected keys and their values are excluded from result diagnostics");
        for (const auto* field : {"more", "resume_cursor"}) {
            auto malformed = valid;
            malformed.set(field, addon::Json_value());
            SDK_CHECK(!addon::decode_poll_observations_result(malformed, 2, &failure) &&
                failure.reason_code == addon::reason::k_frame_violation,
                "mistyped poll result fields retain their existing rejection");
            const auto diagnostic = addon::decode_provider_event_diagnostic(failure.detail);
            SDK_CHECK(diagnostic && diagnostic->event_name == "poll_observations" &&
                diagnostic->field == field && diagnostic->problem == Problem::WRONG_TYPE &&
                diagnostic->actual_type == addon::Json_type::NULL_VALUE,
                "result envelope diagnostics distinguish the precise mistyped member");
        }
        const auto scalar = addon::Json_value("private-metadata-value");
        SDK_CHECK(!addon::decode_observe_result(scalar, &failure) &&
            failure.reason_code == addon::reason::k_frame_violation,
            "a scalar observe result retains its frame violation");
        const auto scalar_diagnostic = addon::decode_provider_event_diagnostic(failure.detail);
        SDK_CHECK(scalar_diagnostic && scalar_diagnostic->event_name == "observe" &&
            scalar_diagnostic->field == "$" && scalar_diagnostic->problem == Problem::WRONG_TYPE &&
            scalar_diagnostic->actual_size == std::string_view("private-metadata-value").size() &&
            failure.detail.find("private-metadata-value") == std::string::npos,
            "observe result diagnostics preserve shape without preserving the private scalar");
    }

    // helper-ingress@2.
    const std::vector<addon::Capability_pair> beside_lifecycle = {
        {addon::k_lifecycle_events_capability_id, addon::k_lifecycle_events_version},
        {addon::k_helper_ingress_capability_id, addon::k_helper_ingress_degradation_version},
    };
    SDK_CHECK(addon::validate_helper_ingress_dependency(beside_lifecycle, &failure),
        "helper ingress beside lifecycle-events@2 satisfies its dependency");
    SDK_CHECK(!addon::validate_helper_ingress_dependency({{addon::k_helper_ingress_capability_id, 2}}, &failure) &&
        failure.reason_code == addon::reason::k_dependency_capability_unavailable,
        "helper ingress without a live lifecycle capability is dependency_capability_unavailable");
    SDK_CHECK(!addon::validate_helper_ingress_dependency({{addon::k_lifecycle_events_capability_id, 3}}, &failure) &&
        failure.reason_code == addon::reason::k_dependency_capability_unavailable,
        "an unknown lifecycle-events version does not satisfy the helper dependency");

    addon::Classify_event_request classify;
    classify.event.name           = "SessionStart";
    classify.event.body           = object_with({
        {"session_id", addon::Json_value("A")},
        {"source",     addon::Json_value("startup")},
    });
    classify.ledger.current_epoch = 0;
    classify.transition_nonce     = std::string(32, '3');
    const addon::Json_value classify_payload = addon::encode_classify_event_request(classify);
    SDK_CHECK(classify_payload.find("ledger")->find("current_transition")->is_null(),
        "the epoch-0 ledger carries a null transition");
    const std::optional<addon::Classify_event_request> decoded_classify =
        addon::decode_classify_event_request(classify_payload, &failure);
    SDK_CHECK(decoded_classify.has_value() && decoded_classify->ledger.current_epoch == 0 &&
        !decoded_classify->ledger.current_transition.has_value() &&
        decoded_classify->event.body == classify.event.body &&
        decoded_classify->transition_nonce == classify.transition_nonce,
        "an epoch-0 classify request round-trips: " + reason_of(failure));
    classify.ledger.current_epoch      = 2;
    classify.ledger.current_transition =
        addon::Ledger_transition{addon::Transition_kind::RESUME, "B", std::string(32, '0')};
    const std::optional<addon::Classify_event_request> decoded_classify_2 =
        addon::decode_classify_event_request(addon::encode_classify_event_request(classify), &failure);
    SDK_CHECK(decoded_classify_2.has_value() && decoded_classify_2->ledger.current_transition.has_value() &&
        decoded_classify_2->ledger.current_transition->kind == addon::Transition_kind::RESUME &&
        decoded_classify_2->ledger.current_transition->provider_session_id == "B",
        "a classify request with a current transition round-trips: " + reason_of(failure));
    const addon::Json_value canonical_event = object_with({
        {"body", classify.event.body},
        {"name", addon::Json_value("SessionStart")},
    });
    SDK_CHECK(addon::provider_event_evidence_digest_hex(classify.event) ==
            addon::to_hex(addon::Sha256::digest(addon::serialize_json_canonical(canonical_event))),
        "the evidence digest is SHA-256 over the canonical event object");
    {
        addon::Json_value payload = addon::encode_classify_event_request(classify);
        payload.set("transition_nonce", addon::Json_value("short"));
        SDK_CHECK(classify_request_rejection(payload) == addon::reason::k_frame_violation,
            "a transition nonce outside the 32-hex grammar is frame_violation");
        payload = addon::encode_classify_event_request(classify);
        payload.find("ledger")->set("current_transition", addon::Json_value());
        SDK_CHECK(classify_request_rejection(payload) == addon::reason::k_frame_violation,
            "a null transition after epoch 0 is frame_violation");
        payload = addon::encode_classify_event_request(classify);
        payload.find("ledger")->set("current_epoch", addon::Json_value(0));
        SDK_CHECK(classify_request_rejection(payload) == addon::reason::k_frame_violation,
            "a transition at epoch 0 is frame_violation");
        payload = addon::encode_classify_event_request(classify);
        payload.find("event")->set("name", addon::Json_value(""));
        SDK_CHECK(classify_request_rejection(payload) == addon::reason::k_frame_violation,
            "an empty event name is frame_violation");
        payload = addon::encode_classify_event_request(classify);
        payload.set("session_capability", addon::Json_value("leak"));
        SDK_CHECK(classify_request_rejection(payload) == addon::reason::k_unexpected_member,
            "an unexpected classify member is unexpected_member");
    }

    addon::Classify_event_result transition;
    transition.classification  = addon::Classification::TRANSITION;
    transition.transition      = addon::Transition_kind::START;
    transition.observations    = {started, fixture_phase_observation("evt-3")};
    transition.helper_response = object_with({{"seen", addon::Json_value("SessionStart")}});
    const std::optional<addon::Classify_event_result> decoded_transition =
        addon::decode_classify_event_result(addon::encode_classify_event_result(transition), &failure);
    SDK_CHECK(decoded_transition.has_value() &&
        decoded_transition->classification == addon::Classification::TRANSITION &&
        decoded_transition->transition == addon::Transition_kind::START &&
        decoded_transition->observations.size() == 2 &&
        decoded_transition->helper_response.has_value() &&
        *decoded_transition->helper_response == *transition.helper_response,
        "a transition classification with a companion observation round-trips: " + reason_of(failure));
    addon::Classify_event_result event;
    event.classification = addon::Classification::EVENT;
    event.observations   = {fixture_phase_observation("evt-4")};
    SDK_CHECK(classify_result_value_rejection(event).empty(),
        "an event classification with one observation decodes");
    addon::Classify_event_result ignore;
    ignore.classification = addon::Classification::IGNORED;
    SDK_CHECK(classify_result_value_rejection(ignore).empty(),
        "an ignore classification with no observations decodes");
    {
        addon::Lifecycle_observation child = fixture_phase_observation("child-event");
        child.subagent_transition = addon::Subagent_transition::STARTED;
        child.agent_id = "child-a";
        addon::Classify_event_result batch;
        batch.classification = addon::Classification::EVENT;
        batch.observations = {child};
        batch.subagent_evidence = addon::Subagent_evidence{
            1,
            addon::Subagent_evidence_kind::BATCH,
            "A",
            "provider-step-1",
            {"child-a"},
            {},
            {},
        };
        addon::Json_value payload = addon::encode_classify_event_result(batch);
        SDK_CHECK(!addon::decode_classify_event_result(payload, &failure, 2),
            "helper-ingress@2 rejects subagent evidence rather than dropping it");
        const auto decoded = addon::decode_classify_event_result(payload, &failure, 3);
        SDK_CHECK(decoded && decoded->subagent_evidence &&
            decoded->subagent_evidence->kind == addon::Subagent_evidence_kind::BATCH &&
            decoded->subagent_evidence->child_ids == std::vector<std::string>{"child-a"},
            "helper-ingress@3 preserves a complete subagent batch");
        batch.subagent_evidence->child_ids = {"child-a", "child-a"};
        SDK_CHECK(!addon::decode_classify_event_result(
                addon::encode_classify_event_result(batch), &failure, 3),
            "a duplicate child cannot make an ambiguous subagent batch");
        batch.subagent_evidence->child_ids = {"child-a"};
        batch.observations.clear();
        for (std::size_t index = 0;
             index < addon::k_max_subagent_evidence_observations;
             ++index)
        {
            addon::Lifecycle_observation next = child;
            next.provider_event_id = "child-event-" + std::to_string(index);
            next.agent_id = "child-" + std::to_string(index);
            batch.observations.push_back(std::move(next));
        }
        batch.subagent_evidence->child_ids.clear();
        for (std::size_t index = 0;
             index < addon::k_max_subagent_evidence_observations;
             ++index)
        {
            batch.subagent_evidence->child_ids.push_back(
                "child-" + std::to_string(index));
        }
        SDK_CHECK(addon::decode_classify_event_result(
                addon::encode_classify_event_result(batch), &failure, 3).has_value(),
            "helper-ingress@3 admits its bounded 32-child batch");
        SDK_CHECK(!addon::decode_classify_event_result(
                addon::encode_classify_event_result(batch), &failure, 2),
            "helper-ingress@2 retains its 16-observation limit");
        addon::Classify_event_result ordinary_v3;
        ordinary_v3.classification = addon::Classification::EVENT;
        ordinary_v3.observations = batch.observations;
        SDK_CHECK(!addon::decode_classify_event_result(
                addon::encode_classify_event_result(ordinary_v3), &failure, 3),
            "helper-ingress@3 keeps the 16-observation bound without a batch");
        batch.subagent_evidence = addon::Subagent_evidence{
            1,
            addon::Subagent_evidence_kind::RECONCILE,
            "A",
            {},
            {},
            "agy-stop-identity",
            {},
        };
        batch.observations = {fixture_phase_observation("agy-stop-identity")};
        SDK_CHECK(addon::decode_classify_event_result(
                addon::encode_classify_event_result(batch), &failure, 3).has_value(),
            "helper-ingress@3 carries a root-awaiting-input child reconciliation");
    }
    {
        addon::Classify_event_result loss;
        loss.lifecycle_degradation = addon::Lifecycle_degradation{};
        addon::Json_value payload = addon::encode_classify_event_result(loss);
        auto decoded = addon::decode_classify_event_result(payload, &failure, 2);
        SDK_CHECK(decoded && decoded->lifecycle_degradation &&
            !decoded->lifecycle_degradation->provider_session_id,
            "helper-ingress@2 preserves delivery loss without inventing a session");
        loss.lifecycle_degradation->provider_session_id = "native-session";
        payload = addon::encode_classify_event_result(loss);
        decoded = addon::decode_classify_event_result(payload, &failure, 2);
        SDK_CHECK(decoded && decoded->lifecycle_degradation->provider_session_id == "native-session",
            "correlation loss preserves the identity to quarantine");
        loss.lifecycle_degradation->version = 2;
        SDK_CHECK(!addon::decode_classify_event_result(addon::encode_classify_event_result(loss), &failure, 2),
            "an unknown degradation record version cannot become a successful ignore");
        loss.lifecycle_degradation->version = 1;
        loss.lifecycle_degradation->provider_session_id = "";
        SDK_CHECK(!addon::decode_classify_event_result(addon::encode_classify_event_result(loss), &failure, 2),
            "an empty correlation identity cannot masquerade as delivery loss");
        loss.lifecycle_degradation->provider_session_id = "native-session";
        loss.classification = addon::Classification::EVENT;
        loss.observations = event.observations;
        SDK_CHECK(!addon::decode_classify_event_result(addon::encode_classify_event_result(loss), &failure, 2),
            "lifecycle degradation cannot be mixed with observations");
    }
    struct Invalid_result
    {
        const char*                  name;
        std::function<void(Result&)> mutate;
    };
    const auto phase_only = [] { return std::vector<Observation>{fixture_phase_observation("x")}; };
    const std::vector<Invalid_result> invalid_results = {
        {"transition without a start",    [&](Result& r) { r = transition; r.observations = phase_only(); }},
        {"transition with two starts",    [&](Result& r) { r = transition; r.observations = {started, started}; }},
        {"transition without its member", [&](Result& r) { r = transition; r.transition.reset(); }},
        {"event carrying a start",        [&](Result& r) { r = event; r.observations = {started}; }},
        {"event without observations",    [&](Result& r) { r = event; r.observations.clear(); }},
        {"event with a transition",       [&](Result& r) { r = event; r.transition = addon::Transition_kind::START; }},
        {"ignore with observations",      [&](Result& r) { r = ignore; r.observations = phase_only(); }},
    };
    for (const Invalid_result& invalid : invalid_results) {
        addon::Classify_event_result result;
        invalid.mutate(result);
        SDK_CHECK(classify_result_value_rejection(result) == addon::reason::k_classification_invalid,
            std::string("classification_invalid: ") + invalid.name);
    }
    {
        addon::Json_value payload = addon::encode_classify_event_result(transition);
        payload.set("classification", addon::Json_value("maybe"));
        SDK_CHECK(classify_result_rejection(payload) == addon::reason::k_classification_invalid,
            "a classification outside the vocabulary is classification_invalid");
        payload = addon::encode_classify_event_result(transition);
        payload.set("epoch", addon::Json_value(3));
        SDK_CHECK(classify_result_rejection(payload) == addon::reason::k_classification_invalid,
            "an adapter that names an epoch is classification_invalid");
        payload = addon::encode_classify_event_result(transition);
        payload.find("observations")->as_array()[0].set("provider_session_id", addon::Json_value(""));
        SDK_CHECK(classify_result_rejection(payload) == addon::reason::k_observation_invalid,
            "an invalid observation inside a classification is observation_invalid");
        payload = addon::encode_classify_event_result(transition);
        addon::Json_value host_sized_response = addon::Json_value::array();
        for (int i = 0; i < 12'000; ++i) {
            host_sized_response.push_back(addon::Json_value(1.0));
        }
        SDK_CHECK(addon::serialize_json_canonical(host_sized_response).size() >
                limits::k_max_helper_response_bytes,
            "the response fixture is larger under the portable canonical spelling");
        payload.set("helper_response", std::move(host_sized_response));
        SDK_CHECK(classify_result_rejection(payload).empty(),
            "the portable decoder delegates the response transport-size limit to the host");
    }
    return true;
}

bool test_helper_client_contract()
{
    addon::Contract_failure failure;
    std::vector<std::pair<std::string, std::string>> environment = {
        {addon::k_helper_env_vnmctl_path,        addon::path_to_utf8(test_executable())},
        {addon::k_helper_env_session_capability, std::string(limits::k_session_capability_hex_chars, 'a')},
        {addon::k_helper_env_package_id,         "org.example.unknown-fixture-provider-sessions"},
        {addon::k_helper_env_generation_digest,  std::string(64, '0')},
        {addon::k_helper_env_generation_nonce,   std::string(32, '0')},
        {addon::k_helper_env_helpers_dir,        "/custody/org.example.unknown-fixture-provider-sessions/helpers"},
        {"PATH",                                 "/usr/bin"},
    };
    const std::optional<addon::Helper_environment> parsed = addon::parse_helper_environment(environment, &failure);
    SDK_CHECK(parsed.has_value() && parsed->vnmctl_path == environment[0].second &&
        parsed->package_id == environment[2].second &&
        parsed->helpers_dir == environment[5].second,
        "a complete bound-terminal environment parses: " + reason_of(failure));
    for (std::size_t index = 0; index < 6; ++index) {
        std::vector<std::pair<std::string, std::string>> incomplete = environment;
        incomplete.erase(incomplete.begin() + (std::ptrdiff_t)index);
        SDK_CHECK(!addon::parse_helper_environment(incomplete, &failure).has_value() &&
            failure.reason_code == addon::reason::k_invalid_helper_environment &&
            failure.detail.find(environment[index].first) == 0,
            "an absent variable is invalid_helper_environment naming it: " + environment[index].first);
    }
    for (const auto& [index, value] : std::initializer_list<std::pair<std::size_t, const char*>>{
             {1, "not-hex"}, {2, "Not.A.Package"}, {3, "abc"}, {4, "abc"}})
    {
        std::vector<std::pair<std::string, std::string>> malformed = environment;
        malformed[index].second = value;
        SDK_CHECK(!addon::parse_helper_environment(malformed, &failure).has_value() &&
            failure.reason_code == addon::reason::k_invalid_helper_environment &&
            failure.detail.find(environment[index].first) == 0,
            "a variable outside its grammar is invalid_helper_environment naming it: " + environment[index].first);
    }

    addon::Helper_client client(*parsed);
    const addon::Json_value body = object_with({
        {"session_id", addon::Json_value("A")},
        {"source",     addon::Json_value("startup")},
    });

    const addon::Provider_event_outcome committed = client.deliver_provider_event("Committed", body);
    SDK_CHECK(client.acknowledge_provider_event("committed-key") ==
        addon::k_helper_exit_committed,
        "Helper_client must invoke the explicit provider-event acknowledgement");
    SDK_CHECK(client.acknowledge_provider_event("") ==
            addon::k_helper_exit_invalid_arguments &&
        client.acknowledge_provider_event(std::string(
            limits::k_max_idempotency_key_bytes + 1, 'k')) ==
            addon::k_helper_exit_invalid_arguments &&
        client.acknowledge_provider_event(std::string("bad\0key", 7)) ==
            addon::k_helper_exit_invalid_arguments,
        "Helper_client must refuse invalid acknowledgement keys before spawning");
    SDK_CHECK(committed.committed && committed.accepted && committed.exit_code == addon::k_helper_exit_committed &&
        committed.epoch == 3 && committed.reason.empty() && committed.helper_response.has_value(),
        "a committed accepted delivery reports exit 0, the epoch and the helper response");
    const addon::Json_value* received = committed.helper_response->find("received");
    SDK_CHECK(received != nullptr && received->is_object() && received->as_object().size() == 3,
        "the wrapper received exactly the three-member argument object");
    const addon::Json_value* minted_key   = received->find("idempotency_key");
    const addon::Json_value* minted_nonce = received->find("transition_nonce");
    const addon::Json_value* sent_event   = received->find("event");
    SDK_CHECK(minted_key != nullptr && minted_key->is_string() &&
        addon::is_valid_generation_nonce_hex(minted_key->as_string()),
        "an omitted idempotency key is minted as 32 lowercase hex");
    SDK_CHECK(minted_nonce != nullptr && minted_nonce->is_string() &&
        addon::is_valid_transition_nonce_hex(minted_nonce->as_string()) &&
        minted_nonce->as_string() != minted_key->as_string(),
        "an omitted transition nonce is minted as 32 lowercase hex distinct from the key");
    SDK_CHECK(sent_event != nullptr && *sent_event->find("name") == addon::Json_value("Committed") &&
        *sent_event->find("body") == body,
        "the event travels as {name, body}");

    const std::string key   = "retry-key-1";
    const std::string nonce = std::string(32, 'b');
    const addon::Provider_event_outcome rejected = client.deliver_provider_event("Rejected", body, key, nonce);
    SDK_CHECK(rejected.committed && !rejected.accepted && rejected.reason == "stale_epoch" && rejected.epoch == 2 &&
        rejected.exit_code == addon::k_helper_exit_committed,
        "a committed rejected delivery reports exit 0 with the reason");
    const addon::Json_value* replayed = rejected.helper_response->find("received");
    SDK_CHECK(replayed != nullptr && *replayed->find("idempotency_key") == addon::Json_value(key) &&
        *replayed->find("transition_nonce") == addon::Json_value(nonce),
        "a caller-supplied key and nonce pass through unchanged");

    const addon::Provider_event_outcome transport = client.deliver_provider_event("Transport", body);
    SDK_CHECK(!transport.committed && transport.exit_code == addon::k_helper_exit_transport_failure,
        "a transport failure reports exit 1 uncommitted");
    const addon::Provider_event_outcome pending = client.deliver_provider_event("Pending", body);
    SDK_CHECK(!pending.committed && pending.exit_code == addon::k_helper_exit_pending,
        "a delivery still pending at the deadline reports exit 4 uncommitted");
    const addon::Provider_event_outcome garbage = client.deliver_provider_event("Garbage", body);
    SDK_CHECK(!garbage.committed && garbage.exit_code == addon::k_helper_exit_transport_failure,
        "a malformed committed outcome is a transport failure");
    const addon::Provider_event_outcome oversize = client.deliver_provider_event("Oversize", body);
    SDK_CHECK(!oversize.committed && oversize.exit_code == addon::k_helper_exit_transport_failure,
        "stdout beyond k_max_helper_client_stdout_bytes is a transport failure");
    const addon::Provider_event_outcome unknown = client.deliver_provider_event("Unscripted", body);
    SDK_CHECK(!unknown.committed && unknown.exit_code == addon::k_helper_exit_invalid_arguments,
        "the wrapper's exit 2 is reported as such");

    addon::Json_value host_sized_body = addon::Json_value::object();
    addon::Json_value host_sized_numbers = addon::Json_value::array();
    for (int i = 0; i < 16'000; ++i) {
        host_sized_numbers.push_back(addon::Json_value(1.0));
    }
    host_sized_body.set("numbers", std::move(host_sized_numbers));
    SDK_CHECK(addon::serialize_json_canonical(host_sized_body).size() >
            limits::k_max_provider_event_bytes,
        "the SDK boundary fixture is larger under the portable canonical spelling");
    const addon::Provider_event_outcome host_sized =
        client.deliver_provider_event("HostSized", host_sized_body);
    SDK_CHECK(host_sized.committed && host_sized.accepted && host_sized.epoch == 4,
        "the portable client delegates the transport-encoded body limit to the host");

    const std::string long_name(limits::k_max_provider_event_name_bytes + 1, 'n');
    const std::string long_key(limits::k_max_idempotency_key_bytes + 1, 'k');
    SDK_CHECK(client.deliver_provider_event("", body).exit_code == addon::k_helper_exit_invalid_arguments &&
        client.deliver_provider_event(long_name, body).exit_code == addon::k_helper_exit_invalid_arguments,
        "an empty or oversize event name is refused before spawning");
    SDK_CHECK(client.deliver_provider_event("Committed", addon::Json_value("body")).exit_code ==
            addon::k_helper_exit_invalid_arguments,
        "a non-object body is refused before spawning");
    SDK_CHECK(client.deliver_provider_event("Committed", body, "", "xyz").exit_code ==
            addon::k_helper_exit_invalid_arguments &&
        client.deliver_provider_event("Committed", body, long_key).exit_code == addon::k_helper_exit_invalid_arguments,
        "a malformed nonce or oversize key is refused before spawning");
    SDK_CHECK(client.set_title("Fixture review session") == addon::k_helper_exit_committed,
        "set-title reaches the wrapper");
    SDK_CHECK(client.set_title("unreachable") == addon::k_helper_exit_transport_failure,
        "set-title reports the wrapper's exit code");
    SDK_CHECK(client.set_title("") == addon::k_helper_exit_invalid_arguments &&
        client.set_title("a\tb") == addon::k_helper_exit_invalid_arguments,
        "an empty or control-character title is refused before spawning");
    const addon::Title_instruction_result instruction =
        client.request_title_instruction("agy", "UserPromptSubmit", true);
    SDK_CHECK(instruction.completed && instruction.accepted &&
        instruction.instruction_required && instruction.instruction == "host instruction" &&
        instruction.exit_code == addon::k_helper_exit_committed,
        "title instruction returns only the host-authored decision and bytes");
    const addon::Title_instruction_result model_instruction =
        client.request_title_instruction("fixture", "PreToolUse", true, addon::Model_context_delivery{});
    SDK_CHECK(model_instruction.completed && model_instruction.accepted &&
        model_instruction.instruction == "host instruction",
        "model-context delivery retains the real hook event and a versioned presence record");
    const addon::Model_context_delivery captured_delivery{addon::Model_context_activation{
        "captured-a", "0123456789abcdef0123456789abcdef"}};
    const auto captured_instruction =
        client.request_title_instruction("fixture", "PreToolUse", true, captured_delivery);
    SDK_CHECK(captured_instruction.completed && captured_instruction.accepted &&
        captured_instruction.instruction == "host instruction",
        "model-context@2 transports the original captured provider identity and nonce");
    auto malformed_delivery = captured_delivery;
    malformed_delivery.activation->transition_nonce = "invalid";
    SDK_CHECK(client.request_title_instruction("fixture", "PreToolUse", false, malformed_delivery).exit_code ==
            addon::k_helper_exit_invalid_arguments,
        "an invalid captured activation is refused before spawning");
    auto encoded_delivery = addon::encode_model_context_delivery(captured_delivery);
    const auto decoded_delivery = addon::decode_model_context_delivery(encoded_delivery, &failure);
    SDK_CHECK(decoded_delivery && decoded_delivery->activation &&
        decoded_delivery->activation->provider_session_id == "captured-a" &&
        decoded_delivery->activation->transition_nonce == "0123456789abcdef0123456789abcdef",
        "model-context@2 has a strict shared wire decoder");
    encoded_delivery.set("version", addon::Json_value(1));
    SDK_CHECK(!addon::decode_model_context_delivery(encoded_delivery, &failure),
        "a captured activation cannot silently become an unfenced version-1 request");
    encoded_delivery = addon::encode_model_context_delivery(addon::Model_context_delivery{});
    encoded_delivery.set("version", addon::Json_value(2));
    SDK_CHECK(!addon::decode_model_context_delivery(encoded_delivery, &failure),
        "model-context@2 requires its captured activation");
    encoded_delivery = addon::encode_model_context_delivery(captured_delivery);
    encoded_delivery.find("activation")->set("provider_session_id", addon::Json_value("bad\nidentity"));
    SDK_CHECK(!addon::decode_model_context_delivery(encoded_delivery, &failure),
        "captured identity uses the bounded lifecycle identity grammar");
    encoded_delivery = addon::encode_model_context_delivery(captured_delivery);
    encoded_delivery.find("activation")->set("unexpected", addon::Json_value(true));
    SDK_CHECK(!addon::decode_model_context_delivery(encoded_delivery, &failure),
        "model-context activation has no unrecognized members");
    const addon::Title_instruction_result title_refusal =
        client.request_title_instruction("refused", "UserPromptSubmit");
    SDK_CHECK(title_refusal.completed && !title_refusal.accepted &&
        title_refusal.reason == "unknown_session" &&
        title_refusal.exit_code == addon::k_helper_exit_committed,
        "a committed title-instruction refusal remains distinct from transport failure");
    SDK_CHECK(client.request_title_instruction("", "UserPromptSubmit").exit_code ==
            addon::k_helper_exit_invalid_arguments,
        "an invalid title-instruction hook fact is refused before spawning");
    SDK_CHECK(client.show_changed_files({"src/a.cpp", "docs/b.md"}) == addon::k_helper_exit_committed,
        "show_changed_files passes its paths");
    SDK_CHECK(client.show_changed_files({}) == addon::k_helper_exit_invalid_arguments,
        "show_changed_files without paths is refused");
    SDK_CHECK(client.show_diff(R"({"files":[]})") == addon::k_helper_exit_committed,
        "show_diff sends the manifest on stdin");
    SDK_CHECK(client.show_diff("not a manifest") == addon::k_helper_exit_invalid_arguments,
        "show_diff refuses a manifest that is not a JSON object");

    addon::Helper_environment missing_wrapper = *parsed;
    missing_wrapper.vnmctl_path = addon::path_to_utf8(test_executable().parent_path() / "no-such-vnmctl");
    addon::Helper_client absent(missing_wrapper);
    SDK_CHECK(!absent.deliver_provider_event("Committed", body).committed &&
        absent.deliver_provider_event("Committed", body).exit_code == addon::k_helper_exit_transport_failure &&
        absent.set_title("title") == addon::k_helper_exit_transport_failure,
        "a wrapper that cannot be spawned is a transport failure");
    return true;
}

} // namespace sdk_tests

