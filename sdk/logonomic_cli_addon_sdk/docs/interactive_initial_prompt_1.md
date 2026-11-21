# interactive-initial-prompt@1

`interactive-initial-prompt@1` lets a launch-preparation@3 `prepare` start a
new interactive session whose first user message the host writes. The sidecar
payload is `{}` and the capability has no operations of its own: the package
lists the pair in its live handshake, the host adds an independently versioned
`initial_prompt` member to the `prepare` request, and the package acknowledges
it in its success patch. The pair is live only through sidecar, handshake and
host-adoption intersection, and only beside `launch-preparation@3` in the same
generation (`dependency_capability_unavailable` otherwise);
`validate_interactive_initial_prompt_dependency` decides the rule. Absence or an
unknown version affects this capability alone and leaves the launch-preparation
grammar unchanged.

## Request member

The host adds the member only when the pair is live and only for a launch that
starts with an initial prompt; every other `prepare` request omits it.

```json
{
  "initial_prompt": {
    "version": 1,
    "text": "Logonomic continuation: read the brief at /home/zoe/.local/state/logonomic/continuation_briefs/7f3c9a2e5b1d4f60/brief.md before anything else and follow it; do not modify the project until the user confirms.",
    "referenced_file": "/home/zoe/.local/state/logonomic/continuation_briefs/7f3c9a2e5b1d4f60/brief.md"
  }
}
```

- `version` is 1.
- `text` is 1 to `k_max_initial_prompt_text_bytes` (1024) bytes of valid UTF-8
  on one line: no byte below 0x20 and no 0x7F.
- `referenced_file` is the file `text` names: a nonempty absolute native path
  of valid UTF-8 with no byte below 0x20 and no 0x7F, of at most
  `k_max_start_absolute_path_bytes`.
  The host writes it before sending `prepare`, in a directory it dedicates to
  this launch, and keeps it while the session exists. A package may let its CLI
  read that file or that directory, for example through a CLI option that adds
  a readable directory, and nothing wider; it never writes, moves or deletes
  either.
- Neither string contains `{{`. Launch-preparation@3 reads `{{` in a
  replacement argv entry as the start of an asset token and rejects every other
  form, and a package places `text`, and possibly the directory of
  `referenced_file`, in its replacement argv. The host fails a launch whose
  prompt or file would contain `{{` instead of sending it.

A malformed member is `frame_violation`, an unknown member inside it
`unexpected_member`, and a `text` over its bound `payload_too_large`.

## Success echo

A success answering a request with the member carries

```json
{"initial_prompt": {"version": 1, "text_sha256": "<64 lowercase hex>"}}
```

with `disposition.prepared: true` and a replacement `argv`. `text_sha256` is
SHA-256 over the exact UTF-8 bytes of `text`
(`initial_prompt_text_digest_hex`). The echo attests that the prepared
invocation submits exactly `text`, verbatim, as the first user message of a new
interactive session. It does not attest that the CLI can read
`referenced_file`, or that the model reads it. A malformed echo is
`launch_patch_invalid`.

The attestation covers what the prepared invocation determines: the program,
every argument it passes, the launcher's own included, the environment patch,
and every layer between the host and the CLI, such as a shell, a shim or a
package runner, together with how the CLI itself handles a first message it
receives that way. Within that, nothing may change, drop, displace or
precede `text`, and the CLI must not take it as anything but a message, for
example as a command. The user's own CLI configuration, which the invocation
neither sets nor can reliably observe, is outside the attestation: a default
agent chosen in the user's settings, the user's hooks, managed or remote
settings, plugins, and the contents of a file that an argument names, such as
a settings file. These act on the first message as they act on any message
the user types, and a package does not refuse because of them. Additions the
package itself makes to the invocation, such as hooks it installs, are part
of the invocation.

`validate_initial_prompt_acknowledgment` holds the host's rules: a missing echo
is `initial_prompt_unacknowledged`, a digest of other bytes
`initial_prompt_mismatch`, and an echo without a prepared replacement
invocation, or one answering a request without the member,
`launch_patch_invalid`. Each fails the preparation; there is no fallback to an
ordinary launch.

## Refusal

A package that cannot carry the prompt fails `prepare` with status
`unavailable` and reason `initial_prompt_unavailable`. It refuses at least
when:

- its CLI cannot start an interactive session with an initial prompt, which
  the package determines by feature detection, never by a version ceiling or
  allowlist;
- the selected launcher cannot be prepared to carry the prompt;
- the whole rendered invocation, including the launcher's existing options
  and the title guidance, would exceed the launch-preparation@3 argument
  bounds (`k_max_argv_entry_bytes` per entry, `k_max_argv_total_bytes` in
  total); the text bound leaves room for the rest of an invocation but does
  not guarantee the fit;
- quoting cannot deliver `text` byte for byte;
- the CLI, as the prepared invocation starts it, would not submit `text`
  unchanged as the first user message: for example, a launcher argument that
  selects an agent whose own prompt the CLI puts first, or a text the CLI
  would trim, run as a command, rewrite or not send;
- its CLI needs read access to `referenced_file` that the package cannot
  grant.

## Versions

The member exists only at launch-preparation version 3, because only a
replacement invocation can carry the prompt. This departs deliberately from
`account_context`, which every launch-preparation version carries. At versions
1 and 2, `encode_prepare_request` and `encode_launch_patch` throw
`std::invalid_argument` rather than drop the member, and the decoders reject it
(`unexpected_member` in a request, `launch_patch_invalid` in a patch).
Interactive resume never carries it.

## Reasons

`initial_prompt_unavailable`, `initial_prompt_unacknowledged`,
`initial_prompt_mismatch` and `dependency_capability_unavailable`, beside the
launch-preparation@3 reasons. `launch_preparation.h` implements the codec.
