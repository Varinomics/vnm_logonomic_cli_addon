# permission-snapshot@1

This optional capability reports a native CLI's current permission-dialog level
when that CLI supplies a session identity and a pending boolean, but no native
request identity. The host owns the lifetime and identity of the resulting
permission attention episode. It does not infer a question, turn, completion or
provider sequence from that level.

The pair is live only beside `lifecycle-events@2`, `helper-ingress@2` or `@3`, and
`managed-configuration@3`. The lifecycle declaration must support `attention`
and `paired_attention`. The host adopts this pair independently of the other
optional attention companions. Existing helper ingress envelopes are unchanged.

## Declaration

```json
{
  "capability_id": "permission-snapshot",
  "version": 1,
  "payload": {
    "relative_path": ".native/settings.json",
    "event_name": "StatusLine"
  }
}
```

`relative_path` follows the fixed-home path grammar of managed-configuration@3.
It must differ from that capability's primary target. `event_name` is a nonempty
UTF-8 provider event name within the helper ingress name bound. `NativeSignal`
is reserved to the host's terminal-title owner and cannot be declared here.
No additional declaration members are accepted.

## Observer installation and enablement

The `transform` operation uses the existing managed configuration request and
result codecs: a pathless, single-use grant, presence flag and base64 file bytes.
It receives no native path. The pure transform must preserve unrelated settings
and compose the prior user command according to its native execution contract.
It must preserve a native disabled setting; the host does not force a callback
to execute when the user has disabled it.

For a managed launch, the host first applies the existing primary
managed-configuration@3 transform and then this capability's observer transform.
Both use the same frozen platform home and the same per-file retained-preimage,
final authority, target-mapping and exact-byte checks before atomic replacement.
The second transform is not called after a first-target failure. A failure or
loss of authority blocks the launch and prevents observer enablement. Once the
adapter has requested managed configuration, failure cannot fall back to the
unprepared command, including a launch with no selected add-on Account.

This is a sequence of per-file commits, not a transaction over both files. If a
later target fails, an earlier committed target retains its verified recovery
record. Installed observers therefore must be inert without a managed-session
enable marker and must preserve original user behavior while inert.

Only after the complete required sequence succeeds, while the same generation
remains live, the host sets `LOGONOMIC_PERMISSION_SNAPSHOT_ENABLED` to that
generation's existing nonempty nonce. A helper or persisted wrapper is enabled
only when the marker equals `LOGONOMIC_PROVIDER_GENERATION_NONCE` byte for byte
and the latter is nonempty. The marker does not create a new identity or native
sequence. This is a reserved host variable; an adapter's launch patch cannot
set or unset it. Successful preparation carries its nonce on the existing
typed provider binding, outside generic environment edits. After checking that
binding against the live generation, the terminal owner contributes the marker
beside the existing provider environment. This preparation authorization is not
persisted in the durable binding record; resume prepares it again.
The adopting host discards that name from ambient child
environments before adding its own value. An older host creates its own provider
generation nonce, so any inherited marker from a different generation is inert.
Without a matching marker, a persisted wrapper emits no snapshot and still runs
the user's original command. Interactive resume uses the same managed
preparation before carrying its enabled environment forward.

A successfully transformed file can retain a disabled native callback. Such a
callback produces no observations. This capability does not promise to observe
a dialog while the native callback is disabled or unavailable.

## Classification

Only an admitted event whose name equals the declaration's `event_name` invokes
this capability's `classify_event`. Every other event retains its negotiated
helper-ingress, attention-kind or async-question-attention classifier. The input
is the ordinary `Classify_event_request`: event, current ledger and transition
nonce. No pending conversational request or invented native correlation is added.

The result is the ordinary classification envelope with one optional member:

```json
"permission_snapshot": {
  "version": 1,
  "observation_index": 0,
  "pending": true
}
```

An accepted level requires `classification: "event"` and exactly one neutral
observation. That observation names the declared event and the current provider
session. Every session, phase, outcome, attention and subagent transition is
`none`, every evidence field is `none`, and all turn, event, sequence, request,
agent, source and end-reason fields are empty. No resume, degradation, subagent
evidence or context update may accompany it. The snapshot member is closed and
its boolean must have the JSON boolean type. Malformed metadata is rejected;
it never supplies a false level.

Invalid or irrelevant native input returns `classification: "ignore"`, no
observations and no snapshot. An absent optional snapshot means no state change.
The adapter must ignore callbacks before a current epoch and callbacks whose
native session differs from the ledger. The host independently checks the
admitted epoch, current transition, native terminal generation and declared
event before reduction. A snapshot cannot start or resume an epoch.

## Host-owned episode

The host retains zero or one request identity for this observer in each live
session epoch. A true level opens a fresh host identity when none is pending;
repeated true levels reuse the episode. A false level resolves and retires only
that identity. Repeated false levels are inert. A later true level creates a
fresh identity, so a retired request's tombstone cannot suppress a new dialog.
The admitted attention kind is `permission` with confirmed native evidence.

Working updates, new root turns and viewing the terminal do not answer a native
dialog. They do not clear the episode. An explicit false level, session end,
epoch replacement or process termination retires it. Other pending permissions,
questions and conversational hints remain independent.

The authenticated helper journal serializes classifications in admission order
when this pair is negotiated. Replays keep their existing journal outcomes.
This does not manufacture a native generation sequence: if the native CLI can
deliver callbacks concurrently without an ordering guarantee, only the host's
admission order is known.
