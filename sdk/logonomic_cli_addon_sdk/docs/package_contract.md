# Package contract

A CLI add-on package is a self-contained directory. This document defines its
identifiers, its layout, the three file formats every reader parses, the
authority each of them carries, and the generation digest that binds them.
The SDK library implements every rule here: `ids.h` for the grammars,
`bootstrap.h`, `integrity_index.h` and `sidecar.h` for the files,
`package_validator.h` for the whole, `generation_digest.h` for the digest.

## Identifier grammars

Every grammar is a byte-level rule. Matching is byte equality; there is no
case-folding and no Unicode normalization anywhere.

- **Package ID**: canonical lowercase ASCII reverse-domain name; 3-128 bytes;
  at least two `.`-separated segments; each segment 1-63 bytes, first and last
  byte a letter or digit, interior bytes letters, digits or `-`. Uppercase,
  other separators, empty segments, leading, trailing or double hyphens and
  any normalization alias are rejected, as is a first segment that is a
  Windows device name (see the path rule below).
- **Capability ID**: 1-128 bytes of lowercase ASCII letters, digits, `.` and
  interior `-`; first and last byte a letter or digit; no `..`, and no `-`
  adjacent to `.`. Interior repeated hyphens are allowed.
- **Operation name**: 1-64 bytes of lowercase ASCII letters, digits and `_`;
  first byte a letter.
- **Reason code**: the operation-name grammar.
- **Version**: a JSON integer in `[0, 65535]`. It is exact in every JSON
  implementation, fits a `std::uint16_t`, and keeps sidecar file names short.
- **Release version** (`release_version`): 1-64 UTF-8 bytes without control
  characters. Informational only; never compared for compatibility.
- **Platform ID**: `<os>-<arch>` in lowercase letters, digits and `_`. The
  fixed platform matrix is `linux-x86_64`, `windows-x86_64` and `macos-arm64`; a platform ID
  in the grammar that a reader does not know is ignored, not rejected.
- **Request ID**: host-issued as one consecutive, monotonically increasing
  numeric sequence; `r` followed by decimal digits without a leading zero
  (`r0` is the only ID whose digits begin with `0`); 1-64 bytes in total;
  unique within one adapter generation.
- **Canonical package-relative path**: UTF-8, `/`-separated, 1-200 bytes,
  1-16 segments of 1-128 bytes; no empty segment, no `.` or `..` segment, no
  leading `/`, no `\`, no control character (bytes below 0x20 and 0x7F), none
  of `< > : " | ? *`, no segment ending in `.` or space, no segment whose name
  before its first `.` is a Windows device name (`con`, `prn`, `aux`, `nul`, or
  `com` or `lpt` followed by `1`-`9` or the superscript digits `¹`, `²`, `³`),
  compared under ASCII case-folding and with or without an extension; the first
  segment is one of `adapter`, `capabilities`, `helpers`, `assets`, `notices`,
  `provenance`. Two indexed paths that are equal under ASCII case-folding are
  rejected, so that a package validated on one platform installs on a
  case-insensitive file system unchanged. Apply the same check to every path
  prefix: an ASCII-case-equivalent prefix must have one exact spelling and
  must be consistently a directory or a file. For example, `assets/Foo` plus
  `assets/foo/x`, and `assets/Foo/x` plus `assets/foo/y`, are invalid index
  trees. Do not merge or normalize conflicting directories to admit them.
- **Generation digest**: 64 lowercase hexadecimal characters.
- **Generation nonce**: 32 lowercase hexadecimal characters, issued by the host
  for each admitted generation.

## Native resume folders

Resume consumers use `normalized_resume_directory` from `launcher_utilities.h`
for absolute local process folders. It accepts UTF-8 native path spelling, normalizes
`.` and `..` and repeated/trailing separators, and returns native separators.
Root paths retain their root separator.

Normalization is lexical and performs no filesystem lookup. Links, junctions
and substituted drives retain their spelling for process launch and provider
project-folder lookup. Resolved paths are only suitable for same-folder
comparisons. Request digests continue to bind the original request bytes;
consumers normalize a separate execution value.

## Package layout and authority

```text
<package directory>/                          the directory name is not identity; the bootstrap is
  logonomic-cli-addon.json                    bootstrap manifest (required)
  package.files.json                          integrity index (required; the name is fixed by the bootstrap)
  adapter/<executable>                        indexed, kind executable
  capabilities/<capability_id>@<version>.json indexed sidecars, kind sidecar
  helpers/  assets/  notices/  provenance/    indexed package bytes of the kind their directory names
```

Authority is explicit:

- the bootstrap is authoritative for package identity, platform, adapter entry
  point, core protocol generations and integrity-index location;
- the integrity index and the host-verified bytes are authoritative for one
  package generation digest;
- each sidecar is authoritative only for the static declaration of its own
  capability version;
- the adapter handshake is authoritative for which declared implementations
  are live in that generation;
- the host's adopted capability versions are authoritative for what the host
  consumes.

Except for the static declarations below, the effective capability set is the
intersection of a valid sidecar, the adapter's handshake declaration and a
host-adopted version. A handshake
identity, generation or core-protocol disagreement invalidates the generation.
A sidecar/handshake disagreement or an unsupported version makes only that
capability unavailable, with a diagnostic. An adapter-only capability has no
sidecar authority and is ignored. Unknown sidecars are retained as immutable
package bytes and ignored by every facility that does not understand them.

Authors advertise [`coordination-delivery@1`](coordination_delivery_1.md) in
the handshake. The host reads its admitted sidecar for an activatable generation.
[`account-profile-managed-configuration@1`](account_profile_managed_configuration_1.md)
is excluded from the runtime handshake and requires its `accounts@2` and exact
managed-configuration dependencies to be live. These exceptions do not make
an unavailable adapter operation live.

The installer owns whole-directory replacement. The desktop scans complete
packages at startup and runs a custodied copy of the selected generation.
Stage updates and keep backups outside all scanned roots. Restart to select
an installation, update or removal. See the [operator's installation and
replacement instructions](https://varinomics.com/docs/logonomic/launchers-accounts-and-addons/#install-and-maintain-cli-add-ons).

## Bootstrap manifest `logonomic-cli-addon.json`

```json
{
  "bootstrap_version": 1,
  "package_id": "org.example.unknown-fixture",
  "release_version": "1.0.0+fixture",
  "platforms": ["linux-x86_64", "windows-x86_64"],
  "adapter": {
    "path": {
      "linux-x86_64":   "adapter/unknown-fixture-adapter",
      "windows-x86_64": "adapter/unknown-fixture-adapter.exe"
    },
    "argv": ["--behavior", "conformant"],
    "environment": {}
  },
  "core_protocol_generations": [1],
  "integrity_index": "package.files.json"
}
```

Rules:

- Strict object: exactly these members and no others; every member is
  required except `adapter.environment`, whose default is empty. The file is at
  most `k_max_bootstrap_bytes` and is valid UTF-8. Numbers are integers without
  fraction or exponent, so canonicalization is exact.
- `bootstrap_version` is read first and must be `1`. Any other value makes the
  package unavailable with reason `unsupported_bootstrap_version`; no other
  member is interpreted.
- `package_id` and `release_version` follow the grammars above.
- `platforms` lists 1-8 distinct platform IDs. A package that does not list the
  host platform is unavailable with reason `platform_not_supported`.
- `adapter.path` is an object keyed by platform ID. Every key appears in
  `platforms` and every listed platform has a path. Each value is a canonical
  relative path whose first segment is `adapter`, indexed with kind
  `executable`. The adapter is invoked directly - `execve` or `CreateProcessW`,
  never a shell. A `.bat`, `.cmd`, `.ps1` or `.sh` extension is rejected as
  `shell_adapter_entry_point`; on Windows the extension must be `.exe`; on
  Linux the file must begin with the ELF magic, so an extension-less script
  with a shebang is rejected the same way. On macOS the image must contain an
  ARM64 Mach-O executable slice; a shared library is not an adapter.
- `adapter.argv` is 0-64 strings of 1-4096 bytes each, at most 11 KiB in
  total, without control characters. The host prepends the adapter's own path
  as `argv[0]` and preflights the fully quoted native Windows command line.
- `adapter.environment` is 0-16 entries; names are 1-128 bytes matching
  `[A-Za-z_][A-Za-z0-9_]*`, values are at most 32766 bytes without NUL. A name
  beginning with `LOGONOMIC_` is rejected with reason
  `reserved_environment_variable`.
- `core_protocol_generations` lists 1-8 distinct versions. If it does not
  intersect the host's supported set, the package is unavailable with reason
  `unsupported_core_generation` and no process is started.
- `integrity_index` is exactly `"package.files.json"` in bootstrap version 1.

Every other defect of the manifest is reported as `invalid_bootstrap` with a
detail naming the member; a manifest that is not valid UTF-8 is `invalid_utf8`.

## Integrity index `package.files.json`

```json
{
  "index_version": 1,
  "files": [
    {
      "path": "adapter/unknown-fixture-adapter",
      "bytes": 1493056,
      "sha256": "9f2c...64 hex...",
      "resource": {"kind": "executable", "origin": "package"}
    },
    {
      "path": "assets/mark.svg",
      "bytes": 2210,
      "sha256": "...",
      "resource": {
        "kind": "asset",
        "origin": "third-party",
        "notice": "notices/mark-license.txt",
        "provenance": "provenance/mark.json"
      }
    },
    {"path": "notices/mark-license.txt", "bytes": 1071, "sha256": "...",
     "resource": {"kind": "notice", "origin": "third-party",
                  "notice": "notices/mark-license.txt",
                  "provenance": "provenance/mark.json"}},
    {"path": "provenance/mark.json", "bytes": 312, "sha256": "...",
     "resource": {"kind": "provenance", "origin": "third-party",
                  "notice": "notices/mark-license.txt",
                  "provenance": "provenance/mark.json"}},
    {"path": "capabilities/profile-storage@1.json", "bytes": 61, "sha256": "...",
     "resource": {"kind": "sidecar", "origin": "package"}}
  ]
}
```

Rules:

- Strict object; `index_version` is `1`; `files` is sorted by `path` bytewise
  ascending with no duplicates, so the canonical form is the file itself and
  an unsorted index is rejected. The file is at most `k_max_index_bytes` and
  holds at most `k_max_files_per_package` entries.
- The bootstrap and the index are not listed; the generation digest covers
  them directly. Every other regular file below the package directory is
  listed. An unindexed file, an entry that is a symbolic link, reparse point,
  device or fifo, and a listed path that is missing make the package
  unavailable: `unindexed_file`, `link_entry`, `missing_file`.
- `bytes` is a non-negative integer at most `k_max_file_bytes`; `sha256` is
  64 lowercase hexadecimal characters; the sum of `bytes` is at most
  `k_max_generation_bytes`. A file whose size or digest differs from its entry
  is `digest_mismatch`.
- `resource.kind` is one of `executable`, `sidecar`, `helper`, `asset`,
  `notice`, `provenance` and agrees with the first path segment (`adapter/`,
  `capabilities/`, `helpers/`, `assets/`, `notices/`, `provenance/`).
- `resource.origin` is `package` or `third-party` and is required on every
  entry: an original package-owned resource records `"origin": "package"`
  explicitly. There is no default and no omission.
- `origin: "third-party"` requires both `notice` and `provenance`, each a
  canonical relative path with first segment `notices` and `provenance`
  respectively, each present in the index, each with `bytes > 0`, each with
  bytes matching its own entry. A missing, empty, escaped, unindexed or
  digest-mismatched record makes the package unavailable with reason
  `third_party_record_invalid`; the detail names the entry. Notice and
  provenance files are indexed like every other byte and carry the
  third-party origin with the same two references.
- `origin: "package"` entries carry no `notice` or `provenance` member.
- Adding a file changes `files` and the generation digest, never
  `index_version`.

A path outside the grammar is `path_escape`; every other structural defect is
`invalid_index`; a size bound is `custody_bound_exceeded` with the bound's
name in the detail.

`logonomic_cli_addon_index <package-dir> [--third-party <resource> <notice>
<provenance>]...` writes this file for a package directory and refuses to
write an index the validator would reject.

## Capability sidecars `capabilities/<capability_id>@<version>.json`

```json
{"capability_id": "profile-storage", "version": 1, "payload": {}}
```

A strict object with exactly these three members. `payload` is any JSON value
the capability owns, bounded by `k_max_sidecar_bytes` and the JSON limits; the
core never interprets it. The file name is exactly
`<capability_id>@<version>.json`, or the package is unavailable with reason
`sidecar_name_mismatch`. A malformed sidecar is `invalid_sidecar`. Two sidecars
for the same capability with different versions may coexist; the adapter
declares which versions are live. A package holds at most
`k_max_sidecars_per_package` sidecars. Unknown capability IDs are retained and
ignored.

## Validation order

The validator applies the rules in one fixed order and reports the first
failure: bootstrap; platform and core-generation membership; index; adapter
presence and executable format; third-party records; directory listing
(unindexed, missing and link entries); size and digest of every file;
sidecars; generation digest. `logonomic_cli_addon_validate <package-dir>` runs
it and prints the outcome.

## Generation digest

```text
generation_digest = SHA-256( LP(bootstrap_bytes) || LP(index_bytes) || D(f_1) || D(f_2) || ... || D(f_n) )
```

`LP(x)` is the 8-byte big-endian byte count of `x` followed by `x`, where the
bootstrap and index bytes are the files exactly as read; `D(f_i)` is the raw
32-byte SHA-256 of the bytes of the i-th index entry in index order. The
digest is rendered as 64 lowercase hexadecimal characters. It is independent
of the platform the package is validated for. The host records it in its
registry, sends it in the hello and sets it in the adapter environment, and
provider bindings record it.

## Linux native runtime deployment

The standard `logonomic_cli_addon_add_package` recipe deploys the complete
non-system ELF closure before writing its index. Custom recipes select the
same producer with `logonomic_cli_addon_package_indexer(out_command
out_dependencies LINUX_RUNTIME CONTROLLED_TARGETS <targets>...
[RUNTIME_NOTICES <directory>])`. The declared executable targets receive
Release compilation without debugging data and symbol-free Release linking;
post-link inspection still checks the delivered bytes. The returned
command takes the package directory and normal index arguments. Include its
returned dependencies in the assembly rule. Assembly must start from fresh,
unindexed program and resource bytes; imported generations are immutable.

Libraries occupy one copy per filename under `adapter/`. Programs and libraries
use relative ELF search paths to that directory, so the indexed package loads
after custody relocation. Every ELF, including each library, must resolve its
non-system dependencies inside the package. The supported OS supplies glibc
and its loader; compiler, HTTP, database and other runtimes belong to the
package. Conflicting library bytes fail assembly.

This producer requires Python 3.11, patchelf, binutils and the Debian or Ubuntu
package database. It records the actual binary and source package versions,
source download routes, original and deployed digests, and copyright material
before indexing. `RUNTIME_NOTICES` can supply copyright files named
`<library filename>.txt`; otherwise the owning installed package supplies them.
Controlled Release programs must contain no debugging data. Supplier attribution
does not establish a vendor debugging-data exception; ordinary release inspection
must identify the necessary binary and the applicable vendor-variant availability
constraint under the release-payload policy.
The provider export gate verifies the final package closure before retaining
its archive.

A custom assembler that already owns copying and RPATH deployment obtains the
same producer with `logonomic_cli_addon_linux_runtime_deployer(out_script)`.
Invoke it with `record --package <directory> --configuration <configuration>`,
one `--input <original-file> <package-member> <original-sha256>` per ELF copy,
and `--executable <package-member>` for each controlled program. Capture original
digests before copying. Run `record` after deployment edits and before indexing;
it verifies complete ELF ownership, unchanged inputs and the private closure,
then writes `provenance/linux-runtime.json` with final hashes. Index each supplied
runtime against this provenance and its notice. This does not change the
assembler's copying or deployment policy.

`record --prebuilt-material <file>` accepts acquisition facts for non-dpkg
vendor inputs. Its capability-owned JSON has `schema_version: 1` and an `inputs`
array. Each row identifies an exact original `source` and `source_sha256`,
`binary_package`, `binary_version`, `source_package`, `source_version`,
`source_url`, `notice`, `notice_sha256`, and an `acquisition` object containing
the supplier's selected distribution coordinates. The producer checks
source and notice bytes before using those facts; unmatched supplied inputs use
the installed-package route. Prebuilt rows add `supplier_kind: "prebuilt"` and
`acquisition` to the same runtime report. Neither source paths nor package-member
names infer supplier identity or exception eligibility.

## QtCore package assembly

`logonomic_cli_addon_add_qt_package(<target> <package_id> <out_directory>
ADAPTER <target> [HELPERS <targets>...] [QTBASE_SOURCE <directory>]
[RUNTIME_NOTICES <directory>] [LINUX_RUNTIME_MATERIAL <file>]
[PATCHELF <executable>] [EXTRA_FILES <source> <package-relative-destination>...]
[OUT_RUNTIME_CHECK <out_command>] [OUT_QTBASE_SOURCE <out_directory>]
[INDEX_ARGUMENTS <arguments>...])` assembles
a package whose selected targets depend on QtCore, including the filesystem
observation service. Add the SDK and select `Qt6::Core` before calling it.
Configure `generated/logonomic-cli-addon.json` in the package's binary directory
first, as for `logonomic_cli_addon_add_package`.

The recipe copies the adapter and helpers into `adapter/` with their actual
target filenames, and every file under
`package/{assets,capabilities,notices,provenance}`. It
resolves the complete native dependency closure of each program, including
non-Qt dependencies such as HTTP libraries. It deploys that closure and its
notices before the SDK indexer seals the final bytes. Its assembly target runs
on each build and compares exact input and output contents and membership;
an unchanged package retains its bytes and timestamps. Changed runtime inputs,
resource deletion, addition and rename regenerate the package.
`EXTRA_FILES` adds source/destination pairs such as a license from the exact
selected dependency checkout. Their bytes participate in the same receipt and
final index. `PATCHELF` selects an explicit Linux relocation tool; otherwise the
recipe finds `patchelf` on the build environment's path. `OUT_QTBASE_SOURCE`
returns the exact selected notice source so an isolated recipe regression can
forward it even when the caller used automatic discovery.

`QTBASE_SOURCE` selects notice material matching `Qt6Core_VERSION`, including
qtbase's `.cmake.conf`, `LICENSES` and third-party notices. When omitted, the
recipe uses the selected kit's adjacent source directory or fetches the
corresponding qtbase tag. `RUNTIME_NOTICES` supplies other libraries' notices
as `<library filename>.txt`. On Linux, the installed Debian/Ubuntu package is
the alternative notice source; `LINUX_RUNTIME_MATERIAL` supplies exact
acquisition facts for non-dpkg inputs using the format above. The Linux and
macOS stages use the SDK's native deployers and retain their provenance.
All programs share one runtime copy: Windows DLLs and Linux libraries reside
in `adapter/`; macOS libraries reside in `helpers/runtime/`. MSVC runtimes come
from the selected compiler's redistributables, with separate private Debug
runtime notices. The recipe preserves the SDK's macOS signing selection.

The function returns the package directory and uses the same export or embedded
registration contract as the standard recipe. It enables no testing.
`OUT_RUNTIME_CHECK` optionally returns a command list that the provider may
register with `add_test` in its standalone build to inspect the actual program
paths, shared runtime layout, indexed provenance and native closure. That check
does not replace package validation or conformance.

### Provider observation preparation

`cmake/prepare_observation_ci.py` exposes `main(provider, arguments=None)` for a
provider's thin CI callback. The provider name selects its dependency-cache
directory and uppercase CMake input prefix, for example `GROK_OBSERVATION_SOURCE`,
`GROK_QTBASE_SOURCE`, `GROK_RUNTIME_NOTICES_DIRECTORY` and
`GROK_LINUX_RUNTIME_MATERIAL`. The callback accepts the native driver's
`--platform`, `--toolchain`, `--toolchain-root`, `--aqt-source`, `--revisions`,
`--record`, `--workspace` and `--output` arguments. It writes the JSON CMake
argument array and records the selected Qt kit and source dependencies.

The shared preparation follows owned dependencies at `master` for development
and checks exact source revisions when the release supplies them. It reuses
the cached vendor Qt installation matching the selected compiler ABI and aqt
source, obtains corresponding notices, and records Linux Qt/ICU acquisition
facts before packaging. The LLVM-MinGW callback selects shared LLVM runtime
linking and supplies its matching notices. Repository credentials remain in
the dependency-fetch subprocess environment and are removed before tool
installation or build configuration. The provider callback loads this module
from the same SDK checkout the driver will configure; it does not fetch a
second SDK or carry a private copy of the preparation implementation.

## Production macOS signing

Package recipes use `logonomic_cli_addon_package_indexer(out_command
out_dependencies)` from `cmake/package.cmake`, execute the returned command
with their package directory and index arguments, and include its dependencies
in the package assembly rule. Setting both
`LOGONOMIC_CLI_ADDON_MACOS_SIGNING_IDENTITY` (a Developer ID Application identity
or certificate hash) and `LOGONOMIC_CLI_ADDON_MACOS_TEAM_ID` selects production
signing. Omitting both preserves development signing. Production requires an
Apple target, Python 3.9 or newer, a Release configuration and the selected
identity in the signing user's keychain.

The command signs every staged Mach-O image with hardened runtime and a secure
timestamp, verifies the Developer ID certificate and team for every architecture,
and rejects debugger entitlements. It updates native runtime provenance before
generating the integrity index. Existing indexed packages are refused: consumers
must preserve imported package bytes and their original generation digest.
Notarization and distribution are the enclosing artifact producer's responsibility.

## Package recipe regression

The SDK's `cmake/package_membership_tests.py` exercises a provider's actual
package target in an isolated source/build copy. A provider registers it from
the SDK target's `SOURCE_DIR`, passing its source directory, package identity,
target, platform, validator and selected build settings. The harness checks
exact resource content, no-op rebuilds, replacement, deletion, backdated
addition and rename. It retains the provider's recipe-test opt-in and runs in
the provider's authorized build lane.

Providers with additional selected inputs pass repeated
`--cmake-definition=NAME=VALUE` arguments, for example their filesystem observer source,
Qt notice source and exact runtime acquisition material. These definitions reach
the isolated configure unchanged so its disconnected build uses the same owned
dependencies and supplier evidence as the invoking package build.

## Exporting a package

A standalone project using `logonomic_cli_addon_install_package` exports its
assembled directory with `cmake --install <build> --prefix <export-root>`.
The destination is `<export-root>/<package-id>`. It must be absent when the
copy starts. An existing file, directory or dangling symlink there fails the
export. Source permissions are preserved. POSIX staging can prepend `DESTDIR`
to the install prefix. Combining a nonempty `DESTDIR` with a drive-letter
prefix fails.

A failed copy can leave a partial export. Retry in a fresh destination or
remove only that disposable partial export first. For an installed generation,
stage the complete replacement outside the active roots before following the
[installation and update instructions](https://varinomics.com/docs/logonomic/launchers-accounts-and-addons/#install-and-maintain-cli-add-ons).

## Embedded build

A package project builds as a top-level CMake project of its own, and as a
subproject of an embedding build: one that adds the SDK once, adds package
projects beside it and stages their packages itself. One recipe serves both.

**The switch.** Before it adds a package project, the embedding build sets the
normal variable `LOGONOMIC_CLI_ADDON_EMBEDDED` to `ON` in the function scope
from which it adds the package. It is never a cache entry, so a standalone
configure never sees it. A package tests `if(LOGONOMIC_CLI_ADDON_EMBEDDED)`,
which is valid before and after its `project()`.

**The recipe functions** (the SDK's `cmake/package.cmake`):

- `logonomic_cli_addon_package_directory(<id> <out>)` places the package in its
  own project's binary directory, `PROJECT_BINARY_DIR`: the build root on its
  own, the package's subdirectory when embedded.
- `logonomic_cli_addon_add_package(<target> <id> <out> ADAPTER <target>
  [HELPERS <targets>...] [INDEX_ARGUMENTS <arguments>...]
  [CAPABILITIES_DIRECTORY <absolute-directory>])` assembles one standard
  native package per project and returns its directory. The caller configures
  `generated/logonomic-cli-addon.json` first. The recipe tracks and copies all
  files under `package/{assets,capabilities,provenance,notices}`, copies the
  adapter and helpers using each target name plus `CMAKE_EXECUTABLE_SUFFIX`,
  and indexes the final bytes with the supplied indexer arguments. It creates
  the assembly target and uses the directory/install helpers for the same
  standalone or embedded behavior. Providers needing extra runtime deployment
  stages keep those stages explicit and use the lower-level helpers.
  `CAPABILITIES_DIRECTORY` optionally replaces `package/capabilities` with an
  existing absolute directory, prepared before this function is called. A caller
  can use `configure_file` to generate platform-specific sidecars in its binary
  tree without duplicating the package recipe. The chosen directory supplies
  the complete capability set; source sidecars are not overlaid. Its files and
  membership participate in the assembly dependencies, including additions
  with older timestamps and removals, and the ordinary index seals the copied
  bytes. Omitting this argument preserves the source-directory behavior.
- `logonomic_cli_addon_install_package(<directory> <id> [TARGET <target>])`
  exports a fresh artifact at install time on its own, where `TARGET` is
  accepted and ignored. Embedded, `TARGET` is required and names the package's
  assembly target, defined before the call. The function then adds no install
  rules and appends `<id>|<directory>|<target>` to the global property
  `LOGONOMIC_CLI_ADDON_EMBEDDED_PACKAGES`, which the embedding build reads once
  it has added its packages. The ID and the target name contain no `|`, so the
  directory is everything between the first and the last `|`; under a
  multi-configuration generator it holds the `$<CONFIG>` component the
  directory function returned. The SDK records each registration as the
  package made it; refusing a package that registered nothing, or two packages
  with one ID, belongs to the embedding build.

**An embeddable package project** guards everything that belongs only to a
top-level build with `if(NOT LOGONOMIC_CLI_ADDON_EMBEDDED)`:

- No global writes: no `CACHE ... FORCE` entry, such as
  `CMAKE_FASTBUILD_CAPTURE_SYSTEM_ENV` or `LOGONOMIC_CLI_ADDON_SDK_BUILD_TESTS`,
  and no append to `CMAKE_TRY_COMPILE_PLATFORM_VARIABLES`. Embedded, the
  package leaves every cache entry of the embedding build as it found it and
  adds none named `CMAKE_*` or `LOGONOMIC_*`, the names the embedding build
  owns, nor `BUILD_SHARED_LIBS` or `BUILD_TESTING`, the build-wide switches
  CMake reads without a prefix; the entries it declares itself are prefixed by
  the package. It also leaves the build properties of the directory that adds
  it (`COMPILE_OPTIONS`, `COMPILE_DEFINITIONS`, `INCLUDE_DIRECTORIES`,
  `LINK_OPTIONS`, `LINK_DIRECTORIES`, `LINK_LIBRARIES`) unchanged, since every
  target the embedding build adds afterwards starts from them.
- No SDK acquisition: the embedding build owns the SDK. Embedded, the package
  requires `TARGET logonomic_cli_addon_sdk::sdk` and fails configuration
  without it.
- Package-owned paths: build paths derive from `CMAKE_CURRENT_BINARY_DIR` or
  `PROJECT_BINARY_DIR`, never from `CMAKE_BINARY_DIR` or `CMAKE_SOURCE_DIR`,
  which name the embedding build's top level when embedded.
- Tests only on its own: `include(CTest)` or `enable_testing()`, every
  `add_test`, the test executables and test-only tool lookups sit under
  `if(NOT LOGONOMIC_CLI_ADDON_EMBEDDED)`, and the embedding build enables no
  testing on the package's account. The switch alone decides, because
  `BUILD_TESTING` exists only once `include(CTest)` has run, and a package
  that calls `enable_testing()` never defines it; a package that uses
  `include(CTest)` keeps it in its standalone build, so that `BUILD_TESTING`
  still switches its tests there.
- Registration: `logonomic_cli_addon_install_package(<directory> <id> TARGET
  <assembly target>)`.
- Scoping: target and fetched-content names are prefixed by the package,
  third-party fetches stay inside the package, and the package reads no
  variable of the embedding build other than the switch.
- Windows definitions: `_WIN32_WINNT`, `NOMINMAX`, `WIN32_LEAN_AND_MEAN`,
  `UNICODE` and `_UNICODE` apply under `WIN32`, not only under `MSVC`, so the
  package builds with whichever Windows toolchain the embedding build uses.

**Embedding does not change the package.** The package project builds the same
targets from the same inputs either way; only the SDK it links is compiled by
whichever build added it. Built on its own, and by an embedding build that
compiles the SDK the same way, with one toolchain and reproducible links, a
package has one generation digest. The README's "Proving an embedded build"
runs that comparison for a package project, with `tests/embedded_host` as the
embedding build.

## Reason-code vocabulary

The SDK and the host report the same reason codes; `ids.h` holds them as
constants. Registry and custody: `invalid_bootstrap`,
`unsupported_bootstrap_version`, `platform_not_supported`,
`shell_adapter_entry_point`, `reserved_environment_variable`,
`unsupported_core_generation`, `invalid_index`, `unindexed_file`,
`missing_file`, `link_entry`, `digest_mismatch`, `path_escape`,
`invalid_utf8`, `third_party_record_invalid`, `sidecar_name_mismatch`,
`invalid_sidecar`, `duplicate_package_id`, `candidate_limit_exceeded`,
`custody_bound_exceeded`, `custody_write_failed`, `source_mutated`,
`transport_conflict`, `registry_not_settled`. The protocol and capability codes
are listed in `core_protocol.md`; `detached_helper_proposal` belongs to
process proposals.
