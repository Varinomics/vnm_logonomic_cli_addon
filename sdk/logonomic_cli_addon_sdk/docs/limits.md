# Limits

Every numeric bound of the contracts lives once, in
`include/logonomic_cli_addon/limits.h` (`namespace
logonomic::cli_addon::limits`, `inline constexpr`). This document mirrors the
values with their rationale. The host links the constants rather than
restating them; a value changes in `limits.h` and here in the same change.

These values are documented engineering defaults enforced by the SDK,
conformance suite and host. Changes are reviewed for correctness, compatibility
and resource effects and update the constants, this document and affected
desktop preflights together. Separate owner ratification of every numeric
value is not required.

## Custody bounds

| Constant | Value | Rationale |
|---|---|---|
| `k_max_candidates_per_root` | 256 | far above any realistic install; a streaming scan retains only the bytewise-first 256 directory/link names and emits one aggregate diagnostic for all omitted entries, bounding selection and diagnostic memory |
| `k_max_admitted_generations` | 64 | one per package; 64 packages per process bounds registry memory |
| `k_max_files_per_package` | 4096 | covers adapters plus helpers, assets, notices and provenance; bundled runtimes with package-manager-scale trees are out of scope |
| `k_max_package_path_segments` | 16 | the package-relative path grammar's existing depth bound; directory enumeration stops before growing a deeper recursion stack |
| `k_max_index_bytes` | 4 MiB | 4096 entries at about 1 KiB worst case each |
| `k_max_file_bytes` | 512 MiB | a statically linked adapter or a vendored CLI binary fits; a larger single file is unreasonable to copy per start |
| `k_max_generation_bytes` | 2 GiB | at most four maximal files; bounds per-start copy time |
| `k_max_aggregate_custody_bytes` | 8 GiB | four maximal packages; protects the profile disk |
| `k_max_bootstrap_bytes` | 64 KiB | the bootstrap is a few hundred bytes; 64 KiB leaves room for 64 argv strings |
| `k_max_sidecar_bytes` | 256 KiB | capability declarations are small; bounds validator memory |
| `k_max_sidecars_per_package` | 64 | 16 capability families at 4 versions each |

## Protocol limits

| Constant | Value | Rationale |
|---|---|---|
| `k_max_frame_bytes` | 4 MiB | the largest payload bound of Logonomic's in-process provider transport (2 MiB) plus envelope headroom |
| `k_max_json_depth` | 32 | capability payloads are shallow; 32 defeats stack abuse |
| `k_max_json_members` | 1024 per object | environment maps and file lists stay well below |
| `k_max_json_string_bytes` | 1 MiB | matches the environment-value bound of Logonomic's in-process provider transport |
| `k_stderr_ring_bytes` | 64 KiB | enough tail to diagnose a crash; never blocks |
| `k_max_stdin_queue_bytes` | 16 MiB | four maximal frames queued to a slow adapter before host-side backpressure |
| `k_max_in_flight_requests` | 64 | equals the admitted-request bound of Logonomic's in-process provider transport; one shared value |
| `k_unlimited_progress_frames` | INT64_MAX | sentinel for no lifetime progress limit in the existing hello field; finite older-peer values remain negotiated limits. Runtime pacing and one pending coalesced update bound active work without eventual exhaustion |
| `k_max_handshake_capabilities` | 256 | 16 families at 16 versions each |
| `k_max_progress_note_bytes` | 256 | one line of UI text |
| `k_max_result_message_bytes` | 1024 | matches the compatibility-warning message bound of Logonomic's in-process provider transport |
| `k_max_argv_entries` / `k_max_argv_entry_bytes` / `k_max_argv_total_bytes` | 64 / 4096 / 11 KiB | a composed executable plus fully quoted argv remains below the native Windows command-line ceiling; the assembled UTF-16 line is still preflighted |
| `k_max_environment_entries` / `k_max_capability_environment_entries` | 16 (manifest) / 64 (capability-scoped) | a manifest fixes a few variables; a capability may request more |
| `k_max_environment_name_bytes` / `k_max_environment_value_bytes` | 128 / 32766 | 128 matches the environment-name bound of Logonomic's in-process provider transport; 32766 UTF-8 bytes conservatively fit Windows' 32767-unit value limit including its terminating NUL |
| `k_max_start_environment_entries` / `k_max_start_environment_bytes` | 34 / 16 KiB | private desktop-to-custodian start: Windows' 18 baseline/PATH slots plus 16 manifest slots; sum of `NAME+VALUE`, excluding `=` |
| `k_max_start_absolute_path_bytes` | 4096 | every absolute path serialized in a private start request; enforced before desktop scratch/lock side effects and before custodian probes |
| `k_max_generation_directory_bytes` / `k_max_adapter_executable_bytes` | 4258 / 4459 | `4096 + 1 + 128 + 1 + 32`, then `+ 1 + 200`; bounds the custodian's derived generation and adapter paths |
| `k_max_reserved_environment_bytes` / `k_max_supervisor_environment_bytes` | 10 KiB / 26658 | seven complete host `NAME=VALUE` reserves, then `16 KiB + 34` `=` bytes + reserve budget; no generic public-capability aggregate is introduced |
| `k_max_windows_native_command_line_units` / `k_max_windows_environment_value_units` | 32766 / 32766 UTF-16 units | both native APIs allow 32767 units including their terminating NUL; production and simulator preflight the post-conversion values and assembled quoted line |
| `k_max_control_message_bytes` (desktop to custodian) | 256 KiB | private encoded start envelope, preflighted before custody channels/custodian spawn and after its real inherited-handle value is resolved |
| `k_max_supervisor_request_bytes` | 320 KiB | private request file and reader/decoder ceiling; separate from the 4 MiB public capability frame |
| `k_max_link_frame_bytes` (desktop to supervisor) | `k_max_frame_bytes` + 4 KiB | one adapter frame plus the link header |

Array lengths and the total number of values in a document have no separate
bound: each value costs at least one byte, so the frame size bounds both.

### Native and composed-envelope proof

`CreateProcessW` and `GetEnvironmentVariableW` have a 32767 UTF-16-unit
limit including the terminating NUL. Every individual contract value is at
most 32766 raw UTF-8 bytes, which is conservative because UTF-8 cannot decode
to more UTF-16 units than input bytes. Both Windows spawn implementations
nevertheless convert first and reject an over-limit native value. They also
quote the complete native argv and reject a line over 32766 units. With the
worst permitted quoted executable (`2 * 4459 + 2`) and manifest argv
(`2 * 11264 + 2 * 64 + 63`), plus their separating space, the command is
at most 31640 units.

The private envelopes have aggregate bounds because the host, unlike a public
capability request, composes ambient environment, manifest values and seven
host values into a smaller private transport. JSON controls expand to six
bytes; argv controls are already forbidden, so only quote expansion applies
there. The serializer oracle constructs every permitted member/count shape and
asserts the following arithmetic, including member names, quotes, colons,
commas, braces and array/object delimiters:

| Envelope | Conservative encoded arithmetic | Ceiling |
|---|---|---|
| start | `327` fixed member syntax + `74173` fixed strings + `22721` quoted argv + `98304` environment object + `49` generations + `19` inherited handle + `27` limit digits = `195620` | 256 KiB |
| supervisor | `164` fixed member syntax + `26756` executable + `49478` argv + `148883` complete environment + `25550` working directory + `27` limit digits = `250858` | 320 KiB |

For the start proof, the three 4096-byte paths and unreserved values are
sixfold escaped; its actual valid absolute-path fixture is smaller only by
the three required leading separators. The supervisor calculation includes
the executable both as `adapter_executable` and `argv[0]`, generation root
both as working directory and `LOGONOMIC_ADDON_PACKAGE_ROOT`, and scratch root
as `LOGONOMIC_ADDON_SCRATCH_DIR`. A generic 41-entry supervisor environment at
its advertised 26658-byte raw budget also serializes below 263 KiB, leaving a
separate margin below 320 KiB. The control channel requests a complete record
plus 4 KiB conservative Linux `SOCK_SEQPACKET` headroom, verifies the effective
post-clamp/doubling socket buffers, and has a below/at/above transfer oracle
on both supported channel implementations.

## Grammar bounds

| Constant | Value |
|---|---|
| `k_max_platforms_per_package` | 8 |
| `k_max_core_generations_per_package` | 8 |
| `k_max_release_version_bytes` | 64 |
| `k_max_version` | 65535 |

## Presentation and SVG bounds

| Constant | Value | Rationale |
|---|---|---|
| `k_max_icon_source_bytes` | 256 KiB | bounds the complete indexed PNG or SVG before parsing |
| `k_min_icon_pixels` / `k_max_icon_pixels` | 16 / 1024 | excludes unusably small raster input and decode-amplification dimensions |
| `k_max_icon_aspect_ratio` | 4 | accepts ordinary marks while rejecting pathological strips |
| `k_canonical_icon_pixels` / `k_max_canonical_icon_bytes` | 64 / 32 KiB | fixes host-generated variant dimensions and encoded-resource storage |
| `k_max_svg_elements` / `k_max_svg_depth` | 1024 / 16 | bounds parser memory and recursive validation |
| `k_max_svg_attributes_per_element` | 32 | bounds per-element validation and canonicalization |
| `k_max_svg_path_data_bytes` / `k_max_svg_path_segments` | 32 KiB / 4096 | bounds path and points parsing independent of the document ceiling |
| `k_max_svg_attribute_bytes` / `k_max_svg_id_bytes` | 1024 / 64 | bounds diagnostics, maps and non-path value validation |
| `k_max_svg_transforms` | 8 per list | bounds transform parsing and renderer work |
| `k_max_svg_reference_chain` / `k_max_svg_expanded_elements` | 4 / 4096 | bounds local gradient/use traversal and repeated-use expansion |
| `k_max_display_name_scalars` / `k_max_display_name_bytes` | 40 / 120 | accommodates a compact Unicode product name while bounding display/resource input |

## Account bounds

| Constant | Value | Rationale |
|---|---|---|
| `k_max_adapter_account_key_bytes` | 256 | accommodates opaque vendor identities while bounding durable selection records |
| `k_max_discovered_accounts` | 64 | bounds inventory reconciliation and accounts response size |

Account labels reuse the presentation display-name bound; real profile paths
reuse `k_max_start_absolute_path_bytes`.

## Launcher bounds

| Constant | Value | Rationale |
|---|---|---|
| `k_max_discovery_environment_names` | 32 | bounds the sidecar's ambient-environment authority |
| `k_max_discovery_candidates` | 16 | bounds one discovery response and host reconciliation work |
| `k_max_script_body_bytes` / `k_max_environment_text_bytes` | 4096 / 4096 | preserves practical interactive launchers while bounding comparison and transport |
| `k_max_working_directory_bytes` | 4096 | covers native absolute paths |
| `k_max_title_instruction_bytes` | 4096 | preserves the complete host instruction within the existing launch script and environment-text budget; the former 256-byte ceiling rejected the production 316-byte title instruction |
| `k_max_initial_prompt_text_bytes` | 1024 | the one-line interactive-initial-prompt@1 text, which names a host-staged file; a quarter of one replacement argv entry, so the launcher's own arguments and title guidance keep room within the argv bounds |
| `k_max_evidence_bytes` | 2048 canonical bytes | evidence is diagnostic, not an auxiliary data channel |
| `k_max_launch_patch_entries` | 64 each for `set` and `unset` | equals the capability-scoped environment-entry ceiling |
| `k_max_asset_tokens_per_value` | 16 | bounds substitution scanning and materialization references |
| `k_max_managed_assets` | 64 | bounds one declaration and materialization transaction |
| `k_max_managed_asset_bytes` / `k_max_managed_assets_bytes` | 4 MiB / 16 MiB | permits configuration/templates while excluding bundled runtimes from inert assets |

## Session bounds

structured-session@3, delegated-session@3 and interactive-resume@2
(`structured_session_3.md` and siblings). Where a value equals a bound the
desktop enforces on its own structured runtime or broker, the desktop links
the constant from here.

| Constant | Value | Rationale |
|---|---|---|
| `k_max_structured_purposes` | 2 | the current two-purpose vocabulary |
| `k_max_structured_prompt_bytes` | 48 KiB | the desktop's structured prompt bound |
| `k_max_structured_stream_bytes` | 1 MiB | the structured runtime's output bound for chunked session-exchange@2 streams |
| `k_max_structured_stderr_bytes` | 64 KiB | the structured runtime's stderr bound |
| `k_max_protocol_state_bytes` | 64 KiB | the broker's protocol-state bound |
| `k_max_exchange_protocol_state_code_units` | 2 Mi UTF-16 units | the version-2 continuation bound after UTF-8 and NUL validation |
| `k_max_exchange_protocol_state_utf8_bytes` | 6 MiB | three UTF-8 bytes per UTF-16 unit; shared continuation parsing uses this byte and string allowance while retaining ordinary JSON depth and member limits |
| `k_max_presentation_records_per_inspection` | 16 | six kinds with counts; 16 allows ordered repeats within one inspection |
| `k_max_delegated_task_servers` | 8 | coordination, delegation and a handful of vendor servers |
| `k_max_delegated_task_id_bytes` | 128 | the desktop's delegation task-id bound |
| `k_max_delegated_server_id_bytes` / `k_max_delegated_server_actions` | 64 / 64 | one identifier bound; the action list shares the capability environment's entry ceiling |
| `k_max_delegated_private_artifacts` | 8 | the broker's private-artifact count |
| `k_max_delegated_private_artifact_bytes` | 1 MiB | the broker's private-artifact size |
| `k_max_delegated_model_bytes` | 256 | one provider-native model identifier in delegated-model-selection@1 |
| `k_max_delegated_reasoning_effort_bytes` | 64 | one provider-native effort setting in delegated-model-selection@1 |
| `k_max_resume_record_bytes` | 16 KiB canonical JSON | bounds the opaque package-owned resume record |

## Lifecycle, ingress and helper-client bounds

lifecycle-events@2, helper-ingress@2/@3 and `helper_client.h`
(`lifecycle_events_2.md`, `helper_ingress_2.md`). Helper observations retain
160-byte agent labels. Continuous observations admit agent labels of at most
768 UTF-8 bytes and 256 UTF-16 code units; other labels retain the common bound.

| Constant | Value | Rationale |
|---|---|---|
| `k_max_lifecycle_identity_bytes` / `k_max_lifecycle_label_bytes` | 256 / 160 | the broker's lifecycle identity and label bounds |
| `k_max_lifecycle_profile_id_bytes` | 64 | one identifier |
| `k_max_observations_per_result` | 16 | 16 observations of at most 2 KiB stay far below the frame bound and below the 60 KiB control payload when relayed |
| `k_max_observe_cursor_bytes` | 256 | an opaque position, one identifier bound |
| `k_max_provider_event_name_bytes` | 256 | one identifier bound |
| `k_max_provider_event_bytes` | 48 KiB host-transport compact JSON | measured on the actual compact `event.body` bytes at the Logonomic wrapper/host bridge, before router dispatch; this leaves envelope headroom below the framework's 64 KiB encoded-argument ceiling, and the portable SDK has no Qt serializer and does not reject by its different canonical byte count |
| `k_provider_event_input_max_bytes` (wrapper-owned raw stdin) | `k_max_control_message_bytes` (256 KiB) | independent pre-parse resource ceiling; refusal is exit 2 with no structured reason, while the parsed body still has its separate 48 KiB host-transport compact limit |
| `k_max_helper_response_bytes` | 32 KiB host-transport compact JSON | measured by the host before outcome commit and journal accounting; the portable adapter codec does not decide this limit from SDK-canonical numeric spelling |
| `k_max_idempotency_key_bytes` | 256 | the desktop's delivery-id bound |
| `k_transition_nonce_hex_chars` | 32 | 128 bits, the generation-nonce length |
| `k_session_capability_hex_chars` | 64 | the desktop's session-capability text size |
| `k_ingress_journal_entries_per_session` | 256 | equals the desktop's lifecycle history limit |
| `k_ingress_journal_bytes_per_session` | 1 MiB | 256 entries at an average of at most 4 KiB of key plus outcome |
| `k_ingress_retired_transition_limit` | 128 | equals the desktop's retired-session limit |
| `k_max_helper_client_stdout_bytes` | 36 KiB | `k_max_helper_response_bytes` plus a 4 KiB envelope for the committed outcome's other members |

## Model catalog

These bounds belong to optional `model-catalog@1`. They do not expand helper
ingress, lifecycle or protocol limits. Producers must leave space for their
native transport framing and report `total_count` and `truncated` truthfully.

| Constant | Value | Rationale |
|---|---|---|
| `k_max_model_catalog_entries` | 128 | Bounds the atomic live registry projection retained by one session. |
| `k_max_model_catalog_snapshot_bytes` | 28 KiB | Bounds canonical snapshot bytes while leaving helper framing space; a provider may need a tighter native-input budget. |

## Deadlines

The capability-local bounds in `session_wake.h` are
`k_max_session_wake_binding_bytes` (16384 canonical JSON bytes),
`k_max_session_wake_identity_bytes` (256 UTF-8 bytes for native identities,
notification IDs and receipts), and `k_max_session_wake_instruction_bytes`
(4096 UTF-8 bytes for a host mailbox reminder). The common argv, directory,
environment and transition-nonce bounds remain unchanged.

| Constant | Value (ms) | Rationale |
|---|---|---|
| `k_custodian_start_deadline_ms` | 2000 | spawn plus the `started` report |
| `k_hello_deadline_ms` | 5000 | equals the provider start deadline of Logonomic's in-process provider transport |
| `k_activation_deadline_ms` | 10000 | custodian, supervisor and adapter handshake end to end |
| `k_default_invoke_deadline_ms` | 5000 | equals the default request deadline of Logonomic's in-process provider transport; capabilities may pass longer per request |
| `k_no_invoke_deadline_ms` | 2147483647 | sentinel for no request timer; results, cancellation, shutdown and generation failure retain their normal behavior |
| `k_cancel_grace_ms` | 2000 | time for a conforming adapter to emit `cancelled` before retirement |
| `k_shutdown_grace_ms` | 5000 | matches the provider-framework unload reservation of Logonomic's in-process provider transport |
| `k_terminate_to_kill_ms` | 2000 | SIGTERM or process termination to SIGKILL or Job termination |
| `k_reap_deadline_ms` | 5000 | the custodian confirms the tree is gone before exiting |
| `k_delegated_prepare_deadline_ms` | 45000 | preparation may write artifacts and start a vendor CLI that extracts its native executable on first use |
| `k_structured_prepare_deadline_ms` | 45000 | structured preparation may start the vendor CLI to verify its native capabilities |
| `k_resume_prepare_deadline_ms` | 45000 | resume preparation may probe the vendor installation before acknowledging the exact conversation |
| `k_launch_prepare_deadline_ms` | 45000 | interactive launch preparation may probe the vendor installation before proposing its invocation |
| `k_resume_inspect_deadline_ms` | 5000 | read-only exact-conversation availability inspection |
| `k_observe_window_ms` | 60000 | a dead continuous adapter is detected within a minute; a window is one bounded request |
| `k_observe_window_grace_ms` | 5000 | equals `k_default_invoke_deadline_ms`; the result must arrive within one default deadline after the window |
| `k_classify_event_deadline_ms` | 2000 | leaves 1 s of the relay's 3 s forward timeout for transport |
| `k_provider_event_client_deadline_ms` | 5000 | the classify deadline plus queueing behind the desktop's manager thread |
| `k_provider_event_poll_interval_ms` | 100 | one core `vnmctl` spawn per poll; 100 ms bounds the spawn rate |
| `k_helper_client_wrapper_deadline_ms` | 10000 | the wrapper's client deadline plus one default deadline for its own spawn and exit |
| `k_ingress_response_ttl_ms` | 60000 | a helper retries within the relay's worst case; a minute covers a stalled vendor hook runner |
| `k_ingress_teardown_drain_ms` | 5000 | equals `k_shutdown_grace_ms` |

## Literals

| Constant | Value |
|---|---|
| `k_stderr_truncation_marker` | `…[logonomic: stderr truncated]…` |
