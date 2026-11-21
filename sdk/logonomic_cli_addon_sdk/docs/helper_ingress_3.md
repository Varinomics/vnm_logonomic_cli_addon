# helper-ingress@3

This independently negotiated helper capability retains the authenticated
provider-event lane, stateless request/ledger grammar, ordinary classifications,
helper-client behavior, privacy and custody rules of
[helper-ingress@2](helper_ingress_2.md). Its sidecar is `{}` and it requires live
lifecycle-events@2 in the same generation.

Version 3 uses the optional subagent record below. It does not accept the
version-2 `lifecycle_degradation` record: subagent loss and whole-lifecycle loss
remain different contracts. The ordinary result bound remains 16 observations.
Only a valid complete `batch` may carry up to 32, including any overlaid root
fact; a thirty-third root observation is not admitted.

Version 3 additionally permits one optional, independently versioned
`subagent_evidence` record. The record always carries a bounded, NUL-free
`provider_session_id` and a `kind`:

```json
"subagent_evidence": {
  "version": 1,
  "kind": "batch",
  "provider_session_id": "A",
  "batch_id": "provider-step-12",
  "child_ids": ["child-a", "child-b"]
}
```

A `batch` is valid beside an `event` or `transition` classification. Its
nonempty `batch_id` and complete, nonempty, unique `child_ids` are bounded
lifecycle identities. The result must contain exactly that child set as
newly-applied `subagent_transition: started` observations for the same live
provider session. The host sorts the set before fingerprinting it with its
session generation and epoch. An exact replay is neutral; a different set with
the same batch identity degrades and reconciles only subagents before it
publishes any child. This is the sole record that uses the 32-observation bound.

```json
"subagent_evidence": {
  "version": 1,
  "kind": "loss",
  "provider_session_id": "A",
  "reason_code": "invalid_subagent_observation"
}
```

A `loss` is valid beside an `event` or `transition` classification and records
one bounded, NUL-free `reason_code`. The ordinary root observation still
applies. The host degrades only the declared subagent feature, clears retained
subagent batch facts, and stops every child it still owns. It neither ends nor
retires the root Session or its epoch.

```json
"subagent_evidence": {
  "version": 1,
  "kind": "reconcile",
  "provider_session_id": "A",
  "provider_event_id": "stop-identity"
}
```

A `reconcile` is valid only for an `event` or `transition` result containing
exactly one newly-applied, matching root `awaiting_input` observation. The host commits
that root fact and then stops every currently active child. It does not end the
root Session or change its epoch. A package uses the provider's deterministic
semantic Stop identity here; transport retry keys remain transport-only.

Version 2 rejects `subagent_evidence` as an unexpected member. Unknown
record versions, unknown kinds, malformed records, and records that do not
match the classified observations reject that result without silently degrading
other lifecycle features. Packages emit the record only under a negotiated
version 3 invocation.
