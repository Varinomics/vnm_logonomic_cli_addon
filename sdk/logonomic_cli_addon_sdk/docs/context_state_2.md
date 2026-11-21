# context-state@2

This optional capability reports current conversation context occupancy and
successful compactions. It supplements `helper-ingress` and continuous
`lifecycle-events`; their versions and lifecycle observation schemas do not
change. The capability sidecar payload is `{}`.

The host opts in by adding `"context_state_version": 2` to an existing
`classify_event`, `observe`, or `poll_observations` request. An absent member
means no context telemetry was requested. Adapters must omit `context_updates`
from their response unless the request opts in. This allows an updated adapter
to continue serving a host that only understands the existing result grammar.

An unsupported or mistyped `context_state_version` makes only context telemetry
unavailable. Request decoders retain valid core request facts, leave the decoded
`context_state_version` at zero, and expose `unsupported_version` through
`context_failure`. An absent member also leaves the version at zero, without a
context failure. Malformed core request members still reject the operation.

For an absolute history count, the host additionally supplies
`"context_snapshot_revision": N` on `classify_event`, where N is an exact
integer from 1 through 9007199254740991. The host issues it immediately before
a serialized classifier invocation, after any asynchronous reservation. The
provider reads the current owned native history after receiving this request
and echoes N as `compaction_count_revision`. This is causal read order, not a
native event sequence or a timestamp. It permits an unreadable history and a
later identical readable history to be different observations of availability.
Cached historical payloads cannot be relabelled as a new read.

The host permits only one such read/acceptance at a time for its attachment,
retains the request revision in the delivery journal, and reuses the journal's
outcome on exact redelivery without invoking another read. A new admitted read
after receipt expiry receives a new revision. The counter does not wrap or
reset while its owning ledger survives; exhaustion removes sampling authority
without disabling lifecycle. The host accepts an absolute count only when its
revision matches the exact current request and its source matches the resulting
authenticated provider session. Retired attempts and wrong runtime, native
generation, package generation or session cannot mutate that state.

Without this member, only occupancy and successful-event telemetry are requested.
Observe and poll requests do not carry absolute-history sampling authority.
Malformed sampling metadata makes only context telemetry unavailable, leaving
valid core request facts usable. These optional fields do not change the
helper-ingress or lifecycle-events versions.

An opted-in result may contain `context_updates`, an array of at most 16 objects.
The array is independent of lifecycle observations; a context-only helper result
uses classification `ignore` with an empty `observations` array. The ordinary
helper delivery journal still acknowledges the accepted context event.

Each update has two required nonempty strings, `provider_session_id` and
`event_id`, bounded by the lifecycle identity byte limit. The session must match
the exact authenticated active provider session. The event ID identifies the
native observation and is stable across redelivery. Optional fields are:

| Field | Meaning |
| --- | --- |
| `used_fraction` | Finite current occupancy from 0 through 1. |
| `used_tokens` | Current context tokens, excluding cumulative conversation billing totals. |
| `context_window` | Positive current context capacity. |
| `usage_unavailable` | Clears the old measurement when no current measurement is available. |
| `compaction_count` | Successful compactions proved by this conversation's history snapshot; a lower bound when incomplete. |
| `compaction_count_revision` | Echo of the host's serialized current-read request, stable across redelivery. Required exactly when `compaction_count` is present. |
| `compaction_count_complete` | The absolute count covers the complete conversation history. |
| `compacted` | One successfully completed and installed compaction, identified by `event_id`. |

Integer measurements are bounded to 0 through 9007199254740991 so the desktop
JSON projection retains them exactly. Optional booleans default to false. A
measurement may be absent without being zero. `usage_unavailable` cannot coexist
with usage measurements. `compaction_count_complete` requires a count, and a
single update cannot carry both an absolute count and `compacted: true`.

The host uses `used_fraction` when supplied; otherwise it derives occupancy
from `used_tokens / context_window`, capped at 1. A partial new measurement does
not combine with older tokens or an older model window. A confirmed compaction
clears old occupancy unless that update also carries a new measurement. Starting
or attempting compaction is not success evidence.

Count revisions identify immutable read outcomes within the active attachment.
A native timestamp, file size, content digest or receipt counter does not prove
freshness. A content digest may still identify the bytes observed by a fresh
read. Completeness needs independent native writer and retention evidence:
reading an entire available file does not establish that no history was removed.
Read loss can report a fresh zero lower bound with completeness false; it does
not establish that the conversation has never compacted.

The host retains the highest accepted count revision. A snapshot with an older
or equal revision leaves the count and completeness unchanged. A newer revision
retains the larger of the previous and incoming counts. This allows a newer incomplete snapshot to withdraw completeness
even when it proves fewer compactions. A complete snapshot must cover all prior
successful compactions, so its count cannot be below the retained count. A
nominally complete lower count contradicts retained evidence and is treated as
incomplete. Later
complete history can restore completeness with a newer revision. These revisions
order count/history snapshots only; occupancy and successful-event admission
keep their separate semantics.

Retained counts do not decrease within the same provider session. Successful
event IDs are deduplicated for that entire session, including across usage and
history updates. A provider uses one compaction authority for its live stream:
absolute counts, or successful events optionally preceded by a complete
historical baseline. It must not report the same compaction as both a new
absolute total and an additional successful event. History uncertainty can
withdraw completeness without adding compactions already reported as events.
Observed events alone cannot make an unknown resumed history complete. A new
provider session resets all state, including count revisions and event IDs.
The host owns this state for the active authenticated native attachment. Closing
or replacing that attachment retires it; resuming a conversation in a new
attachment begins with unknown history until the producer proves otherwise.
No persistent history store or deduplication across distinct attachments is
implied. Within the attachment there is no count-event eviction or TTL.

Missing or malformed context telemetry affects only this capability; a valid
lifecycle result beside it remains usable. An omitted optional member is no new
measurement, not a zero. A result may instead carry
`"context_failure":{"provider_session_id":"source-session",
"reason_code":"observation_invalid","detail":"native history unreadable"}`.
The reason uses the shared reason-code grammar; detail is at most 1024 bytes.
Source identity uses the same bounds as an update. Failure and updates are
mutually exclusive. The typed result exposes `Context_failure`, containing the
source identity and ordinary `Contract_failure` diagnostic; request metadata
failures remain unscoped `Contract_failure` values.

A host withdraws occupancy and completeness only for a failure whose source
matches the authenticated active session; retained counts, revisions and
successful IDs survive. Missing, invalid or mixed source scope remains a
diagnostic with no authority to clear another session. A malformed bounded
update array retains a scope only when every item independently names the same
valid source. Providers may preserve an independently known native source when
wrapping request errors, but must never borrow the host ledger's current session
as error scope. This prevents delayed malformed data from session A from clearing
session B. Failure codecs preserve these facts across classify, observe and poll
without rejecting valid lifecycle alongside them.
A consuming host hides unknown occupancy and incomplete compaction counts instead
of presenting them as zero. Measurements advance at the provider's native
reporting cadence, which need not be each streamed token.
