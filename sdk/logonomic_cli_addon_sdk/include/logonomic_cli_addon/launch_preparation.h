#pragma once

#include "logonomic_cli_addon/accounts.h"
#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/json.h"
#include "logonomic_cli_addon/launcher_discovery.h"
#include "logonomic_cli_addon/profile_storage.h"
#include "logonomic_cli_addon/session_wake.h"
#include "logonomic_cli_addon/terminal_identity.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace logonomic::cli_addon {

/**
 * @brief launch-preparation@3 (docs/launch_preparation_3.md): an adapter
 * capability with one operation, `prepare`, that answers a declarative
 * environment patch, an optional verified invocation replacement and an
 * explicit per-launch restriction of declared feature authority.
 */
inline constexpr const char* k_prepare_operation = "prepare";

/**
 * @brief interactive-initial-prompt@1 (docs/interactive_initial_prompt_1.md):
 * the independently versioned `initial_prompt` member of a
 * launch-preparation@3 `prepare` request and of its success patch. The
 * package's prepared invocation submits the request's `text` verbatim as the
 * first user message of a new interactive session and echoes its digest. The
 * promise covers what the invocation determines, not the user's own CLI
 * configuration, such as a default agent, hooks or plugins.
 */
inline constexpr const char* k_initial_prompt_member = "initial_prompt";

/// `text` is one line of at most k_max_initial_prompt_text_bytes; it names
/// `referenced_file`, a host-owned file the package may let its CLI read.
/// Neither contains "{{", which replacement argv reserves for asset tokens.
struct Initial_prompt_request
{
    std::string text;
    std::string referenced_file;   // a bounded absolute path without control characters
};

struct Initial_prompt_acknowledgment
{
    std::string text_sha256_hex;   // initial_prompt_text_digest_hex of the text submitted
};

/// terminal-codex-invocation@1 asks the package to identify its verified
/// Codex executable prefix so terminal adaptation survives an npm Node launch.
inline constexpr const char* k_codex_invocation_member = "codex_invocation";

struct Codex_invocation_request
{};

struct Codex_invocation
{
    std::size_t argument_prefix_size = 0;   // executable and entrypoint arguments, before Codex CLI arguments
};

Json_value encode_codex_invocation_request(const Codex_invocation_request& request);
Json_value encode_codex_invocation(const Codex_invocation& invocation);

/// Unknown record versions are `unknown_capability`; common launch decoders
/// keep their core and leave this capability absent in that case.
std::optional<Codex_invocation_request> decode_codex_invocation_request(
    const Json_value& payload, Contract_failure* out_failure);
std::optional<Codex_invocation> decode_codex_invocation(
    const Json_value& payload, Contract_failure* out_failure);

/// The independently negotiated sidecar is an empty object.
bool validate_terminal_codex_invocation_declaration(const Json_value& payload, Contract_failure* out_failure);

/// Needs a live interactive invocation producer: launch-preparation@3 or
/// interactive-resume@2 in the same generation.
bool validate_terminal_codex_invocation_dependency(
    const std::vector<Capability_pair>& live_pairs, Contract_failure* out_failure);

/// The `prepare` payload the host sends.
struct Prepare_request
{
    Launcher_request                       launcher;                         // exactly the recognize launcher record
    std::string                            working_directory;
    std::string                            title_instruction;
    std::vector<std::string>               declared_assets;                   // the valid managed-assets@1 declaration, or empty
    std::optional<Profile_storage_context> profile_storage;                   // present when profile-storage@1 is live
    std::optional<Account_context>         account_context = std::nullopt;
    std::optional<Launch_environment>      launch_environment;
    std::vector<std::string>               launcher_argv = {};
    std::optional<Initial_prompt_request>  initial_prompt = std::nullopt;     // version 3 only
    std::optional<Codex_invocation_request> codex_invocation = std::nullopt;
    std::optional<Session_wake_offer> session_wake = std::nullopt;
    std::optional<Terminal_identity_offer> terminal_identity = std::nullopt;
};

/// Throws std::invalid_argument for an unsupported contract version.
Json_value encode_prepare_request(const Prepare_request& request, std::uint16_t version = 3);

/// Adapter side: strict decode. Reasons: `frame_violation`,
/// `unexpected_member`, `payload_too_large`.
std::optional<Prepare_request> decode_prepare_request(
    const Json_value& payload, Contract_failure* out_failure, std::uint16_t version = 3);

enum class Working_directory_patch
{
    INHERIT,           // the terminal keeps the user's repository path
    REPOSITORY_ROOT,   // explicitly selects the host's repository path
};

const char* working_directory_patch_name(Working_directory_patch patch);

struct Launch_disposition
{
    bool prepared = true;
    bool lifecycle_events = true;
    bool coordination_delivery = true;
};

struct Launch_patch
{
    std::vector<std::pair<std::string, std::string>> set;      // name, raw value with asset tokens
    std::vector<std::string>                         unset;
    Working_directory_patch                          working_directory = Working_directory_patch::INHERIT;
    std::optional<Profile_storage_context>           profile_storage;   // the echo, when the request carried one
    std::optional<Account_context>         account_context = std::nullopt;
    std::optional<std::vector<std::string>> argv = std::nullopt;
    std::optional<Launch_disposition> disposition = std::nullopt;
    bool apply_managed_configuration = false;
    std::optional<Initial_prompt_acknowledgment> initial_prompt = std::nullopt;   // version 3 only
    std::optional<Codex_invocation> codex_invocation = std::nullopt;
    std::optional<Session_wake_preparation> session_wake = std::nullopt;
    std::optional<Terminal_identity_preparation> terminal_identity = std::nullopt;
};

/// One `{{asset:<path>}}` or `{{asset_json:<path>}}` token of a value;
/// `begin` and `end` delimit the token bytes in the value.
struct Asset_token
{
    std::string canonical_path;
    bool        json_encoded = false;
    std::size_t begin        = 0;
    std::size_t end          = 0;
};

/// Host side: strict structural decode of the success payload. Reason:
/// `launch_patch_invalid`.
std::optional<Launch_patch> decode_launch_patch(const Json_value& payload, Contract_failure* out_failure, std::uint16_t version = 3);

/// Adapter side. Throws std::invalid_argument for an unsupported contract
/// version or when the required launch disposition is absent.
Json_value encode_launch_patch(const Launch_patch& patch, std::uint16_t version = 3);

/// SHA-256 over the exact UTF-8 bytes of an initial prompt's text, as 64
/// lowercase hexadecimal characters.
std::string initial_prompt_text_digest_hex(std::string_view text);

/// Host side, for a decoded patch answering a request whose initial prompt
/// was `request`: the exact echo (`initial_prompt_unacknowledged`,
/// `initial_prompt_mismatch`) from a prepared launch with a replacement argv,
/// and no echo without a request (both `launch_patch_invalid` otherwise).
bool validate_initial_prompt_acknowledgment(
    const std::optional<Initial_prompt_request>& request,
    const Launch_patch&                          patch,
    Contract_failure*                            out_failure);

/// The offered capability must be acknowledged by a prepared invocation.
/// `actual_argv` is the final command, including a resume bridge when present;
/// its prefix boundary must be nonzero and within that command. Missing
/// acknowledgment is capability-local (`unknown_capability`).
bool validate_codex_invocation_acknowledgment(
    const std::optional<Codex_invocation_request>& request,
    const Launch_patch&                           patch,
    std::span<const std::string>                   actual_argv,
    Contract_failure*                             out_failure);

/// `dependency_capability_unavailable` unless launch-preparation@3 is among
/// the live pairs.
bool validate_interactive_initial_prompt_dependency(
    const std::vector<Capability_pair>& live_pairs,
    Contract_failure*                   out_failure);

/// The tokens of one value in order. Any other `{{` sequence, a token path
/// that is not a canonical path below assets/, or more than
/// k_max_asset_tokens_per_value tokens is `launch_patch_invalid`.
std::optional<std::vector<Asset_token>> scan_asset_tokens(std::string_view value, Contract_failure* out_failure);

/// Policy over a decoded patch: entry bounds (`payload_too_large`), name
/// grammar and the reserved prefix (`reserved_environment_variable`), value
/// bounds and NUL, a name both set and unset, malformed tokens
/// (`launch_patch_invalid`), a token path outside `declared_assets`
/// (`asset_undeclared`) or outside `indexed_assets` (`asset_reference_invalid`).
bool validate_launch_patch(
    const Launch_patch&             patch,
    const std::vector<std::string>& declared_assets,
    const std::vector<std::string>& indexed_assets,
    Contract_failure*               out_failure);

/// Replaces every token of a value: `asset` with the materialized native
/// path, `asset_json` with that path as a JSON string literal. nullopt when
/// a token's path has no entry in `materialized_paths` (canonical path,
/// native path).
std::optional<std::string> substitute_asset_tokens(
    std::string_view                                        value,
    const std::vector<Asset_token>&                         tokens,
    const std::vector<std::pair<std::string, std::string>>& materialized_paths);

} // namespace logonomic::cli_addon
