# Core protocol

The core protocol is what every adapter speaks with its host regardless of
the capabilities it implements: a handshake, invocations with correlation,
progress, cancellation, results and shutdown. Capability payloads travel
inside it opaquely. `protocol.h` implements the messages and the host-side
sequencing rules; `frame.h` implements the transport framing;
`adapter_runtime.h` implements the adapter side.

## Transport

The host writes to the adapter's `stdin` and reads the adapter's `stdout`.
Frames are UTF-8 JSON objects terminated by a single `\n` (0x0A). A frame's
size is its JSON text plus the terminator and is at most the effective
`max_frame_bytes`. A frame contains no raw 0x0A or 0x0D byte - JSON escaping
carries line breaks inside strings - so `\r\n` is a violation. An empty line
is a violation. Bytes on `stdout` that are not a valid frame are a violation.
`stderr` is free-form diagnostics, drained by the host into a bounded ring and
never interpreted.

JSON is strict RFC 8259 with these bounds: the frame size, `max_json_depth`
levels of nesting (the frame object is level 1), `k_max_json_members` members
per object, `k_max_json_string_bytes` per string, exact `int64` integers,
finite doubles, strict RFC 3629 UTF-8, no byte-order mark, no duplicate
member names.

## Messages of generation 1

The envelope member set of every message is closed: a member the generation
does not define is the violation `unexpected_member`, and the core protocol
does not grow inside a generation - a new core member needs generation 2.
Capability payloads are JSON objects the core never inspects.

Host to adapter:

```json
{"type":"hello","core_protocol_generations":[1],
 "package_id":"org.example.unknown-fixture",
 "generation_digest":"<64 hex>","generation_nonce":"<32 hex>",
 "limits":{"max_frame_bytes":4194304,"max_json_depth":32,
           "max_in_flight_requests":64,"max_progress_frames_per_request":9223372036854775807},
 "host":{"product":"logonomic","version":"<informational>"}}
{"type":"invoke","request_id":"r1","capability_id":"profile-storage","version":1,
 "operation":"acknowledge","payload":{...},"deadline_ms":5000}
{"type":"cancel","request_id":"r1"}
{"type":"shutdown","grace_ms":5000}
```

Adapter to host:

```json
{"type":"hello","core_protocol_generation":1,
 "package_id":"org.example.unknown-fixture",
 "generation_digest":"<64 hex>","generation_nonce":"<32 hex>",
 "limits":{"max_frame_bytes":1048576},
 "adapter":{"name":"unknown-fixture-adapter","version":"1.0.0"}}
{"type":"capabilities","capabilities":[{"capability_id":"profile-storage","version":1}]}
{"type":"progress","request_id":"r1","completed":3,"total":10,"note":"scanning"}
{"type":"result","request_id":"r1","status":"success","payload":{...}}
{"type":"result","request_id":"r1","status":"error","reason":"vendor_unreachable","message":"..."}
```

Member rules: `deadline_ms` is a positive integer and `grace_ms` a
non-negative integer, both at most 2147483647; `host.product`,
`host.version`, `adapter.name` and `adapter.version` are strings without
control characters and are informational; `capabilities` holds at most
`k_max_handshake_capabilities` distinct pairs; `progress.completed` is a
non-negative integer, `progress.total` is optional and not below `completed`,
`progress.note` is optional and at most `k_max_progress_note_bytes`;
`result.message` is optional and at most `k_max_result_message_bytes`.

## Sequencing and rules

1. The host writes `hello` first. The adapter answers with exactly one
   `hello` and then exactly one `capabilities`, in that order, before any other
   frame. `core_protocol_generation` is a member of the offered list;
   `package_id`, `generation_digest` and `generation_nonce` are byte-equal to
   the host's. Any mismatch is `identity_mismatch` and invalidates the
   generation; there is no ready state. The handshake completes within
   `k_hello_deadline_ms`, or the generation fails with `hello_timeout`.
2. `limits` in the adapter hello are optional. Each present value is at most
   the host's; a larger value is a violation. The effective limit is the
   smaller. The host never sends a frame larger than the effective
   `max_frame_bytes` or deeper than the effective `max_json_depth`: an
   invocation whose encoded frame would exceed either fails locally with
   status `error`, reason `payload_too_large`, and is not sent.
3. `capabilities` lists `(capability_id, version)` pairs without duplicates.
   Only pairs that also have a valid sidecar and a host-adopted version become
   live.
4. `invoke` carries a host-issued `request_id`, a live capability pair, an
   operation name, a capability-owned `payload` object and `deadline_ms`, which
   is informational to the adapter; the host enforces it. The value
   `k_no_invoke_deadline_ms` (`INT32_MAX`) means no elapsed-time limit: the host
   waits for a result, cancellation or generation failure without arming a
   request timer. Cancellation and shutdown grace periods still apply.
   For a request known to take long the host may bound the adapter's silence
   instead of the request's duration: every frame the generation sends,
   `progress` or `result` for any of its requests, restarts that request's
   deadline, and `deadline_ms` is then
   the silence the host tolerates. A capability pair or
   operation the adapter does not implement is answered with `result` status
   `unavailable`, reason `unknown_operation`.
5. `progress` is optional and only for a live request. The value
   `9223372036854775807` (`INT64_MAX`) for `max_progress_frames_per_request`
   explicitly means no lifetime frame limit; neither endpoint counts toward
   exhaustion in this mode. Smaller positive values retain the finite limit
   negotiated with older peers; exceeding that count is the violation
   `progress_limit_exceeded`. This keeps the existing hello shape and minimum
   negotiation compatible. The SDK runtime sends an initial burst, then
   coalesces updates at a fixed 10 ms spacing, retaining only the latest pending
   update. Transport queue bounds and backpressure still apply. The protocol
   imposes no time-based rate violation.
6. `cancel` is idempotent. The adapter still emits exactly one terminal
   `result`; `cancelled` is the expected status, `success` and `error` are
   also terminal. A cancel for an unknown or finished request is ignored by
   the adapter.
7. Exactly one terminal `result` per `request_id`. Status vocabulary:
   `success` with a `payload` object and no `reason` or `message`;
   `unavailable`, `cancelled` and `error`, each with a `reason` in the
   reason-code grammar, an optional `message`, and no `payload`. A second
   result for the same ID is `duplicate_terminal_result`; a result or progress
   for an ID that is not live is `impossible_correlation`.
8. `shutdown`: the adapter finishes or cancels its live requests, emitting
   their results, then exits 0 within `grace_ms`. After `grace_ms` the host
   terminates the tree. Adapter exit - end of file on `stdout` - with live
   requests fails them with host status `generation_retired`, reason
   `adapter_exited`.
9. The adapter exits on `stdin` end of file.
10. Any violation - framing, JSON bounds, invalid UTF-8, unknown type, strict
    envelope, ordering, correlation, duplicate result, oversize frame -
    atomically retires the generation, fails its live requests and quarantines
    the package until the host restarts. There is no automatic restart.

## C++ invocation handlers

For an expected operational failure, call `context.fail(status, reason, message)` with
a reason meaningful to the caller. The first terminal result wins. Returning
without one produces `error` with reason `handler_incomplete`.

| Handler output or exception | Runtime outcome |
| --- | --- |
| `succeed` receives a non-object or an object that cannot encode as valid JSON | Request error with reason `handler_invalid_result` |
| A result contains invalid UTF-8 | Request error with reason `invalid_utf8` |
| A result exceeds a negotiated frame or JSON resource limit | Request error with reason `payload_too_large` |
| A progress count exceeds `INT64_MAX` or completed exceeds total | Request error with reason `handler_invalid_progress`. The runtime discards that progress report. |
| `std::invalid_argument` escapes the invocation handler, including an SDK encoder rejecting its arguments | Request error with reason `handler_invalid_argument`. An already completed request keeps its result. |
| Any other exception escapes the invocation handler | The adapter writes a fixed diagnostic to stderr and exits unsuccessfully. The host retires that activation and its outstanding requests. |

Allow a validation exception to escape only when the handler has left shared
state usable by the other requests. An adapter that needs to distinguish expected
I/O or domain errors should handle those exceptions and call `fail`. The runtime
omits exception text from results and diagnostics. If the negotiated limits
cannot carry even the terminal error frame, the adapter reports that condition
to stderr and exits unsuccessfully.

## Provider event failure diagnostics

Provider event validators can carry a value-free diagnostic in the existing
failure `message`. `provider_event_diagnostic.h` owns its independently versioned,
closed encoding; it does not change the result envelope, status or reason code:

```json
{"logonomic_provider_event_diagnostic":{"version":1,"event_name":"Stop",
 "field":"prompt_id","problem":"wrong_type","actual_type":"null",
 "rule":"nonempty_identity"}}
```

The event and field identify the rejected source and member; `problem` names
the validation failure. `actual_type` distinguishes missing, null, boolean,
integer, number, string, array and object. Optional `actual_size` and `limit`
describe UTF-8 bytes or container counts, unless the source-defined `rule` names
a different measurement unit. Native event names retain the provider-event
grammar: nonempty, at most 256 UTF-8 bytes and NUL-free. Field labels and rule
identifiers are bounded and source-defined. Values, object keys, prompt or transcript contents, credentials,
filesystem paths and arbitrary exception text never enter the record.

The encoder stays within the existing 1024-byte message limit. Names whose JSON
escaping would exceed that limit use the mutually exclusive `event_name_base64`
member; decoding preserves the exact original UTF-8 bytes. Ordinary names use
`event_name`. The decoder
rejects unknown versions, vocabulary, members, invalid label text and invalid
measurements. Hosts treat decoded records as developer diagnostics, preserve
their request/session attribution in diagnostic logs, and keep the encoded
record out of ordinary user warning prose. A malformed diagnostic is reported
as invalid metadata without logging its contents. Unknown ordinary messages
remain ordinary provider messages. Diagnostic records do not change existing
classification acceptance, ignored events or capability-local degradation.

## Host-side dispositions

What a caller receives when no adapter result decides:

- `unsupported_version`: the capability is live at other versions only.
- `unknown_capability`: the pair is not live for this generation; the reason
  is `unknown_capability`, or `sidecar_handshake_disagreement` when a sidecar
  declares the pair and the handshake does not.
- `package_unavailable`: the package was not discovered, is invalid,
  quarantined, in transport conflict, or the registry has not settled; the
  reason is the code that made it so.
- `timed_out`: `deadline_ms` expired, or, for a request bounded by silence,
  the generation sent nothing for that long. The host sends `cancel` and, if no
  terminal result arrives within `k_cancel_grace_ms`, retires the generation:
  the expired request receives `timed_out` with reason `cancel_grace_expired`
  and every other live request receives `generation_retired` with the same
  reason.
- `generation_retired`: the adapter exited, violated the protocol, or exceeded
  a host transport buffer during the request. `inbound_queue_overflow` means
  the host could not retain another frame while its receiver was busy. The
  pending-frame admission budget is
  `k_max_stdin_queue_bytes + k_max_link_frame_bytes`, accounting for payload
  capacity and frame records rather than exact allocator/container overhead.
  Stderr retains its separate canonical tail. Overflow stops and reaps only
  that generation independently of receiver event dispatch. It is a storage bound, not a
  time-based rate limit or a claim that the buffered frames were malformed.
- `unavailable` with reason `backpressured`: the effective
  `max_in_flight_requests` is reached; nothing is sent.

Every disposition carries a reason code from the vocabulary below.

## Reason-code vocabulary

Activation and protocol: `identity_mismatch`, `hello_timeout`,
`activation_timeout`, `oversize_frame`, `frame_violation`, `invalid_utf8`,
`unknown_message_type`, `unexpected_member`, `out_of_order_message`,
`impossible_correlation`, `duplicate_terminal_result`,
`progress_limit_exceeded`, `stdin_queue_overflow`, `inbound_queue_overflow`, `adapter_exited`,
`supervisor_lost`, `custodian_lost`, `cancel_grace_expired`, `shutdown`.

Capability: `unsupported_version`, `unknown_capability`,
`sidecar_handshake_disagreement`, `payload_too_large`, `unknown_operation`,
`profile_storage_unacknowledged`, `profile_storage_version_mismatch`,
`profile_storage_mismatch`, `backpressured`.

`frame_violation` covers every framing and envelope defect without a more
specific code: an empty line, a carriage return, an unterminated frame at end
of stream, a frame that is not a JSON object, a missing or mistyped member, a
value outside its grammar, or adapter limits above the host's.
