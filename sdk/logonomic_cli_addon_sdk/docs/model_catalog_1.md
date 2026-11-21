# model-catalog@1

This optional capability has a static empty payload `{}`. It reports configured
model availability sampled from the live root's native registry. It is separate
from `model-state@1` (the model actually serving an actor), model selection and
usage. Its presence does not promise a prelaunch model picker or a successful
authentication, access, quota or billing check.

After both peers select this capability, the host adds
`"model_catalog":{"version":1}` to helper classification, `observe`, and
`poll_observations` requests. The adapter omits the result member unless that
request opts in. Omission means no change. A result is either
`{"version":1,"snapshot":{...}}` or
`{"version":1,"failure":{"reason_code":"...","detail":"..."}}`.
Malformed optional data fails this capability alone; neighboring lifecycle
facts remain usable. A diagnostic failure has no withdrawal authority.

## Atomic snapshot

| Member | Meaning |
|---|---|
| `provider_session_id` | Exact live root native identity, at most 256 UTF-8 bytes. |
| `transition_nonce` | The active transition's 32 lowercase hexadecimal nonce, captured at the source. |
| `sequence` | Canonical uint64 decimal text, increasing within this activation. |
| `state` | `available`, `unknown`, or `unavailable`. |
| `models` | At most 128 distinct provider/model pairs. |
| `total_count` | Actual number of models in the sampled native set, an unsigned 32-bit integer. |
| `truncated` | Exactly whether `total_count` exceeds the number of supplied rows. |
| `source` | Nonempty native query/event establishing the snapshot, at most 128 UTF-8 bytes. |

Each model has nonempty `provider` and `model` strings of at most 512 UTF-8
bytes. Optional `display_name` is at most 256 bytes. Optional `reasoning` is
a boolean describing native reasoning capability, not the current effort;
omission means unknown. All strings are valid UTF-8 without control characters.
No credentials, headers, URLs, prices or native error payloads are part of the
entry grammar. The canonical whole snapshot is at most 28 KiB. A provider must
apply a tighter budget if its native helper framing requires one.

An available empty list is an observed zero. Unknown or unavailable snapshots
have an empty list, zero total and `truncated:false`. Unknown withdraws an
observation; unavailable means this native surface cannot establish the set.
An available partial snapshot must report its actual total and truncation
flag. A provider must not silently describe a prefix as complete or cut model
identities. Each snapshot replaces the previous inventory atomically.

## Authority and lifetime

The producer must use the running session's actual profile and native registry.
Credential presence may establish configured availability without proving
access. A registry's explicitly resolvable models may differ from a native
picker's additional filters; the provider documents that distinction and its
sampling cadence. Neither a requested model nor a cached global inventory is
authority for this live catalog.

Capture the activation before asynchronous refresh and reject a retired owner
after it. Assign source order when the native sample is read, before transport
queueing. A background catalog refresh must not block lifecycle delivery or
repeat unchanged large payloads. The host verifies the current package,
runtime generation, root identity and transition before applying the snapshot.
Duplicate, stale and foreign-activation updates are inert. Lifecycle loss
withdraws inventory while preserving the replay barrier. A new activation
clears it. This capability never changes lifecycle, activates a child, or
asserts which model is currently serving.
