# usage-lifetime-summary@1

This optional capability reads a provider's saved lifetime usage snapshot for
one selected source profile. It is independent of `usage-scan` and changes no
shared usage schema. Its sidecar declaration is `{}` and its operation is
`read`.

The request is exactly `{"account_context":{"version":2,
"adapter_account_key":"...","profile_path":"..."}}`. The host freezes the
admitted accounts@2 key and path before dispatch. The adapter must read only
that selected profile and echo the complete context exactly in a successful
result. The broker rejects a different or missing echo.

The successful result has the account context and these required members:

```json
{
  "source_kind": "claude-stats-cache",
  "source_format_version": 2,
  "source_content_digest": "<64 lowercase SHA-256 hex characters>",
  "cache_stated_date": "2026-09-25",
  "historical_principal": "unknown",
  "models": [{
    "model": "provider-model-id",
    "input_tokens": 0,
    "output_tokens": 0,
    "cache_read_tokens": 0,
    "cache_creation_tokens": 0
  }]
}
```

`source_kind` is a bounded lowercase source identifier; `source_format_version`
is a nullable nonnegative observed integer, never an allowlist or admission
gate. `source_content_digest` hashes the exact source bytes. `cache_stated_date`
is a nullable valid `YYYY-MM-DD` date stated by the source, not a claim about
coverage ending on that date. The source profile identifies custody, while
`historical_principal` explicitly records that the provider principal behind
historical counters is unknown.

There are at most 256 rows, with distinct bounded UTF-8 model identifiers.
The four counters are disjoint token classes. Each is an exact nonnegative JSON
integer and their checked sum over all rows is at most 2^53-1. The aggregate
of the four classes is the saved lifetime total; the capability does not emit
dated or session records. A missing or malformed source fails this capability
locally and does not affect a detailed usage scan. The host may retain an
earlier saved snapshot marked stale when a later read fails.
