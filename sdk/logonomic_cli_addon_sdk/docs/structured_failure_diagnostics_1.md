# Structured failure diagnostics

`structured-failure-diagnostics@1` adds optional evidence to a failed
[structured-session@3](structured_session_3.md) inspection, including an
inspection of a delegated process. Its package sidecar payload is `{}`.
It does not replace the inspection's `failure_reason` or change completion,
process custody, authentication, or session-correlation requirements.

The host opts in only after the package declaration and live capability have
been admitted. It adds `"failure_diagnostics": {"version": 1}` to the inspect
request. A missing, malformed, or unsupported optional request leaves the
ordinary inspection request usable and does not request diagnostics. Producers
emit the optional record only when the request opted in.

A complete failed inspection may contain:

```json
{
  "complete": true,
  "protocol_state_base64_chunks": [],
  "failure_reason": "implementation_failed",
  "failure_diagnostics": {
    "version": 1,
    "source": "native_error",
    "message": "The requested model is unavailable for this account.",
    "truncated": false
  }
}
```

The diagnostic record has exactly `version`, `source`, `message`, and
`truncated`. The source is `native_error`, `protocol_error`, or `process_exit`.
The message is nonempty valid UTF-8 without NUL, bounded to 16 KiB
(`limits::k_max_structured_failure_message_bytes`). Newlines and tabs are
permitted. `truncated` is a required Boolean that records whether the producer
shortened the message. Native messages describe errors; producers do not copy
prompts, tool output, or complete protocol records into this field.

The core inspection remains authoritative. Evidence accompanying an incomplete
or successful inspection, an unsupported record version, or a malformed record
is ignored without rejecting a valid core inspection. The host uses admitted
evidence only when the capability was negotiated. A producer must omit evidence
it did not establish; a host must not invent a native cause from a reason code.
