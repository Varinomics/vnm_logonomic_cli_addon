# authoring-session@1

An adapter prepares one native model turn that can read the host's prompt and
return text. The host owns the process and any subsequent decision to apply
the text to a document. A host can include earlier conversation turns and the
current document in a later prompt. Each invocation has its own inspection
state.

This capability is optional. A package declares and advertises it separately
from structured sessions, and the host must adopt the same version. Each
package can support the launcher configurations and native versions for which
it can enforce text-only authoring.

## Declaration

```json
{"capability_id":"authoring-session","version":1,"payload":{"environment_names":[]}}
```

The sidecar payload uses the [launcher environment declaration](launcher_discovery_1.md#sidecar-payload).
`decode_launcher_discovery_declaration` validates its environment names.

## Prepare

Use [structured preparation](structured_session_3.md#prepare) with the fixed
wire value `"purpose":"read_only_review"`. The capability ID selects the
authoring operation. An `implementation` request is
`unavailable`/`purpose_unavailable`. Adapters can use
`decode_authoring_prepare_request` to validate the request. All process,
environment, prompt, bounds and deadline rules of the structured exchange
apply, including validation of the proposal against the frozen launcher.

The model can only produce text from the context supplied for this turn.
The adapter must select a native mode that prevents the model from using
tools to read or write files, execute commands, access external services,
or delegate work. That boundary covers tools loaded from native settings,
extensions and external tool servers, as well as built-in tools. Native
authentication, model inference and transport use the selected launcher's
ordinary facilities. An instruction in the prompt cannot enforce this rule.

Preparation declines a launcher, version or configuration for which the
adapter cannot enforce that boundary, including conflicting launcher options.
An attempted native tool or permission request cannot grant authority.
The adapter must not switch to an implementation run to complete the turn.

An account-selected turn uses the existing [account context](accounts_2.md).
The process proposal and its inspection setup use the ordinary structured
prepare result. Existing exact-acknowledgment rules apply to account and
profile-storage contexts.

## Inspect

Use [structured inspection](structured_session_3.md#inspect) on this same
capability. The purpose remains `read_only_review`; `implementation` is
`unavailable`/`purpose_unavailable`. Adapters can use
`decode_authoring_inspect_request` and the structured inspection result
encoder. Chunk boundaries, final exit facts, continuation, cancellation and
failure reasons retain their structured meanings.

A successful terminal result contains the authenticated assistant response
as UTF-8 text. Native protocol frames, status and reasoning belong to the
adapter's inspection. Optional presentation records convey progress. The host
only interprets an authoring result after successful completion. Validation
and application of any proposed document changes belong to the host.
