# usage-saved-daily-tokens@1

This optional capability reads the dated token totals a provider has saved for
one selected source profile. It is independent of `usage-scan` and
`usage-lifetime-summary`; it changes no shared usage schema. Its sidecar
declaration is `{}` and its operation is `read`.

The request is exactly `{"account_context":{"version":2,
"adapter_account_key":"...","profile_path":"..."}}`. The host freezes the
admitted accounts@2 key and path before dispatch. The adapter reads only that
selected profile and echoes the complete context exactly in a successful
result.

The successful result contains the echoed account context and these members:

```json
{
  "source_kind": "claude-stats-cache",
  "source_format_version": 5,
  "daily_series_format_version": 5,
  "source_content_digest": "<64 lowercase SHA-256 hex characters>",
  "cache_stated_date": "2026-09-15",
  "historical_principal": "unknown",
  "days": [{
    "date": "2026-07-09",
    "models": [{"model": "provider-model-id", "tokens": 0}]
  }]
}
```

`source_kind` is a bounded lowercase source identifier. Both format versions
are nullable nonnegative integers observed from the source, never version
allowlists or admission gates. `source_content_digest` hashes the exact source
bytes. `cache_stated_date` is a nullable valid `YYYY-MM-DD` date stated by the
source, not a coverage cutoff. The source profile identifies custody; the
historical provider principal remains unknown.

There are at most 4096 distinct dated rows, with at most 256 distinct bounded
UTF-8 model identifiers per day and 16384 model rows overall. A day can have
zero model rows. Each token value is an exact nonnegative JSON integer; their
checked sum is at most 2^53-1. The numbers are provider-reported daily
aggregates. They do not identify token categories, sessions, repositories, or
the historical principal. The capability does not claim that all days are
present or that these values reconcile with a lifetime snapshot. Missing dates
are unknown, not zero.

A missing or malformed dated series fails only this capability. It cannot
invalidate a saved lifetime summary or a detailed usage scan. These aggregates
must not be added to detailed, lifetime, or cross-source totals or converted
into dated session records. The host may retain a prior saved dated series
marked stale when a later read fails.
