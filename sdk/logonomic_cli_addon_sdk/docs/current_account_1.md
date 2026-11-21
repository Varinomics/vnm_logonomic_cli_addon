# Current account identity

`current-account@1` is optional presentation evidence attached to an existing
`auth-status` observation or the final authentication result of an independently
negotiated [account-refresh@1](account_refresh_1.md) cycle. Its sidecar payload is
`{}`. It does not replace an
`accounts` key, profile path, session binding, or recorded-usage identity.

The host negotiates this capability independently and opts in by adding
`"current_account": {"version": 1}` to the auth `prepare` and `inspect`
requests. A missing, malformed, or unsupported optional request leaves the
ordinary auth request usable without current identity.

A completed, authenticated result may add this top-level member:

```json
{
  "current_account": {
    "version": 1,
    "scope": {
      "source_account_key": "profile-source",
      "observed_at_ms": 1700000000000,
      "fresh_for_ms": 0
    },
    "identity_key": "opaque-provider-owned-identity",
    "is_default_profile": false
  }
}
```

The scope must exactly equal the accompanying auth observation's source and
times. The source key must be present. `identity_key` follows the adapter
account-key byte grammar, but belongs to a separate namespace. Within its
package it identifies the currently observed provider account, including the
selected workspace and user when those distinguish accounts. Equal email
addresses alone are insufficient. The provider must omit uncertain evidence.

Only the opaque key and profile preference leave the provider's private native
helper. Tokens and raw credential responses, including malformed responses,
errors, and stderr, must remain inside that boundary. Providers own native
authentication and configuration resolution; hosts do not infer identity from
credential files or reproduce native configuration precedence.

Missing, invalid, unknown-version, nonauthenticated, or mismatched optional
evidence leaves valid auth completion intact and supplies no current identity.
The host admits it only for the selected source and current completed operation;
it cannot revive a superseded source. A failed or unavailable current observation
removes prior grouping. Positive freshness expires the grouping; zero retains a
successful observation until its next replacement or invalidation.

Presentation prefers a nondefault profile over a default profile, then the
alphabetically first profile among named aliases. Other launchers can display a
duplicate marker. Every source still owns its original sessions and usage and
continues its own observations. The capability grants no permission to merge
histories, delete profiles, or skip another source's authentication work.

For the separate `quota` and `auth-status` capabilities, an implementation that
needs longer native work for identity must own that work
in the existing auth observation. It must preserve a quota operation's own
completion and native cleanup. Logonomic runs opted-in auth after that refresh's
quota has completed and settled, and serializes later work for the same launcher.
