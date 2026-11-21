# usage-incremental@1

Use this pair to update accounting against a cache snapshot the host has
already accepted. Its sidecar payload is `{}`. Advertise the pair alongside
[usage-scan@3](usage_scan_3.md), which must be live. The initial scan, repair
and reset use the full scan contract.

## Request

The operation is `scan`. It takes the version 3 scan request with
`mode: "incremental"` and a non-null `cache_selection.accepted`. This reference
identifies the baseline by its exact `root`, `snapshot_id` and
`manifest_sha256_hex`. The frozen context, candidate allocation, first-page
nulls and continuation rules are the same as for a full scan.

## Result

Every successful response echoes the exact accepted reference in `baseline`
and the requested `{version, root}` in `profile_storage`. If the host selected
an account, echo the entire requested `account_context`, including its
version. Otherwise omit that member. These acknowledgments belong in the
outer result in both forms.

| Result | Effect |
|---|---|
| `"unchanged": true` | Return on the first request and omit `changes`. The host keeps the accepted accounting and source metadata. |
| `"unchanged": false` | Include `changes`, one complete version 3 scan **page** with its own context acknowledgments and sealed cache candidate. Each page contains changed absolute observations in strictly ascending record-ID order. Omitted IDs retain their accepted values. An empty terminal page can update metadata alone. |

All changed pages refer to the same baseline and sealed candidate. Follow the
scan paging rules until the terminal page. Preserve the accepted cache
unchanged. The host validates the candidate before publishing the updated
accounting and cache selection together. The [C++ codec and cache helpers](../include/logonomic_cli_addon/usage_incremental.h)
provide request/result encoding and immutable file inheritance.
