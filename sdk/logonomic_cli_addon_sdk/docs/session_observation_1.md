# session-observation@1

This optional presence declaration has the empty object `{}`. It allows an
already authenticated multiplexer Session to register independently scoped
observations of native seats. It does not launch a native process, allocate a
terminal, create a delegated Task or authenticate public native metadata.

The parent must be a live terminal bound to the same current package generation
that declares this capability. Calls use the existing authenticated control
lane. A provider's per-launch transport must establish that parent authority
before calling; instance, rig, seat or occupant environment variables cannot
establish it. Do not persist parent or observation credentials in native specs,
wrapper arguments, global environments or diagnostics.

## Registration

`logonomic.session_observation register` takes `session_capability` for the
parent and a `registration` object with these exact fields:

| Field | Contract |
| --- | --- |
| `version` | Integer `1`. |
| `request_nonce` | Fresh 32-character lowercase hexadecimal nonce per native launch. |
| `identity` | Object containing `instance_id`, `group_id`, `seat_id`, `occupant_id`; each is nonempty UTF-8, at most 1024 bytes, without control characters. |
| `native_package_id` | Installed native provider package ID; the host resolves its current generation. |
| `working_directory` | Absolute native working directory. |
| `replaces_session_id` | Optional exact previous host observation ID for this same seat. |

The full registration is immutable. Repeating an identical nonce and payload
returns the same authority only while that registration remains live. Reusing
a nonce with different inputs, replaying a retired registration or replacing
another seat is refused. A current seat must be replaced explicitly. The host
issues a new durable Session ID, credential and launch activation even when
the public occupant and native conversation did not change.

A parent can retain at most 256 concurrent observations and 4096 registration
nonces during one authority lifetime. Retired nonces remain refused; exhausting
that replay bound never discards old nonces to admit a stale registration.

Successful control payloads contain `accepted: true` and `authority`, whose
version-1 object contains `session_id`, `session_capability` and `activation_id`.
The Session ID is a lowercase UUID; the credential is 64 lowercase hexadecimal
characters; the activation is 32 lowercase hexadecimal characters. Failed
admission contains `accepted: false` and `reason`, with no authority. The host
persists identity and parentage before exposing the registration. Authority is
only in memory and is not recovered from history.

The native package owns classification and model-context delivery. Registering
an observation does not declare those native capabilities or provide a smaller
instruction contract. Provider activation requires the host's complete existing
instruction and callback path to work for that observation.

## Preparing a native launch

The authenticated parent broker uses
`logonomic.session_observation prepare_and_register` to combine native
preparation and registration. Its arguments are the same `session_capability`
and `registration` objects, plus `native` with exactly these fields:

| Field | Contract |
| --- | --- |
| `argv` | Array of strings containing the exact native argument vector, including its executable spelling, empty arguments and any `--` boundary. |
| `effective_environment` | Object of string values containing the native process's final environment snapshot. The host does not rerun shell initialization or substitute a saved launcher account. |

The registered working directory is the native launch directory. The host calls
the installed native provider's maintained `launch-preparation@3` producer with
that context and its usual static title guidance. The native provider retains
ownership of managed assets, plugin/settings integration, argument parsing and
classification. The host separately resolves any supported native account
metadata from the same environment for history attribution. Missing metadata
leaves attribution unavailable; the dashboard's account is never substituted.

A successful payload contains `accepted: true`, `authority`, and `launch`:

| Field in `launch` | Contract |
| --- | --- |
| `argv` | The native provider's prepared argument vector. |
| `environment_updates` | String values to set in the original native environment. |
| `environment_unsets` | Environment names to remove from the original native environment. |

The host replaces inherited manager credentials with this observation's fresh
Session capability and the retained native provider generation's helper
environment. It requires the real authenticated control listener before
returning a prepared child. If that listener cannot be created, it refuses the
operation and retires the child rather than returning unusable authority.
The caller applies the returned delta to the original snapshot and executes the
returned argument vector; it must not serialize either into a rig specification.

The nonce binds both the registration and exact native snapshot. A pending
operation returns `registration_in_progress`; a successful replay returns the
same scoped preparation only while its authority is live. A changed snapshot is
`registration_conflict`. Parent revocation or native generation replacement
fences pending results and retires their observation state.

The complete existing instruction producer serves each observation's title,
summary, worktree, changes, delegation, Notes and messaging requests according
to the same availability rules as a standalone Session. Its one-shot delivery,
journal, native activation, archive and Notes binding belong to that durable
observation. A dashboard terminal does not become the native process's terminal
or supply its conversation identity. Continuous native observation is available
only when the retained native provider declares it; no terminal title stream is
invented for a native seat.

This operation is a host broker boundary, not a bootstrap authentication
protocol. A native wrapper must first be attached to a trusted parent broker by
an independently authenticated per-launch transport. That broker keeps the
parent credential and returns only the scoped child result to its authenticated
wrapper. Public seat metadata, inherited tmux variables and a stable wrapper path
do not establish that trust. A multiplexer without such an enrollment seam must
not advertise managed hook support merely because this host capability exists.

## Retirement and lifetime

`logonomic.session_observation retire` takes the authenticated parent
`session_capability` and a `retirement` object containing `version: 1`, the
observation's `session_id`, and its exact `activation_id`. Retirement closes the
observation's journal and coordination grants. Parent revocation and host
shutdown retire dependent observations. Old responses cannot act through a
replacement's authority. Native tmux processes remain owned by the multiplexer;
retirement never sends them a signal or closes an original terminal surface.

The optional durable relation contains the parent Session ID outside the opaque,
independently versioned observation payload. History retains that relation even
when the payload version is unsupported. Observations have no native runtime ID;
they do not use the parent's runtime ID as their own.

The codecs are in `session_observation.h`. This contract is independent of
`structured-model-observation@1`, which reports model facts from a structured
execution's inspect stream.
