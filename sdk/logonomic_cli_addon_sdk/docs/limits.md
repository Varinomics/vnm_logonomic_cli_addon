# Limits

The [shared limits header](../include/logonomic_cli_addon/limits.h) gives the
numeric defaults for package validation, custody and transport. Its constants
are grouped by purpose and spell out their units. Use the header from the SDK
revision that accompanies the host being targeted. A capability can also define
its own limits. For example, the [session-wake header](../include/logonomic_cli_addon/session_wake.h)
gives the canonical JSON byte budget for a binding and the UTF-8 byte budgets
for identities and wake instructions.

## Custody bounds

Discovery limits how many candidate names it retains per root and how many
generations it admits. Each package has independent limits on indexed file
count, path depth, file and index bytes, sidecar count and bytes, and total
generation bytes. The aggregate custody limit bounds disk consumption across
generations. A file fitting its individual limit can still exceed the package's
remaining budget. See the [package contract](package_contract.md) for the
identifier grammars, layout and rejection reasons.

## Protocol limits

Read the host's `hello.limits` when opening a generation. The adapter can
advertise smaller limits. The effective maximum for each field is inclusive.
`max_frame_bytes` bounds an encoded frame, `max_json_depth` bounds nesting,
and `max_in_flight_requests` bounds active requests. A finite
`max_progress_frames_per_request` applies for the request's entire lifetime.
The `k_unlimited_progress_frames` sentinel permits progress throughout a
long-running request while the runtime paces and coalesces updates. Separate
bounds apply to strings and members per object. The frame size bounds total
array elements and document values. See the [core protocol](core_protocol.md)
for framing, backpressure and failure behavior.

### Native and composed-envelope proof

Native Windows command-line and environment limits count UTF-16 units.
The `k_max_windows_native_command_line_units` and
`k_max_windows_environment_value_units` constants exclude the terminating NUL.
The process launchers check converted values and the entire quoted command
line, including the executable, argument separators and escaping.

Starting an adapter also requires room for its private host messages. The
custodian start envelope includes paths, arguments, environment entries and
inherited-handle data. The supervisor request includes the executable both as
a path and as `argv[0]`, plus generation and scratch paths in the environment.
JSON escaping can expand a control byte to six bytes. The desktop's custody
tests serialize these worst cases and compare them with
`k_max_control_message_bytes` and `k_max_supervisor_request_bytes`. On Linux
the control transport also checks the effective socket buffers after kernel
clamping and doubling. These private limits are separate from the public
adapter frame budget.

## Capability payloads

Capability documents specify what their limits measure. For example,
[helper ingress](helper_ingress_2.md) measures the host's compact encoding of
`event.body` against `k_max_provider_event_bytes`. The wrapper also bounds raw
stdin before parsing. An oversized raw request exits with status 2. A request
that parses still goes through the host's compact-byte check. A committed
helper response is measured using the host's compact encoding before journal
accounting. The SDK's canonical JSON encoding can use a different byte count.

[Model catalog](model_catalog_1.md) bounds the entry count and the encoded
snapshot separately. Producers leave room for their native framing and report
`total_count` and `truncated` when only part of the inventory fits.
[Presentation](presentation_1.md) and the [SVG subset](svg_subset.md) limit
both source bytes and decoded or expanded artwork. For sessions, the
[structured exchange](structured_session_3.md) measures stream bytes and
preserves the continuation state's UTF-16 limit when transporting UTF-8 chunks.

## Deadlines

The [process contract](process_contract.md#deadlines) identifies what starts
and ends each timer. The common defaults in the shared header use milliseconds.
The host sends the applicable deadline with each invocation. The
`k_no_invoke_deadline_ms` sentinel leaves the request timer unarmed.
Cancellation, shutdown and generation failure still end the invocation.
