#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

/**
 * @brief Every numeric bound of the package, custody, protocol, process and
 * capability contracts. This header is the single source of the values the
 * SDK, conformance suite and host enforce; docs/limits.md mirrors them with
 * their rationale. These documented engineering defaults are reviewed for
 * correctness, compatibility and resource effects when changed. The host
 * links these constants rather than restating them.
 */
namespace logonomic::cli_addon::limits {

// Custody bounds (package discovery and immutable generation custody).
inline constexpr std::size_t   k_max_candidates_per_root       = 256;
inline constexpr std::size_t   k_max_admitted_generations      = 64;
inline constexpr std::size_t   k_max_files_per_package         = 4096;
inline constexpr std::size_t   k_max_package_path_segments     = 16;
inline constexpr std::uint64_t k_max_index_bytes               = 4u * 1024u * 1024u;
inline constexpr std::uint64_t k_max_file_bytes                = 512ull * 1024ull * 1024ull;
inline constexpr std::uint64_t k_max_generation_bytes          = 2ull * 1024ull * 1024ull * 1024ull;
inline constexpr std::uint64_t k_max_aggregate_custody_bytes   = 8ull * 1024ull * 1024ull * 1024ull;
inline constexpr std::uint64_t k_max_bootstrap_bytes           = 64u * 1024u;
inline constexpr std::uint64_t k_max_sidecar_bytes             = 256u * 1024u;
inline constexpr std::size_t   k_max_sidecars_per_package      = 64;

// Protocol limits (frames, JSON, requests).
inline constexpr std::uint64_t k_max_frame_bytes                   = 4u * 1024u * 1024u;
inline constexpr std::size_t   k_max_json_depth                    = 32;
inline constexpr std::size_t   k_max_json_members                  = 1024;
inline constexpr std::uint64_t k_max_json_string_bytes             = 1024u * 1024u;
inline constexpr std::size_t   k_stderr_ring_bytes                 = 64u * 1024u;
inline constexpr std::uint64_t k_max_stdin_queue_bytes             = 16u * 1024u * 1024u;
inline constexpr std::size_t   k_max_in_flight_requests            = 64;
// The largest protocol integer denotes an unlimited progress lifetime.
inline constexpr std::uint64_t k_unlimited_progress_frames = std::numeric_limits<std::int64_t>::max();
inline constexpr std::size_t   k_max_handshake_capabilities        = 256;
inline constexpr std::size_t   k_max_progress_note_bytes           = 256;
inline constexpr std::size_t   k_max_result_message_bytes          = 1024;
inline constexpr std::size_t   k_max_argv_entries                        = 64;
inline constexpr std::size_t   k_max_argv_entry_bytes                    = 4096;
inline constexpr std::size_t   k_max_argv_total_bytes                    = 11u * 1024u;
inline constexpr std::size_t   k_max_environment_entries                 = 16;   // bootstrap manifest
inline constexpr std::size_t   k_max_capability_environment_entries      = 64;   // capability-scoped public payload
inline constexpr std::size_t   k_max_environment_name_bytes              = 128;
inline constexpr std::size_t   k_max_environment_value_bytes             = 32766;
inline constexpr std::size_t   k_max_start_environment_entries           = 34;   // 18 Windows baseline/PATH + 16 manifest
inline constexpr std::size_t   k_max_start_environment_bytes             = 16u * 1024u;  // NAME + VALUE pairs, no '='
inline constexpr std::size_t   k_max_start_absolute_path_bytes           = 4096;
inline constexpr std::size_t   k_max_package_id_bytes                    = 128;
inline constexpr std::size_t   k_max_generation_nonce_bytes              = 32;
inline constexpr std::size_t   k_max_package_relative_path_bytes         = 200;
inline constexpr std::size_t   k_max_generation_directory_bytes          =
    k_max_start_absolute_path_bytes + 1 + k_max_package_id_bytes + 1 + k_max_generation_nonce_bytes;
inline constexpr std::size_t   k_max_adapter_executable_bytes            =
    k_max_generation_directory_bytes + 1 + k_max_package_relative_path_bytes;
inline constexpr std::size_t   k_max_reserved_environment_bytes          = 10u * 1024u; // seven full NAME=VALUE entries
inline constexpr std::size_t   k_max_supervisor_environment_bytes        =
    k_max_start_environment_bytes + k_max_start_environment_entries + k_max_reserved_environment_bytes;
inline constexpr std::size_t   k_max_windows_native_command_line_units   = 32766; // excludes terminating NUL
inline constexpr std::size_t   k_max_windows_environment_value_units     = 32766; // excludes terminating NUL
inline constexpr std::uint64_t k_max_control_message_bytes               = 256u * 1024u;
inline constexpr std::uint64_t k_max_supervisor_request_bytes            = 320u * 1024u;
inline constexpr std::uint64_t k_max_link_frame_bytes                    = k_max_frame_bytes + 4u * 1024u;

// Identifier and manifest bounds fixed by the grammars in docs/package_contract.md.
inline constexpr std::size_t   k_max_platforms_per_package         = 8;
inline constexpr std::size_t   k_max_core_generations_per_package  = 8;
inline constexpr std::size_t   k_max_release_version_bytes         = 64;
inline constexpr std::uint16_t k_max_version                       = 65535;

// presentation@1 icon media and the SVG subset (docs/presentation_1.md,
// docs/svg_subset.md).
inline constexpr std::uint64_t k_max_icon_source_bytes             = 256u * 1024u;
inline constexpr std::uint32_t k_min_icon_pixels                   = 16;
inline constexpr std::uint32_t k_max_icon_pixels                   = 1024;
inline constexpr std::uint32_t k_max_icon_aspect_ratio             = 4;      // width:height within [1:4, 4:1]
inline constexpr std::uint32_t k_canonical_icon_pixels             = 64;
inline constexpr std::uint64_t k_max_canonical_icon_bytes          = 32u * 1024u;
inline constexpr std::size_t   k_max_svg_elements                  = 1024;
inline constexpr std::size_t   k_max_svg_depth                     = 16;
inline constexpr std::size_t   k_max_svg_attributes_per_element    = 32;
inline constexpr std::size_t   k_max_svg_path_data_bytes           = 32u * 1024u;   // `d` and `points`
inline constexpr std::size_t   k_max_svg_path_segments             = 4096;
inline constexpr std::size_t   k_max_svg_attribute_bytes           = 1024;          // every other attribute
inline constexpr std::size_t   k_max_svg_id_bytes                  = 64;
inline constexpr std::size_t   k_max_svg_transforms                = 8;      // per transform list
inline constexpr std::size_t   k_max_svg_reference_chain           = 4;      // href hops
inline constexpr std::size_t   k_max_svg_expanded_elements         = 4096;   // after every use occurrence expands
inline constexpr std::size_t   k_max_display_name_scalars          = 40;
inline constexpr std::size_t   k_max_display_name_bytes            = 120;

// cooperative-description@1: transport bounds. The host retains its own
// title length, summary word-count and persistence policy.
inline constexpr std::size_t k_max_cooperative_title_bytes = 1024;
inline constexpr std::size_t k_max_cooperative_summary_bytes = 16u * 1024u;
inline constexpr std::size_t k_max_cooperative_description_bytes = 24u * 1024u;

// model-catalog@1 (docs/model_catalog_1.md): an atomic, explicitly partial
// native inventory. The canonical snapshot bound leaves helper framing space.
inline constexpr std::size_t k_max_model_catalog_entries = 128;
inline constexpr std::size_t k_max_model_catalog_snapshot_bytes = 28u * 1024u;

// accounts@2 (docs/accounts_2.md).
inline constexpr std::size_t k_max_adapter_account_key_bytes = 256;
inline constexpr std::size_t k_max_discovered_accounts = 64;

// launcher-discovery@1, launch-preparation@3 and managed-assets@1
// (docs/launcher_discovery_1.md and siblings).
inline constexpr std::size_t   k_max_discovery_environment_names   = 32;
inline constexpr std::size_t   k_max_discovery_candidates          = 16;
inline constexpr std::size_t   k_max_script_body_bytes             = 4096;
inline constexpr std::size_t   k_max_environment_text_bytes        = 4096;
inline constexpr std::size_t   k_max_working_directory_bytes       = 4096;
inline constexpr std::size_t   k_max_title_instruction_bytes       = 4096;
inline constexpr std::size_t   k_max_initial_prompt_text_bytes     = 1024;   // interactive-initial-prompt@1
inline constexpr std::uint64_t k_max_evidence_bytes                = 2048;   // canonical JSON of `evidence`
inline constexpr std::size_t   k_max_launch_patch_entries          = 64;     // `set` and `unset` each
inline constexpr std::size_t   k_max_asset_tokens_per_value        = 16;
inline constexpr std::size_t   k_max_managed_assets                = 64;
inline constexpr std::uint64_t k_max_managed_asset_bytes           = 4u * 1024u * 1024u;
inline constexpr std::uint64_t k_max_managed_assets_bytes          = 16u * 1024u * 1024u;

// structured-session@3, delegated-session@3 and interactive-resume@2
// (docs/structured_session_3.md, docs/delegated_session_3.md,
// docs/interactive_resume_2.md).
inline constexpr std::size_t   k_max_structured_purposes                 = 2;
inline constexpr std::uint64_t k_max_structured_prompt_bytes             = 48u * 1024u;
// k_max_structured_stream_bytes bounds chunked stdout, stdin and
// result payloads alike.
inline constexpr std::uint64_t k_max_structured_stream_bytes             = 1024u * 1024u;
inline constexpr std::uint64_t k_max_structured_stderr_bytes             = 64u * 1024u;
inline constexpr std::size_t   k_max_structured_failure_message_bytes    = 16u * 1024u;
inline constexpr std::uint64_t k_max_protocol_state_bytes                = 64u * 1024u;
// The session exchange preserves the native implementation's UTF-16
// state bound. Chunking keeps each encoded string inside the JSON bound.
inline constexpr std::size_t   k_max_exchange_protocol_state_code_units  = 2u * 1024u * 1024u;
// One UTF-16 code unit needs at most three UTF-8 bytes.
inline constexpr std::size_t   k_max_exchange_protocol_state_utf8_bytes  =
    3u * k_max_exchange_protocol_state_code_units;
inline constexpr std::size_t   k_exchange_chunk_bytes                    = 64u * 1024u;
// An invoke envelope has a 128-byte capability ID, two 64-byte identifiers,
// version, deadline and fixed syntax; a successful result envelope is smaller.
inline constexpr std::uint64_t k_max_exchange_payload_bytes              = k_max_frame_bytes - 512u;
inline constexpr std::size_t   k_max_presentation_records_per_inspection = 16;
inline constexpr std::size_t   k_max_delegated_task_servers              = 8;
inline constexpr std::size_t   k_max_delegated_task_id_bytes             = 128;
inline constexpr std::size_t   k_max_delegated_server_id_bytes           = 64;
inline constexpr std::size_t   k_max_delegated_server_actions            = 64;
inline constexpr std::size_t   k_max_delegated_private_artifacts         = 8;
inline constexpr std::uint64_t k_max_delegated_private_artifact_bytes    = 1024u * 1024u;
inline constexpr std::size_t   k_max_delegated_model_bytes               = 256;
inline constexpr std::size_t   k_max_delegated_reasoning_effort_bytes     = 64;
inline constexpr std::uint64_t k_max_resume_record_bytes                 = 16u * 1024u;   // canonical JSON of `record`

// lifecycle-events@2, helper-ingress@2/@3 and the helper client
// (docs/lifecycle_events_2.md, docs/helper_ingress_2.md).
inline constexpr std::size_t   k_max_lifecycle_identity_bytes            = 256;
inline constexpr std::size_t   k_max_lifecycle_label_bytes               = 160;
inline constexpr std::size_t   k_max_lifecycle_profile_id_bytes          = 64;
inline constexpr std::size_t   k_max_observations_per_result             = 16;
inline constexpr std::size_t   k_max_observe_cursor_bytes                = 256;
inline constexpr std::size_t   k_max_provider_event_name_bytes           = 256;
inline constexpr std::uint64_t k_max_provider_event_bytes                = 48u * 1024u;    // transport-encoded `event.body`
inline constexpr std::uint64_t k_max_helper_response_bytes               = 32u * 1024u;    // host-transport compact `helper_response`
inline constexpr std::size_t   k_max_idempotency_key_bytes               = 256;
inline constexpr std::size_t   k_transition_nonce_hex_chars              = 32;
inline constexpr std::size_t   k_session_capability_hex_chars            = 64;
inline constexpr std::size_t   k_ingress_journal_entries_per_session     = 256;
inline constexpr std::uint64_t k_ingress_journal_bytes_per_session       = 1024u * 1024u;
inline constexpr std::size_t   k_ingress_retired_transition_limit        = 128;
inline constexpr std::uint64_t k_max_helper_client_stdout_bytes          =
    k_max_helper_response_bytes + 4u * 1024u;   // the committed outcome with its envelope

// Deadlines, in milliseconds.
inline constexpr int k_custodian_start_deadline_ms = 2000;
inline constexpr int k_hello_deadline_ms           = 5000;
inline constexpr int k_activation_deadline_ms      = 10000;
inline constexpr int k_default_invoke_deadline_ms  = 5000;
inline constexpr int k_no_invoke_deadline_ms       = std::numeric_limits<std::int32_t>::max();
inline constexpr int k_cancel_grace_ms             = 2000;
inline constexpr int k_shutdown_grace_ms           = 5000;
inline constexpr int k_terminate_to_kill_ms        = 2000;
inline constexpr int k_reap_deadline_ms            = 5000;
inline constexpr int k_delegated_prepare_deadline_ms     = 45000;
inline constexpr int k_structured_prepare_deadline_ms    = k_delegated_prepare_deadline_ms;
inline constexpr int k_resume_prepare_deadline_ms        = k_delegated_prepare_deadline_ms;
inline constexpr int k_launch_prepare_deadline_ms        = k_delegated_prepare_deadline_ms;
inline constexpr int k_resume_inspect_deadline_ms        = 5000;
inline constexpr int k_observe_window_ms                 = 60000;
inline constexpr int k_observe_window_grace_ms           = k_default_invoke_deadline_ms;
inline constexpr int k_classify_event_deadline_ms        = 2000;
inline constexpr int k_provider_event_client_deadline_ms = 5000;
inline constexpr int k_provider_event_poll_interval_ms   = 100;
inline constexpr int k_helper_client_wrapper_deadline_ms =
    k_provider_event_client_deadline_ms + k_default_invoke_deadline_ms;
inline constexpr int k_ingress_response_ttl_ms           = 60000;
inline constexpr int k_ingress_teardown_drain_ms         = k_shutdown_grace_ms;

/// Literal recorded in diagnostics when the stderr ring dropped bytes.
inline constexpr const char* k_stderr_truncation_marker =
    "\xE2\x80\xA6[logonomic: stderr truncated]\xE2\x80\xA6";

} // namespace logonomic::cli_addon::limits
