# coordination-delivery@1

`coordination-delivery@1` declares whether the package delivers Logonomic's
coordination context to the CLI at startup and per turn. Index its sidecar
and advertise the pair in the adapter handshake. Version 1 has no operation.
The desktop reads the admitted sidecar for a generation that can be
activated, independently of whether it has started the adapter.
`lifecycle_events.h` implements the codec
(`decode_coordination_delivery_declaration`).

## Sidecar payload

```json
{"startup_instruction_delivery": "supported", "turn_context_delivery": "unsupported"}
```

Both members are required, each `supported` or `unsupported`. A defect is
`invalid_sidecar`.

## Host mapping

The host carries the declaration as the coordination delivery of every launch
and resume of a bound session: version 1, `supported` as
configured-unobserved and `unsupported` as unsupported. Observed and degraded
states remain host-derived at runtime. A package without the sidecar leaves
coordination delivery undeclared, exactly like a provider that declares
nothing.
