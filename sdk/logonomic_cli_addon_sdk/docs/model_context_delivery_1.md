# model-context-delivery@1

Providers that capture native conversation activations can also negotiate
[`model-context-delivery@2`](model_context_delivery_2.md), which binds each
request to the captured activation. Version 1 remains available for existing
providers and launch routes without that evidence.

This optional capability authorizes the existing authenticated title-instruction
request at a hook whose response actually reaches the root model context. Its
sidecar payload is `{}`. The package must list the pair in its live handshake;
there are no adapter RPC operations. A declared and live helper-ingress pair is
a dependency. An older host which has not adopted this pair cannot admit it.
Absence, an invalid typed payload or an unknown version affects this capability only.
Package admission still validates the generic sidecar envelope, indexed identity
and integrity; failure of those package-level checks may reject the package.

`Helper_client::request_title_instruction(source, event, changed_files,
Model_context_delivery{})` adds `delivery: {version: 1, kind: "model-context"}`.
Omitting the final argument preserves the previous request and its event rules.
The helper retains the real native source and hook event; it must not relabel a
hook as UserPromptSubmit. It requests delivery only where the vendor consumes
returned context, and excludes subagent hooks when delivery belongs to the root.
A native prompt ID is not required.

The public wrapper supplies package identity, generation digest and nonce from
its launch environment. The host checks the authenticated active session, its
open bound helper ledger, exact generation identity and the current live pair
before any one-shot mutation. The record itself grants no authority. Malformed
or unknown delivery records are explicitly refused. Stale generations and
unbound launches receive `accepted: false` without consuming instructions.

Admitted delivery uses the existing title, changed-files and delegation
one-shot state and coordination-notice result. Retries cannot request those
one-shot instructions again after consumption. Existing session capability and
package-generation checks also fence reentrant publication. The helper emits
the returned native hook response only after the host has accepted the request.
No provider name is added to host event routing, and no generation path or
secret is persisted in vendor configuration.
