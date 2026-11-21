# launcher-discovery@1

`launcher-discovery@1` lets a ready package propose launchers and recognize a
user-authored launcher without inheriting the host process environment. The
pair is live only through sidecar, handshake and host-adoption intersection.

## Sidecar payload

```json
{"environment_names": ["EXAMPLE_CLI_HOME", "EXAMPLE_CLI_CONFIG"]}
```

The array holds 0-32 portable environment names. Names follow
`[A-Za-z_][A-Za-z0-9_]*`, have distinct exact spellings, and must not
begin with `LOGONOMIC_` under ASCII case-insensitive comparison. A violation is
`invalid_sidecar` or `reserved_environment_variable`.

## Ambient view

Every request may contain values for `PATH`, `PATHEXT`, `HOME`, `USERPROFILE`,
`LOCALAPPDATA`, `APPDATA`, `XDG_CONFIG_HOME` and `XDG_DATA_HOME`, plus declared
names. Nothing else is inherited. A missing value, invalid UTF-8 value, or a
value above 32766 bytes is omitted. Linux lookup is byte-exact. Windows lookup
and final deduplication are ASCII case-insensitive; a baseline name uses the
canonical spelling shown above. Case-distinct declarations such as `HTTP_PROXY`
and `http_proxy` select separate values on Linux and at most one value on Windows.

## Shared launcher record

The same record is carried by `recognize` and embedded unchanged in
`launch-preparation@3`:

```json
{
  "script_body": "example-cli --chat",
  "runner_key": "direct",
  "environment_text": "EXAMPLE_CLI_MODE=chat"
}
```

`script_body` is valid UTF-8, contains no NUL, and is 1-4096 bytes. For
`direct` it contains no CR or LF. `environment_text` is valid UTF-8, contains
no NUL, and is at most 4096 bytes. Runner keys are `direct`, `bash`,
`powershell` and `cmd`; the latter two are supported only on Windows.

## discover

Request:

```json
{
  "ambient_environment": {"HOME": "/home/user"},
  "default_home_fallback": "/home/user",
  "platform": "linux-x86_64"
}
```

`default_home_fallback` is valid UTF-8, contains no NUL and is at most 4096
bytes. `platform` follows the package platform-ID grammar. The ambient object
is bounded to the eight baseline names plus 32 declared names and applies the
same name, value and Windows case-folding rules as the ambient view above.

Success payload:

```json
{
  "candidates": [{
    "candidate_id": "example_cli_chat",
    "display_name": "Example CLI",
    "script_body": "example-cli --chat",
    "runner_key": "direct",
    "environment_text": "",
    "default": true,
    "evidence": {"source": "path"}
  }]
}
```

There are at most 16 candidates. `candidate_id` follows the operation-name
grammar and is unique. `display_name` follows presentation@1. `evidence` is an
object whose canonical encoding is at most 2048 bytes and is diagnostic only.
At most one supported candidate is default. Candidates using a runner that is
unsupported on the request platform are dropped independently before the
default uniqueness decision; another supported default remains valid. Any
other candidate defect fails the whole result as `discovery_candidate_invalid`.

## recognize

Request:

```json
{
  "launcher": {"script_body": "example-cli", "runner_key": "direct", "environment_text": ""},
  "ambient_environment": {},
  "platform": "windows-x86_64"
}
```

No match is exactly `{"recognized": false}`. A match is:

```json
{
  "recognized": true,
  "display_name": "Example CLI",
  "compatibility_warning": {"code": "old_cli", "message": "Upgrade recommended"},
  "evidence": {}
}
```

The warning is optional; its code follows the reason-code grammar and its
message is control-free and at most 1024 bytes. Evidence follows the discover
bound. Across packages, exactly one recognized result selects a package;
multiple matches are `recognition_ambiguous` and are surfaced for user choice,
never resolved by order, name or package version.
