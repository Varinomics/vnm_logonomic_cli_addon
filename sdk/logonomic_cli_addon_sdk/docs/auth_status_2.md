# auth-status@2

`auth-status` version 2 is an optional, noninteractive observation capability.
Its sidecar payload is `{"environment_names": [...]}`, using the declaration
grammar in [Account probe and usage environments](probe_environment_2.md). It neither requires `accounts`, `quota`,
`usage-scan` nor `structured-session`. Unknown versions and failures remain
local to this capability. It does not log in, launch a browser, export a token,
or provide a credential store.

`prepare` accepts `Probe_prepare_request`: the frozen launcher record,
`launcher_argv`, required frozen `launch_environment`, working directory, admitted asset names and optional
profile-storage/account contexts from `Session_launch_context`. There is no
prompt or structured purpose. The result uses the current
`Structured_prepare_result` grammar of [structured-session@3](structured_session_3.md),
including `incremental`, `keep_stdin_open` and `protocol_state_base64_chunks`: a direct host-owned process proposal,
inspection setup and exact echoes for each supplied context. The host applies
the existing noninteractive process admission, environment, output, deadline,
cancellation and generation-custody rules. Reusing this grammar does not adopt
the structured-session capability.

`inspect` accepts `Probe_inspect_request`: the existing bounded `inspection`
object (`protocol_state_base64_chunks`, `stdout_base64_chunks`,
`stderr_base64_chunks`, `final`, `timed_out`, and `exit_code`) and the original selected `account_context` when supplied. The host
retains the original scope throughout the operation; inspection cannot select
another account.

The result has exactly `complete` (boolean), `protocol_state` (UTF-8 string,
no NUL, bounded by `k_max_protocol_state_bytes`) and, if and only if complete,
`observation`. A final inspection must complete. There is no stdin response,
raw result, adapter diagnostic or free-text error member. Transport errors
remain ordinary protocol failures and produce host-authored messages.

The observation result retains its plain `protocol_state` string; its
representation is distinct from the chunked inspection request and preparation
setup. The host carries that bounded state into the next inspection request.

An observation has exactly `scope` and `state`. States are `authenticated`,
`not_logged_in`, `unknown`, `unavailable`, and `error`. An account's existence
or availability does not imply authentication. These states do not change
account identity and do not suppress a separately available quota probe.

`scope` has exactly:

- `source_account_key`: the exact selected adapter account key, or explicit
  `null` for the package's default scope. Missing is invalid. The host compares
  this to its original selection before publishing the observation.
- `observed_at_ms`: UTC Unix observation time in exact integer milliseconds.
- `fresh_for_ms`: exact integer lifetime in milliseconds; zero means no cached
  freshness. Both integers are within `0..9007199254740991`.

The host derives freshness from the supplied observation time and lifetime,
not request completion. A future timestamp cannot manufacture freshness.
Credentials, raw process bytes and adapter diagnostic text must never enter
the normalized auth projection. Unknown object members are rejected without
echoing their values in decoder diagnostics.

An independently negotiated [current-account@1](current_account_1.md) request
may opt into optional current-account evidence. That evidence is admitted only
beside a completed authenticated observation with the exact same scope.
Missing, malformed, unknown-version or mismatched optional evidence supplies no
identity and does not invalidate otherwise valid authentication completion.
