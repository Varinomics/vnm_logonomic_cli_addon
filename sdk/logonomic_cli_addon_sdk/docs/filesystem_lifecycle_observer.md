# Filesystem lifecycle observer component

`Filesystem_lifecycle_observer` is an opt-in implementation component for
providers using `lifecycle-events@2`, `lifecycle-correlation-lifetime@1` and the
maintained `vnm_filesystem_observation` service. Grok, Qwen and Kimi
share this driver. Their native hook admission, path discovery, parsers and
terminal-evidence rules remain provider code.

## Build and ownership

Select the observer service before adding the component:

```cmake
# The provider's maintained dependency selection defines this target:
# vnm_framework::vnm_filesystem_observation
logonomic_cli_addon_add_filesystem_observer()
target_link_libraries(provider PUBLIC
    logonomic_cli_addon_sdk::filesystem_observer)
```

The helper creates one static target and can be called by several providers in
an embedded build. It links the selected SDK and observer targets; it performs
no acquisition and defines no provider cache settings. Including the SDK does
not create this target, find Qt or link Qt to `logonomic_cli_addon_sdk::sdk`.

The public header is `logonomic_cli_addon/filesystem_lifecycle_observer.h`.
Construct one driver with the process's shared `Service` and validated
`Configuration`. A provider-specific `Filesystem_lifecycle_binding` holds its
native evidence state. Its root `session` and host `nonce` are assigned only
after authenticated root classification, under the driver's `lock()`, and
remain immutable until retirement. `bind()` requires an existing reservation.
Looking up a binding, changing provider evidence and publishing facts all use
that same lock.

Providers with native root-model evidence may implement `root_model_observation()`.
The driver includes the latest sample in an `observe` or `poll_observations`
result when the request selects `model-state@1`, including a poll with no
lifecycle events. The provider captures the activation and native source order
with the sample. During a negotiated watch a change of status, model, provider
or reasoning effort sends paced progress so the host polls even when the
lifecycle queue is empty. Replays and repeated requests for the same model
do not wake the host again. Reading it advances neither cursor. Bindings
without a sample omit the model envelope.

`handle()` consumes the reserve, retire, observe and poll operations. It returns
false for classification and other provider operations. The provider validates
and classifies its own hooks and binds a reserved correlation; the component
does not authorize a native path or infer a root from a child record.

## Native acquisition callbacks

`interests(bindings)` returns the exact files, directories and event classifier
needed by that provider. `lifecycle_file_interests()` supplies the common exact
path classifier where that is sufficient. Kimi retains its wire-specific
modification and directory-creation classifier. `check_owner(bindings)` can
enforce the provider's immutable source ownership before an observe window.

`reconcile(invocation)` acquires one native snapshot, updates provider state and
publishes supported facts. Return true only when that capture is complete.
Return false for incomplete reads or discovery; the service owns retry pacing.
Provider acquisition exceptions have the same incomplete-capture behavior.
Accepted facts stay queued and existing source coverage remains active. Initial
discovery should supply a bounded known frontier if it can; without any usable
interest the driver waits for cancellation and retries discovery only after a
new hook or observe window.

The driver acknowledges a capture only after successful acquisition and actual
coverage. Changing interests replaces the subscription and obtains a new
post-arm capture. `request_rescan()` lets an authenticated native hook request
reconciliation. Wait timeouts only bound cancellation latency; they never
perform filesystem searches or reads. `release()` drops provider acquisition
state during retirement. `wait_completed()` is a notification hook used by
existing Kimi acquisition tests, without changing the production file service.

## Publication and terminal ordering

`publish()` returns whether a fact entered the retained queue. Providers must
not treat refusal as published evidence. The component admits at most 128
active child identities and 128 live identified attention requests, matching
the current host projection limits. It retains at most 4096 queued facts and
reserves room for every admitted child STOP, every admitted attention resolution
and root END. When capacity is unavailable, new child spans or attention
requests are refused locally. A STOP or resolution for an unadmitted identity
does not create an orphan fact.

Native identity and history limits remain with the parsers. Reaching those
limits must not discard previously admitted identities or prevent their valid
terminal evidence from being read. Unknown or malformed child data is not a
terminal fact. The component does not manufacture child stops, expire child
spans or apply one provider's native completion policy to another.

`finish(root_end)` appends the root's final event in its reserved queue slot,
marks the binding closed and stops acquisition. The provider decides when its
native evidence permits that call. Grok performs a bounded best-effort read
before native root END; Qwen orders END behind already queued facts; Kimi keeps
its existing captured-wire final drain. These are native evidence policies,
separate from the shared channel's acquisition-error handling.

The driver implements the existing independent watch and consumption cursors.
Poll pages contain at most the requested SDK limit, and a retried page retains
the same provider facts. Completing or cancelling a normal observe window
returns success with a continuation cursor and consumes no queue entries, as
specified by `lifecycle-events@2`. Retirement wakes an active window with
`correlation_retired`, releases its subscription and removes its binding. A
closed final page stays replayable until the host retires its exact consumed
cursor. At most 256 live reservations are retained.

## Error boundary

Optional source acquisition is distinct from the RPC contract. Unreadable
native storage, incomplete discovery and unsupported native rows cannot turn a
valid correlation into a failed observe or poll response. Provider parsers may
skip invalid evidence or retain a retry; neither action implies a child STOP.
Root hook classification must remain independent of optional child metadata.

Malformed RPC envelopes, invalid or cross-correlation cursors, missing root
admission, attempts to rebind an authenticated root and invalid terminal-consumed
retirement remain explicit protocol errors. A collision found while discovering
optional native storage can instead leave that source unread; it cannot transfer
ownership to another correlation. The component uses the SDK request codecs and
does not relax the host's correlation, page-size or final-cursor contracts.

The provider observer tests exercise this shared implementation with the real
observer service: authenticated native evidence, missing ancestors, read
recovery, independent continuations, retirement, final drain and saturation.
The component adds no private polling loop or alternative filesystem backend.
