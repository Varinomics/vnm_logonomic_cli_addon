#pragma once

#include <cstdint>

namespace logonomic::cli_addon {

inline constexpr const char* k_accounts_capability_id = "accounts";
inline constexpr std::uint16_t k_accounts_launcher_version = 2;
inline constexpr const char* k_account_pet_capability_id = "account-pet";
inline constexpr std::uint16_t k_account_pet_version = 1;

inline constexpr const char* k_auth_status_capability_id = "auth-status";
inline constexpr const char* k_quota_capability_id       = "quota";
inline constexpr const char* k_quota_failure_capability_id = "quota-failure";
inline constexpr std::uint16_t k_quota_failure_version = 1;
inline constexpr const char* k_quota_cost_capability_id = "quota-cost";
inline constexpr std::uint16_t k_quota_cost_version = 1;
inline constexpr const char* k_current_account_capability_id = "current-account";
inline constexpr const char* k_account_refresh_capability_id = "account-refresh";
inline constexpr std::uint16_t k_account_refresh_version = 1;
inline constexpr std::uint16_t k_current_account_version = 1;
inline constexpr const char* k_usage_scan_capability_id = "usage-scan";
inline constexpr const char* k_usage_incremental_capability_id = "usage-incremental";
inline constexpr std::uint16_t k_usage_incremental_version = 1;
inline constexpr const char* k_usage_repository_attribution_capability_id = "usage-repository-attribution";
inline constexpr std::uint16_t k_usage_repository_attribution_version = 1;
inline constexpr const char* k_usage_owned_event_correction_capability_id = "usage-owned-event-correction";
inline constexpr std::uint16_t k_usage_owned_event_correction_version = 1;
inline constexpr const char* k_usage_lifetime_summary_capability_id = "usage-lifetime-summary";
inline constexpr std::uint16_t k_usage_lifetime_summary_version = 1;
inline constexpr const char* k_usage_saved_daily_tokens_capability_id = "usage-saved-daily-tokens";
inline constexpr std::uint16_t k_usage_saved_daily_tokens_version = 1;
inline constexpr const char* k_usage_cross_source_event_capability_id = "usage-cross-source-event";
inline constexpr std::uint16_t k_usage_cross_source_event_version = 1;
inline constexpr const char* k_usage_temporal_capability_id = "usage-temporal";
inline constexpr std::uint16_t k_usage_temporal_version = 1;
inline constexpr const char* k_usage_session_capability_id = "usage-session";
inline constexpr std::uint16_t k_usage_session_version = 1;

/// Capability contracts implemented by this SDK beyond profile-storage@1.
inline constexpr const char* k_presentation_capability_id        = "presentation";
inline constexpr const char* k_launcher_discovery_capability_id  = "launcher-discovery";
inline constexpr const char* k_launcher_approval_capability_id   = "launcher-approval";
inline constexpr const char* k_launch_preparation_capability_id  = "launch-preparation";
inline constexpr const char* k_managed_assets_capability_id      = "managed-assets";
inline constexpr const char* k_managed_configuration_capability_id = "managed-configuration";
inline constexpr const char* k_account_profile_managed_configuration_capability_id =
    "account-profile-managed-configuration";
inline constexpr const char* k_structured_session_capability_id    = "structured-session";
inline constexpr const char* k_authoring_session_capability_id     = "authoring-session";
inline constexpr std::uint16_t k_authoring_session_version = 1;
inline constexpr const char* k_structured_failure_diagnostics_capability_id = "structured-failure-diagnostics";
inline constexpr std::uint16_t k_structured_failure_diagnostics_version = 1;
inline constexpr const char* k_delegated_session_capability_id     = "delegated-session";
inline constexpr const char* k_delegated_model_selection_capability_id = "delegated-model-selection";
inline constexpr std::uint16_t k_delegated_model_selection_version = 1;
inline constexpr const char* k_interactive_resume_capability_id    = "interactive-resume";
inline constexpr const char* k_lifecycle_events_capability_id      = "lifecycle-events";
inline constexpr const char* k_coordination_delivery_capability_id = "coordination-delivery";
inline constexpr const char* k_environment_bootstrap_capability_id = "environment-bootstrap";
inline constexpr std::uint16_t k_environment_bootstrap_version = 1;
inline constexpr const char* k_lifecycle_correlation_lifetime_capability_id =
    "lifecycle-correlation-lifetime";
inline constexpr std::uint16_t k_lifecycle_correlation_lifetime_version = 1;
inline constexpr const char* k_model_context_delivery_capability_id = "model-context-delivery";
inline constexpr std::uint16_t k_model_context_delivery_version = 1;
inline constexpr std::uint16_t k_model_context_delivery_activation_version = 2;
inline constexpr const char* k_interactive_initial_prompt_capability_id = "interactive-initial-prompt";
inline constexpr std::uint16_t k_interactive_initial_prompt_version = 1;
inline constexpr const char* k_terminal_codex_invocation_capability_id = "terminal-codex-invocation";
inline constexpr std::uint16_t k_terminal_codex_invocation_version = 1;
inline constexpr const char* k_session_wake_capability_id = "session-wake";
inline constexpr std::uint16_t k_session_wake_version = 1;
inline constexpr const char* k_terminal_identity_capability_id = "terminal-identity";
inline constexpr std::uint16_t k_terminal_identity_version = 1;
inline constexpr const char* k_conversation_export_capability_id = "conversation-export";
inline constexpr std::uint16_t k_conversation_export_version = 1;
// Version 2 requires every declared source variable in the prepared ambient
// view. The declaration, export operation, request and result are unchanged.
inline constexpr std::uint16_t k_conversation_export_prepared_source_version = 2;
inline constexpr const char* k_helper_ingress_capability_id        = "helper-ingress";
inline constexpr const char* k_context_state_capability_id         = "context-state";
inline constexpr std::uint16_t k_context_state_capability_version = 2;
inline constexpr const char* k_child_state_capability_id = "child-state";
inline constexpr std::uint16_t k_child_state_version = 1;
inline constexpr const char* k_cooperative_description_capability_id = "cooperative-description";
inline constexpr std::uint16_t k_cooperative_description_version = 1;
inline constexpr const char* k_model_catalog_capability_id = "model-catalog";
inline constexpr std::uint16_t k_model_catalog_version = 1;
inline constexpr const char* k_model_state_capability_id = "model-state";
inline constexpr std::uint16_t k_model_state_version = 1;
inline constexpr const char* k_structured_model_observation_capability_id = "structured-model-observation";
inline constexpr std::uint16_t k_structured_model_observation_version = 1;
inline constexpr const char* k_session_observation_capability_id = "session-observation";
inline constexpr std::uint16_t k_session_observation_version = 1;
inline constexpr const char* k_execution_context_capability_id = "execution-context";
inline constexpr std::uint16_t k_execution_context_version = 1;
inline constexpr const char* k_resume_directory_capability_id = "resume-directory";
inline constexpr std::uint16_t k_resume_directory_version = 2;

inline constexpr std::uint16_t k_presentation_version        = 1;
inline constexpr std::uint16_t k_launcher_discovery_version  = 1;
inline constexpr std::uint16_t k_launcher_approval_version   = 1;
inline constexpr std::uint16_t k_managed_assets_version      = 1;
inline constexpr std::uint16_t k_managed_configuration_version = 1;
inline constexpr std::uint16_t k_managed_configuration_selector_version = 2;
inline constexpr std::uint16_t k_managed_configuration_home_version = 3;
inline constexpr std::uint16_t k_account_profile_managed_configuration_version = 1;
inline constexpr std::uint16_t k_lifecycle_events_version = 2;
inline constexpr std::uint16_t k_coordination_delivery_version = 1;

inline constexpr std::uint16_t k_auth_status_environment_version = 2;
inline constexpr std::uint16_t k_quota_exchange_version          = 2;
inline constexpr std::uint16_t k_quota_collection_version        = 3;
inline constexpr std::uint16_t k_usage_scan_acceptance_version   = 3;
inline constexpr std::uint16_t k_interactive_resume_environment_version = 2;
inline constexpr std::uint16_t k_interactive_resume_inspection_version = 3;
inline constexpr std::uint16_t k_interactive_resume_content_version = 4;

inline constexpr std::uint16_t k_launch_preparation_execution_version = 3;
inline constexpr std::uint16_t k_structured_session_version         = 3;
inline constexpr std::uint16_t k_delegated_session_version          = 3;
inline constexpr std::uint16_t k_helper_ingress_degradation_version = 2;
inline constexpr std::uint16_t k_helper_ingress_subagent_reconciliation_version = 3;

} // namespace logonomic::cli_addon
