# delegated-model-selection@1

This optional capability adds per-task model and reasoning-effort selection to
[delegated-session@3](delegated_session_3.md). It does not change the common
protocol, session contract version, selected launcher or account. A request
without selection preserves the launcher's saved defaults and carries no new
request or result member.

## Declaration

`capabilities/delegated-model-selection@1.json` contains the ordinary sidecar
envelope with this payload:

```json
{"model": true, "reasoning_effort": true}
```

Both flags are required booleans and at least one is true. Each flag declares
that the provider can carry that setting into its native delegated invocation.
It is not a list of available models or an assertion that every model accepts
every effort. Values use the selected provider's native spelling; the SDK has
no model inventory or shared effort vocabulary.

The sidecar, adapter handshake and host adoption must intersect on version 1,
with delegated-session@3 live in the same generation. Missing, unsupported or
invalid selection capability data makes selection unavailable while ordinary
delegation and other capabilities retain their own availability.

## Request and acknowledgment

When selection is requested, the delegated `prepare` payload additionally has:

```json
"model_selection": {"version": 1, "model": "gpt-6-astra", "reasoning_effort": "xhigh"}
```

Either setting may be absent, but at least one must be present. Present values
are nonempty UTF-8 strings without NUL, bounded by
`k_max_delegated_model_bytes` (256 bytes) and
`k_max_delegated_reasoning_effort_bytes` (64 bytes). No trimming, case folding,
translation or default substitution is performed by the SDK. An unknown record
version refuses the selection request with `unsupported_version`.

The host calls `validate_delegated_model_selection` against the live declaration
before requesting preparation. Missing capability or unsupported model selection
returns `model_selection_unavailable`; unsupported effort selection returns
`reasoning_effort_unavailable`. The adapter must also refuse native settings it
cannot honor. Native rejection of an unavailable model or effort remains a
failed task; it must never retry with saved defaults or a substituted setting.

The provider applies requested values only to the process being prepared,
overriding the corresponding saved launcher or account default. It preserves
all unrequested settings and the selected account, profile and purpose. Model
selection never grants additional permissions or changes read-only review into
implementation.

Successful preparation echoes the same `model_selection` record at the top
level of its result. `validate_delegated_model_selection_acknowledgment`
requires exact equality of both optional values. Missing, partial, substituted
or unsolicited acknowledgment fails with `acknowledgment_mismatch` before
launch. The echo acknowledges the provider's native invocation configuration;
it does not prove model-service availability or task completion.
