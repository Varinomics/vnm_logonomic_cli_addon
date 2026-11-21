# execution-context@1

This optional static capability uses an empty sidecar payload `{}`. It adds a
capability-owned `execution_context` envelope beside helper classification and
continuous lifecycle requests and results. It does not change their versions.
A host sends `"execution_context":{"version":1}` only after both peers select
`execution-context@1`. A provider omits the result envelope unless requested.

Logonomic does not adopt this capability: nothing it presents reads the
directory a provider reports, and Resume uses the conversation's registered
directory. A package that declares it keeps working, because a host that does
not select it never sends the request envelope. The capability stays in the SDK
until the packages that declare it have removed their declarations and
emitters.

An omitted envelope means **no update**. A result is either
`{"version":1,"updates":[...]}` or
`{"version":1,"failure":{"reason_code":"...","detail":"..."}}`.
There are 1–16 updates, each with exactly these fields:

| Field | Meaning |
| --- | --- |
| `provider_session_id` | Exact native identity, bounded like lifecycle identity. |
| `transition_nonce` | The 32 lowercase hexadecimal nonce of the accepted active native transition. |
| `sequence` | Canonical unsigned decimal **string**, 0–18446744073709551615, increasing within that activation. |
| `state` | `confirmed`, `unknown`, or `unavailable`. |
| `environment` | `local` for confirmed, empty for unknown, `unsupported` for unavailable. |
| `cwd` | Exact absolute default directory for confirmed; empty otherwise. |
| `source` | Native event/query/invariant that established the fact; 1–128 UTF-8 bytes without controls. |

Confirmed directories are valid UTF-8, at most 4096 bytes, absolute in the local
host platform's path namespace, and contain no control characters. Their spelling
is preserved, including a nested subdirectory and spaces. They need not be Git
worktrees. `unavailable` explicitly records an unsupported execution namespace;
no remote or container path is passed to a local filesystem or launcher.

## Evidence and ordering

The directory is the native conversation's actual default execution directory.
Launch input, transcript metadata, a per-command workdir, text resembling `cd`,
and reported repository participation do not establish it. A native provider
invariant is usable only if it proves the actual default for this activation.

Capture transition attribution and source sequence **before** asynchronous work.
A joint native SessionStart fact may use the classification request's transition
nonce and sequence zero. Later observations require a nonce captured at the
native source for that activation; reading the host's current nonce at arrival
would misattribute delayed A → B → A events. A provider lacking source attribution
must omit such observations. Sequence is not a timestamp or host arrival counter.

The host accepts a record only for its live package generation, runtime generation,
exact native identity, active transition nonce and strictly newer source sequence.
Every identity activation resets current context to unknown, including returning
to a previously active native ID. A cwd-only update does not change identity,
Notes, origin provenance or reported worktrees. Joint accepted identity and cwd
facts publish together. Duplicate, stale and retired-activation records are inert.

Malformed optional data and unsupported optional versions fail this capability
alone; a valid lifecycle event beside them still applies. Diagnostics cannot clear
another activation's cwd. Withdrawal requires an attributed `unknown` or
`unavailable` update. Host observation loss clears the current fact while retaining
the last source sequence as a replay barrier.

## Current facts

Current context and immutable launch cwd are separate. A conversation that
returns starts unknown until its activation reports again; origin or launch paths
are not fabricated as a current fact.

This contract does not authorize a resume recipe or establish that process spawn
cwd controls the provider. A host resumes a conversation in the folder it is
registered with, not in a reported directory. A provider may support only joint
SessionStart snapshots until later native events can carry source-captured
activation attribution. This does not establish support for changing a
provider's execution directory.
