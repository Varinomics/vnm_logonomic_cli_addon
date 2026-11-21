# Logonomic CLI add-ons

Logonomic is a desktop workspace for Git repositories, terminals and coding
agents. A CLI add-on connects an installed tool to that workspace. For
example, a user can scan for GitHub Copilot CLI, choose its launcher in a
repository and work in Copilot's own terminal interface. The Copilot add-on
knows how to find the command and which configuration directory it will use.
Logonomic opens and manages the terminal. Copilot handles sign-in and runs
the conversation. [Copilot integration](https://github.com/Varinomics/vnm_logonomic_cli_copilot)

Different CLIs have different launchers, profiles and ways to resume a
conversation or report usage. Each add-on translates the behavior its CLI
supports into capabilities that Logonomic understands. When a CLI changes,
its integration can ship independently while using those desktop capabilities.

**[Start with the CLI add-on SDK](sdk/logonomic_cli_addon_sdk/README.md)** for
the architecture and authoring guide. To build a small working integration,
follow [DeskTool](sdk/logonomic_cli_addon_sdk/examples/desktool/README.md).
To modify a shipped integration, see [Contributing](CONTRIBUTING.md).
See the [repository license](LICENSE) for the terms covering Varinomics code.

## Inside this repository

The [SDK](sdk/logonomic_cli_addon_sdk/) contains the C++ adapter runtime, tools
and [language-neutral contracts](sdk/logonomic_cli_addon_sdk/docs/README.md).
A provider's own repository contains its adapter and package. The optional
[native host](host/) in this repository is the implementation the desktop
uses to start adapters and exchange messages with them. See
[working on the host](CONTRIBUTING.md#working-on-the-host) for its interfaces.

## Build

Build the SDK, simulator and package tools from the SDK directory. These
tools let you develop and test an adapter without the desktop or its native
host. [SDK build and test instructions](sdk/logonomic_cli_addon_sdk/docs/sdk_tools.md#building-the-sdk-tools)

To build the optional native host, configure from **this repository root**
with `-DLOGONOMIC_CLI_ADDON_BUILD_HOST=ON` and use the same build and test
commands. The host also requires CMake 3.22 or newer, Qt 6.12 Core and
access to the sources for [vnm_process_custody](https://github.com/Varinomics/vnm_process_custody)
and [vnm_qt_dispatch](https://github.com/Varinomics/vnm_qt_dispatch).
If your account cannot read the custody repository, you need a source
checkout from its owner to build the native host.
To use local checkouts, set `FETCHCONTENT_SOURCE_DIR_VNM_PROCESS_CUSTODY`
and `FETCHCONTENT_SOURCE_DIR_VNM_QT_DISPATCH` to their absolute paths.

## Match a desktop release

The `cli-addon-core-compatibility.json` file beside the installed Logonomic
executable lists the package formats, protocol generations and capability
versions that desktop supports. After building the SDK tools, run this
command from the SDK directory to compare your package with that desktop.

```sh
build/logonomic_cli_addon_validate /path/to/package --desktop /path/to/cli-addon-core-compatibility.json
```

Inspect the report for each capability your adapter needs. Add `--live` on
the adapter's native platform to include its handshake. Run the provider's
feature tests and try the package in a separate profile of the target desktop.
[Validator output](sdk/logonomic_cli_addon_sdk/docs/sdk_tools.md#validating-a-package) ·
[Testing a provider in a desktop profile](CONTRIBUTING.md#build-a-provider-locally)

To use a particular SDK revision, check out that commit in this repository
and set the provider's `LOGONOMIC_CLI_ADDON_SDK_SOURCE` to the absolute path
of its `sdk/logonomic_cli_addon_sdk` directory. Record the SDK and provider
commits, desktop artifact, package generation digest and platform with your
test results. If a desktop release supplies a `release-source-tuple.json`
with a `vnm_logonomic_cli_addon` entry in `sources`, its `commit` identifies
the exact SDK source used for that release. The compatibility declaration
alone identifies supported contracts. [Provider build instructions](sdk/logonomic_cli_addon_sdk/docs/sdk_tools.md#building-an-existing-provider)
