# conversation-export@2

Version 2 uses the declaration grammar, `export` operation, request, result,
rendering and artifact lifetime of [conversation-export@1](conversation_export_1.md).
Its only difference is source eligibility after a particular launch or resume
has been prepared. The SDK names this version
`k_conversation_export_prepared_source_version`.

The package declares version 2 in its handshake and in
`capabilities/conversation-export@2.json`:

```json
{"capability_id":"conversation-export","version":2,"payload":{"environment_names":["EXAMPLE_PREPARED_SOURCE"]}}
```

Each declared name is a source fact that the provider prepares for that launch.
The host first applies the prepared environment updates, unsets and aliases,
then computes the existing allowlisted `ambient_view` of that finalized
environment. Every declared name must survive in that view, including its
existing value bounds and validation. Name matching follows the recorded
platform's environment rules. An empty declaration requires no additional facts.

When any required name is absent, the host captures no new live or durable
conversation source and offers no extraction or continuation for that source.
The ordinary prepared launch or resume still proceeds. The provider must unset
an inherited source locator when it cannot prepare a valid one, so inherited
state cannot impersonate a fact established for this launch.

A host must also validate the required names in a retained source before
offering extraction through version 2. A missing source record cannot be
reconstructed from the current launcher, an account profile or other ambient
state: those inputs do not establish the required prepared facts. A later
preparation without a source does not erase an earlier exact retained source.
Read or content failures for an eligible source remain ordinary export failures.

Version 1 providers retain their optional-name capture behavior. Hosts that
have not adopted version 2 ignore this optional capability; the package's other
capabilities remain usable. Version 2 changes no launch patch, provider binding,
persisted source-record schema or key, or base protocol. Hosts invoke the
capability version selected with the source; providers use the existing
`handle_conversation_export` handler and codecs for either version.
