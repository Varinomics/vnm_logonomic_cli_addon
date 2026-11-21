# usage-owned-event-correction@1

This optional capability corrects previously counted occurrences that a provider
can prove belong to other original records. Its declaration is `{}` and its only
operation is `scan`. It requires live `usage-scan@3` support and always returns
the unchanged paged `usage-scan@3` result, including when the nested request has
`mode: "incremental"`. It does not change ordinary scan or incremental grammars.

## Request and predecessor stream

The request contains `scan`, `predecessors`, and the common invocation contexts:

```json
{
  "scan": "<usage-scan@3 request object>",
  "predecessors": {
    "path": "<absolute profile-scoped JSONL path>",
    "sha256_hex": "<lowercase SHA256 of the exact file bytes>"
  },
  "account_context": "<common account context object, when selected>",
  "profile_storage": "<common profile storage context object>"
}
```

`scan` is the object defined by [usage-scan@3](usage_scan_3.md), including its
explicit cache selection. The host owns the temporary predecessor file under
the selected profile storage root. Both parties reject links, reparse points,
and paths outside that root. The request codec checks lexical containment;
callers perform the filesystem checks.

The encoder copies `scan.context.account_context` and
`scan.context.profile_storage` to the wrapper's top level using their existing
common codecs. The host proxy injects its trusted account and profile storage
contexts at that same top level. The decoder requires their presence and every
decoded field to equal the nested scan context, including versions, account key,
optional account profile path, and profile storage root. A mismatch refuses the
request; the nested scan cannot override the host's selected context. An absent
account is omitted at both levels, and explicit null contexts are invalid.
`usage-scan@3` requires profile storage for its cache selection, so it is present
at both levels. No other wrapper members are accepted. The result uses the
unchanged `usage-scan@3` top-level context acknowledgments.

The first JSONL line contains exactly `version: 1`, `provider_id`,
`account_context`, `profile_root`, and `accepted_cache`. `provider_id` is the
adapter package ID. The account equals `scan.context.account_context`, the root
equals the selected profile storage root, and the cache equals
`scan.cache_selection.accepted`, including its root. Absent account and cache
are explicit JSON nulls. Null cache means no selected provider cache; host rows
still exist independently of that selection.

Each remaining line has exactly:

```json
{
  "record": "<Usage_record object encoded using usage-scan@3>",
  "predecessor_sha256_hex": "<opaque actual host-row digest>",
  "temporal_acceptance": null
}
```

`temporal_acceptance` is either null or the original host acceptance object. It
is preserved opaquely. The `record` is an accounting projection; the host keeps
the complete original row and unrelated capabilities. Recognized temporal and
repository evidence is included where available. The host hashes its complete
canonical accepted row before projection. The provider echoes that digest and
must not derive it from the projection or infer a temporal parent from the
current cache selection.

Rows are strictly sorted by unique `record_id` in UTF-8 byte order. Before using
the stream, the provider verifies its exact-byte SHA256, header selection,
ordering, and population with cancellation and bounded JSONL parsing. Each line
must fit the existing transport payload bound. An oversized projection makes
the capability unavailable without accepting a candidate.

## Correction envelope

Only a positively selected `usage-owned-event-correction@1/scan` operation
interprets `record.capabilities["usage-owned-event-correction"]`:

```json
{
  "version": 1,
  "disposition": "correct",
  "predecessor_sha256_hex": "<opaque actual host-row digest>",
  "coverage_policy": "records",
  "removed": {
    "total_tokens": 150,
    "total_records": 2,
    "input": {"value": 100, "covered_records": 2},
    "output": {"value": 50, "covered_records": 2},
    "cache_read": {"value": 0, "covered_records": 2},
    "cache_creation": {"value": 0, "covered_records": 0},
    "repository_usage": {
      "allocations": [],
      "unattributed_tokens": 150
    },
    "temporal": null
  },
  "original_record_ids": ["creator-session-17"]
}
```

The exact correction grammar also accepts `coverage_policy:
"retained_unknown_sessions"`. `removed` is a subtraction vector against actual
accepted components. `total_records` counts native usage-record occurrences,
including zero-token events. Counter coverage cannot exceed that count; a
nonzero counter value needs covered occurrences. Nonzero total tokens need at
least one occurrence. Counter categories need not be disjoint, so their sum is
not used as a replacement total.

Repository allocations use the unchanged repository partition grammar and
must sum with `unattributed_tokens` to the removed total. `temporal` is null or
an object with exactly `portions` and `unavailable_tokens`. Its portions use the
unchanged [usage-temporal@1](usage_temporal_1.md) portion shapes, unique temporal
positions and bounds. Portions plus unavailable tokens equal the removed total.
There is no temporal parent in a removal. An absent prior temporal wrapper
permits null; a recognized wrapper requires a proved vector; an opaque wrapper
grants no removal authority. No proportional temporal or repository subtraction
is permitted.

All numeric fields use the existing exact integer range `0..9007199254740991`,
carried by `uint64_t` in C++. Temporal portions remain positive. Digests are
exactly 64 lowercase hexadecimal characters. Original IDs use the existing
usage-record ID grammar: nonempty UTF-8, at most 256 bytes, without NUL. The
owner array is nonempty, strictly sorted and unique. It has no separate count
ceiling; the existing exchange payload budget bounds the complete row and page.

Original IDs exclude the corrected row and name accepted or same-candidate
original owners. The provider proves native occurrence ownership and
conservation; row presence alone is insufficient. The host validates these
relationships and checked subtractions against the actual predecessor. A
correction row is cumulative and explicitly partial; complete correction rows
are rejected.

The host privately adjusts the predecessor only for numeric, date, category,
coverage and repository admission, then applies ordinary partial growth rules.
Zero-token removed occurrences still reduce coverage. A correction with no
growth preserves the accepted date; growth follows ordinary nondecreasing date
rules. Temporal parent, preservation, archive and capacity decisions always use
the actual predecessor. Correction never uses temporal capacity fallback. When
its accounting parent changes, the original recognized wrapper is archived under
its original acceptance; adjusted evidence is never published under that old
acceptance. Current sessions alone feed billing.

## Exact retention

The other disposition has exactly three members:

```json
{
  "version": 1,
  "disposition": "retain",
  "predecessor_sha256_hex": "<opaque actual host-row digest>"
}
```

It grants no subtraction authority. The candidate agrees with the supplied
predecessor's numeric, date, category, provenance and repository projection.
It omits the temporal envelope, because an old parent cannot be emitted under a
new scan page. The host validates stable identity and digest, then retains its
complete actual predecessor byte-for-byte, including the original temporal
wrapper, acceptance and unrelated capabilities. Retention does not reparent
old evidence.

An accepted ambiguous prefix can remain explicitly unresolved while its
observation is integrity-bound. Later independently proved own append growth
uses ordinary partial admission and a new valid current temporal envelope where
recognized, rather than a retain envelope. The candidate derives from the actual
exported predecessor. Untagged rows continue to use ordinary partial admission;
a changed raw population may be accepted and integrity-bound without removal,
then corrected only with independent proof. Fresh unresolved billable rows make
this operation unavailable.

## Selection and publication

Unknown positive envelope versions are capability-local unavailable. They do
not invalidate ordinary records. Ordinary operations strip the correction key
unconditionally. The selected operation strips its authority before storage and
preserves unknown unrelated capabilities. The codec validates shape and local
conservation; neither a successful decode nor an opaque temporal acceptance
object proves host ownership.

Continuation and cache-selection identity bind the capability, operation,
complete nested scan selection and predecessor SHA256. Ordinary continuations
cannot resume this operation's candidate. At atomic cache/history publication,
the host rereads the actual predecessor and digest and checks the existing
expected index/binding revision. Cancellation or rejection accepts no cache
generation.
