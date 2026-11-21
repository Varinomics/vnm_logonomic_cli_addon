# usage-session@1

`usage-session` version 1 supplies the relationship between an opaque usage
ledger key and the native session ID reported by lifecycle observations. It
also states whether a record excludes the turn still in flight. Its sidecar
payload is `{}`. Host adoption requires `usage-scan@3`; the capability does not
change the scan request, usage record schema, bootstrap or shared store format.

The host invokes `describe` only when this exact capability pair is declared,
announced by the adapter and adopted by the host. It submits one accepted scan
page as `{"records": [...]}`. Each entry contains `record_id` and `session_key`;
the latter is an explicit string or null, matching the scan record. There are
1–256 entries, with distinct record IDs. Keys are nonempty, case-sensitive UTF-8,
at most 256 bytes, without NUL.

The result has the same `records` array, in the same order, preserving each
entry's exact `record_id` and `session_key`. Every entry adds `session`, either
null when the provider cannot establish the relationship, or this fact:

```json
{
  "version": 1,
  "provider_session_id": "native-session",
  "excludes_turn_in_flight": true
}
```

`provider_session_id` uses the key grammar and names the actual lifecycle ID.
The boolean is true only when the record's source counts completed turns and
excludes the current incomplete turn. It describes source coverage, not scan
freshness, whole-session completeness or the counter's replace/add semantics.
The provider must describe both fresh and cached records consistently. It must
not derive an unproved native identity from an unknown record.

The host checks the response's package generation, cardinality and every
positional identity before accepting any facts. Missing, unsupported, invalid
or unavailable descriptions affect session projection alone; accepted usage
amounts remain available. The host persists accepted facts under the optional
`capabilities.usage-session` record. It neither changes `usage-record-identity`
nor rewrites provider caches or identity-conversion payloads. An incoming null
description withdraws a previously recognized fact for that current record;
unknown capability envelopes remain preserved and uninterpreted.

Internal ledger queries distinguish opaque exact keys from native lifecycle
IDs. Native queries require a recognized alias or independently retained native
identity; an equal opaque string alone does not prove a native relationship.
An alias applies to all accepted records sharing that ledger key, including
older unannotated records. Conflicting aliases make that native total unavailable.
A total claims to exclude an in-flight turn only
when every counted record establishes that cutoff. Unannotated or unknown-version
records do not establish an alias or cutoff through this capability. Retained
native completed-turn records can independently establish their documented
cutoff through their validated native representation. Existing history acquires
session facts when its provider next scans and describes it.

This capability is projection metadata, not billable-event evidence. Its entire
envelope, including unknown versions, is excluded from cross-source evidence
comparison and temporal accounting-parent comparison. Its presence cannot create an accounting
conflict, double-count an equal event or change temporal history identity.

Adapters do not put this new metadata into ordinary scan responses. An older
host never invokes `describe` and receives the original scan representation,
so it cannot mistake projection metadata for different accounting evidence.
