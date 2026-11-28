# CLI add-on contracts

A capability describes one thing a provider can do for Logonomic, such as
recognize a launcher or prepare a conversation for resume. Each contract
defines what the host supplies, how the adapter responds and which other
capabilities it needs. Start with the [SDK guide](../README.md) for the
architecture or [build DeskTool](../examples/desktool/README.md) to see the
launcher contracts in use.

## Contracts every adapter uses

| Question | Read |
| --- | --- |
| What files go in the package, and what does each declaration mean? | [Package layout, integrity index and capability sidecars](package_contract.md#package-layout-and-authority) |
| How does an adapter start, where may it write, and when can it stop? | [Process and environment](process_contract.md) |
| How do the host and adapter agree on versions, exchange messages and cancel an invocation? | [Core protocol](core_protocol.md) and [protocol limits](limits.md) |
| How do I check my implementation? | [Build and package tools](sdk_tools.md), [conformance checks and simulator transcripts](conformance.md), and [staging prebuilt artifacts](artifact_staging.md) |

## Find a capability by task

**Launch a CLI.** Implement [launcher discovery and recognition](launcher_discovery_1.md)
to find installed commands and recognize saved recipes. Use
[launch preparation](launch_preparation_3.md) to describe how the host should
start one. [Presentation](presentation_1.md) supplies the name, icons and
other display data. [SVG rules](svg_subset.md) specify the supported icon
format. [Launcher approval](launcher_approval_1.md) covers changes to saved
approval arguments.

**Select an account and prepare its environment.** Start with
[accounts](accounts_2.md). Further contracts cover [profile storage](profile_storage_1.md),
[environment bootstrap](environment_bootstrap_1.md), [probe environments](probe_environment_2.md),
[managed assets](managed_assets_1.md) and managed configuration
([version 1](managed_configuration_1.md), [version 2](managed_configuration_2.md),
[version 3](managed_configuration_3.md)). A package can declare
[account-specific managed configuration](account_profile_managed_configuration_1.md)
and an [account pet](account_pet_1.md).

**Report sign-in and quota.** The [authentication](auth_status_2.md) and
[current-account](current_account_1.md) contracts identify the observed
sign-in state. See [account refresh](account_refresh_1.md) and quota
([version 2](quota_2.md), [version 3](quota_3.md)) for fresh observations,
with optional [cost](quota_cost_1.md) and [failure details](quota_failure_1.md).

**Start, resume or export a conversation.** See [interactive initial prompts](interactive_initial_prompt_1.md),
[resume preparation](interactive_resume_2.md), [availability inspection](interactive_resume_3.md),
[content inspection](interactive_resume_4.md), [resume directories](resume_directory_2.md)
and [startup observation](startup_observer_1.md). Conversation export covers
[rendering a conversation](conversation_export_1.md) and
[recording its prepared source](conversation_export_2.md).

**Run structured or delegated tasks.** Read [structured sessions](structured_session_3.md),
[text-only authoring](authoring_session_1.md),
[structured failure diagnostics](structured_failure_diagnostics_1.md),
[delegated sessions](delegated_session_3.md) and [per-task model selection](delegated_model_selection_1.md).
[Coordination delivery](coordination_delivery_1.md) defines how a provider
delivers coordination context. [Session wake](session_wake_1.md) notifies
a running conversation of new messages.

**Observe a live session.** See [lifecycle events](lifecycle_events_2.md),
[correlation lifetime](lifecycle_correlation_lifetime_1.md) and helper ingress
([version 2](helper_ingress_2.md), [version 3](helper_ingress_3.md)). Related
contracts cover [terminal identity](terminal_identity_1.md),
[child state](child_state_1.md), [observed sessions](session_observation_1.md),
[execution context](execution_context_1.md) and [cooperative descriptions](cooperative_description_1.md).
For providers that watch native files, the SDK includes a separate
[filesystem lifecycle observer](filesystem_lifecycle_observer.md).
[Terminal Codex invocation](terminal_codex_invocation_1.md) covers the
terminal's graphics settings for a verified Codex command.

**Report attention, model and context.** Read [attention kinds](attention_kind_1.md),
[asynchronous questions](async_question_attention_1.md), [permission dialogs](permission_snapshot_1.md),
[model catalogs](model_catalog_1.md), [observed model state](model_state_1.md) and
[context occupancy](context_state_2.md). Model-context delivery has
[version 1](model_context_delivery_1.md) and [version 2](model_context_delivery_2.md)
contracts.

**Report token usage.** Start with the [usage scan](usage_scan_3.md). Packages
can add [incremental accounting](usage_incremental_1.md), [session identity](usage_session_1.md),
[event timing](usage_temporal_1.md), [cross-source evidence](usage_cross_source_event_1.md),
[owned event corrections](usage_owned_event_correction_1.md) or
[repository attribution](usage_repository_attribution_1.md). Providers with
saved summaries can report [lifetime totals](usage_lifetime_summary_1.md) and
[daily tokens](usage_saved_daily_tokens_1.md). The [stored accounting format](stored_usage_accounting.md)
documents the provider-side representation used by several adapters.

For a C++ implementation, follow a contract alongside the matching codec in the
[public headers](../include/logonomic_cli_addon/). For a change to a shipped
adapter, use the [Copilot contribution example](provider_contribution.md).
