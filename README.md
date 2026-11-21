# Logonomic CLI add-ons

The CLI add-on SDK supplies the C++ adapter runtime, package tools, simulator and protocol specifications. The optional native host runs these packages using the same generation and process helpers as Logonomic.

Start with the [DeskTool launcher](sdk/logonomic_cli_addon_sdk/examples/desktool/README.md) to connect a command-line tool, or use the [contribution guide](CONTRIBUTING.md) to change an existing provider. The [SDK reference](sdk/logonomic_cli_addon_sdk/README.md) covers packages implemented in C++ and the language-neutral wire contracts.

## Build

The SDK library requires C++20 and CMake 3.21 or newer. The commands below also build and run the SDK tests using Ninja. On Windows and Linux the package-recipe tests require CMake 3.25 or newer. Install Python 3.9 or newer on Windows and macOS. For Linux package tests, install the [Linux runtime packaging prerequisites](sdk/logonomic_cli_addon_sdk/docs/package_contract.md#linux-native-runtime-deployment).

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

Use `-DLOGONOMIC_CLI_ADDON_BUILD_HOST=ON` to include the native host. It also needs CMake 3.22 or newer, Qt 6.12 Core, [vnm_process_custody](https://github.com/Varinomics/vnm_process_custody) and [vnm_qt_dispatch](https://github.com/Varinomics/vnm_qt_dispatch). The [process contract](sdk/logonomic_cli_addon_sdk/docs/process_contract.md) describes how the host starts adapters.

Package authors implement adapter handlers with the SDK. Within the production host, `Request_broker` admits requests against a generation's negotiated capability pairs, enforces deadlines and retires the generation when cancellation grace expires. Its lower-level `Generation_client` owns the per-generation transport, handshake and message exchange. Host callers that need invocation policy submit through the broker. See the [broker interface](host/src/logonomic_cli_addon_request_broker.h) and [generation client](host/src/logonomic_cli_addon_generation_client.h).

## Match a desktop release

Open `cli-addon-core-compatibility.json` beside the Logonomic executable to see which protocol and capability versions that release supports.

Maintainers with the retained release evidence can reproduce its SDK tools and documentation from the `vnm_logonomic_cli_addon` revision in `release-source-tuple.json`. For releases that built the SDK in the desktop repository, use `sdk/logonomic_cli_addon_sdk` at the recorded application revision.
