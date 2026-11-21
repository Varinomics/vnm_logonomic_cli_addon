# account-profile-managed-configuration@1

`account-profile-managed-configuration@1` is optional static metadata. It
binds a retained selected profile from `accounts@2` to one exact live
`managed-configuration` version. It has no adapter operation and is not part
of the runtime capability handshake.

Its sidecar payload has exactly one member:

```json
{
  "managed_configuration_version": 1
}
```

`managed_configuration_version` is either `1` or `2`. The host admits this
binding only when that exact `managed-configuration` pair is live and the
accounts result retains the selected profile. An absent binding leaves the
existing managed-configuration behavior unchanged; it does not select a
profile or make an unavailable managed-configuration version live.

The binding does not alter either managed-configuration wire grammar, its
transform grant, or its file authority.

The selector name comes only from that admitted managed-configuration
sidecar. Its value comes only from the selected account record retained by
the same package generation; a missing or null profile refuses the bound
operation. The host overlays this selector before preparation, uses the same
frozen context for the exact-version transform, and injects it into the native
process environment. A launch patch may not set or unset the bound selector;
a process proposal may not contradict the frozen value. Package-returned
paths and launch patches never nominate the configuration target.
