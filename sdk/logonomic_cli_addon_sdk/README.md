# Logonomic CLI add-on SDK

Use this SDK to connect a command-line tool to Logonomic. Start with the
[DeskTool launcher](examples/desktool/README.md) to build a native adapter,
exercise discovery and launch preparation, and try the package in a separate
desktop profile. To change a shipped integration, follow the
[provider contribution example](docs/provider_contribution.md).

The C++20 core builds with the standard library and platform facilities,
independently of Qt and the desktop sources. It includes npm-generated Windows
launcher templates and their ISC copyright and permission notice in
[npm_node_shim.cpp](src/npm_node_shim.cpp). Preserve that notice when
redistributing copies of that material.

Providers that already use the maintained filesystem observation service can opt
into the separate [filesystem lifecycle observer](docs/filesystem_lifecycle_observer.md)
target. It shares correlation, cursor, queue and watch ownership while leaving
native discovery and decoding in each provider. The core SDK target remains
independent of Qt and the observer.

## What it contains

The optional [resume-directory@2](docs/resume_directory_2.md) declaration states
that a package can resume a conversation in a folder other than its original
one, which the host requires before it offers another folder.

The optional [session-wake@1](docs/session_wake_1.md) contract lets a provider
notify its exact live native conversation after a mailbox commit. Discovery of
the capability, preparation of a particular launch, native notification
acceptance, and recipient acknowledgment are separate facts.

The optional [terminal-identity@1](docs/terminal_identity_1.md) companion lets an
opted-in native launch report an exact foreground conversation before its next
hook. Provider-owned title parsing and frozen native-store context feed the
existing lifecycle writer while preserving human captions and hook envelopes.

| Part | Location | Purpose |
|---|---|---|
| Normative documents | `docs/` | the package, process and wire contracts; `profile-storage@1`, `accounts@2`, [auth-status@2](docs/auth_status_2.md), [current-account@1](docs/current_account_1.md), [account-refresh@1](docs/account_refresh_1.md), [quota@3](docs/quota_3.md) with [quota@2](docs/quota_2.md) compatibility, optional [quota-failure@1](docs/quota_failure_1.md), [usage-scan@3](docs/usage_scan_3.md), [usage-incremental@1](docs/usage_incremental_1.md) and optional [usage-session@1](docs/usage_session_1.md), plus the versionless [stored accounting codec](docs/stored_usage_accounting.md); the presentation, launcher-discovery, launch-preparation, interactive-initial-prompt and managed-assets contracts; the structured-session, delegated-session, [interactive-resume](docs/interactive_resume_2.md), lifecycle-events, coordination-delivery and helper-ingress contracts; the SVG subset annex; every numeric bound; and the conformance suite |
| Library | `include/logonomic_cli_addon/`, `src/` | identifiers, bounded JSON, SHA-256, bootstrap/index/sidecar parsing, package validation, generation digest, newline framing, protocol messages, capability codecs, process proposals, derived provider identity, the helper client, adapter runtime, host simulator, transcripts |
| `logonomic_cli_addon_validate` | `tools/` | validates one package directory and prints its generation digest |
| `logonomic_cli_addon_index` | `tools/` | writes a package's `package.files.json` |
| `logonomic_cli_addon_stage` | `tools/` | assembles exact prebuilt package sets and verifies package evidence for offline payload assembly |
| `logonomic_cli_addon_host_simulator` | `tools/` | drives an adapter with a transcript script and records the transcript |
| `logonomic_cli_addon_conformance` | `tools/` | the black-box conformance suite over one package directory |
| Fixture adapter and packages | `fixtures/` | the provider-unknown fixture adapter, the fixture package roster, and the transcript scripts shared with Logonomic's tests |
| Tests | `tests/` | the SDK's own test executable and scripts, including the embedded-build comparison with its embedding host and fixture package project |

## Building the SDK tools

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

The same directory is consumed in-tree by `add_subdirectory`; the consuming
project then registers `logonomic_cli_addon_sdk_tests` itself and calls
`logonomic_cli_addon_sdk_add_fixture_packages(OUTPUT_DIRECTORY <dir> TARGET
<name>)` to assemble the fixture packages where its tests expect them. The
library target is `logonomic_cli_addon_sdk` with the alias
`logonomic_cli_addon_sdk::sdk`.

Source consumers also receive these package recipe functions:

- `logonomic_cli_addon_add_package(target package_id out_directory ADAPTER
  target ...)` assembles an adapter, its declared sidecars and package files,
  and writes their integrity index. The [DeskTool project](examples/desktool/CMakeLists.txt)
  shows the source layout and invocation. The standard Linux recipe uses
  Python 3.11+, patchelf, binutils and Debian or Ubuntu package metadata for
  bundled compiler runtimes. See [Linux native runtime deployment](docs/package_contract.md#linux-native-runtime-deployment).
- `logonomic_cli_addon_package_platform(out_platform)` requires CMake 3.25 when
  invoked and proves a Windows/Linux x86_64 or macOS arm64 compiler target
  using compile-only probes, including each generated configuration.
  Unsupported or indeterminate targets fail configuration. The SDK's own CMake minimum remains unchanged.
- `logonomic_cli_addon_package_directory(package_id out_directory)` returns
  `<package build>/package/<id>` for a single-configuration generator, or
  `<package build>/package/$<CONFIG>/<id>` for a multi-configuration generator,
  where `<package build>` is the package project's `PROJECT_BINARY_DIR`.
- `logonomic_cli_addon_package_indexer(out_command out_dependencies)` supplies
  the package indexing command and its file dependencies, including optional
  [production macOS signing](docs/package_contract.md#production-macos-signing)
  before the first integrity index is written.
- `logonomic_cli_addon_add_qt_package(target package_id out_directory ADAPTER
  target ...)` assembles a QtCore-based package with its native runtime closure
  and matching notices. Its inputs and standalone validation command are defined
  in [QtCore package assembly](docs/package_contract.md#qtcore-package-assembly).
- `logonomic_cli_addon_install_package(package_directory package_id [TARGET
  assembly_target])` registers a fresh artifact export under the install-time
  prefix, preserving source permissions. Existing per-package destinations,
  including dangling symlinks, are refused before copying. POSIX `DESTDIR`
  staging is supported; a nonempty `DESTDIR` combined with a drive-letter
  prefix is refused. When an embedding build adds the package, it adds no
  install rules and registers the package and its assembly target instead.

A package project also builds as a subproject of an embedding build that
stages its packages itself; the switch, the registration and the guards an
embeddable package needs are specified in `docs/package_contract.md`,
"Embedded build".

Artifact export is not atomic. A failed export may leave a partial directory;
choose a fresh destination or manually remove only the identified disposable
export before retrying. Never use that cleanup to remove an installed generation.
Stage the complete directory outside the active roots before replacing an
installed package. Selection changes on desktop restart. See the
[installation and update guide](https://varinomics.com/docs/logonomic/launchers-accounts-and-addons/#install-and-maintain-cli-add-ons).
Provider recipes select their payload assembly and third-party index
declarations; the shared recipe supplies the native runtime stages.

## Validating a package

```sh
build/logonomic_cli_addon_validate /path/to/package
```

prints `OK <package_id> <generation digest> bytes=<n> declared: <capability@version>...`
on success, or `FAIL <reason code> <detail>` naming the first defect. The
reason codes are the shared vocabulary in `docs/package_contract.md`.

## Staging prebuilt packages

`logonomic_cli_addon_stage --output /payload/cli_addons --input artifacts.json
--evidence /payload/cli-addons.evidence.json` copies exactly the supplied
prebuilt packages. It uses the SDK validator, preserves their generation
digests and optional sidecars, and never compiles or executes package code.
The output directory and evidence file must be absent; their parent
directories must exist. Omitting `--input` stages zero packages.

The input, receipt and read-only verification contracts are specified in
[artifact_staging.md](docs/artifact_staging.md). Payload assemblers must finish
staging before sealing their inventories or signing their own binaries.

## Writing an adapter

[Structured and delegated session version 3](docs/structured_session_3.md)
supports an optional capability-owned stdin EOF request while retaining
inspection through native process exit.

Choose C++20 or another language that can deliver a directly executable
adapter for the target platform. The language-neutral contracts define the
[package layout](docs/package_contract.md), [process environment](docs/process_contract.md)
and [bounded UTF-8 JSON protocol](docs/core_protocol.md). Reserve stdout for
LF-terminated frames and send diagnostics to stderr.

A valid package may declare zero capabilities. Each offered version adds an
indexed sidecar. The generation digest identifies the package bytes, and the
host supplies a nonce for the running generation. Install adapters from a
source you trust to run native code with your account's access.

In C++, link `logonomic_cli_addon_sdk::sdk`, list the implemented pairs in
`Adapter_runtime_options::capabilities`, and dispatch by capability, version
and operation. Each handler receives an `Invocation_context`. Handlers may run
concurrently. Check cancellation during long operations and return one terminal
result. Use the capability codecs for the request and result. The runtime's
`write_raw` method and the fault-injection adapters under `fixtures/` are test
facilities. See the [runtime API](include/logonomic_cli_addon/adapter_runtime.h).

For a discoverable launcher with its own name and icon, declare and advertise
[launcher-discovery@1](docs/launcher_discovery_1.md) (`discover`, `recognize`),
[launch-preparation@3](docs/launch_preparation_3.md) (`prepare`) and
[presentation@1](docs/presentation_1.md). Presentation has no operation.
Both theme variants may name the same indexed asset. Use a reverse-domain
package ID in a namespace you control for distribution.

Check `cli-addon-core-compatibility.json` beside the target desktop executable
for its supported package formats, protocol generations and exact capability
versions. For an ordinary live pair, the indexed sidecar, adapter handshake
and desktop adoption must agree. Capability-specific prerequisites still
apply. A new product action needs a desktop consumer. Consult the
[authority rules](docs/package_contract.md#package-layout-and-authority)
and [protocol and package limits](docs/limits.md).

## Writing a helper

A package's helpers - the processes its proposals launch and the hooks its
CLI runs - talk to Logonomic only through the `vnmctl` executable named by
`LOGONOMIC_VNMCTL_PATH`. `helper_client.h` is that client: it reads the bound
terminal's environment, delivers provider events on the provider-event lane
of `docs/helper_ingress_2.md`, and issues the session-control operations
(`set-title`, title instruction, changed files, diff). It knows the wrapper's
subcommands and exit codes and nothing of the control protocol, the endpoint or
the token,
and it links only this library.

## Proving conformance

```sh
build/logonomic_cli_addon_conformance /path/to/package
```

prints one `PASS`, `FAIL` or `SKIP` per check. Read the skipped checks to
see which behavior still needs package-specific coverage. Drive the launcher
through the simulator and check its actual candidate, recognition and prepared
invocation. Exercise the installed command in the desktop profile for the
end-to-end result. See the [checks and transcript format](docs/conformance.md).

## Proving an embedded build

```sh
cmake -D PACKAGE_SOURCE=/path/to/package-project \
      -D VALIDATE_TOOL=build/logonomic_cli_addon_validate \
      -D TEST_ROOT=/path/to/scratch -D TEST_GENERATOR=Ninja \
      -D TEST_PARALLEL_LEVEL=4 \
      -P tests/embedded_package_tests.cmake
```

builds the package project on its own against this SDK, and as a subproject of
`tests/embedded_host` with the embedded switch set, and proves the contract of
`docs/package_contract.md`, "Embedded build": the standalone install exports
the package; the embedded package registers once, in its own project's binary
directory, changes no cache entry of the embedding build, adds none named
`CMAKE_*`, `LOGONOMIC_*`, `BUILD_SHARED_LIBS` or `BUILD_TESTING`, leaves the
build properties of the directory that added it unchanged, enables no testing
there and installs nothing; and both builds produce the same generation
digest, which the run prints with the cache entries the package added.
`TEST_MAKE_PROGRAM`, `TEST_CXX_COMPILER` and `TEST_TOOLCHAIN_FILE` select the
toolchain; on Windows, run it inside the MSVC developer environment or name
one with `TEST_VCVARSALL` and `TEST_VCVARS_VERSION`. `VALIDATE_TOOL` is the
validator built from this SDK.
The run compiles and acquires no CPUs itself: its builds run one after the
other with `TEST_PARALLEL_LEVEL` compile jobs each (default 1), so where builds
are queued, it runs as one queue request for that many slots. The SDK's own
CTest entry, `logonomic_cli_addon_embedded_package_tests`, declares its width as
its `PROCESSORS` property.

Both builds are Release, compiled locally and, with MSVC, linked with `/Brepro`,
so that identical sources give identical bytes. MSVC derives the mangled names
of anonymous-namespace types from the source path, so two separate runs give
comparable digests only when the package and the SDK are at the same paths in
both. `STANDALONE_ONLY=ON` builds only the standalone half, to record the
digest of a package revision that cannot be embedded yet;
`EXPECTED_GENERATION_DIGEST` then makes a later run require that digest.
