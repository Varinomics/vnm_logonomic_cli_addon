# Startup observation owned by interactive-resume@2

The complete declaration and resume-preparation input are specified in
[`interactive-resume@2`](interactive_resume_2.md).

This optional record belongs to `interactive-resume@2`; it does not add a
common session field or a provider roster. An absent declaration keeps the
existing resume behavior. An unknown record version or mechanism is unavailable
for this capability. Version 1 of interactive resume does not accept the record.

The version-2 sidecar contains its `environment_names` plus an optional
`startup_observer` object with `version: 1`, `mechanism: "http-json-post"` and
`confirmation: "observed-identity"` or `"prepared-identity"`.

The host allocates a private loopback listener for one resume attempt and offers
the same record with `endpoint` and `authorization`. The endpoint is
`http://127.0.0.1:<port>/events`; authorization is `Bearer ` followed by a fresh
256-bit random token encoded as 64 lowercase hexadecimal characters. Preparation
must echo the complete offered record exactly. A package cannot substitute the
endpoint, authority, version, mechanism or confirmation contract.

The package owns its vendor exporter configuration and header encoding. It uses
the authorization only in the launched process's environment, never in argv,
managed/private artifacts, persistent configuration or diagnostics. The trusted
adapter receives this narrowly scoped authority so the host does not encode a
vendor's environment variable or exporter configuration. Transcripts replace the
token everywhere, including an echoed offer and encoded environment updates.
The host retains the listener until runtime settlement; after confirmation it
acknowledges telemetry without interpreting it, and terminal cleanup closes it.

The listener accepts only authenticated HTTP/1.1 POST at its exact path, with
`Content-Type: application/json`, a single bounded Content-Length and no transfer
encoding. It bounds headers to 16 KiB, bodies to 1 MiB, concurrent connections to
four and request lifetime to five seconds. Only one classification is in flight;
another complete request receives 503 rather than entering an unbounded queue.
The host treats the body as opaque bytes.

The `classify_startup` operation receives `{ "body_base64": "..." }` under
interactive-resume@2. A successful result contains `provider_session_ids`, a
list of at most 64 distinct nonempty UTF-8 strings, each at most 256 UTF-16 code
units and containing no NUL. An empty list means valid telemetry with no startup
identity. The package must report malformed or over-bound evidence truthfully;
it must not truncate a conflicting identity out of the result. The host invokes
the captured package generation with a four-second deadline and cancels pending
classification when the resume attempt fails. It compares the response digest,
current generation nonce and runtime generation before accepting a result.

In `observed-identity` mode, runtime readiness alone is insufficient. Every
reported startup identity must equal the exact requested provider session ID;
an observed mismatch rejects the resume. A matching identity is bound through
the ordinary authoritative provenance path, and completion still requires the
terminal runtime to be ready.

`prepared-identity` preserves providers whose native preparation already proves
the exact immutable resume invocation but whose interactive startup telemetry
is not reliably emitted before the checkpoint. A package may acknowledge this
mode only after an equivalent isolated, no-user-conversation startup-support
probe, admission of effective configuration/exporter ownership, and validation
of the exact launch chain, resume argv and opaque session ID. Merely recognizing
a command or locating saved history is not that proof. The host additionally
requires runtime READY. Conflicting observations still reject; if classification
was admitted before READY, its result settles before prepared-identity acceptance.
This mode is a declared proof contract, not permission to assume a successful
resume or to synthesize a conversation ID.
