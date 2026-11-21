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

## Inside this repository

The [SDK](sdk/logonomic_cli_addon_sdk/) contains the C++ adapter runtime, tools
and [language-neutral contracts](sdk/logonomic_cli_addon_sdk/docs/README.md).
A provider's own repository contains its adapter and package. The optional
[native host](host/) in this repository is the implementation the desktop
uses to start adapters and exchange messages with them. See
[working on the host](CONTRIBUTING.md#working-on-the-host) for its interfaces.

## Build

For the SDK alone, use the [SDK build and test instructions](sdk/logonomic_cli_addon_sdk/docs/sdk_tools.md#building-the-sdk-tools).
To include the native host, configure from **this repository root** with
`-DLOGONOMIC_CLI_ADDON_BUILD_HOST=ON` and use the same build and test
commands. The host additionally requires CMake 3.22 or newer, Qt 6.12 Core,
[vnm_process_custody](https://github.com/Varinomics/vnm_process_custody) and
[vnm_qt_dispatch](https://github.com/Varinomics/vnm_qt_dispatch).

## Match a desktop release

Open `cli-addon-core-compatibility.json` beside the Logonomic executable to
see which protocol and capability versions that release supports. To
reproduce its tools and documentation from retained release evidence, use
the `vnm_logonomic_cli_addon` revision in `release-source-tuple.json`. For a
release that built the SDK in the desktop repository, use
`sdk/logonomic_cli_addon_sdk` at the recorded application revision.
