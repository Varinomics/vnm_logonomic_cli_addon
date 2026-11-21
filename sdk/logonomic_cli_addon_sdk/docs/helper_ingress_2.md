# helper-ingress@2

`helper-ingress@2` classifies vendor hook events that the package's own helper
delivers through the authenticated provider-event lane of Logonomic's session
ingress. Each helper-ingress version is live only through sidecar, handshake
and host-adoption intersection, and only beside `lifecycle-events@2` in the
same generation
(`dependency_capability_unavailable` otherwise; the provider-event lane is
never opened for such a package). The sidecar payload is `{}`.
`helper_ingress.h` implements the classification codec, `helper_client.h`
the client a packaged helper ships.

The lifecycle dependency is independent of the negotiated helper-ingress
version. Lifecycle correlation belongs to the continuous observation requests.
Helper results use the shared observation schema with the `HELPER_INGRESS`
admission policy, including its 160-byte `agent_label` bound. A correlation-lifetime declaration
has the additional dependencies documented in
[`lifecycle-correlation-lifetime@1`](lifecycle_correlation_lifetime_1.md).

## classify_event

Request:

```json
{
  "event": {"name": "SessionStart", "body": {"session_id": "A", "source": "startup"}},
  "ledger": {
    "current_epoch": 2,
    "current_transition": {"kind": "start", "provider_session_id": "B", "nonce": "0f..."}
  },
  "transition_nonce": "3c9a..."
}
```

`event.name` is 1-256 bytes (`k_max_provider_event_name_bytes`); `event.body`
is an object of at most `k_max_provider_event_bytes` (48 KiB), measured as its
compact JSON encoding at the host transport bridge. This is deliberately not
the SDK canonical spelling: the SDK and Qt may normalize the same JSON number
differently.
`ledger` is the host's view for this session: `current_epoch` is 0 before any
transition and `current_transition` is null exactly at epoch 0.
`transition_nonce` is the 32 lowercase hex nonce
(`k_transition_nonce_hex_chars`) this delivery would open a transition under.

Success:

```json
{
  "classification": "transition",
  "transition": {"kind": "start"},
  "observations": [
    {"provider_event_name": "SessionStart", "session_transition": "started",
     "provider_session_id": "A", "session_source": "startup", "...": "none",
     "resume_record": {"provider_session_id": "A"}}
  ],
  "helper_response": {"hookSpecificOutput": {"additionalContext": "..."}}
}
```

`classification` is `transition` (exactly one observation carries
`session_transition: started`, and `transition.kind` is `start` or
`resume`), `event` (1–16 observations, none `started`, no `transition` member) or `ignore` (no observations, no `transition` member).
`observations` holds at most `k_max_observations_per_result` (16),
each valid under the helper observation policy of
[lifecycle-events@2](lifecycle_events_2.md) (`observation_invalid` otherwise).
`helper_response` is optional, any JSON value of at most
`k_max_helper_response_bytes` (32 KiB) in the host's compact transport
encoding, returned to the helper verbatim and never inspected, logged or
projected by the host. The portable adapter codec does not decide this limit
from its different canonical numeric spelling; the host measures it before
committing the outcome. Anything else is `classification_invalid`. The deadline
is `k_classify_event_deadline_ms` (2000).

The adapter is stateless across invocations: everything it needs about the
session arrives in `ledger`, and the host is the only writer of epochs. An
adapter never names an epoch.

## The provider-event lane

The lane is host-owned; it is documented here for helper authors. Provider
class `logonomic.provider_event` on the same endpoint and token as the
session-control lane, authenticated by the session capability plus the
package binding, with three actions.

`deliver` arguments as they reach the host:

```json
{
  "session_capability": "<64 lowercase hex>",
  "package_id": "org.example.unknown-fixture-provider-sessions",
  "generation_digest": "<64 lowercase hex>",
  "generation_nonce": "<32 lowercase hex>",
  "idempotency_key": "<1-256 bytes>",
  "transition_nonce": "<32 lowercase hex>",
  "event": {"name": "SessionStart", "body": {"...": "..."}}
}
```

Result: `{"accepted": true, "kind": "provider_event_admitted"}`; for a key
already committed, `{"accepted": true, "kind": "provider_event_committed",
"outcome": {...}}`; otherwise `{"accepted": false, "reason": "<lane reason>"}`.

`result` arguments: `{"session_capability": "...", "idempotency_key": "..."}`.
Result: `{"status": "pending"}` or `{"status": "committed", "outcome":
{"accepted": bool, "reason": "...", "epoch": 3, "helper_response": ...}}` -
`reason` only when not accepted, `helper_response` only when the adapter
returned one, `epoch` the epoch the event was applied to.

`acknowledge` arguments are the same `session_capability` and
`idempotency_key`. A committed live-Session key returns
`{"status": "acknowledged"}`; repeating the acknowledgement returns the same
status. Later `result` calls retain only that compact status until the original
TTL expires, rather than replaying the committed outcome.
Acknowledging a pending key returns `{"status": "pending"}`; an unknown key
returns `{"status": "unknown_request"}`. Neither response consumes or creates
an outcome. After acknowledgement, retrying the original `deliver` is refused
as `{"accepted": false, "reason": "response_acknowledged"}` because the
original helper response has been explicitly discarded; this does not revise
the already committed provider-event outcome.

The two-step shape exists because the host's control handlers are
synchronous: `deliver` admits and starts classification, `result` reads the
committed journal.

### The wrapper

A helper never supplies authority itself. The Logonomic-owned wrapper
`logonomic_vnmctl provider-event deliver --args-stdin` reads

```json
{"idempotency_key": "...", "transition_nonce": "...", "event": {"name": "...", "body": {}}}
```

from stdin (`idempotency_key` and `transition_nonce` optional; minted as 32
lowercase hex when absent), adds `session_capability`, `package_id`,
`generation_digest` and `generation_nonce` from `LOGONOMIC_SESSION_CAPABILITY`,
`LOGONOMIC_PROVIDER_PACKAGE_ID`, `LOGONOMIC_PROVIDER_GENERATION_DIGEST` and
`LOGONOMIC_PROVIDER_GENERATION_NONCE`, and performs both steps: `deliver`,
then `result` every `k_provider_event_poll_interval_ms` (100) until
`committed` or `k_provider_event_client_deadline_ms` (5000). It prints the
committed `outcome` object of the `result` response - and only that object -
as one JSON document on stdout, so a helper makes one process invocation. A
raw `call logonomic.provider_event ...` is refused: Logonomic supplies this
Session's authority.

After consuming a committed outcome, a helper may explicitly release its
retained response with
`logonomic_vnmctl provider-event acknowledge <idempotency-key>`. This command
uses the same environment authority and prints no replacement outcome. It exits
0 for `acknowledged`, 4 while the key is still pending, and 1 for a well-formed
`unknown_request` refusal or transport failure.

The wrapper's `k_provider_event_input_max_bytes`, equal to the existing 256 KiB
`k_max_control_message_bytes`, caps raw stdin as an independent resource ceiling
before JSON parsing. This does not define the event-body limit: after parsing,
the wrapper separately applies the 48 KiB compact host-transport body limit
before router dispatch. Exceeding the raw ceiling produces exit code 2 and no
structured reason because the wrapper deliberately stops reading before it
parses an outcome object.

Exit codes: 0 committed (accepted or not; the JSON says which), 1 transport
failure, 2 invalid arguments or environment, 4 still pending at the deadline.

A retry after a lost response reuses the same `idempotency_key` and
`transition_nonce`. The helper is stateless across processes, so the replay
identity of a transition is the helper-minted nonce plus the host-computed
SHA-256 of the canonical `event` object (`provider_event_evidence_digest_hex`);
the host records both. The transition nonce is identity, not a secret: it
makes two deliveries of one transition recognizable, and it grants nothing.
Authentication is the session capability and the package binding the wrapper
supplies.

Exact response replay is a live-Session guarantee. It lasts until
acknowledgment, the response TTL, Session-capability revocation or endpoint
shutdown. A manager restart does not revive the old capability, endpoint or
response journal. Durable provenance, package binding and authorized resume
records are separate and remain durable.

## The helper client

`helper_client.h` is the only client code a package ships against Logonomic.
It knows the wrapper's subcommands and exit codes and nothing of the control
protocol, the endpoint or the token.

```cpp
struct Helper_environment            // read once from the process environment
{
    std::string vnmctl_path;         // LOGONOMIC_VNMCTL_PATH
    std::string session_capability;  // LOGONOMIC_SESSION_CAPABILITY
    std::string package_id;          // LOGONOMIC_PROVIDER_PACKAGE_ID
    std::string generation_digest;   // LOGONOMIC_PROVIDER_GENERATION_DIGEST
    std::string generation_nonce;    // LOGONOMIC_PROVIDER_GENERATION_NONCE
    std::string helpers_dir;         // LOGONOMIC_PROVIDER_HELPERS_DIR
};
std::optional<Helper_environment> read_helper_environment(Contract_failure* out_failure);

struct Provider_event_outcome
{
    bool                         committed = false;   // false: transport failure, invalid arguments or the deadline
    bool                         accepted  = false;
    std::string                  reason;              // when !accepted
    std::optional<std::uint64_t> epoch;
    std::optional<Json_value>    helper_response;
    int                          exit_code = 1;
};

class Helper_client
{
public:
    explicit Helper_client(Helper_environment environment,
        std::function<bool()> cancellation_requested = {});
    Provider_event_outcome deliver_provider_event(
        std::string_view event_name, const Json_value& body,
        std::string_view idempotency_key = {},      // minted (32 hex) when empty
        std::string_view transition_nonce = {});    // minted (32 hex) when empty
    int acknowledge_provider_event(std::string_view idempotency_key);
    int set_title(std::string_view title);                       // logonomic_vnmctl set-title
    Title_instruction_result request_title_instruction(
        std::string_view source, std::string_view hook_event_name,
        bool changed_files_requested = false);
    int show_changed_files(const std::vector<std::string>& paths);
    int show_diff(std::string_view manifest_json);
};
```

`request_title_instruction` returns `Title_instruction_result`. A completed,
accepted result always carries `instruction_required` and the exact
host-authored `instruction`; a completed refusal carries its `reason` and no
instruction fields. A wrapper or response failure leaves `completed` false.
The title request does not require lifecycle event admission, so an installed
hook may request its one-shot instruction while lifecycle observation is
unavailable.

`changed_files_requested` is advisory. The host delivers its show-changes
guidance with the first accepted instruction-bearing request of the active
conversation, whatever the value, so a provider does not have to recognize
requests to show changes: it may keep sending the hint it computes or leave the
default `false`. The argument stays accepted and validated so existing helpers
keep working.

`read_helper_environment` fails with `invalid_helper_environment` naming the
variable that is absent, empty or outside its grammar. Every call spawns the
wrapper at `vnmctl_path` with the current environment - in the calling
process's process group or Job, because a helper's descendants stay under
the custodian - writes the argument object to its stdin and reads its stdout
up to `k_max_helper_client_stdout_bytes`; a wrapper that does not exit within
`k_helper_client_wrapper_deadline_ms` is killed and the call is a transport
failure. Random material comes from the SDK's nonce source
(`random_nonce_hex`), never from time. `deliver_provider_event` refuses,
before spawning and with exit code 2, an empty or oversize event name, a body
that is not an object, a key above `k_max_idempotency_key_bytes` or a nonce
outside its grammar; the outcome's `reason` then names the contract reason.
The SDK cannot authoritatively enforce the 48 KiB body limit because it does not
carry Qt's serializer. The Logonomic-owned wrapper/host bridge measures the
actual compact transport encoding and rejects `payload_too_large` before router
dispatch. The host repeats the check at the authenticated ingress boundary.
`acknowledge_provider_event` requires the nonempty key supplied to the original
delivery and refuses keys outside the same grammar before spawning.

An optional nonblocking, nonthrowing cancellation predicate is checked before
spawning and during each wrapper wait. Cancellation terminates and reaps the
owned wrapper and returns a transport failure; later deliveries or
acknowledgements through the same cancelled client spawn nothing. The wrapper
stays in the inherited custodian process group or Job. A package plugin can
keep its helper's stdin pipe open after one newline-delimited event and close
that pipe at disposal; the helper supplies its EOF observation as the predicate
and exits after SDK cleanup. Killing only the helper PID is not cancellation:
it can abandon the helper's wrapper. Cancellation does not roll back a
provider event the host has already admitted.
The wrapper deadline bounds when termination is requested, not when the OS
confirms exit. If exit or reaping is delayed, the helper call remains pending
and retains its process ownership; it cannot report cleanup complete from a
timer alone.

## Reasons

Adapter-facing: `classification_invalid`, `observation_invalid`,
`payload_too_large`, `frame_violation`, `unexpected_member`,
`dependency_capability_unavailable`. Lane reasons are the host's.

The diagnostic-only host reason `provenance_unavailable` identifies a failed
optional provenance or resume-record step. Its fixed stage distinguishes
`provenance_plan_failed`, `provenance_owner_conflict`,
`provenance_write_failed`, `resume_record_unavailable`, and
`resume_record_write_failed`; diagnostics carry no observation body or record.
The validated lifecycle batch still commits and its journal outcome remains
accepted. Continuous observation also continues. Provenance queries report
whether the current identity was bound; resume availability depends on the
record that was successfully persisted for the requested historical identity.

## Lifecycle loss evidence

Version 2 permits the optional, independently versioned result record
`"lifecycle_degradation": {"version": 1}`. It is valid only with
`"classification": "ignore"`, empty observations, and no transition. Without
`provider_session_id`, the fact reports delivery loss and degrades the entire
terminal lifecycle. With a nonempty `provider_session_id`, it reports correlation
loss: the entire lifecycle is degraded and that provider session is quarantined.
The identity is bounded by `k_max_lifecycle_identity_bytes` and contains no NUL.
It is never a new epoch, capability feature downgrade, or provider identity policy.

The authenticated helper admission, package generation and current session
capability checks remain mandatory. The host checks retired, quarantined and
current provider identities before accepting a session-bearing fact. The generic
journal's epoch authority is unchanged: a committed transition retires older
admitted entries, and their late classification cannot apply facts to the new
epoch. Epoch zero is allowed: no initial SessionStart is required to report lost
delivery or correlation. Duplicate deliveries replay their journal outcome; they
do not reapply the fact. Diagnostics do not contain the provider identity or raw
event body.

Absent records retain ordinary classification behavior. Unknown record versions,
malformed records, and records on another helper version reject classification without
retiring the provider or its sibling capabilities. Packages must emit this record
only under a negotiated version 2 invocation.


[helper-ingress@3](helper_ingress_3.md) is a separately supported subagent-evidence
contract. Whole-lifecycle degradation belongs only to version 2; choosing version
3 does not silently admit that record. The host invokes the highest live adopted
version in the generation's provider binding.
