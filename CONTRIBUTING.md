# Contributing to Logonomic CLI add-ons

Use this repository for changes to the SDK, package format, tools or shared native host. A provider's own repository is the upstream for its CLI recognition, account handling and launch behavior. Both routes require repository access.

## Build a provider locally

From the provider checkout, use its documented platform dependencies and a C++20 toolchain. Point the build at the SDK directory in your add-on system checkout.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DLOGONOMIC_CLI_ADDON_SDK_SOURCE=/absolute/path/to/vnm_logonomic_cli_addon/sdk/logonomic_cli_addon_sdk
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

Provider packages assemble under `build/package/<package-id>`. On Linux the standard package recipe needs Python 3.11+, patchelf, binutils and the installed runtime packages' provenance and notices. Providers with filesystem observation also require Qt 6.12 Core and the maintained observer. Each provider README lists its additional inputs.

To try a replacement, use the original package ID. Point a separate desktop profile's development root at the parent of the assembled directory, restart with that profile and scan for the launcher. The [Copilot worked example](sdk/logonomic_cli_addon_sdk/docs/provider_contribution.md) includes a disposable profile, exact test commands, and removal of the override. For a new integration, the [DeskTool example](sdk/logonomic_cli_addon_sdk/examples/desktool/README.md) includes the package, three launcher declarations and a marker command.

Submit the implementation and a focused regression case to the repository that owns the behavior. Maintainers select accepted provider revisions for official release, signing and desktop distribution. Local tests use the checked-out sources and need no signing or release credentials.
