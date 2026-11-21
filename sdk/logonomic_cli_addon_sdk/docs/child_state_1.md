# Child state, version 1

`child-state@1` is an optional companion to accepted lifecycle observations.
Its static sidecar is `{}`. It does not change the helper-ingress version,
admit a child by itself, or infer that a missing child has completed.

The host opts in with `"child_state":{"version":1}` on classify, observe and
poll requests. Results may omit the envelope, or return exactly one of
`updates`, `loss`, or `failure` inside `child_state` with `version:1`.
An invalid optional envelope produces `child_failure` without rejecting an
otherwise valid adjacent lifecycle result. No negotiated capability means no
child envelope is requested or consumed.

## Native observations

An update contains these required fields:

| Field | Meaning |
| --- | --- |
| `provider_session_id` | Native root session, also for descendants |
| `transition_nonce` | Root activation captured when the fact was observed |
| `sequence` | Canonical decimal uint64 string, captured before asynchronous delivery |
| `agent_id` | Exact independently admitted native child identity |
| `parent_agent_id` | Native structural parent; empty for a direct child of the root |
| `state` | `running`, `succeeded`, `failed`, `interrupted`, or `unknown` |
| `source` | Bounded native observation source, without tool arguments or output |

Optional `depth` is a positive uint32: a root child has depth 1, any nonempty
parent has depth greater than 1. Parent and depth are observed structure;
providers must not derive them by parsing names or dotted identifiers.

Optional `description` is at most 512 UTF-8 bytes. LF and tab are allowed; other
ASCII controls are not. `current_tool` is a nonempty name of at most 128 UTF-8
bytes without controls. `progress` is an observed finite fraction from 0 to 1,
not an estimate invented by the adapter. `source` is nonempty and at most 128
UTF-8 bytes without controls. Identities use the lifecycle identity bound.

Every update is a full replacement of that child's descriptive sample. An
omitted optional field clears a prior value. Live tool/progress presentation
is meaningful only for `running`; a terminal sample preserves its observed
outcome rather than being collapsed to a generic stop. An unknown observation
withdraws liveness and never means success. Providers must not promote queued
or preflight-only native task records to children without affirmative execution
evidence. An accepted start or stop remains the lifecycle authority.

One batch contains 1–16 updates from the same root activation, in strictly
increasing sequence order. The provider uses one source sequence across all
child samples and losses for that activation. Sequence counters for other
optional capabilities are independent.

## Child-only observation loss

`loss` contains `provider_session_id`, `transition_nonce`, `sequence`,
`reason_code`, and `source`. The first three have the same rules as updates;
`reason_code` uses the SDK reason-code grammar and `source` the bound above.
It is an affirmative report that child coverage was lost, such as bounded
snapshot truncation or failed delivery of captured child facts.

A host accepts a loss only for the current native process/root activation and
only when its sequence is newer than the last accepted child fact or loss.
It withdraws running child facts, reconciles their lifecycle liveness, and
degrades only the child feature. It must not end, complete or quarantine the
root. Previously proven terminal child outcomes can remain as history.
Delayed samples at or below the loss barrier cannot restore withdrawn facts.
This route does not replace `helper-ingress@2` whole-lifecycle degradation.

`failure` contains the SDK `reason_code` and bounded `detail` fields. It rejects
this optional sample, without silently turning malformed data into an
authoritative root or child loss. A provider that knows coverage has been lost
must emit an attributed `loss`.

## Host ownership

The host scopes observations to the live package generation, native process,
provider root and activation nonce. It accepts samples only for children whose
start was admitted in that activation, including starts in the same committed
classification. It can retain bounded descriptive history after an accepted
stop, but must not use descriptive samples to invent a new child or reactivate
a stopped identity. Stale or duplicate delivery must not rewrite observation
time. Capacity loss must be explicit; evicting old completed history may not
silently discard active child evidence.

This contract carries only the native scope the provider can observe. Public
nested Task snapshots, for example, do not establish universal coverage of
every internal agent, advisor, or independent session.
