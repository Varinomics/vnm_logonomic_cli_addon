# Process and environment contract

An adapter runs as a process the host owns. This document fixes what that
process sees - its working directory, environment, inherited descriptors -
what it may do with descendants, how its standard streams are handled, and the
deadlines that bound it. `host_simulator.h` implements the environment
construction (`build_adapter_environment`) and the stream handling that
Logonomic's own process custody applies.

## Working directory

The working directory is the generation custody directory: the host's
published copy of the package. POSIX removes write mode from the directory and
indexed files. Windows marks indexed files `FILE_ATTRIBUTE_READONLY` and the
directory inherits the ACL of the host-owned custody root; neither platform
claim is a sandbox against same-user hostile native code. Conforming adapters
write only below the roots the host hands them: `LOGONOMIC_ADDON_SCRATCH_DIR`,
and an acknowledged profile-storage root (`profile_storage_1.md`).

## Environment

The environment is built from an allowlisted platform baseline, plus
`adapter.environment` from the bootstrap, plus host-set reserved variables.
Nothing else from the host's environment is inherited; in particular every
ambient `LOGONOMIC_*` variable is absent, and no `SSH_*`, `*_TOKEN`, `*_KEY`,
proxy or agent variable is in the baseline. Credentials are never inherited.

- Linux baseline: `PATH` (host-constructed as `<custody>/helpers`,
  `/usr/local/bin`, `/usr/bin`, `/bin`), `HOME`, `USER`, `LOGNAME`, `LANG`,
  `LC_ALL`, `LC_CTYPE`, `TZ`, `TMPDIR`.
- Windows baseline: `PATH` (host-constructed as `<custody>\helpers`,
  `%SystemRoot%\System32`, `%SystemRoot%`, `%SystemRoot%\System32\Wbem`),
  `PATHEXT`, `SystemRoot`, `SystemDrive`, `windir`, `TEMP`, `TMP`,
  `USERPROFILE`, `USERNAME`, `APPDATA`, `LOCALAPPDATA`, `ProgramData`,
  `ProgramFiles`, `ProgramFiles(x86)`, `ProgramW6432`,
  `NUMBER_OF_PROCESSORS`, `PROCESSOR_ARCHITECTURE`, `OS`. Names are matched
  case-insensitively and written in these canonical spellings.
- Manifest variables (`adapter.environment`) are set after the baseline and
  may replace a baseline value. A reserved name in a manifest is rejected by
  the validator.
- Reserved, host-set, never overridable by a manifest or a process proposal -
  the whole `LOGONOMIC_` prefix is reserved and these are the variables set:
  `LOGONOMIC_ADDON_PACKAGE_ID`, `LOGONOMIC_ADDON_GENERATION_DIGEST`,
  `LOGONOMIC_ADDON_GENERATION_NONCE`, `LOGONOMIC_ADDON_PACKAGE_ROOT` (the
  custody directory), `LOGONOMIC_ADDON_SCRATCH_DIR` (a host-owned
  per-generation scratch directory, removed at retirement),
  `LOGONOMIC_ADDON_CORE_PROTOCOL_GENERATIONS` (the offered set,
  comma-separated), `LOGONOMIC_ADDON_HOST_PRODUCT` (the host product name;
  Logonomic sets `logonomic`).

Paths in the environment are in the platform's native form.

For a desktop activation, the unreserved baseline plus manifest environment is
limited to 34 entries and 16 KiB of `NAME+VALUE` bytes. The seven reserved
variables have a separate 10 KiB `NAME=VALUE` budget. The complete environment
therefore remains within 26658 raw bytes before the private supervisor-request
JSON is written. These are private composed-start bounds only: public
capability process proposals, discovery and launch patches continue to use
their explicit 4 MiB frame limits rather than acquiring a new aggregate
environment restriction.

## Inherited descriptors and handles

Exactly `stdin`, `stdout` and `stderr` are inherited. Everything else is
closed: on Linux every host descriptor is `O_CLOEXEC` and the spawn closes
from descriptor 3 upward before `execve`; on Windows the child receives an
explicit `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` of the three standard handles.

Windows requires each listed handle to carry `HANDLE_FLAG_INHERIT` during the
`CreateProcessW` call even though the handle list confines what that target
child receives. The spawn wrapper creates one inheritable duplicate for every
requested child entry and puts only those duplicates in the handle list; it
never changes an original handle's inheritance flag. The duplicates are owned
by the spawn and are closed on every success and failure path. Concurrent
wrapper spawns are isolated by their explicit lists, but an unmanaged external
`bInheritHandles=TRUE` spawn without its own list can still capture a temporary
duplicate (never a custody original). Add-on activation must not be deliberately
overlapped with such an unmanaged inheriting spawn. This accepted risk can delay
custody EOF until that external child exits, but grants no package credential or
protocol authority.

## Descendants: the no-detach rule

A conforming adapter or helper spawns only children that remain in the
custodian-owned session and process group (Linux) or Job (Windows).
Daemonizing, double-forking to orphan a process, `setsid`, `setpgid` into
another group, `CREATE_BREAKAWAY_FROM_JOB`, `DETACHED_PROCESS` into a new
session, and a process proposal with detached, new-session or breakaway
intent are conformance violations. The rule is a trusted-code contract, not a
sandbox: the custodian still reaps and kills whatever reparents to it, and a
descendant that escapes the group is what the conformance suite's
`descendants.owned` check and the proposal validator's
`detached_helper_proposal` rejection exist to catch.

## Process proposals

A capability whose result needs a process the host owns answers a process
proposal (`process_proposal.h`): a direct executable, argv, a working-directory
intent, stdin behaviour, a start intent, environment names to pass through
and values to set, package assets to materialize, and a cleanup intent. The
host validates it against the frozen request context and performs the launch.
Adapters never return shell command strings.

`executable` is one of two forms:

- a canonical package path below `helpers/` or `adapter/`, as the string
  itself;
- `{"launcher_program": true}`, the launcher-program reference: the program
  of the launcher the host recognized. The host resolves it from the
  launcher's `argv[0]`: an absolute path is used as-is, otherwise the name is
  looked up in the `PATH` of the frozen launcher environment; failure is
  `launcher_program_unavailable`.

Any other object form is `frame_violation`. `Process_executable` carries the
two forms; `validate_process_proposal` takes a `Process_proposal_policy` whose
`launcher_program_allowed` is true for the session contracts
(structured-session@3, delegated-session@3, interactive-resume@2) and false for
the conformance probe, where a launcher-program reference is `path_escape`.

## Standard streams

`stdout` is protocol-only and continuously drained into the frame decoder;
anything on it that is not a frame retires the generation.

`stderr` is continuously drained into a ring of `k_stderr_ring_bytes`. When
the ring overflows, the oldest bytes are dropped and the retained diagnostic
begins with the literal marker `…[logonomic: stderr truncated]…`. Overflow is
recorded as a truncated diagnostic; it never blocks the adapter and never
fails the generation or any request. An adapter that writes more than the
ring holds is conforming; it has only made its own diagnostics harder to
read.

Writes to the adapter's `stdin` are bounded and serialized: at most
`k_max_stdin_queue_bytes` are queued for an adapter that does not read, after
which the generation retires with `stdin_queue_overflow`. After retirement no
further frame is written.

## Deadlines

| Bound | Meaning |
|---|---|
| `k_custodian_start_deadline_ms` | custodian spawn until its `started` report |
| `k_hello_deadline_ms` | host `hello` sent until adapter `capabilities` received |
| `k_activation_deadline_ms` | custodian spawn until adapter `capabilities`, end to end |
| `deadline_ms` per request | `invoke` sent until its terminal result; then `cancel`; `k_no_invoke_deadline_ms` leaves the request timer unarmed |
| `k_cancel_grace_ms` | `cancel` sent until the terminal result must have arrived |
| `k_shutdown_grace_ms` | `shutdown` sent until the adapter must have exited |
| `k_terminate_to_kill_ms` | terminate (SIGTERM / process termination) until kill (SIGKILL / Job termination) |
| `k_reap_deadline_ms` | kill until every descendant status has been collected |

The values are in `limits.md`.
