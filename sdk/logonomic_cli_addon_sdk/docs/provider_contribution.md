# Change a shipped provider

Changes to a CLI's discovery, launch arguments, profiles or native journal
parser belong in its maintained add-on repository. Keep the package ID when
testing a replacement. The development root takes precedence over the bundled
copy with the same ID. A second ID can make recognition of the saved command
ambiguous.

## Example: Copilot's JavaScript npm launcher

The native Copilot CLI and its JavaScript npm entry point select profiles
differently. The JavaScript launcher runs through the saved shell and reads
`.copilot` under `XDG_CONFIG_HOME` or the user's home. It has no native usage
receipts. The adapter handles npm layout, account selection, launch preparation
and usage in `src/copilot_npm.cpp`, `src/copilot_accounts.cpp`,
`src/copilot_launcher.cpp` and `src/copilot_usage.cpp`. The
[provider change](https://github.com/Varinomics/vnm_logonomic_cli_copilot/commit/bc665640732bced77d5af62d88b32fa87b07e2ea)
shows the matching test and implementation together.

From a checkout of the provider, use CMake 3.25 or newer, Ninja and a C++20
compiler. On Linux the standard package recipe also needs Python 3.11+,
patchelf, binutils and Debian or Ubuntu runtime package metadata and notices.
Point CMake at your checkout of the SDK.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DLOGONOMIC_CLI_ADDON_SDK_SOURCE=/absolute/path/to/sdk
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure \
  -R '^copilot_(package_validation|package_conformance|launcher_protocol|accounts_protocol|usage_protocol)$'
```

With a supported JavaScript npm installation on Linux x86-64, exercise the
actual npm layout. Supply the directory containing its `copilot` shim.

```sh
python3 tests/npm_discovery_tests.py build/copilot_adapter linux-x86_64 \
  /absolute/path/to/node_modules/.bin --javascript
```

The expected line is `PASS JavaScript npm launcher and profile discovery`.
The script checks the selected account, preserved shell arguments and
unavailable usage result. Omitting `--javascript` selects the native npm case.

## Try the replacement in Logonomic

Use a separate desktop profile with `--user-data-dir <absolute-directory>`.
With that instance closed, set `development_root` under `[cli_addons]` in its
`settings/Varinomics/Logonomic.ini` to the absolute path to `build/package`.
It should contain `logonomic.cli.copilot/logonomic-cli-addon.json`. Put the
test npm bin directory on the desktop's inherited `PATH` and set
`XDG_CONFIG_HOME` to a disposable directory containing the test `.copilot`
profile. The Logonomic profile and the vendor profile are separate.

Start Logonomic with the same profile, scan in **Launcher Configurations**,
and launch the saved shell recipe in a repository. Verify that Copilot uses
the selected profile. To return to the bundled generation, remove the
development override with the instance closed and restart. See the
[installation and replacement instructions](https://varinomics.com/docs/logonomic/launchers-accounts-and-addons/#install-and-maintain-cli-add-ons).

With repository access, propose the fixture and fix in the
[maintained Copilot provider](https://github.com/Varinomics/vnm_logonomic_cli_copilot).
Provider maintainers own the accepted code. The bundle owner selects official
packages for desktop distribution.
