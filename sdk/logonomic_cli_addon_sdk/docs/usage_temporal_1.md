# usage-temporal@1

This optional capability attaches temporal evidence to an existing
`usage-scan@3` accounting record. Its declaration is `{}` and requires
negotiated `usage-scan@3`. It creates no additional counted records and
does not change accounting totals or identities.

The record's `capabilities.usage-temporal` envelope contains `version: 1`,
`metric: "total_tokens"`, `parent`, `portions`, `unavailable_tokens`, and
`coverage_reason`. The parent contains the exact `record_id`, `total_tokens`,
`snapshot_id`, and `source_account_key` (explicit null for default scope).
These must match the enclosing accounting record and acknowledged scan page.

Every portion is a non-overlapping proved increment. An instant portion is
`{basis: "instant", instant: <RFC 3339 timestamp>, total_tokens: <integer>}`.
Retain the original offset and fractional precision. Offsetless timestamps
and the RFC 3339 unknown offset `-00:00` cannot establish an instant.
Accept lowercase `t`/`z` and any fractional digit count within the envelope
budget, as specified by [RFC 3339 sections 5.6–5.7](https://www.rfc-editor.org/rfc/rfc3339#section-5.6).
Leap-second notation is checked at its offset-adjusted UTC month boundary;
the provider's evidence must prove the actual insertion. Preserve `:60`
and the original timestamp, without rounding or converting it for storage.
A source-calendar portion is `{basis: "source-calendar", date: <ISO date>,
calendar: <declared source-calendar meaning>, total_tokens: <integer>}`.
It must never be converted to an invented midnight instant or assigned to
another display timezone without source proof. Combine repeated positions.
This capability allocates only total tokens; accounting categories and
repository attribution remain independent.

Portion amounts are positive exact integers; the remainder is nonnegative.
Their sum equals the parent's exact total. `coverage_reason` is `complete`,
`missing-evidence`, `capacity`, or `unsupported`; `complete` requires a zero
remainder. Absence, unknown versions and unsupported evidence are unavailable,
and cannot establish measured zero activity.
Unsupported evidence contains no proved portions and retains the whole amount
in its unavailable remainder.

The SDK bounds the optional envelope to 4096 portions and 768 KiB. Existing
accounting, transport and storage bounds remain in force for every scan version.
The page packer measures the whole encoded page against the existing transport
payload budget and enforces the existing 256-record limit. Initialize its byte
counter to zero for an empty page and retain the next spool record when
appending returns false. There is no lifetime history limit. At temporal capacity,
preserve previously accepted portions and publish the exact unavailable
remainder with `capacity`; never truncate silently. If the accounting
observation is unchanged and capacity would discard accepted portions, the
host refuses that temporal replacement and retains the original bound wrapper.
Reliable accounting corrections remain publishable with their current valid
claims or unavailable remainder; they never inherit stale temporal claims.

Retained evidence may identify the selected accepted cache with optional
`retained_from: {snapshot_id, manifest_sha256_hex}`. The host validates this
against the accepted cache selected for the scan. Reemit the coherent evidence
with its current accounting parent; equal record IDs or totals alone never
authorize copying prior temporal claims after an accounting correction.

The host persists a capability-local wrapper with `version: 1`, the provider
`evidence` envelope, and `acceptance` containing `provider_id`, `account_id`,
`source_account_key`, `snapshot_id`, `manifest_sha256_hex`, and
`negotiated_version`. It records the validated sealed cache digest after
sealing, avoiding a circular provider-cache hash. Claims are usable only with
the negotiated capability and accepted parent linkage. Unknown provider
envelopes round-trip opaquely through typed SDK records and host storage.
Malformed known claims refuse publication without replacing accepted history.

The host conserves displaced evidence in zero-token `RETAINED_SESSION` shards
using its independent `usage-temporal-history@1` carrier. The provider cannot
declare this reserved host capability. A carrier has zero total tokens,
`counter_field: "total_tokens"`, `counter_semantics: "cumulative_final"`, the
original completed date, and storage-only session IDs
`usage-temporal-history:<SHA-256 of original UTF-8 record JSON>:<8-digit chunk index>`.
Its capability contains `version: 1`, `record_sha256_hex`, `record_bytes`,
`chunk_index`, `chunk_count`, and canonical padded `chunk_base64`. Each chunk
contains at most 512 KiB of the exact canonical original record bytes. The full
original accounting parent, opaque payloads, raw evidence and acceptance context
remain untouched. Each serialized carrier and its actual retained-shard framing
must fit the existing 4 MiB storage bound. The original remains governed by its
existing core storage bound; chunking adds no accounting size ceiling.
Known groups validate ordered complete coverage, decoded length, whole digest,
canonical original JSON, and original parent/context after the group ends.
Missing, conflicting or corrupt groups cannot prove continuity. Unknown versions
remain opaque and unavailable. Host-owned creation and trusted retained-store
provenance supply authority; a matching digest alone does not.
History uses bounded streaming sort/deduplication and reconstructs at most one
bounded original at a time. It has no lifetime count
limit. Equivalent coherent reemission needs no archive per refresh; displaced,
withdrawn, corrected and capacity-refused evidence is preserved once per digest.
Current accounting and daily totals never include archive carriers. A reader
may use a retained original wrapper only with a validated matching carrier and
the entire unchanged normalized accounting observation/context; ID, total or
fresh snapshot equality alone cannot prove that continuity. Transfer preserves
known validated carriers and unknown carrier versions without rebinding them.
