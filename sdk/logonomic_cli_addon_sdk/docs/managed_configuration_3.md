# managed-configuration@3

Version 3 grants a bounded transform of one configuration file below the frozen
platform home, with a retained preimage before each changed replacement. Its
declaration has exactly one member:

```json
{"relative_path": ".example/config.json"}
```

`relative_path` uses the canonical package-relative path grammar. The host uses
the single frozen `USERPROFILE` value on Windows and `HOME` elsewhere. An absent,
empty, ambiguous, or relative home is refused; no account-profile fallback,
selector, or temporary directory is substituted. Every existing path component
must be an ordinary directory, and an existing target must be a regular file.
Symbolic links and Windows reparse points are refused. Missing directories and
the target may be created. The adapter never receives or nominates a native path.

The `transform` request and response retain the opaque grant, presence, bounded
base64 content, and transcript redaction of version 1. Versions 1 and 2 retain
their independently negotiated target and write semantics. Unknown versions
remain unavailable locally; they do not select another version's semantics.
Version 3 cannot be selected through `account-profile-managed-configuration@1`,
which binds an account to a selector-based version 1 or 2 target.

The host locks and snapshots the target, validates the preparation, and checks
the returned grant. An unchanged byte result causes no write or recovery record.
For a changed result, the host creates a unique directory adjacent to the target:

```text
<target>.logonomic-recovery-<32 lowercase hexadecimal characters>/
    content.bin
    metadata.json
```

`content.bin` retains the exact snapshot bytes. `metadata.json` records
`recovery_record_version: 1`, `present`, `requested_path`, `resolved_path`, and
the lowercase SHA-256 `content_sha256`. A missing target has `present: false`
and an empty content file, preserving absence independently of an empty existing
file. Content and metadata are exclusively created, flushed, and read back;
the final metadata filename is published only after verification. Partial
attempts have no final metadata and cannot authorize replacement.
On Unix, the recovery directory permits access only to its owner; Windows
records inherit the adjacent directory's access control.

The host rechecks target mapping, presence, and bytes after backup preparation
and revalidates the live package generation and cancellation immediately before
atomic replacement. Backup failure, an observed target change, or lost authority
fails preparation without replacing the target. As with version 1, this does
not claim universal compare-and-swap against noncooperating external writers
between the last check and filesystem replacement.

Successful recovery records remain until explicitly removed; the application
never overwrites them or expires them automatically. For a present preimage,
`content.bin` restores the identified target's prior bytes. For an absent
preimage, restoration means removing the newly created target. A retained record
does not itself claim that replacement completed: it also survives a later
replacement failure or cancellation. Configuration bytes may be private and
must not be copied into diagnostics or transcripts.

Normal launch preparation also completes this transform before exact interactive
resume is proposed. A failed transform prevents hook-enabling environment changes
from being delivered to the launched process.
