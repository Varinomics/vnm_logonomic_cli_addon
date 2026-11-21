# model-state@1

This optional static capability has an empty sidecar payload `{}`. It carries
native observations of the model serving a root or admitted child agent. It
does not select models, list available models, or reinterpret usage records.

After both peers select `model-state@1`, the host adds
`"model_state":{"version":1}` to helper classification, `observe`, or
`poll_observations` requests. A provider omits the result envelope unless the
request opts in. An omitted envelope means no update. A result is either
`{"version":1,"updates":[...]}` or
`{"version":1,"failure":{"reason_code":"...","detail":"..."}}`.
There are 1–16 updates. Each update has these members:

| Member | Meaning |
|---|---|
| `provider_session_id` | The exact root native session identity. |
| `transition_nonce` | The active root transition's 32 lowercase hexadecimal nonce, captured at the native source. |
| `sequence` | Increasing source order for this actor and activation, encoded as canonical uint64 decimal text. |
| `agent_id` | Empty for the root, otherwise the exact child lifecycle identity admitted by the host. |
| `state` | `confirmed`, `unknown`, or `unavailable`. |
| `provider` | Optional native serving-provider identity. Omit it when the native record establishes the model but does not identify its backend. Empty is also accepted. Never infer it from the CLI name, launcher, or a model prefix. |
| `model` | Native resolved model identity; required and nonempty for confirmed facts, empty otherwise. |
| `reasoning_effort` | Optional observed native reasoning level. Omission means unknown and replaces any previous level. Native `off` remains the literal string `off`. Omitted for unknown/unavailable facts. |
| `source` | The native event/query establishing the observation. |

Identities use the lifecycle identity bound. Provider and model are at most
512 UTF-8 bytes each; reasoning effort and source are at most 128 bytes.
All strings must be valid UTF-8 without control characters. Source is always
nonempty. Unknown or unavailable states contain no provider, model or effort.
The provider is optional independently of a confirmed model. This permits
model-only native reports without inventing a backend identity. Existing
peers that require a provider may reject that optional observation; lifecycle
and execution results remain usable. Encoders keep an empty `provider` member
for unknown and unavailable updates so those peers can still apply withdrawals.
Unknown means observation loss;
unavailable means this native surface cannot
establish the fact. Diagnostic failure envelopes have no withdrawal authority.

## Authority and freshness

The sample describes the native model at its declared observation boundary.
Requested launcher options, delegated model selections, aliases, an earlier
conversation's model and billed model names are not current native evidence.
A provider must document its sampling cadence. Event-bound samples do not
promise immediate observation of native changes for which no public event
exists. Native fallback or escalation is reported as the model actually
observed, independently of any selection contract.

Capture the native activation and source sequence before asynchronous work.
Using a host nonce read on arrival would falsely authorize delayed A → B → A
events. Joint SessionStart model facts can share that accepted transition;
later facts require the source-captured nonce. Sequence must not be assigned
in completion order. Each observation fully replaces its actor's model facts.

The host additionally verifies package generation, runtime generation, active
root identity and transition, and the child's admitted lifecycle identity.
A child cannot replace the root's state. Stale, duplicate and foreign-actor
updates are inert. Activation changes clear all actor facts. Child departure
removes its live model from presentation. Host observation loss clears facts
while retaining source sequence barriers. A new model observation never
creates a child or changes root lifecycle or completion.

Malformed optional data and unsupported versions fail this capability alone;
valid adjacent lifecycle facts remain usable. The host keeps admitted samples
in the session owner and exposes them to the existing session presentation.
There is no global model cache or cross-session fallback.

## Structured inspections

The independently negotiated `structured-model-observation@1` capability
opts a structured adapter into the model envelope. Its sidecar payload is
`{}`. The host adds `"model_state":{"version":1}` to a `structured-session@3`
`inspect` request when the adapter declares the structured capability in its
package and handshake and the selector leaves room for the inspection's
continuation and stdout progress. Interactive model reporting has its own
negotiation and can be supported independently.
The adapter returns evidence only when that request opted in. An inspection
result may carry this optional envelope beside its normal streaming,
completion, or failure result:

```json
{
  "model_state": {
    "version": 1,
    "observation": {
      "model": "literal-native-model",
      "source": "native.assistant_message"
    }
  }
}
```

`observation.provider` is optional and uses the same rule as interactive
observations. Model and source are required and use the bounds above. A
structured model fact belongs to the exact host-owned operation whose output
is being inspected. The existing serialized inspection stream supplies its
order; no interactive session ID, transition nonce, child identity or new
ordering protocol is introduced. Return the latest valid native model in the
inspected bytes. A malformed sibling record cannot withdraw confirmed evidence.
The host retains that evidence across inspections. When a response has room
only for the core inspection, the encoder omits the optional model envelope
to preserve the continuation. Requested configuration and selection
acknowledgments cannot establish a model.

An omitted envelope means no observation. The optional failure form is the
same as the interactive envelope and has no withdrawal authority. Malformed
model evidence, unsupported model versions and unsupported sampling leave the
core inspection result intact. Final inspection may establish the model after
native exit and before the host publishes the execution's final disposition.
