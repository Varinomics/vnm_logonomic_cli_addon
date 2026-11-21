# Conformance

The `org.example.unknown-fixture-provider-usage-cache-acceptance` package
advertises `usage-scan@3` and serves two pages from one sealed host allocation.
The observation tests exercise its real adapter process, per-page profile
acknowledgment enforcement, stable descriptor, and cache reopen validation.
Usage-page codecs also check account versions, absent/null semantics and
strict owned acknowledgment records against the host acknowledgment helpers.
Cache codec/storage tests cover strict request
selection, reset refusal of retained input, account binding, immutable sealing,
reserved paths and corruption. These are protocol/storage gates, not native
model qualification or proof of a production host's index publication.

The conformance suite proves adapter and package behaviour against the
contracts in this directory from the outside: it validates the package
directory, starts the adapter exactly as a host does, and observes only what
crosses the process boundary. A pass proves contract behaviour. Integration
with a host is a separate claim, made only when the same packaged fixture
traverses that host's own registry, broker and custody.

## Provider test support

Provider test targets link `logonomic_cli_addon_sdk::test_support` to use the
headers under `tests/support/logonomic_cli_addon_test`. The target is available
when the SDK's own tests are disabled and adds no test headers to production
SDK consumers. `invocation_context.h` supplies a context that records a result
or throws on failure, and a cancelled scan context that rejects completion.
`usage_scan.h` invokes the actual `Host_simulator`, checks each decoded page's
account and storage echo, and collects pages while checking candidate identity.
Tests continue to own their package, host lifetime, storage allocation, provider
fixtures and accounting assertions.

## The suite

```sh
logonomic_cli_addon_conformance <package-dir> [--platform <id>] [--no-isolation]
```

Every check prints one line `PASS|FAIL|SKIP <check-id> <detail>`. The exit
code is 1 when any check failed, 0 otherwise. `SKIP` identifies declaration
payloads the suite does not assess, and checks that probe
`conformance-probe@1` or `profile-storage@1` when those capabilities are not live.

`structured-session@3` and `delegated-session@3` declarations are assessed
through `decode_purposes_declaration`, including their frozen-environment grammar, and
`managed-configuration` versions 1, 2 and 3, through the managed-configuration
declaration decoder. Malformed known declarations fail before the adapter is
started. Other IDs and future versions are explicitly unassessed, not rejected
or interpreted using an older grammar. The production package validator still treats capability payloads as opaque.
This is not exhaustive capability validation, session execution qualification,
or a check of host-dependent admission prerequisites.

The suite runs the simple probes in one adapter session and gives each
destabilizing check a session of its own, so that a crash or hang is
attributed to the check that provoked it. When the handshake itself fails,
every later check that needs a session reports that failure.

### Checks

| Check | Proves |
|---|---|
| `package.validates` | the directory passes the package contract; prints the package ID and generation digest |
| `declaration.session_purposes` | each known structured/delegated session declaration passes its existing versioned purposes/environment codec; failures name the pair and codec reason |
| `declaration.unassessed` | `SKIP`: this declaration ID/version is outside the suite's declaration decoder coverage |
| `handshake.completes` | `hello` and `capabilities` arrive in order within the hello deadline with identity, generation and limits accepted |
| `declaration.managed_configuration` | declared `managed-configuration@1`, `@2`, and `@3` payloads satisfy their independently versioned target grammar |
| `handshake.sidecars_live` | every sidecar-declared pair except static `account-profile-managed-configuration@1` metadata is in the handshake |
| `handshake.no_undeclared_pairs` | every handshake pair has a sidecar |
| `profile_storage.acknowledged` | `acknowledge` echoes the allocated root and version byte for byte |
| `invoke.echo_round_trip` | a payload returns unchanged |
| `result.unknown_operation` | an undefined operation on a live capability answers `unavailable`/`unknown_operation` |
| `capability.unknown_pair_answered` | an undeclared capability pair, sent deliberately, answers `unavailable`/`unknown_operation` |
| `capability.unknown_version_local` | the host decides `unsupported_version` locally for a live capability at another version |
| `capability.unknown_local` | the host decides `unknown_capability` locally for an unknown pair |
| `malformed.payload_rejected` | a malformed payload yields a terminal `error` or `unavailable` and the adapter stays ready |
| `progress.delivery` | progress arrives, a rapid stream coalesces, and finite older-peer limits remain respected |
| `failure.package_local` | an `error` result leaves the adapter answering the next request |
| `unavailable.reported` | an `unavailable` result is delivered as such |
| `proposal.detached_rejected` | the process proposal the probe returns with detached intent is rejected as `detached_helper_proposal` |
| `limits.frame_below` | a frame at half the effective bound round-trips |
| `limits.frame_at` | a frame of exactly the effective bound round-trips |
| `limits.frame_above` | a frame one byte over is refused locally as `payload_too_large` and the adapter is untouched |
| `limits.depth_at` | a payload nested to exactly the depth bound round-trips |
| `limits.depth_above` | one level deeper is refused locally as `payload_too_large` |
| `result.terminal_uniqueness` | no second frame follows a terminal result |
| `liveness.ready_after_probes` | the session is still ready after every probe |
| `cancel.sleep_cancelled` | a long operation answers `cancelled` within the cancel grace after `cancel` |
| `deadline.cancel_honoured` | the cancel the host sends at deadline expiry is answered `cancelled` within the grace |
| `descendants.owned` | a spawned grandchild is in the adapter's process group (Linux) or Job (Windows) and the adapter reports no detachment |
| `shutdown.graceful` | `shutdown` is answered by exit code 0 within the grace |
| `path.confinement` | the package directory validates to the same generation digest after the run |

### conformance-probe@1

The checks that exercise behaviour use the SDK-only capability
`conformance-probe@1`, declared by the sidecar
`capabilities/conformance-probe@1.json` with `payload {"operations": [...]}`.
No host adopts it; an adapter implements it to be probed. Operations:

| Operation | Payload | Result |
|---|---|---|
| `echo` | any object | `success` with the same object |
| `progress` | `{"count": n}` | `n` progress reports, then `success {"reported": n}`; the SDK runtime sends the first sixteenth of the negotiated progress limit, and at least its first 16, as frames and paces the rest |
| `sleep` | `{"ms": n}` | `success {"slept_ms": n}`, or `cancelled` when cancelled first |
| `spawn_child` | `{"ms": n}` | starts a child that sleeps `n` ms; `success {"child_pid": pid, "detached": false}` |
| `fail` | `{"reason": r, "message": m}` | `error` with that reason and message |
| `unavailable` | `{}` | `unavailable`/`probe_unavailable` |
| `propose_detached` | `{}` | `success {"process_proposal": {...}}` with `start: "detached"`, for the proposal validator to reject |

A malformed payload is answered `error`/`invalid_payload`.

## Fixture roster

Every fixture package is built from one adapter binary,
`unknown-fixture-adapter`, whose behaviour is selected by `--behavior <name>`
in the bootstrap's `adapter.argv`. The package IDs are opaque and name no
production provider. `fixtures/packages.cmake` assembles them;
`logonomic_cli_addon_sdk_add_fixture_packages(OUTPUT_DIRECTORY <dir> TARGET
<name>)` puts each under `<dir>/<package id>/`.

The positive package `org.example.unknown-fixture` declares
`profile-storage@1`, `conformance-probe@1` and `org.example.unadopted@7`
(declared by sidecar and handshake alike and implemented by no operation: a
host that does not adopt it never sees it, and one that does is answered
`unavailable`/`unknown_operation`), one original package-owned asset
(`assets/fixture-banner.txt`, origin `package`) and one third-party asset
(`assets/mark.svg`) with digest-bound `notices/mark-license.txt` and
`provenance/mark.json`. It passes every check.

Behaviour variants `org.example.unknown-fixture-<behaviour>` share the
positive package's files and differ in `adapter.argv`:

| Behaviour | Reproduces | Fails |
|---|---|---|
| `crash-on-invoke` | the adapter aborts on the first invoke | every check that invokes: `generation_retired`/`adapter_exited` |
| `hang-on-invoke` | the adapter never answers | invoking checks after `deadline` plus `k_cancel_grace_ms`: `timed_out`/`cancel_grace_expired` |
| `hang-on-hello` | no hello | `handshake.completes` with `hello_timeout`, and every session check |
| `double-result` | a second result after every terminal one | the first invoking check retires the session with `duplicate_terminal_result` and every later session check reports it; `result.terminal_uniqueness` names the rule |
| `oversize-frame` | a frame above the effective bound | invoking checks: `oversize_frame` |
| `invalid-utf8-frame` | bytes outside RFC 3629 in a frame | invoking checks: `invalid_utf8` |
| `stdout-garbage` | non-frame bytes on stdout | invoking checks: `frame_violation` |
| `stderr-flood` | a megabyte on stderr | nothing; the diagnostic is truncated and the request succeeds |
| `bidirectional-flood` | valid progress beyond a kernel pipe while the host sends a large invoke | the flood request reaches its ordinary deadline and retires after cancel grace, without transport deadlock |
| `stdin-stall` | a valid handshake followed by an adapter that never reads another stdin byte | queued invokes reach the actual byte bound and retire with `stdin_queue_overflow` |
| `result-then-exit` | a terminal result immediately followed by process exit | nothing; bounded post-exit drain preserves the result |
| `wrong-nonce-hello` | a different nonce echoed | `handshake.completes` with `identity_mismatch` |
| `wrong-core-generation` | an unoffered generation | `handshake.completes` with `identity_mismatch` |
| `sidecar-without-handshake` | `profile-storage@1` sidecar, no handshake pair | `handshake.sidecars_live`; `acknowledge` is `unknown_capability`/`sidecar_handshake_disagreement` |
| `handshake-without-sidecar` | handshake pair `org.example.extra@1`, no sidecar | `handshake.no_undeclared_pairs`; the pair is ignored |
| `orphan-grandchild` | a child that outlives the adapter in the same group | nothing in the suite; the host's custody tests kill it through the group |
| `detach-grandchild` | a child moved to a new session behind a double fork | `descendants.owned` |
| `ignores-cancel` | `sleep` runs to completion after `cancel` | `cancel.sleep_cancelled`, `deadline.cancel_honoured`: `cancel_grace_expired` |
| `slow-shutdown` | exit long after the shutdown grace | `shutdown.graceful`; the host terminates it |
| `slow-result` | every result 1500 ms late | nothing with the suite's deadlines; a host test places a deadline on either side of the delay |
| `profile-omit-echo` | `acknowledge` without `profile_storage` | `profile_storage.acknowledged`: `profile_storage_unacknowledged` |
| `profile-wrong-root` | the root with `/other` appended | `profile_storage.acknowledged`: `profile_storage_mismatch` |
| `profile-default-dir` | a vendor dot-directory below the home directory | `profile_storage.acknowledged`: `profile_storage_mismatch` |
| `profile-wrong-version` | version 2 echoed | `profile_storage.acknowledged`: `profile_storage_version_mismatch` |
| `provider-sessions` | the provider-session surfaces: structured-session@3 and delegated-session@3 with both purposes, interactive-resume@2, lifecycle-events@2 in `events` mode with root turn start, root turn completion, confirmed phase status and attention supported, coordination-delivery@1 with startup delivery supported, and helper-ingress@2, beside presentation@1, launcher-discovery@1, launch-preparation@3 and profile-storage@1; ships the packaged helper under `helpers/` | nothing; every check passes |
| `provider-sessions-assets` | the provider-session preparation surfaces with managed-assets@1 declaring `assets/mark.svg`; its launch, structured, delegated and resume preparations return an asset token and the request's profile-storage root, and its structured proposal uses removable scratch | nothing in the suite; a host's lazy activation, context and materialization cases |
| `provider-sessions-continuous` | `provider-sessions` with lifecycle-events@2 in `continuous` mode, its observations read from the JSON-lines file `.unknown-fixture-events.jsonl` in the session working directory | nothing; every check passes |
| `provider-sessions-classify-hang` | the `provider-sessions` behaviour with the variant flag `--classify-hang` in `adapter.argv`: `classify_event` answers nothing until the host cancels it, then `cancelled` | nothing in the suite; a host's classification-timeout case |
| `provider-sessions-foreign-digest` | the `provider-sessions` behaviour with the variant flag `--resume-foreign-digest` in `adapter.argv`: `prepare_resume` acknowledges the digest of `{"provider_session_id": "<the received id>-other"}` instead of the record it received | nothing in the suite; a host's acknowledgment-mismatch case |
| `provider-initial-prompt` | interactive-initial-prompt@1 beside presentation@1, launcher-discovery@1, launch-preparation@3 and profile-storage@1. It recognizes `unknown-fixture-cli` and `unknown-fixture-cli --prompt <mode>` launchers and prepares each as its own invocation (`prepared: true`, lifecycle and coordination off); a request with `initial_prompt` gets the text appended as the last argument and its digest echoed. Mode `refuse` answers `unavailable`/`initial_prompt_unavailable` instead, `unacknowledged` omits the echo and `mismatched` echoes the digest of other bytes | nothing; a host's initial-prompt cases |

### The provider-session packages

The five provider-session packages share the sidecars of
`fixtures/packages/_sessions_shared/` and carry the packaged helper
`helpers/unknown_fixture_helper`, a Qt-free executable built on the SDK
(`fixtures/unknown_fixture_helper.cpp`). coordination-delivery@1 is static;
the adapter lists it in the handshake, as it lists presentation@1, so that
sidecars and handshake agree pair for pair, and answers any operation on it
`unavailable`/`unknown_operation`. The adapter keeps no state across
invocations beyond the sessions an `observe` window has named. Its answers:

- structured-session@3 `prepare` proposes the helper,
  `helpers/unknown_fixture_helper structured --purpose <purpose>`, with the
  prompt as the stdin payload, `PATH` passed through,
  `UNKNOWN_FIXTURE_PURPOSE` set to the purpose, `session_workspace`,
  `remove_scratch`, incremental inspection and an empty initial
  `protocol_state_base64_chunks` array with `keep_stdin_open: false`. The `provider-sessions-assets` variant instead proposes
  removable scratch and adds its declared asset token. A prompt that is not
  UTF-8 text is `error`/`invalid_utf8`,
  because the stdin payload is a JSON string. Preparation consumes the required
  frozen launcher environment; inspection uses the current chunked state and
  stream request/result codec.
- `inspect` parses the helper's line protocol: `unknown-fixture-helper 1
  <mode> <purpose>` (the header), `presentation <kind>` for the six
  presentation kinds, `result-begin`, the result lines, `result-end`. Every
  other line - the helper's own `prompt-bytes`, `prompt-sha256`, `servers` and
  `artifact` lines as much as vendor noise - is ignored, and a line above 4096
  bytes is noise whatever it says. After the first inspection
  `protocol_state_base64_chunks` carries the adapter's UTF-8 JSON state (the pending partial line and
  the result so far); a state the fixture did not issue is
  `error`/`inspection_invalid`. An incremental inspection answers `complete:
  false`; the final one answers `complete: true` with `result_base64_chunks` (the
  lines between `result-begin` and `result-end`) or one failure reason:
  `process_timed_out`, `process_exited_<code>` (`process_exited_minus_<n>` for
  a negative code), `startup_failed` when no header was seen,
  `incomplete_output` when the result never closed, `result_too_large` above
  32 KiB. `presentation` lists the presentation lines of that increment with
  their counts and is absent when there were none.
- delegated-session@3 `prepare` proposes `delegated --purpose <purpose>
  --artifact-root <private_artifact_root>` with `retain_scratch`, acknowledges
  every `server_id` of the control, and returns `mcp/servers.json`, naming the
  servers' ids, programs and actions, with no credential-bearing environment.
  It also declares the helper-generated `unknown-fixture-helper/summary.json`
  so the host owns that file's cleanup. Connection-bearing requests use the
  admitted relay route and exact connection acknowledgment.
- interactive-resume@2 `prepare_resume` needs `record.provider_session_id`,
  1-256 bytes without control characters (`error`/`resume_record_invalid`
  otherwise), and answers the launcher-program reference with argv `--resume
  <id>`, `session_workspace`, `stdin: inherit`, `retain_scratch`, the patch
  `UNKNOWN_FIXTURE_RESUME=1` and the digest of the record it received. With
  `provider-sessions-assets`, the launch and resume patches also carry its
  declared asset token and the request's exact profile-storage root. With
  `--resume-foreign-digest` the digest is that of `{"provider_session_id":
  "<id>-other"}` instead, and nothing else of the answer changes.
  For host failure-presentation tests, a record with `fixture_failure_reason`
  returns that reason instead of a proposal. `fixture_failure_description`
  supplies its description (empty when absent); `fixture_failure_unavailable`
  selects `unavailable` when true and `error` otherwise. These controls exercise
  package descriptions separately from host-generated diagnostics.
- helper-ingress@2 `classify_event` maps `SessionStart` to a `start`
  transition (`resume` when the body's `source` is `resume`) whose
  observation names the body's `session_id`, `source` and the resume record
  `{"provider_session_id": <session_id>}`, extended with a `revision` field
  when the body names a `resume_revision` string that is control-free and
  within `k_max_lifecycle_label_bytes` (a non-string, over-long or
  control-bearing `resume_revision` is `error`/`observation_invalid`); `Stop`
  to an `event` with
  `awaiting_input`/`confirmed`; `UserPromptSubmit` to an `event` with
  `working`/`confirmed`, both naming the body's `session_id` and `turn_id`
  when present; and every other name, `Ignored` included, to `ignore`. Every
  observation's `provider_event_id` is the evidence digest of the event, so a
  retried delivery names the same observation, and `provider_sequence` is the
  body's non-negative integer `sequence` when present. A `SessionStart`
  without a `session_id` is `error`/`observation_invalid`. Every result echoes
  `{"seen": "<name>"}` as `helper_response`. With `--classify-hang` the
  operation waits for the host's cancel and answers `cancelled`.
- lifecycle-events@2 in `continuous` mode: `observe` names the session by the
  first 16 hex digits of SHA-256 over its working directory, re-reads
  `.unknown-fixture-events.jsonl` there every 100 ms and reports each growth
  as one progress frame (`completed` is the count of complete lines beyond the
  cursor, `note` is `{"available": <count>}`); the window ends with `success`
  and the cursor advanced past the announced lines, or with `cancelled` when
  the host cancels it. A cursor is `<session token>:<line index>`; the empty
  cursor is the most recently observed session from its first line.
  `poll_observations` returns the complete lines from the cursor, each one
  observation object, up to `limit`, with `more` truthful; a line that is not
  a valid observation is `error`/`observation_invalid` naming the line, a
  cursor the fixture did not issue or that names no observed session is
  `error`/`frame_violation`. In `events` mode every lifecycle operation is
  `unavailable`/`unknown_operation`.

The helper's modes:

| Mode | Does | Exit code |
|---|---|---|
| `event <name> [--body-file F] [--key K] [--nonce N]` | `deliver_provider_event` with the body object read from `F` (`{}` without it); prints the committed outcome `{"accepted", "reason"?, "epoch"?, "helper_response"?}` as one line | the client's: 0 committed, 1 transport failure, 2 invalid arguments or environment, 4 pending |
| `set-title <title>` | `set_title` | the client's |
| `changed-files <paths...>` | `show_changed_files` | the client's |
| `show-diff --manifest-file F` | `show_diff` with the file's text | the client's |
| `coordination-mcp <server> [arguments...]`, `delegation-mcp <server> [arguments...]` | starts the server with the inherited environment on the helper's own standard streams - the byte-exact proxy - and waits for it | the server's (128 plus the signal on POSIX); 1 when it cannot start |
| `structured --purpose <purpose>` | reads the prompt on stdin, writes the line protocol | 0 |
| `delegated --purpose <purpose> --artifact-root <dir>` | as `structured`, plus `servers <ids>` from `<dir>/mcp/servers.json` and `artifact unknown-fixture-helper/summary.json`, the summary it writes below `<dir>` | 0; 1 when the summary cannot be written |

A usage error exits 2. The four host-call modes are the `helper_client.h`
client over the `vnmctl` executable named by `LOGONOMIC_VNMCTL_PATH`; a
missing or malformed bound environment exits 2 with the
`invalid_helper_environment` detail on stderr.

Package-file variants keep the conformant behaviour and break the package:

| Variant | Defect | Reason |
|---|---|---|
| `-invalid-bootstrap` | `platforms` missing | `invalid_bootstrap` |
| `-reserved-environment` | `LOGONOMIC_ADDON_INJECTED` in `adapter.environment` | `reserved_environment_variable` |
| `-escaped-path` | index entry `assets/../escaped.txt` | `path_escape` |
| `-invalid-utf8-bootstrap` | a `0xFF` byte inside `release_version` | `invalid_utf8` |
| `-digest-mismatch` | wrong digest on the package-owned asset | `digest_mismatch` |
| `-unsupported-core-generation` | `core_protocol_generations: [2]` | `unsupported_core_generation` |
| `-third-party-ok` | none; the third-party records are intact | validates |
| `-third-party-missing-notice` | third-party entry without `notice` | `third_party_record_invalid` |
| `-third-party-empty-notice` | zero-byte notice file | `third_party_record_invalid` |
| `-third-party-escaped-notice` | `notice` reference `../notices/...` | `third_party_record_invalid` |
| `-third-party-unindexed-notice` | notice file present, its index entry removed | `third_party_record_invalid` |
| `-third-party-digest-mismatch` | wrong digest on the notice entry | `third_party_record_invalid` |

The index defects are produced by `logonomic_cli_addon_index --inject-defect
<name>` with the names `escaped-path`, `digest-mismatch`,
`third-party-missing-notice`, `third-party-escaped-notice`,
`third-party-unindexed-notice`, `third-party-digest-mismatch` and
`third-party-empty-notice`; the last leaves the index faithful to the
zero-byte notice file and only lifts the tool's refusal to write it. The
option exists for fixture assembly; without it the tool refuses to write an
index the validator rejects.

## Scripts

A transcript script is a JSON document of stimulus steps the host simulator
executes against one package. Scripts carry no expectations: the transcript
is the observation, and the same script runs against any fixture.

```json
{
  "script_version": 1,
  "steps": [
    {"op": "invoke", "id": "ack", "capability_id": "profile-storage", "version": 1,
     "operation": "acknowledge", "payload": {}, "deadline_ms": 5000, "profile_storage": true},
    {"op": "wait_result", "id": "ack", "timeout_ms": 8000},
    {"op": "shutdown", "grace_ms": 5000}
  ]
}
```

Steps:

| `op` | Members | Effect |
|---|---|---|
| `invoke` | `id` (script-local name), `capability_id`, `version`, `operation`, `payload` (object, default `{}`), `deadline_ms` (default `k_default_invoke_deadline_ms`), `profile_storage` (boolean; the host allocates the package's root and injects the `profile_storage` member), `bypass_liveness` (boolean; send even for a pair that is not live) | sends the invoke, or refuses it locally |
| `cancel` | `id` | sends `cancel` for that request |
| `wait_result` | `id`, `timeout_ms` | pumps the session until the request has an outcome or the timeout passes |
| `pump_ms` / `sleep_ms` | `ms` | pumps the session for that long, observing whatever arrives |
| `shutdown` | `grace_ms` (default `k_shutdown_grace_ms`) | sends `shutdown`, waits for exit, terminates and kills on expiry |
| `wait_exit` | `timeout_ms` | pumps until the adapter has exited or the timeout passes |

The five scripts in `fixtures/scripts/` - `acknowledge.json`,
`cancel_during_hang.json`, `crash.json`, `double_result.json`,
`shutdown.json` - are shared with Logonomic's broker tests, which execute the
same steps through the production broker and compare transcripts. Five more
drive the provider-session packages: `structured_prepare_inspect.json`
(prepare, an incremental inspection from the initial state, a final one over
a complete helper output), `delegated_prepare.json`, `resume_prepare.json` (a
usable record, then one without a session identifier),
`classify_events.json` (start, resume, `Stop`, `UserPromptSubmit`, `Ignored`)
and, for `provider-sessions-continuous`, `observe_poll.json` (a short window,
then a poll). Their session working directory `/unknown-fixture-session`
exists on no host, so the window observes no events file and the poll returns
none.

```sh
logonomic_cli_addon_host_simulator <package-dir> --script fixtures/scripts/acknowledge.json [--transcript <file>]
```

## Transcripts

A transcript is the normalized record of one adapter session, one line per
frame or host event:

- `> <json>`: a frame the host sent;
- `< <json>`: a frame the adapter sent;
- `! <kind> [<detail>]`: a host-side observation that is not a frame.

Frames are rendered in canonical JSON (members sorted bytewise, no
whitespace) after normalization: every `request_id` becomes `<r#N>` with `N`
the order of first appearance; the generation digest becomes `<digest>`, the
nonce `<nonce>`, and a host-allocated root `<root>` wherever it appears,
including as a prefix of a longer string; `host.product` becomes `<product>`
and `host.version` and `adapter.version` become `<version>`. No timestamp is
recorded. Two hosts that drive the same adapter with the same script record
byte-equal transcripts when their observations agree; `compare_transcripts`
reports the first line that differs.

Events: `adapter_exited exit_code=<n>` or `adapter_exited signal=<n>`,
`violation <reason>`, `retired <reason>`, `killed`, `terminated`,
`hello_timeout`, `deadline_expired <request>`, `cancel_grace_expired
<request>`, `profile_storage acknowledged|<reason>`, `payload_too_large
bytes|depth`, `unknown_capability <pair>`, `unsupported_version <pair>`,
`backpressured`, `stderr_truncated`, `package_invalid <reason>`,
`spawn_failed`.

Retirement is recorded in a fixed order, so that a host and the simulator
driving the same adapter produce the same lines:

- a protocol violation records `! violation <reason>`, `! retired <reason>`
  and `! killed` (the host kills the adapter's process group or Job), and
  then, when the session is pumped further, `! adapter_exited signal=9`
  (Linux) or `! adapter_exited exit_code=1` (Windows);
- an expired cancel grace records `! cancel_grace_expired <request>`,
  `! retired cancel_grace_expired`, `! killed`, and the same exit line when
  pumped further;
- an adapter exit the host did not request records `! adapter_exited
  exit_code=<n>` or `! adapter_exited signal=<n>` and then
  `! retired adapter_exited`;
- an exit after `shutdown` records only `! adapter_exited exit_code=<n>`;
  the generation retires with reason `shutdown` without a `retired` line,
  because the host asked for it. A shutdown the adapter does not honour within
  its grace records `! terminated`, then `! adapter_exited signal=15` (Linux)
  or `! adapter_exited exit_code=1` (Windows) when the adapter dies on
  termination, or `! killed` and the killed exit line when the
  terminate-to-kill interval also expires; no `retired` line follows a
  requested shutdown.

A shutdown of the positive package records exactly five lines: the host
hello, the adapter hello, the capabilities, the shutdown, and
`! adapter_exited exit_code=0`.


### Independent observation fixtures

`org.example.unknown-fixture-provider-{auth,quota,usage,auth-quota,auth-usage,
quota-usage,observations}` covers every nonempty auth-status/quota/usage-scan
subset; each also has an `-accounts` variant. The final name means all three.
They share presentation/discovery/launch preparation and a packaged native
helper, without structured-session sidecars. Account variants additionally admit current accounts@2 discovery and resolution.
Only `org.example.unknown-fixture-provider-observations-accounts` also declares
quota-failure@1, so a classified cause or an unusable explanation reaches the host
there alone; the other quota packages report the same failures neutrally.
Provider-surfaces and provider-accounts fixtures retain empty observation subsets.

`provider-observations` adapter flags select only the declared capabilities:
`--auth-version`, `--quota-version`, `--usage-version`, and `--with-accounts`.
The current fixture defaults are accounts/auth/quota 2, usage 3, launch
preparation 3, structured/delegated sessions 2, interactive resume 2, and helper
ingress 2. Account-refresh remains 1 with the current frozen/chunked probe
grammar. Version 3 structured/delegated/helper variants remain separate current
contracts. The ordinary provider-session fixture owns the current resume cases;
there is no duplicate version-only resume fixture.
`--probe-mode` selects controlled auth/quota failures, a quota failure with a
classified cause (`quota-request-failed`, `quota-timed-out`,
`quota-credentials-expired`, `quota-invalid-response`) or with an explanation
the host cannot use (`quota-unusable-{reason,stage,diagnostic,version}`), a quota
preparation refused with a reason code, a quota observation that reports no quota
(`unavailable`), nonzero helper exit, timeout, wrong account, stale
timestamp, not-logged-in, duplicate usage IDs, partial paging, snapshot
change, repeated cursor, limited historical coverage, cancellation, an
opening usage request that works for two seconds while it reports progress,
scanning one account at a time, or one that reports once and then ignores
cancellation. Packages ending `unknown-auth`, `unknown-quota` and
`unknown-usage` declare an unsupported future version only for the named
sibling: auth uses 3; quota and usage use 4. Other declared siblings retain
their current versions (auth/quota 2, usage 3). The host must isolate the
unsupported capability.

The `observation` helper mode runs through real host process ownership and
returns its inherited selected scope and actual UTC timestamp. Its errors
include fixture diagnostic bytes so host tests can establish that those bytes
do not reach normalized snapshots. Successful quota is 25.5%/80% in two
windows with exact credit balance `12.3400`. Successful usage has two pages,
stable IDs `record-01`/`record-02` and 60/40 tokens.
