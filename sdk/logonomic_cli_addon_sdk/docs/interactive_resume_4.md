# interactive-resume@4

This optional capability extends the read-only `inspect_resume` operation with
conversation-content evidence. Native resumability and content are independent:
a CLI can resume a conversation whose transcript contains only initialization
or other metadata.

Declare version 4 alongside [interactive-resume@2](interactive_resume_2.md) and
[interactive-resume@3](interactive_resume_3.md), using the same declaration
payload. Hosts select version 4 when the package advertises it and otherwise
retain their negotiated version 3 or version 2 behavior. Version 2 remains the
preparation and startup-observation contract. The core protocol and stored
opaque resume envelope do not change.

## Request and result

The request, resolved read-only context, byte bounds and acknowledgment are
identical to version 3. Use `Resume_inspect_request` and
`resume_inspect_request_digest_hex`. No new request fields or native state
mutations are permitted.

```json
{
  "availability": "available",
  "request_digest": "<64 lowercase hexadecimal characters>",
  "content": "empty"
}
```

`availability` retains its version 3 meaning. The optional `content` member is
one of:

- `empty`: complete, understood evidence establishes that the exact
  conversation contains no retained conversation content according to the
  provider's native semantics. Initialization and presentation metadata alone
  do not make a conversation nonempty.
- `nonempty`: the provider established that the exact conversation contains
  retained conversation content.
- `unknown`: the provider cannot establish either content state. Missing or
  unrecognized content values also mean unknown. A non-string value is a
  malformed result.

The provider owns the native content predicate. Token usage, a title, file size
alone, lack of observed prompt hooks, or the absence of evidence from a partial
read cannot establish emptiness. Inaccessible storage, incomplete records,
unsupported native formats or uncertain replay semantics remain unknown.
An unavailable operation, failed acknowledgment, transport failure or malformed
result also supplies no content evidence.

`Resume_inspect_result::content` defaults to `Resume_content::UNKNOWN`.
Pass the negotiated version to `encode_resume_inspect_result` and
`decode_resume_inspect_result`. Version 3 encoding omits content even when the
provider has computed it, so an installed version 3 host receives its unchanged
closed result shape. Version 4 decoding accepts an omitted content member
without changing availability. Packages that declare only earlier versions
need no changes.

## History projection

A host may withhold a remembered conversation after an acknowledged `absent`
availability result or an acknowledged `empty` content result. Neither result
deletes native history, the opaque resume record, ownership history or dismissal
state. Unknown evidence never means empty.

These answers describe an inspection, not an irrevocable fact. Reinspect when
the native transcript or resolved context changes and expire cached results as
for version 3. A later write can change empty to nonempty or unknown; either
must allow the retained row to return. Explicit preparation still evaluates
native availability afresh through version 2 and does not depend on a cached
content answer.
