# launch-preparation@3

Account-selected preparation requests and successful results may carry the
independently versioned `account_context` member specified by
[accounts@2](accounts_2.md). When present, its exact acknowledgment is required;
absence leaves this capability's ordinary request and result grammar unchanged.

`launch-preparation@3` prepares an interactive launcher against its frozen
effective environment. The pair is live only through sidecar, handshake and
host-adoption intersection. Its operation is `prepare`.

The host waits `k_launch_prepare_deadline_ms` ([limits.md](limits.md)) for the
answer, including any installed-CLI probe needed to prepare the invocation.

The sidecar payload is `{"environment_names": [...]}`, using the
launcher-discovery allowlist grammar. The host includes baseline and declared
names plus admitted explicitly authored launcher names. Referencing an ambient
variable in an assignment does not by itself expose that variable.

## Request

```json
{
  "launcher": {
    "script_body": "example-cli --chat",
    "runner_key": "direct",
    "environment_text": ""
  },
  "launcher_argv": ["example-cli", "--chat"],
  "launch_environment": {"platform": "windows-x86_64", "effective_environment": {}},
  "working_directory": "C:\\repo",
  "title_instruction": "Example CLI chat",
  "declared_assets": ["assets/config.json"],
  "profile_storage": {"version": 1, "root": "C:\\profile\\org.example.cli\\default"}
}
```

`launcher` is exactly the typed launcher record of launcher-discovery@1.
Required `launcher_argv` is the host-resolved complete invocation, including the
selected shell when applicable. Required `launch_environment` is the same frozen
evaluation that supplies this launcher. It follows the public `Launch_environment`
codec and preserves overrides and removals. The adapter must preserve the selected
launcher and shell semantics when proposing replacement argv.
`working_directory` is valid UTF-8 and at most 4096 bytes;
`title_instruction` is valid UTF-8 and at most 4096 bytes; neither contains
NUL. `declared_assets` is the admitted
managed-assets@1 list, or an empty list, with at most 64 distinct canonical
paths below `assets/`. `profile_storage` is present only when the host supplies
that context and follows profile-storage@1.

## Success patch

```json
{
  "environment": {
    "set": {
      "EXAMPLE_CLI_CONFIG": "{{asset:assets/config.json}}",
      "EXAMPLE_CLI_CONFIG_JSON": "{{asset_json:assets/config.json}}"
    },
    "unset": ["EXAMPLE_CLI_OLD_CONFIG"]
  },
  "disposition": {"version": 1, "prepared": true, "lifecycle_events": false, "coordination_delivery": false},
  "working_directory": "inherit",
  "profile_storage": {"version": 1, "root": "C:\\profile\\org.example.cli\\default"}
}
```

`set` and `unset` each hold at most 64 entries. Environment names follow the
portable grammar, are compared ASCII case-insensitively for uniqueness and
set/unset overlap, and cannot begin with `LOGONOMIC_` under the same comparison.
Raw values contain no NUL and are at most 32766 bytes.

`working_directory` is `inherit` or `repository_root`; both retain the
host-selected repository root. A request carrying profile storage
must receive its exact version/root echo or fail under profile-storage@1.

## Asset tokens

A value may contain at most 16 tokens. `{{asset:assets/path}}` expands to the
native path of the host-materialized byte-for-byte asset.
`{{asset_json:assets/path}}` expands to that path serialized as one JSON string
literal. Any other `{{` form is invalid. The path must be in both the sidecar's
declared asset set and the custody index. Substitution is all-or-nothing and
the final expanded value, not only its template, is bounded to 32766 bytes.

Malformed structure, duplicate/overlapping names or tokens are
`launch_patch_invalid`; bounds are `payload_too_large`; reserved names are
`reserved_environment_variable`; undeclared or unindexed references are
`asset_undeclared` or `asset_reference_invalid`. Any failure leaves the
original launcher unchanged and uses the product's deterministic unavailable
preparation path.

A `prepare` request may also carry the `initial_prompt` member of
[interactive-initial-prompt@1](interactive_initial_prompt_1.md); without it,
this request and result grammar is unchanged.

## Invocation disposition and optional replacement

The required `disposition` record contains version 1 and the three Boolean
flags shown above. These flags restrict the package's declared capabilities for
this launch. They cannot grant undeclared capabilities. A prepared launch may admit coordination
without lifecycle observation. An unmanaged launch sets all three flags false,
omits `argv`, and returns an empty environment patch. This is a successful,
unchanged ordinary launch rather than a launch refusal.

Optional `argv` replaces the complete process invocation. It has 1–64 entries,
a nonempty executable, at most 4096 UTF-8 bytes per entry, and at most 11264
aggregate bytes. NUL is forbidden. Arguments may reference indexed and declared
managed assets using the existing asset-token grammar; the host validates and
materializes those references before launch and rechecks expanded bounds.
Literal immutable package paths are also permitted. Shell command arguments
must quote concrete final paths correctly; raw token substitution is not a
shell-quoting operation.

No package may change reserved host environment names through this patch.

Optional boolean `apply_managed_configuration` requests the separately declared
and live `managed-configuration` transformation at its adopted version. It
defaults to false and is forbidden on an unmanaged launch. The host validates
the preparation and its assets first, then snapshots the declared target, invokes the pure transform,
and commits only under its grant and unchanged-content checks. The target comes
from the frozen effective launcher context and capability declaration; neither
the launch patch nor the transform response can name another target. A failed
transformation fails preparation before the provider process is launched.
