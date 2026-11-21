# profile-storage@1

`profile-storage@1` is the capability through which an adapter acknowledges
the isolated profile root the host allocates for its package. Any later
capability operation that needs managed profile storage carries the same
context and is subject to the same acknowledgment rule.

## Declaration

Presence-tagged by the sidecar `capabilities/profile-storage@1.json`:

```json
{"capability_id": "profile-storage", "version": 1, "payload": {}}
```

The adapter lists `{"capability_id": "profile-storage", "version": 1}` in its
handshake. Version 1 has one operation: `acknowledge`.

## Invocation

The host validates the package ID and lexical containment before creating any
directory, allocates the root, canonicalizes it (absolute, symlink-free, native
separators) and sends it as the `profile_storage` member of the payload. On
POSIX new package-owned suffix directories receive owner-only mode; existing
permissions are preserved. On Windows the host rejects reparse points and the
suffix inherits the profile local-data root's ACL; this contract does not
replace that ACL with a separate DACL.

```json
{"profile_storage": {"version": 1, "root": "/abs/canonical/host/allocated/root"}}
```

The same member is embedded in the invocation payload of any later capability
operation that requires managed profile storage.

## Result

A successful result echoes the member byte for byte:

```json
{"profile_storage": {"version": 1, "root": "/abs/canonical/host/allocated/root"}}
```

The host compares as bytes; there is no normalization. The three failures are
hard failures of that operation, reported with status `error`:

| Condition | Reason |
|---|---|
| no `profile_storage` object in a `success` payload | `profile_storage_unacknowledged` |
| `version` absent, not an integer, or different | `profile_storage_version_mismatch` |
| `root` absent, not a string, or different - a vendor default directory, a path outside the allocation, any other byte difference | `profile_storage_mismatch` |

A package without the sidecar, or without the pair in its handshake, receives
`unknown_capability` for `acknowledge`, and every later operation that
requires managed profile storage hard-fails for it.

## Allocation

The root is `<local_data_root>/cli-addons/profile-storage/<package_id>/default`
below the host's profile local-data root for operations without a selected
account. With a selected accounts@2 key, `default` is replaced by the
`account_identity` digest from [accounts@2](accounts_2.md). Existing historical
default scopes remain exact; selection does not relocate their files.
The host allocates per package ID and account scope,
so no other package ever receives that root and a foreign root echoed by an
adapter is a mismatch by construction. The host never rewrites the root for an
adapter that answers with a different one; it fails the operation.

`profile_storage.h` implements the host side - `allocate_profile_storage_root`,
`profile_storage_member`, `profile_storage_acknowledgment_failure` - and the
host simulator applies it to every invocation that carries a profile-storage
context.
