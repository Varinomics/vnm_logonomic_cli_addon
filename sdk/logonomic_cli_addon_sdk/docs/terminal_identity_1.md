# terminal-identity@1

This optional helper-ingress companion reconciles the conversation selected in
an existing native terminal before a vendor's next hook. It uses the existing
`classify_event` operation and lifecycle transition writer. It requires
helper-ingress@2 or @3 and lifecycle-events@2. Its declaration payload is `{}`.

The host offers `terminal_identity:{version:1}` on interactive launch and resume
preparation only when this pair is live. A provider opts in for that particular
launch by returning `terminal_identity:{version:1,context:{...}}` in its launch
patch. The opaque context freezes the provider's native configuration and store
namespace. It is at most 16 KiB of canonical JSON. Return it only after applying
the native title format needed to establish exact foreground identity. Missing
preparation preserves the existing hook and title behavior. A provider must not
apply an identity overlay without the offer and returned preparation. A malformed
claimed preparation fails launch validation, since silently discarding it would
leave an unmanaged title overlay active.

The base classify request gains an optional input:

```json
{"terminal_identity":{"version":1,"context":{},"terminal_event":null}}
```

The terminal owner supplies the same frozen context and its latest actual raw
`terminal_title_changed` event, including terminal ID, native generation, source,
stream, sequence and payload. Before any event, `terminal_event` is null. The
scoped event is at most 16 KiB of canonical JSON. A helper cannot author this
input. Ordinary helper, attention and asynchronous-question requests retain
their existing envelopes and results; this optional base input lets the provider
reject a delayed contradictory hook before producing context or execution
updates. A same-identity delayed start may confirm execution context under the
current transition nonce without starting another epoch.

For a title observation, the owner selects `terminal-identity@1` with the base
event `{"name":"NativeSignal","body":{"origin":"host_terminal"}}`. Its
result is the existing base classification plus required `terminal_identity`,
either null or `{"provider_session_id":"exact-full-id","title":"human caption"}`.
The exact identity has the lifecycle identity bound; the caption is at most
1024 UTF-8 bytes and has the provider's identity suffix removed. It preserves
the original configured native title, including any leading activity marker.
The host applies its ordinary display-title normalization and manual/agent title
precedence. Identity metadata is never used as a human title.

An unavailable or unrecognized title returns `ignore` and null. Proof for the
current identity returns `ignore` with the caption. A different selected identity
returns a typed transition with exactly one started observation for that same
full identity and a valid native resume record. Selection proves a native
foreground conversation, not how it was created; `resume` admits both an unseen
conversation and an explicit return to a retained conversation. No title alone
proves a working directory, prompt, turn completion or attention state.

The host keeps one classification in flight and coalesces newer observations.
It fences completion by live provider binding, native generation, epoch and
normalized title revision, generates a fresh transition nonce and commits
through its existing canonical lifecycle/provenance writer. Raw title sequences
still serve attention. Only the identity revision canonicalizes an isolated
leading Unicode activity glyph or ASCII spinner (`|`, `/`, `-`, `\`), optionally
after an unchanged recording indicator (`●` or `*`). It also coalesces the
leading `[ ! ]` / `[ . ]` marker, including after that indicator, while preserving
all following status text. Indicator presence and the complete remaining title,
including the identity-bearing content, still distinguish revisions. This key
does not change visible caption normalization or raw attention evidence.
Providers must qualify only layouts stable under these leading animation forms.
For Codex this means absent or leading activity; custom nonleading or multiple
activity layouts retain their original config and hook behavior without an
identity overlay.

An undispatched native hook admitted during that same title revision may be
classified after its owner selection commits. Only this opted-in path refreshes
the request's ledger to the new epoch, preserving first-turn facts queued behind
title resolution. Its native generation and stable title revision must still
match. Classification receives the committed identity and latest title proof,
and contradicting transitions are rejected before any context update. In-flight
or differently scoped old-epoch deliveries retain the ordinary rejection rules.

If title content changes while an ordinary hook is being classified, the host
discards that reply and resolves the latest title before classifying the same
admitted hook again. It retains the original key, nonce, epoch and total
classification deadline. Only proof for the same identity and epoch permits a
refreshed revision; selecting another identity retires the in-flight hook.
Unresolved evidence cannot authorize old context or execution updates.

Provider-specific title syntax, version qualification and exact-ID resolution
stay in the provider. A truncated prefix is not an identity. A mere title change
is not a selection. Ambiguous, truncated, unknown-version or unresolvable native
evidence yields no proof. Lookup uses only the launch's frozen native namespace.
Latest title authority also gates delayed hooks; a stale reply or late old
SessionStart must not undo A to B or an explicit B to A return.
