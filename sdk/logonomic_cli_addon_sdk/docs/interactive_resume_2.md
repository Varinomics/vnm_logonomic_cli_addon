# interactive-resume@2

A package that can resume a conversation in a folder other than its original
one declares [resume-directory@2](resume_directory_2.md); the folder is this
request's ordinary `context.working_directory` either way.

Account-selected preparation requests and successful results may carry the
independently versioned `account_context` member specified by
[accounts@2](accounts_2.md). When present, its exact acknowledgment is required;
absence leaves this capability's ordinary request and result grammar unchanged.

`interactive-resume@2` relaunches an interactive terminal that continues a
stored native provider session. Its sidecar, handshake and host-adoption
intersection select the capability. Absence or an unsupported pair affects
this capability alone. The current session conventions are documented in
[structured-session@3](structured_session_3.md); `session_contracts.h` owns the
resume codec.

[interactive-resume@3](interactive_resume_3.md) adds optional read-only
availability inspection alongside this preparation contract.

## Declaration

The sidecar has required `environment_names` and optional `startup_observer`:

```json
{"environment_names": ["EXAMPLE_CLI_HOME"]}
```

The environment array follows the launcher-discovery grammar: 0–32 unique
names beyond the host's baseline, with reserved authority names forbidden.
The array is required even when empty. Unknown members are errors. The SDK
resume declaration and prepare codecs use version 2 by default.
An optional observer and its exact acknowledgment are specified in
[Startup observation](startup_observer_1.md).

## The resume record

The host keeps one capability-owned record per started provider session, in
its own sidecar keyed by that identity's immutable provenance session token:

```json
{"version": 1, "record": {"provider_session_id": "A"}}
```

`record` is the `resume_record` object that the package's lifecycle-events@2
observation carried at `session_transition: started`, or, when that
observation carried none, the host default
`{"provider_session_id": "<the observation's provider_session_id>"}`. It is
an object of at most `k_max_resume_record_bytes` (16 KiB canonical) and is
opaque to the host beyond that default. `version` is 1.
`encode_resume_record` and `decode_resume_record` implement the envelope.

## prepare_resume

Request:

```json
{
  "launcher": {"script_body": "unknown-fixture-cli", "runner_key": "direct", "environment_text": ""},
  "launcher_argv": ["unknown-fixture-cli"],
  "launch_environment": {"platform": "linux-x86_64", "effective_environment": {"EXAMPLE_CLI_HOME": "/home/example/.example-cli"}},
  "working_directory": "/repo",
  "title_instruction": "...",
  "declared_assets": [],
  "profile_storage": {"version": 1, "root": "/profile/org.example.unknown-fixture/default"},
  "resume": {"version": 1, "record": {"provider_session_id": "A"}}
}
```

`title_instruction` is at most `k_max_title_instruction_bytes` (4096).
The required `launch_environment` is the host's frozen effective selected
launcher environment, including its platform, baseline and declared names,
with overrides and removals already applied. Absent names stay absent; the
adapter cannot replace them with its own ambient values. The public
`Launch_environment` codec enforces platform, member, reserved-name and value
bounds, including Windows case-insensitive alias exclusion. Independently
negotiated [environment-bootstrap@1](environment_bootstrap_1.md) may add its
optional `bootstrap` evidence. An offered declared `startup_observer` travels
with the request and requires an exact result acknowledgment.

Success:

```json
{
  "process": {
    "executable": {"launcher_program": true},
    "argv": ["--resume", "A"],
    "working_directory": "session_workspace",
    "stdin": "inherit",
    "start": "attached",
    "environment_names": [], "environment": {}, "assets": [],
    "cleanup": "retain_scratch"
  },
  "environment": {"set": {"EXAMPLE_CLI_RESUME": "1"}, "unset": []},
  "acknowledgment": {"version": 1, "record_digest": "<64 lowercase hex>"},
  "profile_storage": {"version": 1, "root": "/profile/org.example.unknown-fixture/default"}
}
```

Rules:

- The host waits `k_resume_prepare_deadline_ms` (limits.md) for the answer,
  including any installed-CLI probe needed to acknowledge the exact conversation.
- `process.working_directory` is `session_workspace`, `stdin` is `inherit`
  and `start` is `attached` (`launch_patch_invalid`,
  `detached_helper_proposal`); `validate_resume_process_proposal` applies
  them with the general proposal rules and the launcher-program reference.
- `process.environment_names`, `process.environment`, and `process.assets`
  must be empty (`launch_patch_invalid`). The terminal applies the launch
  patch to its environment; process-level environment and asset requests
  have no resume consumer. Asset references belong in the patch's values.
- `environment` uses the current shared declarative `set`/`unset` patch
  fields and their environment-name, value, overlap and asset-token rules
  documented in [launch preparation](launch_preparation_3.md). Resume owns
  these fields directly; it does not carry a launch `disposition` or invoke a
  different preparation capability. A defect is `launch_patch_invalid`.
- `record_digest` is SHA-256 over the canonical bytes of the `record` the host
  sent (`resume_record_digest_hex`). A mismatch is `acknowledgment_mismatch`
  (`validate_resume_acknowledgment`) and the resume fails as no longer
  resumable.

The result does not echo `launch_environment`. It exactly acknowledges every
offered account, profile-storage and startup-observer record; an unsolicited
observer or absent/changed acknowledgment refuses the resume.

The host maps the result onto its interactive launch: the argv is the
resolved executable followed by `process.argv`, the environment updates and
unsets come from the patch, and the launch is acknowledged when the digest
matches. The lifecycle profile and coordination delivery of the resumed
session come from the bound package's adopted lifecycle-events and
coordination-delivery@1 declarations, not from this payload.

## Derived provider identity

A resume routes by the package binding of the session, never by a label. The
binding names the package by its canonical package ID and gives every bound
session one internal provenance identity, `derived_provider_id(package_id)`
(`provider_identity.h`): `p` followed by the first 63 lowercase hex digits of
SHA-256 over the length-delimited UTF-8 package ID, the bytes
`decimal(len) ":" package_id`. For `org.example.unknown-fixture` that is
`p1d191838f9eedd48f5a10b85f78d4cf274612b66874c6a8dca5f8180f799c74`. The
result is 64 characters within the session-provenance `provider_id` grammar,
and `is_derived_provider_id` recognizes the form. Two packages can never share
one derived identity: at discovery the bytewise-later package of a colliding
pair is unavailable with `provider_id_collision`, and a binding whose derived
identity is already recorded for a different package fails with the same
reason and leaves the session unbound.

## Reasons

`launch_patch_invalid`, `launcher_program_unavailable`,
`acknowledgment_mismatch`, `resume_record_invalid` (the adapter cannot use
the record: its members are unusable), `payload_too_large`,
`frame_violation`, `unexpected_member`, `provider_id_collision`, plus the
launch-preparation@3 and profile-storage@1 reasons.
