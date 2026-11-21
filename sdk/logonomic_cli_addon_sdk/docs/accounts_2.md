# accounts@2

Accounts is an independently negotiated capability. Its absence does not
disable launch preparation, sessions, profile storage, authentication, quota,
or usage. A failed accounts request cannot be interpreted as an empty inventory.
The host retains ownership of selection, durable identities, accents, privacy,
and persistence. The adapter owns vendor profile interpretation.

## Declaration and requests

The sidecar payload is `{"environment_names": [...]}`, with the declaration
grammar of [launcher-discovery@1](launcher_discovery_1.md). Both `discover` and
`resolve` receive `Accounts_launcher_request`: the typed `launcher`, required
frozen `launch_environment`, `default_home_fallback`, and
`normalization_directory`. The latter is the host process working-directory
snapshot used to normalize native relative profile paths. Environment-text
expansion belongs to the host. The adapter interprets its recognized launcher
syntax, including supported leading script assignments, using this input.

Discovery includes the profile selected by this exact launcher, even outside
the ambient default inventory. Resolution must name a key in that inventory.
The host retains the admitted inventory with its package generation. Accounts
negotiation is independent of launcher-discovery negotiation.

When rediscovery retains a key, the host keeps its first admitted profile path
for subsequent grants and their exact acknowledgments. On Windows, a new
spelling may identify the same native path when the parent of each differently
cased component establishes case-insensitive lookup. Paths that name distinct
directories, or that change between present metadata and omitted metadata,
invalidate the rediscovery.

Providers deriving keys from native paths can hash the opaque bytes returned
by `native_account_profile_identity`. On Windows it preserves the provider's
existing folding convention for insensitive components and separates children
of case-sensitive directories, including all-lowercase spellings, from the
historically folded key. Where a provider uses invariant Unicode lowercase,
linguistic case peers outside one native ordinal case class also get distinct
identities. Other characters in that component still follow native case
lookup. Providers with historical ASCII-only folding preserve their existing
keys for these Unicode names. A separated key encodes the lookup rule of
each component. Moving case sensitivity to a different directory in the path
cannot transfer the key to a sibling. Missing descendants inherit the nearest
known lookup rule for ordinary key derivation. In a separated key, a cased
component whose fold is only inferred also retains its exact spelling and the
fact that the fold was inferred. Creating its parent may establish a new
key when the original lookup rule was unknown. When the volume or its directory policy
is inaccessible, an unverified identity preserves exact component spelling in
a separate domain. Comparing a retained grant through
`same_native_account_profile` requires independently observed casing rules
at each component whose spelling differs. A byte-identical missing suffix
retains its binding after an observed ancestor's casing alias.

For a previously issued path-bearing account context, a provider may verify
its old key against the hash input from `historical_account_profile_identity`.
This requires the originally granted locator, the same selected native path,
and observable containing directories. The operation echoes its original
grant exactly. A historical key without a locator cannot authorize either
member of a native-name collision. An unverified volume or vanished parent
cannot authorize a former key through a guessed case rule.

A provider that issued the former folded key must check
`account_profile_allows_key_only` as well as current key equality for a
key-only grant. This rejects an old-compatible key when a missing parent
makes the remaining casing ambiguous. A newly separated key or an uncased
suffix remains eligible. Discovery still produces prospective accounts for
profiles a launcher will create.

## Inventory and resolution

`discover` returns `{"accounts": [...]}`. Each record contains:

- `adapter_account_key`: 1–256 UTF-8 bytes, without NUL. It is stable, opaque,
  case-sensitive and non-secret. The host never trims, folds, parses, or logs it.
- `display_name`: the bounded display-name grammar of presentation@1. It tells
  the account apart from the provider's other accounts, and every adapter
  derives it the same way. An account with a filesystem profile is named by
  `account_display_name(profile_path)`: the name of the profile's folder, the
  last component of the path with trailing separators and dot components
  ignored, such as `.example-cli` for the default profile or
  `.example-cli-work` for one a launcher selects by override. A root, such as
  a drive or a Windows share (`\\server\share`), or a folder named only by
  white space, is named by its whole path. Surrounding
  white space is trimmed and controls and line breaks become U+FFFD. A name
  longer than the grammar allows keeps as many of its leading and trailing
  characters as fit around `…` (U+2026), the leading part never the shorter:
  first 20 and last 19 characters when the character bound decides. An account
  without a filesystem profile is named by its adapter after the provider's
  own label for that account, such as the login it signs in as. The name is
  never a constant product or provider name, which would name every account
  alike.
- `status`: `unknown`, `available`, or `unavailable`. This describes account
  availability, not authentication or quota status.
- Optional `profile_path`: a nonempty absolute native UTF-8 filesystem path,
  at most 4096 bytes without NUL. The adapter supplies a real path only when
  this account has a filesystem profile. Absence is represented by omission;
  null, empty strings and synthetic placeholder paths are invalid. The host
  does not treat this metadata as its allocated managed profile root.

There are at most 64 records. A malformed record, unknown member, or duplicate
exact key invalidates the entire result with `account_record_invalid`.
Keys differing only in case are distinct. Credentials, arbitrary provider data,
auth status, quota and usage are not account record fields. Diagnostics identify
the failing rule without quoting account values.

`resolve` returns `{"adapter_account_key": "..."}` or
`{"adapter_account_key": null}`. A selected key must exist in the inventory
admitted from the same package. Resolution cannot introduce a second inventory.
Null means that the launcher selects no account; the host must not silently
substitute the first discovered account. Failure remains an explicit failure.

The durable identity is the tuple `(package_id, adapter_account_key)`.
`account_identity` produces `a` followed by lowercase SHA-256 hex over each
member of `(package_id, "adapter-account", adapter_account_key)` encoded as
decimal UTF-8 byte length, `:`, then exact member bytes. The discriminator
keeps the account identity domain separate from other host identities.
Opaque keys never become filesystem path components.

When an operation selects an account, its preparation request carries the
accounts-owned member:

```json
{"account_context": {"version": 1, "adapter_account_key": "Primary"}}
```

The account-context record is independently versioned from the accounts
capability. Version 1 contains the exact key. Version 2 additionally requires
`profile_path`, either the exact canonical native profile path or explicit null:

```json
{"account_context": {"version": 2, "adapter_account_key": "Primary", "profile_path": null}}
```

Both record versions are current. They preserve native path metadata without
encoding it into the bounded opaque key. A result echoes the requested record
version and every member exactly.

This member is independently versioned in launch-preparation, structured and
delegated preparation, and interactive resume. Its presence requires accounts
support. An unsupported version fails the selected-account operation, leaving
unrelated operations available. Every successful preparation result echoes the
exact member. Missing or mismatched echoes fail with
`account_context_unacknowledged` or `account_context_mismatch`; an adapter cannot
silently prepare a different account. The enclosing capability versions and
core protocol generation stay unchanged.

Managed storage keeps the existing profile-storage@1 root/version echo. An
account scope uses `<profile_storage_root>/<package_id>/<account_identity>`;
an operation without accounts retains `<profile_storage_root>/<package_id>/default`.
Both use the same confinement, directory ownership and link checks. Historical resume preserves its recorded account and root through a compatible
current launcher. A launcher selecting another account cannot substitute that
account or its profile root. If no compatible launcher exists, resume reports
unavailable; ambiguous selection reports ambiguity.

An account key identifies the selected source profile for custody and routing.
It does not attest which provider principal produced historical usage in that
profile, including evidence retained after a sign-in change. Historical principal
attribution requires separate provider evidence. Absence, unsupported versions,
and failures of accounts remain local to this capability.
