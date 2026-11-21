#pragma once

// Shared harness helpers for the SDK test groups. A failed check reports
// through stderr and makes its group return false; nothing aborts.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace sdk_tests {

bool test_usage_session_contracts();
bool test_provider_event_diagnostic_contracts();
bool test_usage_repository_attribution_contracts();
bool test_usage_owned_event_correction_contracts();

/// Prints `FAIL [test]: message` to stderr when the condition is false.
bool check(bool condition, const char* test, const std::string& message);

/// Directory of assembled fixture packages (`--fixture-packages`), empty when
/// not given.
const std::filesystem::path& fixture_packages_directory();

/// Directory of transcript scripts (`--scripts`), empty when not given.
const std::filesystem::path& scripts_directory();

/// Absolute path of this test executable, used by child-process oracles.
const std::filesystem::path& test_executable();

void set_fixture_packages_directory(std::filesystem::path directory);
void set_scripts_directory(std::filesystem::path directory);
void set_test_executable(std::filesystem::path executable);

/// A fresh empty directory below the system temporary directory.
std::filesystem::path make_temp_directory(std::string_view stem);

bool write_file(const std::filesystem::path& path, std::string_view bytes);
std::optional<std::string> read_file(const std::filesystem::path& path);

/// Bytes that satisfy the validator's executable-format check on Linux and
/// the extension rule on Windows.
std::string fake_adapter_bytes();

/// The test executable doubles as the scripted `vnmctl` wrapper the helper
/// client is tested against: main() hands an invocation whose first argument
/// is one of the wrapper's subcommands to run_fake_vnmctl, which answers by
/// the event name, title or paths it receives.
bool is_fake_vnmctl_subcommand(std::string_view argument);
int run_fake_vnmctl(int argc, char** argv);

// Test groups.
bool test_accounts_contracts();
bool test_account_pet_contracts();
bool test_account_display_names();
bool test_startup_observer_contracts();
bool test_session_observation_contracts();
bool test_environment_bootstrap_contracts();
bool test_lifecycle_correlation_contracts();
bool test_lifecycle_correlation_lifetime_contracts();
bool test_managed_configuration_contracts();
bool test_permission_snapshot_contracts();
bool test_quota_failure_compatibility();
bool test_quota_cost_contracts();
bool test_auth_quota_contracts();
bool test_account_refresh_contracts();
bool test_conversation_export_contracts();
bool test_context_state_contracts();
bool test_execution_context_contracts();
bool test_model_catalog_contracts();
bool test_cooperative_description_contracts();
bool test_model_state_contracts();
bool test_child_state_contracts();
bool test_usage_scan_contracts();
bool test_observation_fixture_subsets();
bool test_json_accepts_rfc8259_documents();
bool test_json_rejects_rfc8259_violations();
bool test_json_utf8_follows_rfc3629();
bool test_json_bounds_and_duplicates();
bool test_json_numbers_are_exact();
bool test_json_serialization_forms();
bool test_json_utilities();
bool test_identifier_grammars();
bool test_sha256_fips_vectors();
bool test_generation_digest_definition();
bool test_validator_accepts_a_conforming_package();
bool test_validator_reports_each_defect();
bool test_package_source_listing_bounds();
bool test_file_time_ticks_text();
bool test_index_parser_static_rules();
bool test_frame_decoder();
bool test_stderr_ring();
bool test_protocol_round_trips();
bool test_protocol_strict_envelopes();
bool test_session_tracker_rules();
bool test_process_proposal_policy();
bool test_transcript_normalization();
bool test_adapter_environment_contract();
bool test_simulator_stream_backpressure();
bool test_simulator_darwin_retains_wait_until_cleanup();
bool test_macos_adapter_executable_format();
bool test_simulator_windows_native_limits();
bool test_launcher_capability_contracts();
bool test_launcher_approval_contracts();
bool test_interactive_initial_prompt_contracts();
bool test_terminal_codex_invocation_contracts();
bool test_session_wake_contracts();
bool test_terminal_identity_contracts();
bool test_presentation_media_contracts();
bool test_assets_import_and_storage_contracts();
bool test_session_contracts();
bool test_delegated_model_selection_contracts();
bool test_resume_inspection_contracts();
bool test_lifecycle_and_ingress_contracts();
bool test_helper_client_contract();
bool test_fixture_positive_package();
bool test_adapter_runtime_worker_lifetime();
bool test_adapter_runtime_payload_lifetime();
bool test_fixture_package_file_variants();
bool test_fixture_behavior_variants();
bool test_fixture_paced_progress_lifetime();
bool test_fixture_transcripts();

} // namespace sdk_tests

#define SDK_CHECK(condition, message)                                        \
    do {                                                                     \
        if (!sdk_tests::check((condition), __func__, (message))) {           \
            return false;                                                    \
        }                                                                    \
    } while (false)
