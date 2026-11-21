# Quota failure explanation

`quota-failure@1` is optional presentation evidence attached to a failed
[quota@2](quota_2.md) or [quota@3](quota_3.md) observation. A quota@2 observation
can also appear inside the quota member of an [account-refresh@1](account_refresh_1.md)
result. Its sidecar payload is `{}`.
It replaces nothing in the selected quota version: a package that does not
declare it, or a host that does not adopt it, exchanges exactly that quota grammar, where a failed
observation is neutral and only `failure_reason` `authentication_required` names
a cause.

The host negotiates this capability independently and opts in by adding
`"quota_failure": {"version": 1}` to the quota or account-refresh `inspect`
request. A missing, malformed, or unsupported optional request leaves the
ordinary quota request usable. A package emits the member below only when it
declared the capability and the request opted in.

A completed result whose observation `state` is `error` may add this member
beside `observation`:

```json
{
  "quota_failure": {
    "version": 1,
    "reason": "quota_request_failed",
    "diagnostic": {"stage": "http_response", "code": 503}
  }
}
```

`reason` is one of `authentication_required`, `quota_credentials_expired`,
`quota_request_rejected`, `quota_request_failed`, `quota_invalid_response`,
`quota_timed_out`, `quota_process_failed`, and `quota_registry_failed`. The
member names a known failure without exposing native text. Authentication loss
is distinct from a failed request or a malformed reply. The SDK supplies the
common failure producer, the reason parser, and the safe presentation
descriptions.

`diagnostic` is optional developer evidence: an object with exactly `stage` and
integer `code`. Stages are `http_response`, `process_start`, `process_exit`, and
`registry_read`. HTTP codes are in 100..599; native codes are in
-2147483648..4294967295 to retain signed and unsigned native 32-bit values. The
host never presents it as a description. No raw process, file, or network
contents are permitted.

Missing, unknown-version, malformed, unknown-reason, unknown-stage, or
misplaced evidence leaves the quota observation and any sibling authentication
result intact and supplies no cause: the failure stays neutral. The host does
not echo unrecognized values. A producer must omit evidence it did not
establish.
