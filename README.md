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

**What can this desktop run?** Open `cli-addon-core-compatibility.json` beside
the target Logonomic executable. It lists that artifact's supported package
formats, protocol generations and exact capability versions. To see what
was **tested**, consult the release owner's retained qualification evidence
for the package and platform. Match the package generation digest and core
artifact digest to the native test results. The desktop support policy
explains how a release selects its oldest and newest maintained endpoints.
[Publication qualification](https://github.com/Varinomics/Logonomic/blob/master/docs/external_agent_client_support_policy.md#publication-qualification) ·
[Shared provider test workflow](https://github.com/Varinomics/Logonomic/blob/master/.github/workflows/cli-addon-provider.yml)

**Which SDK source should I use?** For a released desktop, take the full
`commit` from the `sources` row with key `vnm_logonomic_cli_addon` in its
retained `release-source-tuple.json`. Use the repository in that row. For an
older desktop that carried the SDK in-tree, use its `application` row and
the same `sdk/logonomic_cli_addon_sdk` subdirectory. For your own tested
combination, record the full SDK and provider commits with the desktop
artifact and test results. With access to the SDK repository, a clean
checkout for an independently maintained SDK looks like this.

```sh
git clone --no-checkout https://github.com/Varinomics/vnm_logonomic_cli_addon.git pinned-addon-sdk
git -C pinned-addon-sdk checkout --detach <full-sdk-commit>
git -C pinned-addon-sdk rev-parse HEAD
```

Replace `<full-sdk-commit>` with the recorded 40-character value and confirm
that `rev-parse` prints it. Set the provider's
`LOGONOMIC_CLI_ADDON_SDK_SOURCE` to the absolute
`pinned-addon-sdk/sdk/logonomic_cli_addon_sdk` directory at configure time.
Use a fresh build tree when switching revisions. To assess a subsequent
update, compare the relevant contracts and headers between the two commits
and rerun the provider tests. [Provider build instructions](sdk/logonomic_cli_addon_sdk/docs/sdk_tools.md#building-an-existing-provider)

The SDK repository is private. The repository owner handles access and
questions about redistribution. The SDK version identifies its source
library. The support policy specifies no SDK-wide source or binary
compatibility guarantee or minimum retention period for a capability pair.
[Desktop and add-on support policy](https://github.com/Varinomics/Logonomic/blob/master/docs/external_agent_client_support_policy.md#independently-released-core-and-add-on-packages)
