#include "sdk_test_support.h"

#include "logonomic_cli_addon/capability_ids.h"
#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/launch_preparation.h"
#include "logonomic_cli_addon/limits.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
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

// The brief the host stages for one continuation, below the user's profile.
std::string brief_path(std::string_view user)
{
#if defined(_WIN32)
    return "C:\\Users\\" + std::string(user) +
        "\\AppData\\Local\\Logonomic\\continuation_briefs\\7f3c9a2e5b1d4f60\\brief.md";
#else
    return "/home/" + std::string(user) + "/.local/state/logonomic/continuation_briefs/7f3c9a2e5b1d4f60/brief.md";
#endif
}

std::string absolute_path_of_size(std::size_t size)
{
#if defined(_WIN32)
    return "C:\\" + std::string(size - 3, 'a');
#else
    return "/" + std::string(size - 1, 'a');
#endif
}

std::string continuation_text(const std::string& brief)
{
    return "Logonomic continuation: read the brief at " + brief +
        " before anything else and follow it; do not modify the project until the user confirms.";
}

addon::Prepare_request continuation_request(const addon::Initial_prompt_request& prompt)
{
    addon::Prepare_request request;
    request.launcher           = {"example-cli", addon::k_runner_direct, ""};
    request.working_directory  = "/srv/repositories/example";
    request.title_instruction  = "Set a concise session title once the task is clear.";
    request.launch_environment =
        addon::Launch_environment{addon::host_platform_id(), addon::Json_value::object(), std::nullopt};
    request.launcher_argv      = {"example-cli"};
    request.initial_prompt     = prompt;
    return request;
}

// A launch prepared to submit the prompt: the replacement invocation carries
// the text as the CLI's initial prompt and the patch echoes its digest.
addon::Launch_patch prepared_patch(const addon::Initial_prompt_request& prompt)
{
    addon::Launch_patch patch;
    patch.argv           = std::vector<std::string>{"example-cli", prompt.text};
    patch.disposition    = addon::Launch_disposition{};
    patch.initial_prompt = addon::Initial_prompt_acknowledgment{addon::initial_prompt_text_digest_hex(prompt.text)};
    return patch;
}


} // namespace

bool test_interactive_initial_prompt_contracts()
{
    // Oracle: docs/interactive_initial_prompt_1.md; the digest also FIPS 180-4.
    bool ok = true;
    const char* id = __func__;
    addon::Contract_failure failure;

    const std::string brief = brief_path("Zo\xC3\xAB Example");
    const addon::Initial_prompt_request prompt{continuation_text(brief), brief};
    const addon::Json_value request_payload = addon::encode_prepare_request(continuation_request(prompt), 3);
    const auto decoded_request = addon::decode_prepare_request(request_payload, &failure, 3);
    ok &= check(decoded_request && decoded_request->initial_prompt &&
        decoded_request->initial_prompt->text == prompt.text &&
        decoded_request->initial_prompt->referenced_file == prompt.referenced_file, id,
        "a version 3 request carries the prompt text and its file byte for byte");

    const addon::Json_value patch_payload = addon::encode_launch_patch(prepared_patch(prompt), 3);
    const auto decoded_patch = addon::decode_launch_patch(patch_payload, &failure, 3);
    ok &= check(decoded_patch && decoded_patch->initial_prompt &&
        addon::validate_launch_patch(*decoded_patch, {}, {}, &failure) &&
        addon::validate_initial_prompt_acknowledgment(prompt, *decoded_patch, &failure), id,
        "the exact echo from a prepared replacement invocation acknowledges the prompt");

    ok &= check(addon::initial_prompt_text_digest_hex("abc") ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", id,
        "the echo digest is SHA-256 over the exact text bytes");

    // Absent unless set: packages decode strictly, so an ordinary launch must
    // not carry the member.
    addon::Prepare_request ordinary = continuation_request(prompt);
    ordinary.initial_prompt.reset();
    const auto ordinary_payload = addon::encode_prepare_request(ordinary);
    const auto decoded_ordinary = addon::decode_prepare_request(ordinary_payload, &failure);
    ok &= check(!ordinary_payload.find(addon::k_initial_prompt_member) &&
        decoded_ordinary && !decoded_ordinary->initial_prompt, id,
        "a current ordinary request carries no initial prompt member");
    addon::Launch_patch ordinary_patch = prepared_patch(prompt);
    ordinary_patch.initial_prompt.reset();
    const addon::Json_value ordinary_patch_payload = addon::encode_launch_patch(ordinary_patch, 3);
    const auto decoded_ordinary_patch = addon::decode_launch_patch(ordinary_patch_payload, &failure, 3);
    ok &= check(ordinary_patch_payload.find(addon::k_initial_prompt_member) == nullptr &&
        decoded_ordinary_patch && !decoded_ordinary_patch->initial_prompt &&
        addon::validate_initial_prompt_acknowledgment(std::nullopt, *decoded_ordinary_patch, &failure), id,
        "an ordinary launch neither carries nor needs an echo");

    // Request member grammar and bounds.
    const auto admits = [&failure](const addon::Initial_prompt_request& candidate) {
        return addon::decode_prepare_request(
            addon::encode_prepare_request(continuation_request(candidate), 3), &failure, 3).has_value();
    };
    ok &= check(admits({std::string(limits::k_max_initial_prompt_text_bytes, 'x'), brief}), id,
        "a text at its byte bound is admitted");
    ok &= check(!admits({std::string(limits::k_max_initial_prompt_text_bytes + 1, 'x'), brief}) &&
        failure.reason_code == addon::reason::k_payload_too_large, id,
        "a text one byte over its bound is payload_too_large");
    ok &= check(!admits({"", brief}), id, "an empty text is refused");
    for (const char control : {'\n', '\r', '\t', '\0', '\x7F'}) {
        std::string text = prompt.text;
        text.insert(text.find(" before"), 1, control);
        ok &= check(!admits({text, brief}), id,
            "a text with control byte " + std::to_string((int)(unsigned char)control) + " is refused");
        std::string file = brief;
        file.insert(file.rfind("brief.md"), 1, control);
        ok &= check(!admits({prompt.text, file}), id,
            "a referenced file with control byte " + std::to_string((int)(unsigned char)control) + " is refused");
    }
    ok &= check(!admits({prompt.text + "\xC3\x28", brief}), id, "a text that is not UTF-8 is refused");
    const std::string braced = brief_path("ann{{draft}}");
    ok &= check(!admits({continuation_text(braced), brief}), id,
        "a text with {{ is refused: replacement argv reserves it for asset tokens");
    ok &= check(!admits({prompt.text, braced}), id,
        "a referenced file with {{ is refused: its directory may enter replacement argv");
    const std::string single_brace = brief_path("ann{draft}");
    ok &= check(admits({continuation_text(single_brace), single_brace}), id,
        "a single brace is ordinary path text");
    ok &= check(!admits({prompt.text, "continuation_briefs/7f3c9a2e5b1d4f60/brief.md"}), id,
        "a relative referenced file is refused");
    ok &= check(admits({prompt.text, absolute_path_of_size(limits::k_max_start_absolute_path_bytes)}), id,
        "a referenced file at the absolute-path bound is admitted");
    ok &= check(!admits({prompt.text, absolute_path_of_size(limits::k_max_start_absolute_path_bytes + 1)}), id,
        "a referenced file over the absolute-path bound is refused");
    const auto admits_member = [&failure, &request_payload](std::string name, addon::Json_value value) {
        addon::Json_value payload = request_payload;
        payload.find(addon::k_initial_prompt_member)->set(std::move(name), std::move(value));
        return addon::decode_prepare_request(payload, &failure, 3).has_value();
    };
    ok &= check(!admits_member("encoding", addon::Json_value("utf-8")) &&
        failure.reason_code == addon::reason::k_unexpected_member, id,
        "the request member has a closed member set");
    ok &= check(!admits_member("version", addon::Json_value(2)), id,
        "an unknown request member version is refused");

    // Echo grammar.
    const auto admits_echo = [&failure, &patch_payload](std::string name, addon::Json_value value) {
        addon::Json_value payload = patch_payload;
        payload.find(addon::k_initial_prompt_member)->set(std::move(name), std::move(value));
        return addon::decode_launch_patch(payload, &failure, 3).has_value();
    };
    std::string uppercase_digest = addon::initial_prompt_text_digest_hex(prompt.text);
    std::transform(uppercase_digest.begin(), uppercase_digest.end(), uppercase_digest.begin(),
        [](unsigned char c) { return (char)std::toupper(c); });
    ok &= check(!admits_echo("text_sha256", addon::Json_value(uppercase_digest)), id,
        "an echo digest must be lowercase hex");
    ok &= check(!admits_echo("text_sha256", addon::Json_value(uppercase_digest.substr(1))), id,
        "an echo digest must be 64 characters");
    ok &= check(!admits_echo("version", addon::Json_value(2)), id, "an unknown echo version is refused");
    ok &= check(!admits_echo("text", addon::Json_value(prompt.text)), id, "the echo has a closed member set");

    // The host's acknowledgment rules.
    addon::Launch_patch unacknowledged = prepared_patch(prompt);
    unacknowledged.initial_prompt.reset();
    ok &= check(!addon::validate_initial_prompt_acknowledgment(prompt, unacknowledged, &failure) &&
        failure.reason_code == addon::reason::k_initial_prompt_unacknowledged, id,
        "a success without the echo is initial_prompt_unacknowledged");
    const std::string other_brief = brief_path("Ann");
    const addon::Launch_patch other_prompt = prepared_patch({continuation_text(other_brief), other_brief});
    ok &= check(!addon::validate_initial_prompt_acknowledgment(prompt, other_prompt, &failure) &&
        failure.reason_code == addon::reason::k_initial_prompt_mismatch, id,
        "the digest of another text is initial_prompt_mismatch");
    ok &= check(!addon::validate_initial_prompt_acknowledgment(std::nullopt, prepared_patch(prompt), &failure) &&
        failure.reason_code == addon::reason::k_launch_patch_invalid, id,
        "an echo cannot answer a request without a prompt");
    addon::Launch_patch unmanaged = prepared_patch(prompt);
    unmanaged.argv.reset();
    unmanaged.disposition = addon::Launch_disposition{false, false, false};
    ok &= check(!addon::validate_initial_prompt_acknowledgment(prompt, unmanaged, &failure), id,
        "an unchanged ordinary launch cannot acknowledge a prompt");
    addon::Launch_patch unreplaced = prepared_patch(prompt);
    unreplaced.argv.reset();
    ok &= check(!addon::validate_initial_prompt_acknowledgment(prompt, unreplaced, &failure), id,
        "an acknowledged prompt needs a replacement invocation");

    const addon::Capability_pair prompt_pair{
        addon::k_interactive_initial_prompt_capability_id,
        addon::k_interactive_initial_prompt_version,
    };
    ok &= check(addon::validate_interactive_initial_prompt_dependency({
            prompt_pair,
            {addon::k_launch_preparation_capability_id, addon::k_launch_preparation_execution_version},
        }, &failure), id, "launch-preparation@3 in the same generation satisfies the dependency");
    ok &= check(!addon::validate_interactive_initial_prompt_dependency({
            prompt_pair,
        }, &failure) &&
        failure.reason_code == addon::reason::k_dependency_capability_unavailable, id,
        "without launch-preparation@3 the pair is not live");
    return ok;
}

} // namespace sdk_tests
