# SDK build and package tools

Use this reference to build the SDK, validate a package, test its protocol or
stage it for distribution. For a first adapter, the
[DeskTool walkthrough](../examples/desktool/README.md) builds a command and its
add-on together. The [contract index](README.md) covers the package and wire formats.

## Tools and source layout

| Part | Location | Purpose |
|---|---|---|
| Library | [Public headers](../include/logonomic_cli_addon/), [implementation](../src/) | identifiers, bounded JSON, SHA-256, bootstrap/index/sidecar parsing, package validation, generation digest, newline framing, protocol messages, capability codecs, process proposals, derived provider identity, the helper client, adapter runtime, host simulator, transcripts |
| `logonomic_cli_addon_validate` | [tools/](../tools/) | validates one package directory and prints its generation digest |
| `logonomic_cli_addon_index` | [tools/](../tools/) | writes a package's `package.files.json` |
| `logonomic_cli_addon_stage` | [tools/](../tools/) | assembles exact prebuilt package sets and verifies package evidence for offline payload assembly |
| `logonomic_cli_addon_host_simulator` | [tools/](../tools/) | drives an adapter with a transcript script and records the transcript |
| `logonomic_cli_addon_conformance` | [tools/](../tools/) | the black-box conformance suite over one package directory |
| Fixture adapter and packages | [fixtures/](../fixtures/) | the provider-unknown fixture adapter, the fixture package roster, and the transcript scripts shared with Logonomic's tests |
| Tests | [tests/](../tests/) | the SDK's own test executable and scripts, including the embedded-build comparison with its embedding host and fixture package project |

For binary redistribution of the SDK or its tools, include the
[Varinomics BSD 2-Clause license](../../../LICENSE) with the distribution.
The Windows static SDK library also contains npm-generated launcher templates.
Include the [npm cmd-shim ISC notice](../notices/npm-cmd-shim.txt) when distributing
that archive or a binary containing the templates from `npm_node_shim_entry`.

The optional
[filesystem lifecycle observer](filesystem_lifecycle_observer.md) has its own
target for providers that use the maintained filesystem observation service.
For protocol fault tests, the adapter runtime exposes `write_raw`, and
the fixture adapters can deliberately send malformed frames. A normal
adapter implements its operations through
[`Invocation_context`](../include/logonomic_cli_addon/adapter_runtime.h).

## Building the SDK tools

Run these commands from **`sdk/logonomic_cli_addon_sdk`**. The SDK library
requires C++20 and CMake 3.21 or newer. Package-recipe tests on Windows and
Linux require CMake 3.25 or newer. Install Python 3.9 or newer on Windows
and macOS. For Linux package tests, install the
[Linux runtime packaging prerequisites](package_contract.md#linux-native-runtime-deployment).

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

An in-tree consumer adds the SDK directory with `add_subdirectory`,
registers `logonomic_cli_addon_sdk_tests` itself and calls
`logonomic_cli_addon_sdk_add_fixture_packages(OUTPUT_DIRECTORY <dir> TARGET
<name>)` to assemble the fixture packages where its tests expect them. The
library target is `logonomic_cli_addon_sdk` with the alias
`logonomic_cli_addon_sdk::sdk`.

Source consumers can use the following package recipe functions.

- `logonomic_cli_addon_add_package(target package_id out_directory ADAPTER
  target ...)` assembles an adapter, its declared sidecars and package files,
  and writes their integrity index. The [DeskTool project](../examples/desktool/CMakeLists.txt)
  shows the source layout and invocation. See [Linux native runtime deployment](package_contract.md#linux-native-runtime-deployment)
  for the Linux recipe's build prerequisites.
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
  [production macOS signing](package_contract.md#production-macos-signing)
  before the first integrity index is written.
- `logonomic_cli_addon_add_qt_package(target package_id out_directory ADAPTER
  target ...)` assembles a QtCore-based package with its native runtime closure
  and matching notices. Its inputs and standalone validation command are defined
  in [QtCore package assembly](package_contract.md#qtcore-package-assembly).
- `logonomic_cli_addon_install_package(package_directory package_id [TARGET
  assembly_target])` registers an [artifact export](package_contract.md#exporting-a-package)
  for standalone builds. In an embedded build it registers the assembly target.

A package project can also build as a subproject of a host that stages its
own packages. The [embedded build contract](package_contract.md#embedded-build)
specifies the CMake switch, registration and guards a package needs.

Stage the complete directory outside the active roots before replacing an
installed package. Selection changes on desktop restart. See the
[installation and update guide](https://varinomics.com/docs/logonomic/launchers-accounts-and-addons/#install-and-maintain-cli-add-ons).
Provider recipes select their payload assembly and third-party index
declarations. The shared recipe supplies the native runtime stages.

## Building an existing provider

From the provider checkout, install its documented dependencies and point its
build at the SDK directory in your add-on system checkout.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DLOGONOMIC_CLI_ADDON_SDK_SOURCE=/absolute/path/to/vnm_logonomic_cli_addon/sdk/logonomic_cli_addon_sdk
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

With this single-configuration generator the assembled package is under
`build/package/<package-id>` in the provider checkout. Providers using the
[filesystem lifecycle observer](filesystem_lifecycle_observer.md) list their
additional dependencies in their own README.

## Validating a package

Run the SDK tools below from the SDK directory used in [Building the SDK tools](#building-the-sdk-tools).

```sh
build/logonomic_cli_addon_validate /path/to/package
```

The validator prints `OK <package_id> <generation digest> bytes=<n> declared: <capability@version>...`
on success, or `FAIL <reason code> <detail>` naming the first defect. The
reason codes are the shared vocabulary in the [package contract](package_contract.md#reason-code-vocabulary).

To compare a package with a particular desktop, pass the compatibility file
beside that desktop's executable. This command works offline.

```sh
build/logonomic_cli_addon_validate /path/to/package --desktop /path/to/cli-addon-core-compatibility.json
```

Offline inspection defaults to the machine's own platform because the desktop
declaration omits the platform. To inspect a Linux x86-64 package from another
platform, add `--platform linux-x86_64`.

The JSON report identifies the supplied directory and validated generation,
checks the target's bootstrap, index and core protocol versions, and lists each
exact capability pair. `declared` comes from the package's indexed sidecars.
`adopted` comes from the target file. Add `--live` on the adapter's native
platform to start the adapter in the SDK simulator and record its fresh
`advertised` pairs. An unobserved handshake leaves that field `null`. The
simulator sends no feature invocations.

`role` identifies the static coordination-delivery and account-profile
binding contracts. The latter omits its own handshake entry and names the
managed-configuration version it requires alongside accounts@2. A
`contract_issue` identifies a problem the inspector can establish from a
known SDK contract, the supplied sidecars, the target's adopted pairs, and
any observed advertisement. Other desktop declaration checks, external
prerequisites and installation precedence require the actual desktop. Run the
package's feature tests to establish behavior.

Exit status 0 means the inspection completed with compatible package formats,
including the handshake when requested. Individual capability differences
remain in the report. Status 1 identifies a declaration, package, target format
or activation failure. Status 2 indicates invalid command arguments.

## Staging prebuilt packages

`logonomic_cli_addon_stage --output /payload/cli_addons --input artifacts.json
--evidence /payload/cli-addons.evidence.json` copies exactly the supplied
prebuilt packages. It uses the SDK validator, preserves their generation
digests and optional sidecars, and never compiles or executes package code.
Create the parent directories first. The tool requires an unused output
directory path and evidence file path. Omitting `--input` stages zero packages.

The input, receipt and read-only verification contracts are specified in
[artifact_staging.md](artifact_staging.md). Payload assemblers must finish
staging before sealing their inventories or signing their own binaries.

## Proving conformance

```sh
build/logonomic_cli_addon_conformance /path/to/package
```

The tool prints one `PASS`, `FAIL` or `SKIP` per check. Read the skipped checks to
see which behavior still needs package-specific coverage. Drive the launcher
through the simulator and check its actual candidate, recognition and prepared
invocation. Exercise the installed command in the desktop profile for the
end-to-end result. See the [checks and transcript format](conformance.md).

## Proving an embedded build

```sh
cmake -D PACKAGE_SOURCE=/path/to/package-project \
      -D VALIDATE_TOOL=build/logonomic_cli_addon_validate \
      -D TEST_ROOT=/path/to/scratch -D TEST_GENERATOR=Ninja \
      -D TEST_PARALLEL_LEVEL=4 \
      -P tests/embedded_package_tests.cmake
```

The check builds the package project on its own against this SDK and as a subproject of
`tests/embedded_host` with the embedded switch set. It checks the
[embedded build contract](package_contract.md#embedded-build). The standalone install exports
the package. The embedded package registers once, in its own project's binary
directory, changes no cache entry of the embedding build, adds none named
`CMAKE_*`, `LOGONOMIC_*`, `BUILD_SHARED_LIBS` or `BUILD_TESTING`, leaves the
build properties of the directory that added it unchanged, enables no testing
there and installs nothing. The check requires identical generation digests
from both builds and prints the digest with the cache entries the package added.
`TEST_MAKE_PROGRAM`, `TEST_CXX_COMPILER` and `TEST_TOOLCHAIN_FILE` select the
toolchain. On Windows, run it inside the MSVC developer environment or name
one with `TEST_VCVARSALL` and `TEST_VCVARS_VERSION`. `VALIDATE_TOOL` is the
validator built from this SDK.
The script runs its builds one after the other with `TEST_PARALLEL_LEVEL`
compile jobs each (default 1). On a machine that queues builds, submit the
script as one queue request for that many slots. The SDK's own
CTest entry, `logonomic_cli_addon_embedded_package_tests`, declares its width as
its `PROCESSORS` property.

The check compares standalone and embedded Release packages built locally from
one source tree with the same toolchain. With MSVC it links with `/Brepro` and
bypasses FASTBuild's object cache for these two builds. Cache preprocessing
can give MSVC different input paths and change the generated names of
anonymous-namespace types. To compare digests from separate MSVC runs, use
the same absolute package and SDK source paths in both.

`STANDALONE_ONLY=ON` builds only the standalone half, to record the digest of a package revision that cannot be embedded yet.
`EXPECTED_GENERATION_DIGEST` then makes a later run require that digest.
