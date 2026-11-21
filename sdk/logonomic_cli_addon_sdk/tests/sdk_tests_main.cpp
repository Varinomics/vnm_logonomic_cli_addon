// logonomic_cli_addon_sdk_tests [--fixture-packages <dir>] [--scripts <dir>]
//                              [--exclude-group <group>] [<group>...]
//
// With no group filters, every group runs. Failures are reported
// on stderr, and the exit code is the count of failed groups capped at 1.
// The fixture groups need the assembled fixture packages and the transcript
// scripts; without the two directories they fail rather than skip.

#include "sdk_test_support.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

int run_stream_flood_child()
{
    (void)std::setvbuf(stdout, nullptr, _IONBF, 0);
    (void)std::setvbuf(stderr, nullptr, _IONBF, 0);
    const std::array<char, 65536> stdout_chunk = [] {
        std::array<char, 65536> bytes{};
        bytes.fill('o');
        return bytes;
    }();
    const std::array<char, 65536> stderr_chunk = [] {
        std::array<char, 65536> bytes{};
        bytes.fill('e');
        return bytes;
    }();
    std::thread stdout_writer([&stdout_chunk]() {
        while (std::fwrite(stdout_chunk.data(), 1, stdout_chunk.size(), stdout) == stdout_chunk.size()) {
            // Continue until the supervisor closes the stream or terminates us.
        }
    });
    while (std::fwrite(stderr_chunk.data(), 1, stderr_chunk.size(), stderr) == stderr_chunk.size()) {
        // Continue until the supervisor closes the stream or terminates us.
    }
    stdout_writer.join();
    return 0;
}

int run_native_preflight_child()
{
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    std::error_code path_error;
    sdk_tests::set_test_executable(std::filesystem::absolute(argv[0], path_error));
    if (argc == 2 && std::strcmp(argv[1], "--stream-flood-child") == 0) {
        return run_stream_flood_child();
    }
    if (argc >= 2 && std::strcmp(argv[1], "--native-preflight-child") == 0) {
        return run_native_preflight_child();
    }
    if (argc >= 2 && sdk_tests::is_fake_vnmctl_subcommand(argv[1])) {
        return sdk_tests::run_fake_vnmctl(argc, argv);
    }

    std::vector<std::string> selected;
    std::vector<std::string> excluded;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--fixture-packages" && i + 1 < argc) {
            sdk_tests::set_fixture_packages_directory(argv[++i]);
        }
        else
        if (argument == "--scripts" && i + 1 < argc) {
            sdk_tests::set_scripts_directory(argv[++i]);
        }
        else
        if (argument == "--exclude-group" && i + 1 < argc) {
            excluded.push_back(argv[++i]);
        }
        else {
            selected.push_back(argument);
        }
    }

    const struct
    {
        const char* name;
        bool (*function)();
    }
    groups[] = {
        {"quota_failure_compatibility",             sdk_tests::test_quota_failure_compatibility},
        {"quota_cost_contracts",                    sdk_tests::test_quota_cost_contracts},
        {"auth_quota_contracts",                    sdk_tests::test_auth_quota_contracts},
        {"account_refresh_contracts",               sdk_tests::test_account_refresh_contracts},
        {"conversation_export_contracts",           sdk_tests::test_conversation_export_contracts},
        {"usage_scan_contracts",                    sdk_tests::test_usage_scan_contracts},
        {"usage_session_contracts",                 sdk_tests::test_usage_session_contracts},
        {"usage_repository_attribution_contracts",   sdk_tests::test_usage_repository_attribution_contracts},
        {"usage_owned_event_correction_contracts",    sdk_tests::test_usage_owned_event_correction_contracts},
        {"observation_fixture_subsets",             sdk_tests::test_observation_fixture_subsets},

        // Codec and identifiers: oracles are RFC 8259, RFC 3629 and the grammars.
        {"json_accepts_rfc8259_documents",          sdk_tests::test_json_accepts_rfc8259_documents},
        {"json_rejects_rfc8259_violations",         sdk_tests::test_json_rejects_rfc8259_violations},
        {"json_utf8_follows_rfc3629",               sdk_tests::test_json_utf8_follows_rfc3629},
        {"json_bounds_and_duplicates",              sdk_tests::test_json_bounds_and_duplicates},
        {"json_numbers_are_exact",                  sdk_tests::test_json_numbers_are_exact},
        {"json_serialization_forms",                sdk_tests::test_json_serialization_forms},
        {"json_utilities",                          sdk_tests::test_json_utilities},
        {"identifier_grammars",                     sdk_tests::test_identifier_grammars},

        // Digests: FIPS 180-4 vectors and the generation digest definition.
        {"sha256_fips_vectors",                     sdk_tests::test_sha256_fips_vectors},
        {"generation_digest_definition",            sdk_tests::test_generation_digest_definition},

        // Package contract.
        {"validator_accepts_a_conforming_package",  sdk_tests::test_validator_accepts_a_conforming_package},
        {"validator_reports_each_defect",           sdk_tests::test_validator_reports_each_defect},
        {"package_source_listing_bounds",           sdk_tests::test_package_source_listing_bounds},
        {"file_time_ticks_text",                    sdk_tests::test_file_time_ticks_text},
        {"index_parser_static_rules",               sdk_tests::test_index_parser_static_rules},

        // Transport and protocol.
        {"frame_decoder",                           sdk_tests::test_frame_decoder},
        {"stderr_ring",                             sdk_tests::test_stderr_ring},
        {"protocol_round_trips",                    sdk_tests::test_protocol_round_trips},
        {"protocol_strict_envelopes",               sdk_tests::test_protocol_strict_envelopes},
        {"session_tracker_rules",                   sdk_tests::test_session_tracker_rules},
        {"process_proposal_policy",                 sdk_tests::test_process_proposal_policy},
        {"transcript_normalization",                sdk_tests::test_transcript_normalization},
        {"adapter_environment_contract",            sdk_tests::test_adapter_environment_contract},
        {"simulator_stream_backpressure",           sdk_tests::test_simulator_stream_backpressure},
        {"simulator_darwin_retains_wait_until_cleanup", sdk_tests::test_simulator_darwin_retains_wait_until_cleanup},
        {"macos_adapter_executable_format",         sdk_tests::test_macos_adapter_executable_format},
        {"simulator_windows_native_limits",         sdk_tests::test_simulator_windows_native_limits},

        // Provider capabilities and presentation.
        // Their numeric bounds remain pending explicit owner ratification.
        {"accounts_contracts",                      sdk_tests::test_accounts_contracts},
        {"account_pet_contracts",                   sdk_tests::test_account_pet_contracts},
        {"account_display_names",                   sdk_tests::test_account_display_names},
        {"environment_bootstrap_contracts", sdk_tests::test_environment_bootstrap_contracts},
        {"startup_observer_contracts",              sdk_tests::test_startup_observer_contracts},
        {"session_observation_contracts",           sdk_tests::test_session_observation_contracts},
        {"lifecycle_correlation_contracts",         sdk_tests::test_lifecycle_correlation_contracts},
        {"lifecycle_correlation_lifetime_contracts", sdk_tests::test_lifecycle_correlation_lifetime_contracts},
        {"managed_configuration_contracts",         sdk_tests::test_managed_configuration_contracts},
        {"permission_snapshot_contracts",           sdk_tests::test_permission_snapshot_contracts},
        {"launcher_capability_contracts",           sdk_tests::test_launcher_capability_contracts},
        {"launcher_approval_contracts",             sdk_tests::test_launcher_approval_contracts},
        {"interactive_initial_prompt_contracts",    sdk_tests::test_interactive_initial_prompt_contracts},
        {"terminal_codex_invocation_contracts",     sdk_tests::test_terminal_codex_invocation_contracts},
        {"session_wake_contracts",                  sdk_tests::test_session_wake_contracts},
        {"terminal_identity_contracts",             sdk_tests::test_terminal_identity_contracts},
        {"presentation_media_contracts",            sdk_tests::test_presentation_media_contracts},
        {"assets_import_and_storage_contracts",     sdk_tests::test_assets_import_and_storage_contracts},

        // The helper-client contract uses this executable as its scripted wrapper.
        {"session_contracts",                       sdk_tests::test_session_contracts},
        {"delegated_model_selection_contracts",      sdk_tests::test_delegated_model_selection_contracts},
        {"resume_inspection_contracts",             sdk_tests::test_resume_inspection_contracts},
        {"lifecycle_and_ingress_contracts",         sdk_tests::test_lifecycle_and_ingress_contracts},
        {"provider_event_diagnostic_contracts",     sdk_tests::test_provider_event_diagnostic_contracts},
        {"context_state_contracts",                 sdk_tests::test_context_state_contracts},
        {"execution_context_contracts",             sdk_tests::test_execution_context_contracts},
        {"model_catalog_contracts",                 sdk_tests::test_model_catalog_contracts},
        {"cooperative_description_contracts",      sdk_tests::test_cooperative_description_contracts},
        {"model_state_contracts",                   sdk_tests::test_model_state_contracts},
        {"child_state_contracts",                   sdk_tests::test_child_state_contracts},
        {"helper_client_contract",                  sdk_tests::test_helper_client_contract},

        // Fixture packages through the host simulator.
        {"fixture_positive_package",                sdk_tests::test_fixture_positive_package},
        {"adapter_runtime_worker_lifetime",          sdk_tests::test_adapter_runtime_worker_lifetime},
        {"adapter_runtime_payload_lifetime",         sdk_tests::test_adapter_runtime_payload_lifetime},
        {"fixture_package_file_variants",           sdk_tests::test_fixture_package_file_variants},
        {"fixture_behavior_variants",               sdk_tests::test_fixture_behavior_variants},
        {"fixture_paced_progress_lifetime",         sdk_tests::test_fixture_paced_progress_lifetime},
        {"fixture_transcripts",                     sdk_tests::test_fixture_transcripts},
    };

    std::vector<std::string> requested = selected;
    requested.insert(requested.end(), excluded.begin(), excluded.end());
    for (const std::string& name : requested) {
        bool known = false;
        for (const auto& group : groups) {
            known = known || name == group.name;
        }
        if (!known) {
            std::fprintf(stderr, "unknown SDK test group: %s\n", name.c_str());
            return 1;
        }
    }

    int failed = 0;
    int ran    = 0;
    for (const auto& group : groups) {
        if (std::find(excluded.begin(), excluded.end(), group.name) != excluded.end()) {
            continue;
        }
        if (!selected.empty()) {
            bool wanted = false;
            for (const std::string& name : selected) {
                wanted = wanted || name == group.name;
            }
            if (!wanted) {
                continue;
            }
        }
        ++ran;
        if (!group.function()) {
            std::fprintf(stderr, "group failed: %s\n", group.name);
            ++failed;
        }
    }

    if (failed != 0) {
        std::fprintf(stderr, "%d of %d SDK test group(s) failed\n", failed, ran);
        return 1;
    }
    std::fprintf(stdout, "%d SDK test group(s) passed\n", ran);
    return 0;
}
