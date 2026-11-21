# model-context-delivery@2

Version 2 binds a model-context request to the native conversation activation
captured by the provider. It uses the authenticated Session, package generation,
and current transition already owned by the helper-ingress ledger. It creates
no new epoch and carries no provider-specific command or event policy.

Declare the `model-context-delivery@2` pair with an empty-object sidecar. The
same generation must have live helper-ingress and lifecycle capabilities. A
provider can offer both versions; negotiation selects the highest common live
version. A host that negotiates version 2 also accepts the unchanged version-1
request for routes without captured activation evidence. A version-2 request
requires version 2 in the bound and currently live package generation.

## Captured request

`Helper_client::request_title_instruction` retains its existing signature.
Supply the native activation in the final argument:

```cpp
Model_context_delivery delivery;
delivery.activation = Model_context_activation{provider_session_id, transition_nonce};
const auto result = client.request_title_instruction(source, hook_event_name, false, delivery);
```

The SDK sends:

```json
{
  "source": "provider",
  "hook_event_name": "NativeHook",
  "changed_files_requested": false,
  "delivery": {
    "version": 2,
    "kind": "model-context",
    "activation": {
      "provider_session_id": "conversation-a",
      "transition_nonce": "0123456789abcdef0123456789abcdef"
    }
  }
}
```

All objects have closed member sets. `provider_session_id` is nonempty UTF-8,
has no control characters, and is at most `k_max_lifecycle_identity_bytes`.
`transition_nonce` is the exact 32-lowercase-hex nonce of the source activation.
The provider captures both before asynchronous delivery begins and uses the
same pair throughout the request. It must not substitute the transition that
happens to be current when a delayed callback runs. Returning A after A→B→A has
a new nonce even though its provider session ID is again A.

An empty `Model_context_delivery{}` still encodes version 1. A provider that has
captured an activation must not retry a refused version-2 request as version 1.
Older hosts or wrappers may refuse version 2; that refusal does not authorize
unfenced consumption.

## Host admission and consumption

Before observing the hook or claiming any one-shot instruction, the host
requires an open ledger whose current transition matches both captured fields.
There must be a current transition; epoch zero is not a match. A mismatch returns
`accepted:false, reason:"stale_activation"` with no instruction and consumes no
new context. Missing or retired capability negotiation retains the existing
`model_context_delivery_unavailable` refusal.

The host revalidates the captured activation after synchronous publications,
before one-shot and message consumption, and at the response boundary. Refusing
an older request must not release or overwrite claims belonging to a newer
activation or a successfully completed sibling request. A message read already
committed to the old activation is not rolled back if its publication triggers
a subsequent native transition; the stale response is suppressed.

This admission occurs on the host owner thread, using its existing transition
authority. A queued request remains fenced if its native helper, wrapper, or
transport dies before host execution. Process lifetime and a transport failure
are not proof that a host request was canceled.

The result retains `Title_instruction_result`: `completed` distinguishes a
parsed host decision from a transport failure. The host owns rendered bytes and
one-shot decisions. Providers emit accepted bytes only through a native hook
that consumes model context, retain its real source and hook name, and suppress
output if their captured activation has since changed. A context refusal does
not veto the native CLI operation or end its Session.
