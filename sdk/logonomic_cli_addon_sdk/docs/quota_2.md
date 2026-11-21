# quota@2

`quota` version 2 is optional and independent of auth-status, accounts,
usage-scan and structured-session. Its sidecar payload is `{"environment_names": [...]}`.
`prepare` and `inspect` use the frozen process request/result, custody, exact
account acknowledgment, completion and observation-scope rules described in
[auth-status@2](auth_status_2.md). The operations are independent invocations;
failure or absence of one probe must not erase its sibling's accepted result.

A completed result's `observation` contains `scope`, `state` and the optional
fields below. `state` is `ready`, `not_logged_in`, `unavailable` or `error`.
An unavailable or failed observation contains no quota values. There are no raw output or
diagnostic members.

- `primary`, `secondary`: optional independent windows. A present window has
  required finite numeric `used_percent`, preserving native overage without
  clamping, and may have positive exact integer `window_minutes`, nonnegative exact integer UTC
  `resets_at_ms`, `reset_text`, and `label`.
- `plan`: optional display label.
- `reset_credits`: optional nonnegative exact integer credit count.
- `credits_balance`: optional exact decimal display string, at most 64 ASCII
  bytes. Its grammar is `-?(0|[1-9][0-9]*)(\.[0-9]+)?`; no exponent,
  whitespace, currency conversion or floating-point rounding. Trailing
  fractional zeros are retained.

Labels are nonempty UTF-8 strings of at most 128 bytes without control
characters. All integers have maximum `9007199254740991`. Optional members
are omitted, not null. Omission means unavailable metadata; an observed zero
percentage or credit balance is not omission. Every object has a closed member
set. The host uses normalized reset time and scope to build its existing
desktop/companion quota representation; it does not project process streams.

While incomplete, the result may include nonempty `stdin_base64` containing the
next protocol message, bounded to 1 MiB decoded. A completed observation cannot
write stdin. This current probe response uses one base64 string; the inspection
request still uses the chunked stream grammar described in auth-status@2.

`not_logged_in` is a completed observation with no quota values. It does not
require an auth-status capability. Optional `failure_reason` is exactly
`authentication_required`, and is valid only beside a completed `error`
observation. This carries explicit authentication loss without exposing raw
diagnostics. Any other known cause of a failed observation travels in the
independently negotiated [quota-failure@1](quota_failure_1.md) member, which this
grammar never requires. Timeouts use the host's observed timeout fact and the
package presentation name; an adapter cannot manufacture timeout evidence.

Optional [quota-cost@1](quota_cost_1.md) carries scoped monetary consumption
beside any completed observation. Its independent negotiation
and failure handling leave this quota grammar unchanged.
