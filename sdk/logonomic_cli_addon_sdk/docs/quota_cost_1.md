# Monetary observations

`quota-cost@1` is optional monetary evidence beside a completed
[quota@2](quota_2.md) or [quota@3](quota_3.md) observation. Its sidecar payload
is `{}`. It does not change either quota grammar or require a percentage
window. The same extension may accompany the embedded quota@2 observation in
[account-refresh@1](account_refresh_1.md).

The host negotiates the capability independently and adds
`"quota_cost": {"version": 1}` to the quota or account-refresh `inspect`
request. Missing, malformed, or unsupported negotiation leaves the ordinary
request usable without opting in. A package emits the result member only when
it declares the capability and the request opts in.

A completed result may carry `quota_cost` beside `observation`, independently
of the quota state:

```json
{
  "quota_cost": {
    "version": 1,
    "amount": 6.6474052,
    "currency": "USD",
    "estimated": true,
    "label": "Recorded sessions"
  }
}
```

`amount` is a finite, nonnegative numeric consumption value in `currency`, a
three-uppercase-ASCII-letter currency code. `estimated` is required: true for
local estimates, false only for a source that reports actual billed spend.
`label` describes the source's actual coverage, such as recorded local sessions
or an identified billing period. It uses the quota label rules: nonempty valid
UTF-8, at most 128 bytes, with no control characters. The label must not imply
account-wide, current-period, or historical-principal coverage that the source
does not establish. This evidence inherits the accompanying observation's
account scope and observation/freshness times.

Optional numeric `available` is a finite, nonnegative remaining available
amount in the same currency, supplied only when the source establishes it for
the same account and coverage. It is not a subscription percentage, spend cap,
or amount inferred from token counts. Hosts can show consumption / available
when both are known. Monetary observations do not supply a progress fraction.

Absent evidence means no known monetary observation, never zero. An explicit
source-reported zero is preserved. A cost source read, parse, or consistency
failure has a distinct, mutually exclusive form:

```json
{"quota_cost": {"version": 1, "error": true}}
```

This form conveys no amount and no raw diagnostic text. It cannot invalidate
healthy quota windows or sibling authentication. Ordinary absence of a local
source is not an error.

Missing, unknown-version, malformed, or misplaced evidence never invalidates
the core quota result. An unusable supplied member produces `cost_ignored`;
recognized error evidence produces `cost_failed`. Neither supplies a monetary
value. Cost evidence on an incomplete result is ignored, and the SDK does not
emit it without a completed observation. The ordinary account-scope validation
still applies. Recorded historical cost can accompany failed quota or
not-logged-in observations: it establishes neither successful quota retrieval
nor current authentication, and must not change or conceal those statuses.
Every object in this extension has a closed member set; amount and error forms
cannot be mixed. No source price table, billing period, balance, or account
classification is inferred by the SDK.
