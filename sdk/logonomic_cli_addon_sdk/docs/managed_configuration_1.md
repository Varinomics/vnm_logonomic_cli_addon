# managed-configuration@1

`managed-configuration@1` lets an adopted package transform one bounded
configuration snapshot during launch preparation. It does not grant the package
file-system access or a configuration pathname.

Its declaration has exactly these members:

```json
{
  "environment_name": "EXAMPLE_HOME",
  "relative_path": "config.toml",
  "default_home_relative_path": ".example"
}
```

All paths use the canonical relative-path grammar. The host resolves this
selector from its frozen effective launch environment. If the declared
`environment_name` is absent or trim-empty there, it uses frozen `USERPROFILE` on
Windows or `HOME` elsewhere when that variable is present (including a present
empty value); only an absent platform-home variable uses the frozen default
home fallback. It then combines that home with
`default_home_relative_path` and `relative_path`; otherwise it combines the
trimmed declared environment value and `relative_path`. A relative result is
resolved from the frozen normalization directory. The package cannot supply or
learn the resolved path.

The sole operation is `transform`. The host sends:

```json
{
  "grant": "32 lowercase hexadecimal characters",
  "present": true,
  "content_base64": "raw snapshot bytes"
}
```

`content_base64` decodes to at most 786,432 bytes, so the field stays within
the SDK's one-megabyte JSON string bound. A larger configured file is refused
before the package runs and is never modified. The package returns exactly:

```json
{
  "grant": "the exact request grant",
  "content_base64": "raw transformed bytes"
}
```

The host serializes Logonomic transforms for the resolved target, checks the
grant, then rechecks the target's mapping, presence and bytes immediately
before atomic replacement. A changed observed snapshot fails without a write.
A selected symlink chain is resolved completely with cycle rejection; each
symlink remains in place and its final target receives the replacement. An
unchanged result produces no write. A noncooperating external writer can still
change the file between that recheck and the filesystem replacement, as with
the native update path; this protocol does not claim universal cross-process
compare-and-swap authority.

Snapshot and replacement bytes may contain private configuration. The shared
transcript recorder replaces `content_base64` in grant-bearing records with a
fixed placeholder; adapters must not copy those bytes into diagnostics.
