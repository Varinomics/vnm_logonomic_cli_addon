# structured-session@3

The independently negotiated [structured-model-observation@1](model_state_1.md#structured-inspections)
capability may attach the native model observed in an operation's own output to
each `inspect` result. This optional evidence does not change the structured
session version or its completion contract.

Account-selected preparation requests and successful results may carry the
independently versioned `account_context` member specified by
[accounts@2](accounts_2.md). When present, its exact acknowledgment is required;
absence leaves this capability's ordinary request and result grammar unchanged.

`structured-session@3` prepares one-shot structured processes - a review or an
implementation run - that the host's structured runtime executes. The runtime
owns the process; the adapter prepares it and interprets its output. The pair
is live only through sidecar, handshake and host-adoption intersection.
`session_contracts.h` implements every codec and rule of this document.

## Conventions shared by the session contracts

- Member names are snake_case. Member sets are closed (`unexpected_member`),
  members are typed (`frame_violation`), bounds are `payload_too_large`, and
  invalid UTF-8 and NUL are rejected (`invalid_utf8`, `frame_violation`).
- "Canonical bytes" of a JSON value are the SDK's canonical serialization.
- `*_base64` members carry raw bytes in the standard alphabet with padding and
  no whitespace; a bound on such a member applies to the decoded size.
- `launcher` is exactly the typed launcher record of launcher-discovery@1
  (`script_body`, `runner_key`, `environment_text`). `launcher_argv` is the
  host-resolved argv of that launcher: 1-64 entries, each at most 4096 bytes,
  at most 11 KiB in total. `working_directory` (at most 4096 bytes, no NUL),
  `declared_assets` and `profile_storage` follow launch-preparation@3. A
  request carrying `profile_storage` must receive its exact echo or fail with
  that contract's reasons.
- `process` members are those of a process proposal (`process_contract.md`,
  "Process proposals") with the launcher-program reference of that document.
  The adapter proposes; the host validates against the frozen request and
  performs the launch. Adapters never return shell strings.
- Results are `success`, `unavailable`, `cancelled` and `error` per
  `core_protocol.md`. An operation the capability does not define answers
  `unavailable`/`unknown_operation`.
- `prepare` uses `k_structured_prepare_deadline_ms` to allow a native CLI
  capability probe. `inspect` uses `k_default_invoke_deadline_ms`.

## Sidecar payload

```json
{"purposes": ["read_only_review", "implementation"], "environment_names": ["HOME"]}
```

`purposes` has 1-2 distinct entries (`k_max_structured_purposes`) from that
vocabulary. A host request for a purpose the sidecar does not declare is
`unavailable`/`purpose_unavailable`. Quota and authentication status are not
purposes of this capability; they belong to `quota@2` and `auth-status@2`.

## prepare

Request:

```json
{
  "purpose": "read_only_review",
  "launcher": {"script_body": "unknown-fixture-cli", "runner_key": "direct", "environment_text": ""},
  "launcher_argv": ["unknown-fixture-cli"],
  "launch_environment": {"platform": "linux-x86_64", "effective_environment": {"HOME": "/home/example"}},
  "working_directory": "/repo",
  "prompt_base64": "UmV2aWV3IHRoaXMuLi4=",
  "declared_assets": [],
  "profile_storage": {"version": 1, "root": "/profile/org.example.unknown-fixture/default"}
}
```

`environment_names` uses the launcher-discovery allowlist grammar. The required
`launch_environment` is the host's frozen effective launcher environment,
including its platform, baseline and declared input names. The adapter uses
this snapshot rather than its own supervisor's ambient environment. Declaration,
handshake and host adoption must intersect on version 3; the SDK codecs expose
one representation of this contract.

`prompt_base64` decodes to at most `k_max_structured_prompt_bytes` (48 KiB).

Success:

```json
{
  "process": {
    "executable": {"launcher_program": true},
    "argv": ["--review", "--prompt-stdin"],
    "working_directory": "session_workspace",
    "stdin": "payload",
    "stdin_payload": "Review this...",
    "start": "attached",
    "environment_names": ["PATH", "HOME"],
    "environment": {"EXAMPLE_CLI_MODE": "review"},
    "assets": [],
    "cleanup": "remove_scratch"
  },
  "inspection": {"incremental": true, "keep_stdin_open": true, "protocol_state_base64_chunks": []},
  "profile_storage": {"version": 1, "root": "/profile/org.example.unknown-fixture/default"}
}
```

`inspection.incremental` asks the host to inspect stdout as it accumulates.
`keep_stdin_open` requires incremental inspection and cannot inherit stdin.
The host writes initial proposal input, keeps the pipe open, then writes later
inspection input in order. With this flag false, the ordinary stdin policy
applies. Completion, cancellation and teardown release the pipe through the
owned runtime. Protocol state is carried in the required chunk array.

The host maps the proposal onto its structured process specification:

| Proposal | Rule |
|---|---|
| `executable` | the launcher-program reference or a canonical package path; failure is `launcher_program_unavailable` or `path_escape` |
| `argv` | the process arguments, as given |
| `environment_names`, `environment` | `NAME=VALUE` assignments; pass-through names are copied from the frozen launcher environment, capability values win, and a name beginning `LOGONOMIC_` is `reserved_environment_variable` |
| `stdin: payload`, `stdin_payload` | written at start; stdin closes unless `keep_stdin_open` was admitted; initial payload is at most `k_max_structured_stream_bytes` |
| `stdin: closed` | stdin is closed at start |
| `stdin: inherit` | rejected (`launch_patch_invalid`): the runtime has no interactive stdin |
| `working_directory` | `session_workspace` is the launcher-selected directory; `package_root` the generation directory; `scratch_directory` a fresh host-owned directory, removed afterwards when `cleanup` is `remove_scratch`; `profile_storage_root` the request's `profile_storage.root`, `launch_patch_invalid` when the request carried none |
| `assets` | materialized before launch exactly as managed-assets@1 prescribes |
| `start` | must be `attached` (`detached_helper_proposal` otherwise) |

`validate_structured_process_proposal` applies the proposal policy of this
table: the launcher-program reference is allowed, `stdin` may not be
`inherit`, and the general proposal rules hold.

## Chunk representation and bounds

Every `*_base64_chunks` member is an array of canonical padded base64 strings.
Each string decodes to 64 KiB, except the final chunk, which decodes to 1-64 KiB.
An empty byte sequence is represented by `[]`. Empty chunks, noncanonical
base64, excess chunks, and decoded bytes beyond the member's limit are rejected.
Chunk boundaries are byte boundaries and may split a UTF-8 sequence; validity
of protocol state is checked after reassembly.

Protocol state is valid UTF-8 without NUL and contains at most 2 MiB UTF-16
code units (`k_max_exchange_protocol_state_code_units`). This preserves the
native desktop continuation bound, including non-ASCII text. stdout, stdin,
and result each retain the 1 MiB decoded stream bound; stderr retains its
64 KiB decoded bound.

Individual member limits do not promise that all maxima fit simultaneously.
The canonical encoded payload must also fit `k_max_exchange_payload_bytes`
(4 MiB minus 512 bytes reserved for the RPC envelope). Producers validate that
aggregate bound before invocation or response. The host splits accumulated
stdout into ordered inspections that fit alongside the continuation state;
each byte is delivered exactly once. Only the inspection containing the last
stdout bytes carries final process facts. If state and mandatory facts cannot
fit even with empty stdout, the exchange fails with `payload_too_large`.

## inspect

The host invokes `inspect` for structured and delegated processes, incrementally
when requested and once more with final process facts after exit. A request is:

```json
{
  "purpose": "read_only_review",
  "inspection": {
    "protocol_state_base64_chunks": [],
    "stdout_base64_chunks": [],
    "stderr_base64_chunks": [],
    "final": false,
    "timed_out": false,
    "exit_code": 0
  }
}
```

`final` and `timed_out` are required Booleans; `exit_code` is a signed 32-bit
integer. Incremental requests deliver bytes accumulated since the previous
inspection. The final request carries exit facts and the remaining output.

An incomplete response can continue with input:

```json
{"complete": false, "protocol_state_base64_chunks": [], "stdin_base64_chunks": ["bmV4dA=="]}
```

A complete successful response is:

```json
{"complete": true, "protocol_state_base64_chunks": [], "result_base64_chunks": ["b2s="]}
```

- `complete` and `protocol_state_base64_chunks` are required.
- An incomplete result may carry `stdin_base64_chunks` and has neither terminal
  result nor failure. The host writes input in order under the admitted pipe policy.
- A complete result has exactly one of `result_base64_chunks` and
  `failure_reason`, no stdin, and an empty continuation array. Final inspection
  requires a complete response; a violation is `inspection_invalid`.
- `failure_reason` is `hook_approval_required`, `update_required`,
  `authentication_required`, `startup_failed`, or a package-defined
  identifier in the reason-code grammar: 1-64 lowercase letters, digits and
  underscores beginning with a letter.
- `presentation` is optional: `version` 1 and 1-16 `records`
  (`k_max_presentation_records_per_inspection`) whose `kind` is one of
  `review_initialized`, `reasoning`, `analysis_update`, `tool_response`,
  `rate_limit_status`, `result_authenticated`, with `count` at least 1. An
  absent member leaves presentation unavailable; a member that fails this
  strict decode is recorded as invalid with its error, and the inspection
  otherwise stands. `decode_structured_inspect_result` reports the three
  dispositions as `Presentation_state`.

The independently negotiated
[structured-failure-diagnostics@1](structured_failure_diagnostics_1.md)
extension can attach a bounded native or protocol explanation beside a failed
inspection's unchanged reason code.

Member sets are closed for each negotiated capability. Chunking retains raw
stream bytes and does not relax provider authentication, session correlation,
record limits or readiness requirements. Persistent continuation travels in the
protocol; it grants no authority to persist prompts or session credentials.

## Stdin closure

An incomplete inspection result may additionally contain `close_stdin`, a
Boolean. An absent or false value has no effect. True requests EOF after all
accepted stdin bytes, including bytes in this reply, have been written. This
requires incremental inspection with `keep_stdin_open: true`. A completed
result cannot contain this member. The SDK represents absence as false and
omits false on encoding; encoding true for a completed result is an error.

Closure is monotonic. Repeated closure without input is harmless; later
nonempty stdin is an inspection failure. Hosts that split one inspection into
bounded requests preserve the closure boundary when combining replies and
reject later input after an earlier part requested closure. If the final part
already completes final native inspection, the internal closure action is
unnecessary. A nonfinal part cannot use closure to authenticate early success.

Closing stdin does not complete the operation. The host continues ordered,
bounded stdout inspection, then delivers final exit facts and any partial tail.
The adapter retains its continuation until it validates final native output.
Final inspection still requires completion with exactly one result or failure,
empty continuation and no stdin. The original operation deadline is unchanged;
timeout, failed writes, failed process custody and failed exit remain failures.
EOF does not enable the authenticated-completion forced-reap exception.

This contract supports native protocols whose final response can precede
advisory output and whose stdin EOF initiates shutdown. Providers must validate
their own record grammar and session identity through final inspection. No
bytes are discarded, and the host continues rejecting output after completed
inspection.

## Reasons

`purpose_unavailable`, `launcher_program_unavailable`, `launch_patch_invalid`,
`detached_helper_proposal`, `shell_adapter_entry_point`, `path_escape`,
`reserved_environment_variable`, `asset_undeclared`, `payload_too_large`,
`inspection_invalid`, `frame_violation`, `unexpected_member`, and the
profile-storage@1 reasons.
