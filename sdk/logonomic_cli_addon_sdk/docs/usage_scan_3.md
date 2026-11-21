# usage-scan@3

This independently negotiated capability produces bounded pages of one
consistent historical usage snapshot and one sealed host-selected cache
candidate. It is independent of accounts, authentication, quota and structured
sessions. Unknown versions and failures are capability-local; the core protocol,
bootstrap and shared store schema do not change.

## Declaration and frozen input

The sidecar is `{"environment_names": [...]}` using the launcher-discovery
allowlist grammar, or `{}` for an empty additional-name list. This current
declaration directly owns the frozen usage environment; another capability's
sidecar does not supply it. The host includes its baseline and admitted explicitly
authored launcher names, preserves overrides/removals, and retains the same
capture for every page. See [Account probe and usage environments](probe_environment_2.md).

Optional [usage-session@1](usage_session_1.md) describes native session aliases
and completed-turn coverage through a separate bounded operation. It does not
change this request, record or cache representation.

With an accepted cache baseline, [usage-incremental@1](usage_incremental_1.md)
can report unchanged history or page through changed observations.

## Scan pages

`scan` carries the same frozen launcher/profile/account context as probe
preparation, including required frozen `launch_environment`, plus `mode` (`incremental`, `repair`, or `reset`), `snapshot_id`
and `cursor`. Both latter members are explicit null on the first request and
both are nonempty keys on continuation. Keys are case-sensitive UTF-8 bytes,
nonempty, at most 256 bytes, without NUL. A mode never authorizes an incomplete
protocol result to replace accepted data.

Every page contains `scope` (the observation scope from
[auth-status@2](auth_status_2.md)), `snapshot_id`, `next_cursor`, `complete`,
`incomplete_historical_coverage`, and `records`, together with the required
cache and context acknowledgments specified below. Snapshot IDs and cursors use
the key grammar. `next_cursor` is null if and only if `complete` is true.
`incomplete_historical_coverage` is a required boolean declaring whether the
vendor's history omits older records; it is distinct from unfinished paging.
A result has at most 256 records and remains under negotiated frame limits.

The host requires a fixed snapshot ID, fixed scope/observation/coverage
metadata, advancing cursors, and strictly ascending record IDs across every
page. Ordering compares UTF-8 bytes as unsigned bytes. Each SDK page decoder
checks its own ordering; the host checks the boundary between pages and the
whole scan. Progress frames carry counts and an optional bounded note
(`k_max_progress_note_bytes`). The host bounds every request by the adapter's
silence rather than by time (see [the core protocol](core_protocol.md),
rule 4), so an adapter that builds a large snapshot reports progress while it works. A stall, cancellation, errors,
generation retirement, stale binding, changed snapshot, repeated cursor or
duplicate record IDs discard private staging and preserve accepted usage.
A terminal page establishes protocol completeness, not complete historical
coverage. A completed limited-history snapshot retains previously accepted records
missing from that snapshot through the existing store owner; it cannot replace
complete history with the limited subset.

## Accounting records

Every record contains:

- `record_id`: stable key, not an ordinal assigned afresh each scan.
- Optional `session_key`: stable provider session key with the same grammar.
- `completed_date`: valid Gregorian date in `YYYY-MM-DD`, year `0001..9999`.
- `total_tokens`: nonnegative exact integer.
- Optional `source_path` and `repository_path`: real absolute UTF-8 paths,
  under existing path bounds. Omit unknown paths; do not invent a path or
  account merely to satisfy a consumer.
- `semantics`: `cumulative-final` or `delta-event`. Cumulative records replace
  the previous final observation for their stable ID; each delta event has a
  distinct stable event ID. Repeating a complete snapshot is never additive
  replay.
- Required `input`, `output`, `cache_read`, `cache_creation`: each has explicit
  Boolean `applicable`, optional exact-integer `value`, and required `coverage`
  with `covered_records`, `total_records`, Boolean `overflow`, and `support`
  (`unknown`, `supported`, or `unsupported`). Covered count cannot exceed total.
  Unsupported means not applicable, zero covered records, no value and no
  overflow. Other support states are applicable. Overflow omits the value.
  Supported, nonoverflowed amounts require their known sum, including zero.
  Unknown support may omit the value only with zero covered records; an unknown
  amount is distinct from an observed zero or a partial known sum.
- Required `coverage_unit`: `records` or `sessions`, matching the native evidence.
- Required `aggregate_evidence`: `complete` or `partial`, under the authority
  rules below.

All counts are integers in `0..9007199254740991`.
Fractions, exponent-form numbers and overflow are invalid; the host also checks
aggregate arithmetic. Token categories need not sum to total because vendor
cache categories can overlap.


Optional `repository_usage` contains at most 4096 unique absolute repository
allocations and `unattributed_tokens`; their sum equals `total_tokens`. It cannot
coexist with `repository_path`. The host normalizes platform identities and
rejects duplicate normalized roots. A repository need not remain on disk to
retain its accepted historical attribution.

Optional `model`, `counter_field`, and `source_counter_semantics` are bounded
UTF-8 source metadata without control characters. Source semantics is independent
of replacement/idempotency policy: a session aggregate of native event deltas
can be a cumulative-final record with `source_counter_semantics: per_event_sum`.
The admitted source-semantics values and aggregate evidence rules are below.

The richer record keeps its stable native record/session identity. Accepted
source evidence needed to reconstruct historical aggregates remains in the
selected provider cache. Missing current transcripts or an incomplete-history
flag cannot fabricate missing events or justify losing retained accepted totals.

Optional independently versioned records live in `capabilities`. Their own
contracts govern admission and opaque preservation; they do not change the base
record's meaning. In particular, recognized [usage-temporal@1](usage_temporal_1.md)
must bind this record ID and total. The host retains one current persisted
authority for token breakdown, observation semantics and record identity rather
than introducing another identity store from the scan wire.

## Selected cache and acknowledgments

Every request carries the required member:

```json
{"cache_selection":{"version":1,"accepted":null,"candidate_root":"/profile/package/account/candidates/scan-17"}}
```

`accepted` is explicit null or a reference with exactly `root`, `snapshot_id`,
and `manifest_sha256_hex`. The root is an absolute normalized path, snapshot
is a bounded opaque usage key, and the hash is 64 lowercase hexadecimal
characters. It names the cache selected by the host's durable usage state,
and is the only authority for selecting retained cache input.

The host exclusively allocates an empty candidate directory before page one.
Both roots are strictly beneath the acknowledged profile root and disjoint;
links/reparse points are rejected. The host holds their lifetimes through
paging and publication. All continuation requests retain identical selection,
mode, environment and account context. A request with no accepted
cache must not silently bootstrap from unselected provider storage.
Allocations must remain disjoint from live provider scratch/workspace ownership:
retiring a scan workspace cannot delete a host-owned selected cache or candidate.
The provider refuses conflicting allocations.

Every page adds required `cache_candidate`, a reference whose root equals the
allocation and whose snapshot equals the page snapshot. The descriptor remains
identical across all pages. A candidate must be durably sealed before page one,
including for zero records. Returning it means prepared, not accepted. The
provider must not subsequently mutate or prune it. There is no provider commit
operation or independently updated accepted pointer on this path.

Every page also carries the existing `profile_storage` acknowledgment record:
`{version:1,root}` echoes the exact requested version and root, without path
normalization. When an account is selected, `account_context` echoes the full
requested accounts-owned record: version 1 carries `adapter_account_key`;
version 2 additionally carries the required nullable `profile_path`. The
requested version is preserved. Without a selected account this member is
omitted, not null, and `scope.source_account_key` is null. With an account,
the scope key equals the acknowledgment key. Unknown members remain errors.

`Usage_scan_page::account_context` and `Usage_scan_page::profile_storage`
use the existing optional `Account_context` and `Profile_storage_context`
types. Providers populate both from the validated request context before
encoding every version-3 page, including empty and continuation pages.
The version-3 decoder requires the profile record and checks account/scope
consistency; the host compares both acknowledgments with the request using
the existing accounts and profile-storage acknowledgment checks. These
operation acknowledgments do not establish cache acceptance or replace
candidate integrity validation.

`reset` requires `accepted:null`. It reconstructs
only from current sources. It must not copy retained cache into the candidate.
Failure preserves the host's previous selection; successful reset may reduce
usage. Missing, malformed, unsupported-format or corrupt selected caches fail
explicitly; there is no latest-cache or empty-cache fallback.

## Cache integrity and resource ownership

`cache-manifest.json` has exactly `version:1`, nullable `account_context`,
`snapshot_id`, `format`, and `files`. Account context echoes the full selected
account context, not merely a display name. `format` is a bounded provider-owned
format identifier; unknown formats fail the requested capability.

Each file entry has exactly `path`, `bytes`, and `sha256_hex`. Names are unique,
ascending ASCII flat names of at most 128 bytes, using lowercase letters,
digits, `.`, `_`, and `-`; `.`/`..`, trailing dots, reserved device names, and
`cache-manifest.json`/`cache-manifest.pending` are forbidden. Files are regular
files only. The inventory is exact: unlisted files or directories fail.
Candidate file counts and total bytes have no fixed history-capacity ceiling.
File byte counts must fit the JSON integer representation, and their sum must
fit an unsigned 64-bit count. Manifest I/O streams individual metadata values
and hashes the exact bytes; it does not load file payloads or the complete JSON
document into memory. Inventory metadata occupies memory proportional to the
number of files. Individual values retain the SDK's JSON parsing bounds.
Limit exhaustion fails rather than deleting historical evidence. Hashes cover
exact bytes, including the exact manifest bytes named by the reference.

The SDK codecs validate envelopes; `validate_usage_cache_allocation` checks
an empty link-free allocation. `seal_usage_cache_candidate` hashes and flushes
the files and durably installs the manifest. `validate_usage_cache_candidate`
stream-verifies exact inventory, account, snapshot, size and digest. The host
validates the returned candidate before making it authoritative. Providers
validate selected input before consuming it. The SDK never deletes candidates.
`validate_usage_cache_root` checks lexical scope; filesystem callers additionally
reject links/reparse points. Manifest and consumed-file validation support
incremental reads without rehashing every historical payload on every page.

Only the host owns reclamation of this namespace. It protects live/recovery
references and in-flight allocations. Unaccepted candidates from failed or
crashed requests can be removed only after their writer has quiesced; age is
not evidence of that. A lost response does not authorize provider cleanup.

## Aggregate evidence

`source_counter_semantics` preserves native counter derivation independently of
the record's `semantics` aggregation policy. Version 3 admits `cumulative_final`,
`per_event_sum`, and `cumulative_turn_delta`; the latter describes a native turn
delta derived from cumulative counters, not an authoritative whole-history
aggregate. It does not imply `aggregate_evidence:complete`. Unknown or malformed
present values fail the capability rather than being relabeled or omitted.
Absent provenance remains absent.

Each version-3 record additionally requires `aggregate_evidence`, either
`complete` or `partial`. This is independent of snapshot historical coverage,
pagination completeness, and dimension coverage.

`complete` asserts that an authoritative native counter or all contributing
events establish this exact aggregate. Retained evidence may contribute if its
identity and integrity are validated. Successful parsing of a surviving subset
does not establish completeness. A provider must explain the native evidence
behind that assertion; it may conservatively use `partial`.

A complete cumulative observation can correct a previous total downward.
An incomplete aggregate cannot justify reducing accepted totals, known
dimensions, or replacing coherent attribution with a partial allocation.
Reject an incompatible replacement rather than computing maxima independently
for fields. Reject its cache candidate as well: keeping an old displayed total
while accepting a cache that lost its supporting history is not preservation.
Delta-event identity remains immutable and idempotent. Explicit
successful reset uses its separate source-reconstruction semantics.

A partial cumulative observation may refine unknown repository tokens into
known allocations when every previously known allocation survives. Resolving
an unchanged working directory after `git init` is such a refinement; it does
not reduce the accepted total or known token dimensions. If a known allocation
is missing or smaller, restoring it may use only incoming unknown tokens above
the previously accepted unknown amount. Refinement cannot move known tokens
to another repository or hide a lost allocation.

## Host acceptance

The host publishes one authority for usage and cache selection. For an
index-selected owned sidecar, seal the candidate and sidecar before replacing
the index. Select the sidecar by the exact final index bytes after rebase, not
by totals or an account manifest alone. Existing index generation increments
distinguish cache-only acceptance without changing the shared schema. This
wire contract does not prescribe a host's private sidecar serialization.

Before publication, failure leaves the previous index/cache pair selected.
After publication, recovery selects the matching cache without a provider
acknowledgment. An uncertain index replacement retains both generations until
the store owner resolves it. Account and live provider-generation authorization
must hold at the host publication boundary.

A mismatched index and optional cache-selection metadata do not establish
acceptance. That capability remains unavailable until explicit validated
reconstruction, while unrelated core behavior remains available. Unknown
capability-owned artifacts are preserved.

Providers that retain current private accounting records may use the
[stored usage accounting codec](stored_usage_accounting.md). A stored record is
not a scan observation: current aggregate completeness and optional evidence
must be established by the owning provider before it emits a page.
