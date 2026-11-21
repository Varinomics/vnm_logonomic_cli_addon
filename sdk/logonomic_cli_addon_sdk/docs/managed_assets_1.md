# managed-assets@1

`managed-assets@1` declares package bytes that a host may materialize for a
CLI. It grants the host read/copy authority only: the host never executes
declared assets, loads them as UI code, interprets them as a component name,
or searches outside the immutable generation. A provider may reference copied
assets in its native CLI configuration, including plugin code that the CLI
loads under its own execution model. The pair is live only through sidecar,
handshake and host-adoption intersection.

Version 1 has no operation; the sidecar payload is the record:

```json
{"assets": ["assets/config.json", "assets/templates/default.txt"]}
```

The array holds 1-64 distinct canonical package-relative paths whose first
segment is `assets`. Each path must be in the immutable integrity index with
kind `asset`. A declared file is at most 4 MiB and all declared files together
are at most 16 MiB. The index's admitted byte counts are the authority for
these checks.

Wrong shape or duplicates are `invalid_sidecar`; a path outside `assets/` is
`path_escape`; a missing/wrong-kind index entry is `asset_reference_invalid`;
an individual or aggregate byte overrun is `asset_too_large`.

## Materialization

When launch-preparation@3 references a declared asset, the host copies the
custodied generation byte for byte into a new host-owned, package-scoped
materialization root. It creates parent directories from canonical path
segments, refuses links/reparse points and foreign objects, uses a temporary
file plus atomic publication, and never consults the mutable install source.
The resulting native path is supplied only through validated asset tokens.

Materialization is transactional for one prepare result: a missing index entry,
write failure, confinement failure or generation retirement exposes no partial
path and fails with `managed_asset_materialization_failed` (or the more
specific validation reason above). Removing or replacing a generation removes
its authority; consumers do not retain package paths as durable identity.
