# usage-cross-source-event@1

This optional capability lets a provider attest that one `usage-scan@3` record
is a complete atomic token event. It does not identify a provider principal,
and it does not change source-profile custody. Its sidecar declaration is `{}`.
The provider advertises the pair in its handshake. There is no separate
operation or shared usage schema version change.

A proved record carries an independently versioned presence tag:

```json
"capabilities": {
  "usage-cross-source-event": {
    "version": 1,
    "namespace": "openai.codex.completed-turn",
    "event_id": "<rollout-uuid>:<turn-uuid>"
  }
}
```

`namespace` is a stable provider-owned authority domain, independent of the
profile path and account key. Within that domain, equal event IDs identify the
same event across source profiles; distinct IDs identify disjoint events. The
proof attests that the enclosing record's `total_tokens` is the complete token
amount of that one event. A source may expose only a subset of its history, so
`aggregate_evidence:partial` can coexist with a complete event proof.

The namespace is 1–128 bytes of lowercase ASCII letters, digits, dot, and
hyphen. The event ID is 1–256 UTF-8 bytes without control characters. Unknown
versions, malformed proofs, and absent proof remain local to this capability:
the base usage record still decodes, but contributes no cross-source proof.
The host must consume only a recognized proof from a live declared pair and
bind it to the accepted source snapshot and exact record amount. It must not
derive an event ID from a record ID, account key, path, or matching totals.
Every valid source usage record counts even without proof. The combined total
deduplicates only positively established copies; unresolved overlap or
conflicting proof makes that combined figure estimated. Neither condition is
a detailed-scan error or a reason to ask the user to retry.

Codex emits this proof only for a completed turn with a known pre-turn
cumulative baseline, valid observed total samples, no counter reset or
discontinuity, and a checked turn delta. Partial native cache aggregates and
imported history without such evidence remain source totals only.
