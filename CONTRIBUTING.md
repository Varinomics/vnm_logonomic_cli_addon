# Contributing to Logonomic CLI add-ons

Choose the repository that owns the behavior you want to change. If Copilot
selects the wrong profile, for example, the fix belongs with the Copilot
adapter. A change to the common message format belongs in this repository.
A new action in Logonomic needs a desktop consumer.

## Build a provider locally

Use the [provider build instructions](sdk/logonomic_cli_addon_sdk/docs/sdk_tools.md#building-an-existing-provider)
to build and test the package against a local SDK checkout. A replacement
should keep the original package ID. Point a separate desktop profile at the
parent of the assembled package and restart that profile to load it.

- **Change an existing integration.** The [Copilot worked example](sdk/logonomic_cli_addon_sdk/docs/provider_contribution.md)
  follows a profile-selection fix from the provider source and regression test
  through a disposable desktop profile and removal of the override.
- **Add a CLI.** The [DeskTool walkthrough](sdk/logonomic_cli_addon_sdk/examples/desktool/README.md)
  includes a sample command, its adapter and a complete package you can adapt.
- **Change the shared SDK or a contract.** Locate the affected [contract](sdk/logonomic_cli_addon_sdk/docs/README.md)
  and its [C++ interface](sdk/logonomic_cli_addon_sdk/include/logonomic_cli_addon/).
  The [SDK build instructions](sdk/logonomic_cli_addon_sdk/docs/sdk_tools.md#building-the-sdk-tools)
  include the library and its tests.

Submit the implementation and a focused regression case to the owning
repository. Maintainers select accepted provider revisions for official
release, signing and desktop distribution. Local tests use the checked-out
sources and need no signing or release credentials.

In provider repositories with a manual GitHub Actions check, maintainers can
run the same provider tests on native hosted runners and validate the
exported package with the SDK. [Standalone provider workflow](.github/workflows/provider-standalone.yml)

## Working on the host

The desktop ships the native host. To build the host itself, run the
[SDK build commands](sdk/logonomic_cli_addon_sdk/docs/sdk_tools.md#building-the-sdk-tools)
from the repository root and add `-DLOGONOMIC_CLI_ADDON_BUILD_HOST=ON` to
the configure command. The [host build](host/CMakeLists.txt) uses Qt Core,
[vnm_qt_dispatch](https://github.com/Varinomics/vnm_qt_dispatch) and
`vnm_process_custody`, the private library that manages the lifetime of
adapter processes. To build with a local copy of that library, set
`FETCHCONTENT_SOURCE_DIR_VNM_PROCESS_CUSTODY` to its absolute path.

Within the shared host, the [request broker](host/src/logonomic_cli_addon_request_broker.h)
admits requests against the running adapter's negotiated capabilities,
enforces deadlines and retires the activation when cancellation grace
expires. Callers that need invocation policy use the broker. The
[generation client](host/src/logonomic_cli_addon_generation_client.h) owns the
transport, handshake and message exchange for each activation.
[Process contract](sdk/logonomic_cli_addon_sdk/docs/process_contract.md)
