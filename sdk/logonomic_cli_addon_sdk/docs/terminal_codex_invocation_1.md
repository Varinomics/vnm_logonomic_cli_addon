# terminal-codex-invocation@1

This capability identifies a package's verified interactive Codex invocation
so the terminal can apply its Codex graphics environment after executable
resolution. It covers native Codex, an npm Node entrypoint, and a resume bridge.
The package's executable spelling or package identity is not the declaration.

The sidecar payload is `{}`. The pair has no operations of its own and becomes
live through the existing sidecar, adapter-handshake and host-adoption
intersection. It requires a live `launch-preparation@3` or
`interactive-resume@2` producer in the same generation.

An offered `prepare` request carries
`"codex_invocation": {"version": 1}`. The member is optional in the common
launch and resume grammars and is independently versioned. Packages emit no
result member unless the request offers the capability. Structured and
delegated process preparation does not offer terminal adaptation.

The interactive launch patch or resume result carries
`"codex_invocation": {"version": 1, "argument_prefix_size": N}`. In the SDK,
the result is `Launch_patch::codex_invocation` for both paths. `N` counts the
complete executable prefix of the command the host will actually start:
one for native Codex, two for `node.exe` plus its verified `codex.js`, and
one additional argument for a resume bridge that forwards that invocation.
The arguments at and after index `N` are Codex CLI arguments. Every literal
argument, prompt byte and existing argument boundary retains its meaning.

`N` is a positive integer no larger than `k_max_argv_entries` or the actual
command size. An interactive patch must be prepared and replace its argv;
resume supplies the final command from its process proposal. The host validates
the result against that final command through
`validate_codex_invocation_acknowledgment`. An unsolicited or invalid result is
`launch_patch_invalid`; a missing acknowledgment is `unknown_capability`.

Known records have a closed member set. Malformed request records fail as
`frame_violation` or `unexpected_member`; malformed result records fail as
`launch_patch_invalid`. An unknown positive record version makes this capability
unavailable. Common launch and resume decoders preserve the unrelated core and
leave the capability optional unset. A host that offered the capability then
reports `unknown_capability` when checking its acknowledgment. An unrelated
launch remains available. Readers do not rewrite unknown capability records.
