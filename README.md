# Logonomic CLI add-ons

The CLI add-on SDK supplies the C++ adapter runtime, package tools, simulator and protocol specifications. The optional native host runs these packages using the same generation and process helpers as Logonomic.

Start with the [DeskTool launcher](sdk/logonomic_cli_addon_sdk/examples/desktool/README.md) to connect a command-line tool, or use the [contribution guide](CONTRIBUTING.md) to change an existing provider. The [SDK reference](sdk/logonomic_cli_addon_sdk/README.md) covers packages implemented in C++ and the language-neutral wire contracts.

## Build

The default build uses a C++20 compiler and CMake 3.21 or newer. Configuring the SDK alone requires no Qt installation or private desktop checkout.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

Use `-DLOGONOMIC_CLI_ADDON_BUILD_HOST=ON` to include the native host. It also needs Qt Core and access to [vnm_process_custody](https://github.com/Varinomics/vnm_process_custody). The [process contract](sdk/logonomic_cli_addon_sdk/docs/process_contract.md) describes how the host starts adapters.

## Match a desktop release

The desktop's `cli-addon-core-compatibility.json` lists the capability pairs it adopts. Release source tuples identify the add-on system checkout under `vnm_logonomic_cli_addon`. Use that exact checkout for its SDK documentation and tools.

The Windows 1.0.6 portable predates the separate source repository. Its source revision `9aea4c8bae16d29ccca8f195071d7bd55676596c` contains SDK tree `ba826f6d17ae65231c725042eccdea0e0c87091d`. The [matching SDK snapshot](https://github.com/Varinomics/vnm_logonomic_cli_addon/tree/4be45c07c1f0547ab9d0b6f4e3f497da17e5f84e/sdk/logonomic_cli_addon_sdk) preserves that tree, including its fixtures and documentation.

This repository is private. Access to its source and the maintained providers requires repository access. Third-party notices accompany their respective sources.
