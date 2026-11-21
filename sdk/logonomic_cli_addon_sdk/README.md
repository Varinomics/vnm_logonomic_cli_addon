# Logonomic CLI add-on SDK

Use this SDK to connect a command-line tool to Logonomic, a desktop workspace
for Git repositories, terminals and coding agents. Say you have a command called
`desktool`. You want a user who has installed it to see **DeskTool** when
scanning for launchers and to open it in a repository's terminal.

The **DeskTool provider** is the integration you write for Logonomic. Its
**adapter** is a program that finds the separately installed `desktool`
command and tells Logonomic how to launch it. You distribute the adapter in
an **add-on package** with a manifest and files declaring which features the
provider supports. Logonomic calls these features **capabilities**. Launcher
discovery is one example. When DeskTool changes, you can update the integration
independently as long as it uses capabilities the desktop already understands.

## From a scan to a running CLI

1. **Logonomic finds the add-on.** At startup the desktop reads its package
   directories. The DeskTool package tells Logonomic which executable is
   the adapter and that it supports launcher discovery.
2. **The adapter finds the CLI.** When the user scans for launchers,
   Logonomic starts each eligible adapter as needed in a separate process.
   It sends requests to the adapter's standard input and reads replies from
   its standard output. DeskTool's adapter looks for `desktool` on the supplied
   `PATH` and returns a candidate when it finds one. It can also recognize a saved
   Direct launcher such as `desktool "two words"`. [Example adapter](examples/desktool/adapter.cpp)
3. **Logonomic starts the command.** When the user opens a saved launcher in
   a repository, the selected adapter can propose changes to the arguments and
   environment. Logonomic validates the result and starts the CLI in the
   user's terminal in the selected repository directory. DeskTool's adapter
   requests no changes. The `desktool` command prints `DESKTOOL_READY`.
   [Build and run DeskTool](examples/desktool/README.md)

Providers for larger CLIs use the same arrangement. An adapter can tell
Logonomic which account a launcher selects or how to resume a conversation.
The CLI still controls sign-in and the conversation in the terminal. For an
example from a shipped provider, see [how the Copilot add-on handles a
different installation of the same CLI](docs/provider_contribution.md).

## Writing an adapter

**[Build the DeskTool launcher](examples/desktool/README.md)** for a complete
first package. It includes the sample CLI, adapter, name and icon, package
recipe and a test that exercises discovery, recognition and preparation.

The C++20 SDK links as `logonomic_cli_addon_sdk::sdk` and builds independently
of Qt and the desktop sources. In C++, list your implemented capability and
version pairs in `Adapter_runtime_options::capabilities` and register an
invocation handler. Use the capability's codecs to decode requests and
encode results. Handlers can run concurrently. Check cancellation during
long operations and send one terminal result per invocation.
[C++ adapter runtime](include/logonomic_cli_addon/adapter_runtime.h) ·
[Launcher discovery types](include/logonomic_cli_addon/launcher_discovery.h) ·
[Launch preparation types](include/logonomic_cli_addon/launch_preparation.h)

Authors using another language can supply a directly executable adapter that
speaks the same protocol. Requests and responses are UTF-8 JSON frames
terminated by a newline. Send diagnostics to stderr. A live operation
requires agreement between the indexed capability declaration, the adapter's
handshake and the target desktop's supported versions. Each capability
defines its own prerequisites. Check the desktop's
`cli-addon-core-compatibility.json` when choosing what to implement.
[Package layout](docs/package_contract.md#package-layout-and-authority) ·
[Process environment](docs/process_contract.md) · [Core protocol](docs/core_protocol.md)

Use a reverse-domain package ID in a namespace you control. A valid package
can offer zero capabilities. Each capability version it does offer has its
own indexed declaration file, called a sidecar. DeskTool has three sidecars.
Discovery and launch preparation have callable operations. Presentation
supplies the display name and icon as data, and both themes can use the same
asset. An activated adapter runs native code with the user's account
permissions. Install packages from a source you trust.
[DeskTool package source](examples/desktool/package/) ·
[Capability contracts](docs/README.md)

## Writing a helper

Some CLIs run hooks or need a helper to report events from a live terminal.
The C++ helper client reads the session's environment and uses the
`vnmctl` executable named by `LOGONOMIC_VNMCTL_PATH`. It can submit provider
events or request session actions such as setting a title or showing a diff.
The helper links only the SDK. The wrapper handles the connection to Logonomic.
[C++ helper client](include/logonomic_cli_addon/helper_client.h) ·
[Helper ingress](docs/helper_ingress_2.md)

## What it contains

| If you want to… | Start here |
| --- | --- |
| Understand a feature or implement the wire contract | [Contract index](docs/README.md) |
| Find a C++ type or its API comments | [Public headers](include/logonomic_cli_addon/) |
| <a id="building-the-sdk-tools"></a>Build the SDK, its tools or a package project | [Build instructions and CMake recipes](docs/sdk_tools.md#building-the-sdk-tools) |
| <a id="validating-a-package"></a>Check the contents of a package | [Run the package validator](docs/sdk_tools.md#validating-a-package) |
| <a id="proving-conformance"></a>Exercise an adapter and inspect its transcript | [Conformance and simulator tools](docs/sdk_tools.md#proving-conformance) |
| <a id="staging-prebuilt-packages"></a>Assemble a desktop payload from built packages | [Stage prebuilt packages](docs/sdk_tools.md#staging-prebuilt-packages) |
| <a id="proving-an-embedded-build"></a>Verify the same package builds standalone and embedded | [Embedded build check](docs/sdk_tools.md#proving-an-embedded-build) |
