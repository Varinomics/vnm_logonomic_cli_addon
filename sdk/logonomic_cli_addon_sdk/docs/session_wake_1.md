# session-wake@1

This optional capability lets a package notify the exact live native conversation
that Logonomic has committed new mailbox messages. The provider must use a native
API that safely admits model input independently of the terminal input editor.
Keystrokes, terminal pastes, shell input, simulated Enter, restarting a session,
loading a historical conversation into another process, and approving a pending
request do not implement this capability.

The sidecar, live adapter handshake and host adoption must all admit the pair.
Absence, malformed typed data or an unknown version disables wake alone. The
ordinary launcher, lifecycle, mailbox, read state and recipient acknowledgment
remain available. `session_wake.h` implements the codecs.

## Declaration and preparation

The sidecar uses the environment-name allowlist of launcher-discovery@1:

```json
{"environment_names":["PROVIDER_HOME"]}
```

Before an interactive launch or resume the host may offer the independently
versioned member `"session_wake":{"version":1}`. `Prepare_request` and
`Session_launch_context` carry the offer. Only a negotiated package receives it.
The package answers through `Launch_patch.session_wake`:

```json
{"session_wake":{"version":1,"available":true,"binding":{"registration_id":"launch-7f3c"}}}
```

or:

```json
{"session_wake":{"version":1,"available":false,"reason":"native_wake_unsupported"}}
```

An available record requires an object `binding` of at most 16384 canonical
JSON bytes and no reason. An unavailable record requires a reason code and no
binding. The binding is package-owned evidence from this launch: for example a
private plugin-registration identity, native daemon scope, or verified executable
capabilities. A later installed executable does not establish the capabilities
of an older already-running conversation. Missing, unknown or malformed records
leave wake unavailable without rejecting the ordinary preparation or resume.
The host admits a record only if it offered the live capability; an unsolicited
record grants no authority. An unmanaged launch cannot acquire wake authority.

The host retains this binding only in live session state. It must not publish it
in desktop snapshots, lifecycle observations, message histories or diagnostics,
or persist it in an orchestration manifest. Package-private credentials stay in
the package's protected live registry. A resume is a new live binding and requires
new preparation; a historical record cannot revive a native endpoint.

## Exact target

Both operations carry this target:

```json
{
  "session":{
    "provider_session_id":"native-thread-51",
    "working_directory":"/srv/project",
    "launcher_argv":["provider","--interactive"],
    "launch_environment":{"platform":"linux-x86_64","effective_environment":{"PROVIDER_HOME":"/home/zoe/.provider"}}
  },
  "correlation":{"version":1,"transition_nonce":"7f3c9a2e5b1d4f607f3c9a2e5b1d4f60"},
  "binding":{"registration_id":"launch-7f3c"}
}
```

The host supplies the currently admitted native session ID and authenticated
helper transition nonce, exact package generation, launch-time binding, original
launcher argv and frozen launch environment. The environment includes only the
discovery baseline and this capability's declared names, never reserved host
authority variables. Account/profile choices belong to the same original launch;
the adapter cannot substitute its ambient environment or the newest global session.

The session ID is nonempty UTF-8 without NUL, at most 256 bytes. The working
directory and argv retain the session-contract bounds: 4096 bytes for the directory
and each argument, 1–64 arguments with a nonempty executable, 11264 argument bytes
in total. Correlation has version 1 and a 32-character lowercase hexadecimal
transition nonce. Binding has the preparation bound. Object member sets are
closed. Invalid UTF-8, missing/mistyped members, unknown members and bounds use
`invalid_utf8`, `frame_violation`, `unexpected_member` and `payload_too_large`.

The host fences callbacks by live session capability, package generation and
admitted transition. A native conversation transition cancels outstanding work
and revokes its readiness and notification authority. Per-launch preparation
evidence may remain only within the same live native process; new notifications
require fresh inspection of the newly admitted native ID and transition. Pending
notifications from the previous conversation must not wake its replacement.
Session end, process replacement, package generation retirement or control
shutdown revokes the preparation too. Cancellation cannot undo native input
already accepted. A package validates its native endpoint, instance and
conversation before notifying; session ID alone is insufficient where multiple
accounts or processes can own that identity.

## inspect

`inspect` is read-only and receives the target above. It returns
`{"available":true}` or `{"available":false,"reason":"unbound_session"}`.
Reasons follow the ordinary reason-code grammar. Packages distinguish unsupported
native facilities, launch-time disablement, an unbound/disconnected conversation,
and native pause/attention states rather than reporting all of them as supported.

Availability is a momentary observation, not authority to submit later. `notify`
must still validate its target and respect the native state at submission. Busy
or paused consumption can remain available when a native queue safely retains
notifications without interrupting or clearing that state.

## notify

`notify` adds `notification_id` and `instruction` to the target. Both are nonempty
UTF-8 without NUL; limits are 256 and 4096 bytes respectively. The ID identifies
one host notification. The instruction is a host-authored mailbox reminder with
clear Logonomic attribution, not the sender's message body or a human approval.
The package must submit it as native model input rather than parse it as a command.
It preserves native agent/model/account selection and user interruption, pause,
approval and question policies. It cannot cancel work to make a target eligible.

Success returns one of these payloads:

| Disposition | Meaning | Other members |
|---|---|---|
| `accepted` | The native API accepted this notification, possibly into its queue. | Optional nonempty `receipt`, at most 256 UTF-8 bytes; no reason. |
| `deferred` | Nothing was submitted; a later eligibility event may permit a retry. | Required reason; no receipt. |
| `unavailable` | This target cannot accept native wake. Nothing was submitted. | Required reason; no receipt. |
| `uncertain` | Submission may have succeeded but acceptance is not known. | Required reason; no receipt. |

Accepted is not proof that a turn started, the model read its mailbox or a recipient
acknowledged a message. Those facts use their existing independent evidence.
Deferred guarantees no native enqueue/turn side effect: the host may retry the same
ID after a new eligibility event. An accepted or uncertain request is never retried
automatically. A timeout, transport loss or cancellation after dispatch is uncertain
unless the package can prove no submission occurred; operation error status alone
does not establish that proof. Inspect failures have no notification side effect.

The host issues notifications only after mailbox commit, coalesces new arrivals,
and does not wake repeatedly merely because messages remain unread or unacknowledged.
Where a provider can reconcile or deduplicate its native calls it should reuse the
notification ID. This contract does not promise exactly-once native execution.

## Lifetime and validation

Provider listeners, helpers and queues remain under the existing process custody
and cancellation contracts. A plugin bridge is package-private: the common host
does not route on provider names or learn native wire formats. No endpoint address,
credential or raw notification content belongs in general diagnostic output.

The decisive integration check leaves the original native UI idle after a turn,
commits a message from another Logonomic session, and observes another turn in the
same conversation without keyboard input. Busy, approval/question pending,
interrupted, disconnected and stale-generation cases must preserve their native
policies. A fixture or a hook returning context during an existing turn proves
neither idle wake nor safe native submission.
