# quota@3

`quota` version 3 reports an ordered collection of quota windows. It is optional
and independent of authentication, accounts and usage. Its declaration and
`prepare`/`inspect` requests use the unchanged [quota@2](quota_2.md) process and
probe version 2 grammar. Select result version 3 explicitly when calling the
quota result codec. An unsupported quota version disables only that capability.

A completed `ready` observation requires `windows`, an array containing every
observed quota window in provider order. An empty array is a successful report
of no windows and replaces earlier quota data. Absence is invalid on `ready`;
`not_logged_in`, `unavailable` and `error` carry no quota values. There is no
fixed count, duration, scope, title or identifier requirement. Existing envelope
size bounds apply.

Each window has required finite numeric `used_percent`, preserving native
overage without clamping. Optional members are positive exact integer
`window_minutes`, nonnegative exact integer UTC `resets_at_ms`, `reset_text`,
`label`, and boolean `headline`. Labels describe the provider's observed scope;
the host does not infer duration from a title. `headline` records the provider's
summary selection. False or absent does not remove a window. Unknown metadata
is omitted, never guessed or represented by null.

Scope, plan, credits, integer and label bounds, completion, account
acknowledgment, incremental stdin, and optional [quota-failure@1](quota_failure_1.md)
follow quota@2. Objects have closed member sets. The failure extension remains
independently negotiated.

Optional [quota-cost@1](quota_cost_1.md) carries scoped monetary consumption
beside any completed observation. It does not require or
replace any percentage window and is negotiated independently.

The SDK keeps the quota@2 source fields for existing producers. The explicit
result version selects one representation: version 3 accepts `windows` and
rejects `primary`/`secondary`; version 2 accepts `primary`/`secondary` and rejects
`windows` and `headline`. Hosts normalize accepted version 2 windows into the
same collection used for version 3. `account-refresh@1` continues to embed
quota@2 and does not silently acquire version 3 semantics.
