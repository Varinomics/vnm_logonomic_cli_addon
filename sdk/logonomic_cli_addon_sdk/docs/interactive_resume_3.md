# interactive-resume@3

This optional capability adds the read-only `inspect_resume` operation. A
package declares it alongside [interactive-resume@2](interactive_resume_2.md),
with an identical declaration payload. Version 2 remains the preparation and
startup-observation contract. The stored opaque resume envelope stays at
version 1. Hosts and packages that negotiate only version 2 retain their
existing behavior.

[interactive-resume@4](interactive_resume_4.md) adds optional conversation-content
inspection. Version 3 requests and responses retain the contract below; hosts
negotiate version 4 before requesting content evidence.

## Inspection request

`Resume_inspect_request` in `session_contracts.h` uses the version 2 resolved
launch context and the exact stored `resume` envelope:

```json
{
  "launcher": {"script_body": "example-cli", "runner_key": "direct", "environment_text": ""},
  "launcher_argv": ["example-cli"],
  "working_directory": "/repo",
  "declared_assets": [],
  "resume": {"version": 1, "record": {"provider_session_id": "conversation-a"}},
  "launch_environment": {
    "platform": "linux-x86_64",
    "effective_environment": {"HOME": "/home/alex", "EXAMPLE_CLI_HOME": "/home/alex/.example-cli"}
  }
}
```

`profile_storage` and `account_context` are optional and use their existing
codecs. They identify the read-only resolved profile and selected account;
inspection does not allocate profile storage. `declared_assets` must be empty.
There is no title instruction, startup observer, wake offer, invocation
replacement, managed configuration preparation, hook installation or process
proposal. Unknown members are rejected. All context and record bounds remain
those of version 2 preparation.

The provider must use the request's effective environment, account, profile,
working directory and exact opaque record. It must not consult ambient values
as substitutes, retarget another conversation, create or change native state,
or launch an interactive session. Inspection and explicit preparation share
the provider's exact-conversation availability predicate; preparation evaluates
it afresh before performing its side effects.

## Inspection result

```json
{"availability": "available", "request_digest": "<64 lowercase hexadecimal characters>"}
```

`availability` is one of:

- `available`: the exact requested conversation is present and resumable.
- `absent`: the provider definitively established that the exact conversation
  is absent or has no resumable native content within the resolved scope.
- `unknown`: inspection could not establish either conclusion, including
  inaccessible storage, incomplete evidence or unsupported native content.

An unavailable operation, transport error, timeout or malformed result is also
unknown to the caller. None establishes absence. Only a valid, acknowledged
`absent` answer may withhold a remembered row. It neither deletes the record nor
changes ownership or dismissal history.

`request_digest` is SHA-256 over the canonical JSON bytes produced by
`encode_resume_inspect_request`. It binds the complete request, including the
opaque record and resolved context. Use `resume_inspect_request_digest_hex` and
`validate_resume_inspection`; a different digest invalidates every disposition.
No separate profile or account acknowledgment is needed for this operation.

Hosts may cache answers using package generation, exact token/record and the
complete resolved context, and must refresh because native content can change
independently. Logonomic inspects asynchronously with at most four calls in
flight and FIFO admission. A result is fresh for 30 seconds; a five-second
timer requests refresh of expired entries. Each dispatched call has a
five-second deadline. Entries unused for two minutes are removed. Lifecycle
record publication invalidates the corresponding token, and any changed
generation, record, account, profile or environment selects another cache key.
Pending refreshes retain the last answer; a completed unknown result replaces
it. Explicit resume always uses fresh provider preparation.
