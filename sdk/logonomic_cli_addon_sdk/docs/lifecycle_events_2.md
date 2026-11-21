# lifecycle-events@2

`lifecycle-events@2` declares which lifecycle facts a package produces, fixes
the schema of one observation, and, in continuous mode, defines the bounded
long-lived observation protocol. The host owns the canonical session state
machine: it validates ordering, deduplication, legal transitions, terminality
and correlation before updating the session model, and vendor event names
never enter host policy. The pair is live only through sidecar, handshake and
host-adoption intersection. `lifecycle_events.h` implements the codecs.

## Sidecar payload

```json
{
  "profile_id": "unknown-fixture-events",
  "features": {
    "root_turn_start": "supported",
    "root_turn_completion": "supported",
    "confirmed_phase_status": "supported",
    "attention": "supported",
    "paired_attention": "unsupported",
    "subagents": "unsupported",
    "cooperative_description": "unsupported",
    "blockable_completion": "unsupported"
  },
  "turn_origin_correlation": "unsupported",
  "observation": "events"
}
```

`profile_id` is 1-64 bytes of `[a-z0-9-]` (`k_max_lifecycle_profile_id_bytes`).
All eight `features` members are required, each `supported` or `unsupported`.
`turn_origin_correlation` is `supported` or `unsupported`. `observation` is
`events` (facts arrive through helper-ingress@2/@3 classification) or
`continuous` (the adapter watches vendor state through `observe`). A defect
is `invalid_sidecar`.

The host composes a bound session's lifecycle features from the declaration
at session creation: the profile id, the package release version as provider
version, `package_declaration` as evidence, each `supported` feature as
configured-unobserved and each `unsupported` feature as unsupported, and the
turn-origin correlation as declared. Its existing feature gate and the
promotion from configured to observed then apply unchanged.

The feature gate is `lifecycle_profile_admits`. An observation carrying a fact
its profile does not support is rejected whole as `unsupported_profile_event`:
confirmed phase or outcome evidence needs `confirmed_phase_status`, `working`
needs `root_turn_start`, `awaiting_input` needs `root_turn_completion`, any
attention needs `attention` and one naming an `attention_request_id` also
`paired_attention`, and a subagent transition needs `subagents`. A package's
tests apply the same function to every observation shape it emits, against its
own declaration.

## Observation

Every element of an `observations` array carries exactly these members:

| Member | Type / bound | Values |
|---|---|---|
| `provider_event_name` | string 1-256 (`k_max_lifecycle_identity_bytes`) | required |
| `session_transition` | string | `none`, `started`, `ended` |
| `phase_transition` | string | `none`, `working`, `awaiting_input` |
| `phase_evidence` | string | `none`, `inferred`, `confirmed` |
| `outcome_transition` | string | `none`, `succeeded`, `failed`, `interrupted` |
| `outcome_evidence` | string | `none`, `inferred`, `confirmed` |
| `attention_transition` | string | `none`, `requested`, `resolved` |
| `attention_evidence` | string | `none`, `inferred`, `confirmed` |
| `subagent_transition` | string | `none`, `started`, `stopped` |
| `provider_session_id`, `provider_turn_id`, `provider_event_id`, `provider_sequence`, `attention_request_id`, `agent_id` | string at most 256 | may be empty |
| `session_source`, `session_end_reason` | string at most 160 (`k_max_lifecycle_label_bytes`) | may be empty |
| `agent_label` | helper ingress: at most 160 bytes; continuous: at most 768 UTF-8 bytes and 256 UTF-16 code units | may be empty |
| `resume_record` | object at most 16 KiB canonical (`k_max_resume_record_bytes`) | optional; only with `session_transition: started` |

The shared observation decoder takes an explicit `Lifecycle_observation_policy`.
`HELPER_INGRESS` retains the helper result's 160-byte `agent_label` bound.
`CONTINUOUS` preserves native child display labels: valid UTF-8, at most 768
wire bytes and at most 256 UTF-16 code units. Supplementary code points count
as two units; invalid UTF-8 and surrogate code points are rejected. The
`poll_observations` result decoder selects the continuous policy; helper result
decoders select the helper policy. Session source, end reason, identities and
all other fact semantics have the same bounds in both policies.

Every string member is on the wire, empty when it does not apply. Validity:
each `*_transition`/`*_evidence` pair is `none` together or neither; an
outcome names a `provider_turn_id`; a session transition names a
`provider_session_id`; a subagent transition names an `agent_id`;
`provider_sequence` is empty or a canonical decimal (no leading zero) that
fits 64 bits; an anonymous attention is only `requested`+`inferred`; a
`confirmed` attention names an `attention_request_id`; `resume_record`
accompanies `started` only. `provider_event_id` is the stable identity the
host deduplicates on, `provider_sequence` the sequence evidence, the
transitions the intent and the evidence members the quality. A violation is
`observation_invalid`; a missing or mistyped member is `frame_violation`, a
foreign member `unexpected_member`, and a bound `payload_too_large`.

`resume_record` on a `started` observation becomes the interactive-resume@2
record of the provider session that observation started
(`interactive_resume_2.md`).

## Continuous mode

Operations exist only when `observation` is `continuous`; in `events` mode
every invoke answers `unavailable`/`unknown_operation`.

### Correlation

Both `observe` and `poll_observations` requests may carry an optional
`correlation` member:

```json
{"version":1,"transition_nonce":"0123456789abcdef0123456789abcdef"}
```

The nonce is the authenticated helper transition nonce already admitted by the
host ledger. It selects the package's frozen context for that exact transition;
it is not a provider session ID, a filesystem path or an authorization token.
The host sends it only after admission and preserves it across that observation
chain. A replacement admitted helper transition cancels the old watch and poll,
resets both cursors, and starts a new chain with the new nonce and exact provider
session ID. Package generation retirement and session end cancel the chain too.

This correlation is necessary when a provider reuses the same session ID under
different account homes. The existing session ID, working directory and launcher
arguments can all match while the authenticated helper captured different homes.
Guessing a most recent session or using a process-wide home would misattribute
observations. Packages resolve the nonce to their authenticated helper context
and verify the requested provider session ID before reading vendor state.

Both first request cursors remain present empty strings. The correlation routes
the first poll even before the package returns a cursor. Subsequent cursors are
opaque package values; a package must reject a cursor belonging to another
correlation. The watch continuation cursor and the poll consumption cursor are
independent: completing a watch never acknowledges unread observations. The host
drains a completed window before starting the next, and discards progress and
completion callbacks from canceled or replaced chains.

Correlation is optional, allowing packages to discover sessions through watches
without helper ingress. The correlation object has exactly the two members
shown, version 1 and a 32-character lowercase hexadecimal nonce. Unknown
versions, unknown members, missing/malformed nonces and mistyped objects are
rejected. Lifecycle adoption is independent of helper and title capability
versions.

### observe

One bounded window of watching:

```json
{
  "session": {"working_directory": "/repo", "launcher_argv": ["unknown-fixture-cli"], "provider_session_id": ""},
  "resume_cursor": "",
  "window_ms": 60000
}
```

`window_ms` is `k_observe_window_ms` (60000). The adapter watches vendor state
and, whenever new observations become available, emits one progress frame per
batch: `completed` is the count of observations available since the window
began, `total` is absent, and `note` is at most `{"available": 3}`
(`observe_progress_note`). Progress frames wake the host; they never carry
observations. The operation succeeds when the window elapses or the host
cancels it:

```json
{"resume_cursor": "opaque, at most 256 bytes"}
```

The host arms the request deadline at `window_ms` plus
`k_observe_window_grace_ms` (5000) and invokes the next window immediately
with the returned cursor. A window that ends without a result is a package
failure (`timed_out`, then the broker's retirement rules).

### poll_observations

The pull the host issues on every wake:

```json
{"resume_cursor": "...", "limit": 16}
```

```json
{"observations": [{"...": "..."}], "resume_cursor": "...", "more": false}
```

`limit` is 1-16 (`k_max_observations_per_result`); observations are in
sequence order and at most `limit`; `resume_cursor` follows the last
returned observation; an empty list is valid. The host keeps invoking while
`more` is true. Cursors are opaque, at most `k_max_observe_cursor_bytes`
(256), and live only as long as the session.

## Reasons

`observation_invalid`, `payload_too_large`, `frame_violation`,
`unexpected_member`, `unknown_operation`, `invalid_sidecar`.
