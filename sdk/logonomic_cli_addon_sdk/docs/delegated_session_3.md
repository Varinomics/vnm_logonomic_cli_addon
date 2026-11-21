# delegated-session@3

Account-selected preparation requests and successful results may carry the
independently versioned `account_context` member specified by
[accounts@2](accounts_2.md). When present, its exact acknowledgment is required;
absence leaves this capability's ordinary request and result grammar unchanged.

Per-task model and reasoning-effort overrides use the optional, independently
negotiated [delegated-model-selection@1](delegated_model_selection_1.md)
`model_selection` member. Its absence preserves saved defaults.

`delegated-session@3` prepares a structured process that additionally receives
task-control MCP servers and private artifacts. It requires
`structured-session@3` live in the same generation and declaring every delegated
purpose: inspection is structured-session@3 `inspect`, and a generation that
declares `delegated-session@3` without that dependency has the pair projected
as not live (`dependency_capability_unavailable`), with delegation reported as
unavailable in its recognitions. `validate_delegated_session_dependency`
decides the rule. The conventions of `structured_session_3.md` apply.

## Sidecar payload

```json
{"purposes": ["read_only_review", "implementation"], "environment_names": []}
```

The purpose vocabulary, bounds and environment-name declaration grammar are
those of [structured-session@3](structured_session_3.md). Preparation requires
the same frozen `launch_environment`; its input cannot come from the adapter's
ambient environment.

## prepare

Request:

```json
{
  "purpose": "implementation",
  "launcher": {"script_body": "unknown-fixture-cli", "runner_key": "direct", "environment_text": ""},
  "launcher_argv": ["unknown-fixture-cli"],
  "launch_environment": {"platform": "linux-x86_64", "effective_environment": {}},
  "working_directory": "/repo",
  "prompt_base64": "...",
  "control": {
    "version": 1,
    "task_id": "task-7f3a",
    "servers": [
      {
        "server_id": "coordination",
        "program": "/opt/logonomic/logonomic_coordination_mcp",
        "actions": ["status", "notice"],
        "environment": {}
      }
    ],
    "private_artifact_root": "/repo/.logonomic/delegation/task-7f3a",
    "connection": {
      "version": 1,
      "relay_program": "/opt/logonomic/logonomic_task_server_relay",
      "descriptor_path": "/repo/.logonomic/delegation/task-7f3a/.logonomic/task-servers.json"
    }
  },
  "declared_assets": [],
  "profile_storage": {"version": 1, "root": "/profile/org.example.unknown-fixture/default"}
}
```

`control.version` is 1. `task_id` is 1-128 bytes
(`k_max_delegated_task_id_bytes`) without control characters. `servers` has
1-8 entries (`k_max_delegated_task_servers`), each with a distinct
`server_id` of 1-64 bytes (`k_max_delegated_server_id_bytes`), an absolute
native `program` path, at most 64 distinct `actions`
(`k_max_delegated_server_actions`, each 1-64 bytes without control
characters) and at most 64 `environment` entries under the capability
environment bounds. `private_artifact_root` is the host-owned absolute
unique per-Task directory the package may populate through the result; the
adapter never writes there itself. Do not add a second task/hash directory.
The host reserves `.logonomic/` for its own files. Adapter artifacts must not
claim that namespace.

### Optional host connection v1

`control.connection`, when present, is exactly `{version: 1, relay_program,
descriptor_path}`. Both paths are bounded absolute native paths under the
existing path grammar. The host supplies the shipped relay executable and
the future host-owned `.logonomic/task-servers.json` descriptor path during
prepare; the descriptor need not exist until launch. Do not read it at prepare.
Configure each server connection as that executable with arguments
`--descriptor <descriptor_path> --server <server_id>`. `servers[].program`
still identifies the original server; it is not a disguised relay command.

With connection present, real server environments remain only in the host's
private original control and actual server launch. The host must redact every
server environment to `{}` in the adapter request copy. SDK request encoding
also performs this redaction without modifying its input object; decoding a
connection-bearing request with nonempty server environments fails. Never
return credentials in process environment, MCP configuration, private
artifacts, inspection results or acknowledgment. Do not fall back to launching
the original server or forwarding its environment.

The host commits a nonsecret v1 descriptor with `version`, `task_id`,
`relay_program` and `servers` entries containing `server_id` and `endpoint`.
The host owns actual server launch, connection authorization and custody;
descriptor metadata or a successful connection acknowledgment is not proof
of tool readiness or successful tool execution. Provider-specific readiness
requirements continue to require their actual evidence.

Absence is representable for preparation that does not require delegated tools, but a consumer
requiring delegated tools must refuse absent or unknown connection locally.
Unknown versions are `unsupported_version`; malformed records fail decoding.
Ordinary launch and unrelated structured capabilities remain available.
The host never retries a refused connection-bearing request without connection.

Success:

```json
{
  "process": {"executable": {"launcher_program": true}, "argv": ["--implement"], "working_directory": "session_workspace",
              "stdin": "payload", "stdin_payload": "...", "start": "attached",
              "environment_names": [], "environment": {}, "assets": [], "cleanup": "remove_scratch"},
  "acknowledgment": {"version": 1, "server_ids": ["coordination"], "connection": {"version": 1}},
  "private_artifacts": [
    {"relative_path": "mcp/servers.json", "content_base64": "..."}
  ],
  "inspection": {"incremental": true, "keep_stdin_open": true, "protocol_state_base64_chunks": []},
  "profile_storage": {"version": 1, "root": "/profile/org.example.unknown-fixture/default"}
}
```

Rules:

- `acknowledgment.version` is 1 and `server_ids` is set-equal to
  `control.servers[].server_id`, else `acknowledgment_mismatch`
  (`validate_delegated_acknowledgment`).
- A connection-bearing request requires `acknowledgment.connection` exactly
  `{version: 1}` after configuring the relay route. Missing, unsolicited or
  mismatched connection acknowledgment is refused before artifact materialization
  or main-process launch. Unknown versions fail decoding. A server-ID
  acknowledgment alone does not prove that the adapter used the relay; the host
  must preserve the expected connection for validation and never synthesize its
  acknowledgment. This marker acknowledges configuration, not readiness.
- `private_artifacts` has at most 8 entries
  (`k_max_delegated_private_artifacts`), each decoding to at most 1 MiB
  (`k_max_delegated_private_artifact_bytes`), each with a distinct
  `relative_path` that is a '/'-separated relative path without empty, `.`
  or `..` segments, `\` or NUL (`is_valid_private_artifact_relative_path`);
  otherwise `private_artifact_invalid`.
- The `process` follows the structured mapping and policy of
  structured-session@3 (`validate_structured_process_proposal`).

The host writes the artifacts below `private_artifact_root` before launch and
scrubs them on any failure. Artifact content never appears in diagnostics,
transcripts or projections, and the SDK's failure details name members, never
values. Declare each file the launched process generates, including summary,
result and temporary publication files; host cleanup tracks declared files,
not arbitrary descendants. The host separately owns its reserved descriptor.

The host allows `k_delegated_prepare_deadline_ms` for preparation. A vendor
installation may extract its native executable before answering a capability probe.

## inspect

Inspection uses [structured-session@3](structured_session_3.md), including its
optional monotonic `close_stdin` action after accepted input has been written.
The adapter still authenticates the final native output; EOF is neither operation
completion nor permission to discard a partial tail. Bounds, process custody,
cancellation, private-artifact cleanup and the original deadline remain intact.

## Reasons

Those of structured-session@3 plus `acknowledgment_mismatch`,
`private_artifact_invalid` and `dependency_capability_unavailable`.

All prepare and inspect payloads obey the aggregate encoded bound and chunked
exchange rules of structured-session@3. Persistent stdin requires incremental
inspection and cannot inherit stdin. The delegated prepare codecs and the
paired structured inspection codecs expose this single representation.
Declaration, handshake and host adoption must intersect on version 3. Host process,
artifact and task authority continue through cancellation and cleanup.
