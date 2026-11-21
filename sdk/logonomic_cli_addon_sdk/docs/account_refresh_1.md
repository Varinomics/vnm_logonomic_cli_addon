# Shared account refresh

`account-refresh@1` is an optional, independently negotiated capability for
one fresh native quota and authentication cycle per source profile. Its sidecar
uses the `launcher-discovery@1` declaration grammar (`environment_names`).
Absence or an unsupported version leaves the separate `quota` and `auth-status`
capabilities usable. Their versions and completion rules do not change.
An adapter may also decline `prepare` with `unavailable` for a selected context,
before proposing any native process. The host then uses those separate
capabilities. Other preparation failures remain failures; once a native cycle
has started, failure never triggers an implicit second process.

## Preparation and inspection

The operations are `prepare` and `inspect`. The request codecs in
`account_refresh.h` reuse the probe request grammar with the frozen effective
launcher environment and account context defined by probe version 2. A prepare
result uses the shared `Structured_prepare_result` grammar and must select
incremental inspection. This is grammar reuse, not a dependency on adopting
`structured-session@3` or changing its version.

The host selects the launcher, profile, working directory and effective
environment once. The provider starts one fresh native instance, performs its
native initialization, obtains quota, then observes authentication in that same
instance. Native configuration loading, credential renewal and account/workspace
resynchronization remain authoritative. The contract does not promise a frozen
filesystem snapshot. It does not authorize persistent native processes,
cross-profile reuse, host-side credential interpretation, or implicit retry in a
second process after the cycle starts.

The host requests optional identity with the independently negotiated
`current-account@1` member in the prepare and inspect requests. Missing or
unsupported identity evidence cannot invalidate ordinary authentication.
The independently negotiated [quota-failure@1](quota_failure_1.md) member of the
inspect request likewise lets a failed quota observation carry its cause; unusable
cause evidence cannot invalidate the quota or authentication observations.
The independently negotiated [quota-cost@1](quota_cost_1.md) member likewise
allows scoped monetary evidence beside any completed quota result;
unusable cost evidence leaves quota and authentication intact.

An inspect response contains `complete`, `protocol_state`, and optional `quota`
and `auth` members:

- `complete` must equal the request's `final` native-inspection fact. Observing
  quota does not complete, reap, or release the native operation.
- `protocol_state` uses the existing bounded opaque state grammar.
- `quota`, when present, is one completed `Quota_inspect_result` in the quota
  version 2 grammar, with no stdin write. It appears at most once during a
  cycle, including final inspection. It may report ready, not logged in,
  unavailable, or failed quota.
- `auth` is a completed `Auth_inspect_result`. It is required at final
  inspection and forbidden before it. Optional current-account evidence remains
  inside this authentication result, subject to its existing scope rules.

Each observation must name the selected source account and carry its own
observation and freshness times. The host rejects conflicting, duplicate,
malformed, mismatched, cancelled, or superseded results. A final result may omit
quota already sent; a cycle ending without a quota observation produces an
explicit failed quota disposition.

## Publication and native ownership

The host may publish a valid quota observation before native exit. Later
authentication failure, timeout, malformed authentication evidence, or unsuccessful
native exit does not revoke that accepted quota for the still-current source.
Normal source/profile invalidation still applies. Authenticated identity requires
valid final evidence and successful native completion; failure supplies no
current identity and clears previous grouping.

The first terminal quota observation admits the authentication stage. Each stage
keeps the existing observation deadline; activity and resynchronization cannot
extend it indefinitely. The native owner retains cancellation, stream bounds,
generation custody and descendant cleanup through common retirement. Publishing
quota does not admit a new cycle for that source. Later refresh requests wait for
the previous owner's retirement and then start a fresh instance.

The native helper must retain raw credential responses, malformed native records,
native errors and stderr within its private boundary. Only bounded sanitized
quota and authentication observations leave that boundary. Every source profile
continues to be observed independently; this capability does not merge histories
or remove work for duplicate-account presentation.
