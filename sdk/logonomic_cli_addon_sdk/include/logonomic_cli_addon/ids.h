#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace logonomic::cli_addon {

/**
 * @brief A reason code from the shared vocabulary below plus free-form detail
 * naming the offending entry, member or bound. Every validation and protocol
 * failure in the SDK is reported in this shape.
 */
struct Contract_failure
{
    std::string reason_code;
    std::string detail;
};

/**
 * @brief One `(capability_id, version)` pair, as declared by a sidecar, by an
 * adapter handshake, or as selected for an invocation.
 */
struct Capability_pair
{
    std::string   capability_id;
    std::uint16_t version = 0;

    bool operator==(const Capability_pair& other) const
    {
        return version == other.version && capability_id == other.capability_id;
    }
    bool operator!=(const Capability_pair& other) const { return !(*this == other); }
};

/**
 * @brief Identifier grammars of the package contract (docs/package_contract.md,
 * "Identifier grammars"). Every predicate is a pure byte-level check: no
 * case-folding, no Unicode normalization, no trimming.
 */

/// Canonical lowercase reverse-domain package ID: 3-128 bytes, two or more
/// segments of 1-63 bytes, letters/digits at both ends, interior hyphens only;
/// adjacent hyphens are rejected.
bool is_valid_package_id(std::string_view id);

/// 1-128 bytes of lowercase letters, digits, '.', and interior '-'; no "..",
/// no '-' adjacent to '.'. Interior repeated hyphens are allowed.
bool is_valid_capability_id(std::string_view id);

/// 1-64 bytes of lowercase letters, digits and '_', first byte a letter.
bool is_valid_operation_name(std::string_view name);

/// Reason codes share the operation-name grammar.
bool is_valid_reason_code(std::string_view code);

/// A version is an integer in [0, 65535].
bool is_valid_version(std::int64_t version);

/// 1-64 UTF-8 bytes without control characters; informational only.
bool is_valid_release_version(std::string_view release_version);

/// `<os>-<arch>` of lowercase letters, digits and '_'.
bool is_valid_platform_id(std::string_view platform_id);

/// Host-issued request ID: 'r' followed by decimal digits without a leading
/// zero, 1-64 bytes in total.
bool is_valid_request_id(std::string_view request_id);

/// `[A-Za-z_][A-Za-z0-9_]*`, 1-128 bytes. Does not apply the reserved-prefix
/// rule; see is_reserved_environment_name.
bool is_valid_environment_name(std::string_view name);

/// Every name beginning with `LOGONOMIC_` is reserved to the host.
bool is_reserved_environment_name(std::string_view name);

/// Canonical package-relative path: '/'-separated, 1-200 bytes, 1-16 segments
/// of 1-128 bytes, no '.' or '..' segment, no leading '/', no '\\', no control
/// or `< > : " | ? *` characters, no segment ending in '.' or space, and a
/// first segment naming one of the six package directories.
bool is_valid_package_relative_path(std::string_view path);

/// Splits a path on '/'. Empty segments are preserved so callers can reject
/// them; the canonical grammar never produces one.
std::vector<std::string_view> split_path_segments(std::string_view path);

/// First segment of a package-relative path, or empty when there is none.
std::string_view path_first_segment(std::string_view path);

/// ASCII-only lowercase mapping used to detect paths that would collide on a
/// case-insensitive file system.
std::string ascii_lowercase(std::string_view text);

/// 64 lowercase hexadecimal characters.
bool is_valid_sha256_hex(std::string_view hex);

/// 32 lowercase hexadecimal characters.
bool is_valid_generation_nonce_hex(std::string_view hex);

/// 32 lowercase hexadecimal characters: the transition nonce a helper mints
/// for helper-ingress@2/@3 (docs/helper_ingress_2.md). The grammar equals the
/// generation nonce's; the two identify different things.
bool is_valid_transition_nonce_hex(std::string_view hex);

/// 32 lowercase hexadecimal characters drawn from the platform's random
/// source (std::random_device), never from time: the producer of generation
/// nonces, transition nonces and idempotency keys.
std::string random_nonce_hex();

/// The session-provenance `provider_id` grammar: 1-64 bytes, the first a
/// lowercase letter, the rest lowercase letters, digits, '-' or '_'. Derived
/// identifiers (provider_identity.h) satisfy it by construction.
bool is_valid_provider_id(std::string_view provider_id);

/// Platform identifiers of the fixed platform matrix.
inline constexpr const char* k_platform_linux_x86_64   = "linux-x86_64";
inline constexpr const char* k_platform_windows_x86_64 = "windows-x86_64";
inline constexpr const char* k_platform_macos_arm64    = "macos-arm64";

/// The platform ID this SDK build runs on.
const char* host_platform_id();

/// First path segments a package may contain, with the resource kind each
/// segment classifies.
inline constexpr const char* k_segment_adapter      = "adapter";
inline constexpr const char* k_segment_capabilities = "capabilities";
inline constexpr const char* k_segment_helpers      = "helpers";
inline constexpr const char* k_segment_assets       = "assets";
inline constexpr const char* k_segment_notices      = "notices";
inline constexpr const char* k_segment_provenance   = "provenance";

/**
 * @brief The shared reason-code vocabulary. The host and the SDK report the
 * same strings; docs/package_contract.md and docs/core_protocol.md describe
 * when each is produced.
 */
namespace reason {

// Registry and custody.
inline constexpr const char* k_invalid_bootstrap             = "invalid_bootstrap";
inline constexpr const char* k_unsupported_bootstrap_version = "unsupported_bootstrap_version";
inline constexpr const char* k_platform_not_supported        = "platform_not_supported";
inline constexpr const char* k_shell_adapter_entry_point     = "shell_adapter_entry_point";
inline constexpr const char* k_reserved_environment_variable = "reserved_environment_variable";
inline constexpr const char* k_unsupported_core_generation   = "unsupported_core_generation";
inline constexpr const char* k_invalid_index                 = "invalid_index";
inline constexpr const char* k_unindexed_file                = "unindexed_file";
inline constexpr const char* k_missing_file                  = "missing_file";
inline constexpr const char* k_link_entry                    = "link_entry";
inline constexpr const char* k_digest_mismatch               = "digest_mismatch";
inline constexpr const char* k_path_escape                   = "path_escape";
inline constexpr const char* k_invalid_utf8                  = "invalid_utf8";
inline constexpr const char* k_third_party_record_invalid    = "third_party_record_invalid";
inline constexpr const char* k_sidecar_name_mismatch         = "sidecar_name_mismatch";
inline constexpr const char* k_invalid_sidecar               = "invalid_sidecar";
inline constexpr const char* k_duplicate_package_id          = "duplicate_package_id";
inline constexpr const char* k_candidate_limit_exceeded      = "candidate_limit_exceeded";
inline constexpr const char* k_custody_bound_exceeded        = "custody_bound_exceeded";
inline constexpr const char* k_custody_write_failed          = "custody_write_failed";
inline constexpr const char* k_source_mutated                = "source_mutated";
inline constexpr const char* k_transport_conflict            = "transport_conflict";
inline constexpr const char* k_registry_not_settled          = "registry_not_settled";

// Activation and protocol.
inline constexpr const char* k_identity_mismatch             = "identity_mismatch";
inline constexpr const char* k_hello_timeout                 = "hello_timeout";
inline constexpr const char* k_activation_timeout            = "activation_timeout";
inline constexpr const char* k_oversize_frame                = "oversize_frame";
inline constexpr const char* k_frame_violation               = "frame_violation";
inline constexpr const char* k_unknown_message_type          = "unknown_message_type";
inline constexpr const char* k_unexpected_member             = "unexpected_member";
inline constexpr const char* k_out_of_order_message          = "out_of_order_message";
inline constexpr const char* k_impossible_correlation        = "impossible_correlation";
inline constexpr const char* k_duplicate_terminal_result     = "duplicate_terminal_result";
inline constexpr const char* k_progress_limit_exceeded       = "progress_limit_exceeded";
inline constexpr const char* k_stdin_queue_overflow          = "stdin_queue_overflow";
inline constexpr const char* k_inbound_queue_overflow        = "inbound_queue_overflow";
inline constexpr const char* k_adapter_exited                = "adapter_exited";
inline constexpr const char* k_supervisor_lost               = "supervisor_lost";
inline constexpr const char* k_custodian_lost                = "custodian_lost";
inline constexpr const char* k_cancel_grace_expired          = "cancel_grace_expired";
inline constexpr const char* k_shutdown                      = "shutdown";

// Capability.
inline constexpr const char* k_unsupported_version               = "unsupported_version";
inline constexpr const char* k_unknown_capability                = "unknown_capability";
inline constexpr const char* k_sidecar_handshake_disagreement    = "sidecar_handshake_disagreement";
inline constexpr const char* k_payload_too_large                 = "payload_too_large";
inline constexpr const char* k_unknown_operation                 = "unknown_operation";
inline constexpr const char* k_profile_storage_unacknowledged    = "profile_storage_unacknowledged";
inline constexpr const char* k_profile_storage_version_mismatch  = "profile_storage_version_mismatch";
inline constexpr const char* k_profile_storage_mismatch          = "profile_storage_mismatch";
inline constexpr const char* k_backpressured                     = "backpressured";

// Process proposals.
inline constexpr const char* k_detached_helper_proposal          = "detached_helper_proposal";

// presentation@1 (docs/presentation_1.md, docs/svg_subset.md).
inline constexpr const char* k_presentation_invalid              = "presentation_invalid";
inline constexpr const char* k_icon_media_unsupported            = "icon_media_unsupported";
inline constexpr const char* k_icon_too_large                    = "icon_too_large";
inline constexpr const char* k_icon_dimensions_invalid           = "icon_dimensions_invalid";
inline constexpr const char* k_svg_subset_violation              = "svg_subset_violation";

// launcher-discovery@1 and launch-preparation@3.
inline constexpr const char* k_discovery_candidate_invalid       = "discovery_candidate_invalid";
inline constexpr const char* k_recognition_ambiguous             = "recognition_ambiguous";
inline constexpr const char* k_launch_patch_invalid              = "launch_patch_invalid";
inline constexpr const char* k_asset_undeclared                  = "asset_undeclared";
// A cancellation acknowledged while launch preparation was checking the CLI version.
inline constexpr const char* k_launcher_version_cancelled        = "launcher_version_cancelled";

// interactive-initial-prompt@1.
inline constexpr const char* k_initial_prompt_unavailable        = "initial_prompt_unavailable";
inline constexpr const char* k_initial_prompt_unacknowledged     = "initial_prompt_unacknowledged";
inline constexpr const char* k_initial_prompt_mismatch           = "initial_prompt_mismatch";

// managed-assets@1.
inline constexpr const char* k_asset_reference_invalid           = "asset_reference_invalid";
inline constexpr const char* k_asset_too_large                   = "asset_too_large";
inline constexpr const char* k_managed_asset_materialization_failed = "managed_asset_materialization_failed";

// structured-session@3, delegated-session@3 and interactive-resume@2.
inline constexpr const char* k_purpose_unavailable               = "purpose_unavailable";
inline constexpr const char* k_launcher_program_unavailable      = "launcher_program_unavailable";
inline constexpr const char* k_inspection_invalid                = "inspection_invalid";
inline constexpr const char* k_acknowledgment_mismatch           = "acknowledgment_mismatch";
inline constexpr const char* k_private_artifact_invalid          = "private_artifact_invalid";
inline constexpr const char* k_resume_record_invalid             = "resume_record_invalid";

// delegated-model-selection@1.
inline constexpr const char* k_model_selection_unavailable        = "model_selection_unavailable";
inline constexpr const char* k_reasoning_effort_unavailable       = "reasoning_effort_unavailable";

// lifecycle-events@2 and helper-ingress@2/@3.
inline constexpr const char* k_observation_invalid               = "observation_invalid";
inline constexpr const char* k_classification_invalid            = "classification_invalid";

// Capability dependencies and derived provider identity.
inline constexpr const char* k_dependency_capability_unavailable = "dependency_capability_unavailable";
inline constexpr const char* k_provider_id_collision             = "provider_id_collision";

// The helper client (helper_client.h): a bound terminal's environment is
// incomplete or outside its grammar.
inline constexpr const char* k_invalid_helper_environment        = "invalid_helper_environment";

} // namespace reason

} // namespace logonomic::cli_addon
