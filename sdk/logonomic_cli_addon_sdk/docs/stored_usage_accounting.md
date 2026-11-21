# Current stored usage accounting

`stored_usage_accounting.h` owns the current provider-private accounting record
representation used by Grok, Kimi and Qwen caches. It has no capability sidecar,
wire-version argument, host negotiation or acceptance operation:

```cpp
Json_value encode_stored_usage_accounting(const Usage_record& record);
std::optional<Usage_record> decode_stored_usage_accounting(
    const Json_value& payload, Contract_failure* out_failure);
```

The codec shares the real identity, date, count, path, coverage and repository
validators with current scan observations, while enforcing a distinct stored
accounting contract. Its exact emitted field ordering and absence rules remain
stable because current cache ownership proofs can bind those bytes. It does not
change a provider's cache format, manifest, receipt identity or accepted cache
selection.

## Stored record

Required fields are `record_id`, `completed_date`, `total_tokens`, `semantics`,
`input`, `output`, `cache_read`, `cache_creation`, and `coverage_unit`. Optional
fields are `session_key`, `source_path`, `repository_path`, `repository_usage`,
`model`, `counter_field`, and `source_counter_semantics`. They retain the current
accounting grammar documented in [usage-scan@3](usage_scan_3.md): bounded stable
UTF-8 keys, Gregorian dates, exact nonnegative counts, real absolute paths,
reconciling repository allocations and cumulative-final/delta-event identity.
Unknown members fail decoding.

Each dimension requires explicit applicability and its complete coverage record.
Unsupported coverage is inapplicable with zero covered records, no value and no
overflow. Other support states are applicable and require a known exact value
unless overflow is true; overflow requires the value to be absent. Unknown
support still records an explicit known sum here, including zero. Stored
`source_counter_semantics`, when present, is `cumulative_final` or `per_event_sum`.
These rules preserve the validation of the current private accounting bytes.

## Observation authority remains with the provider

Stored encoding does not emit `aggregate_evidence`, `capabilities`, typed temporal
or cross-source evidence. Stored decoding rejects observation-only members and
leaves aggregate evidence absent and optional typed evidence unset. Encoding is
therefore a deliberate projection of the accounting fields, not an authorization
to emit an incomplete public observation.

Providers retain their current cache-owned correction, receipt, ownership and
temporal metadata in their existing enclosing records. On replay they validate
that metadata and derive the current scan's aggregate evidence and optional
observations from the actual source/cache evidence. A retained record alone
cannot assert a complete current aggregate or recover evidence that was never
stored. Missing upstream transcripts do not discard accepted current cache
records, and a current cache's integrity and source semantics remain mandatory.

Public usage requests and pages use usage-scan@3 codecs. They require explicit
aggregate evidence, can represent unknown amounts without a value under the
zero-covered rule, and admit the current `cumulative_turn_delta` source semantics.
The stored accounting API exposes neither those observation authorities nor an
integer version switch to a retired public wire grammar.
