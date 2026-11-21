# A native launcher add-on

This example builds two programs. The [`desktool` command](desktool.cpp)
prints `DESKTOOL_READY`. The [adapter](adapter.cpp) answers Logonomic's
discovery and preparation requests. It looks for the command on the supplied
`PATH`, recognizes saved Direct invocations such as `desktool "two words"`
and preserves their arguments, environment and working directory. By the
end you'll be able to click **DeskTool** in a repository and see the command
run in its terminal. You can use it without an account.

The [CMake recipe](CMakeLists.txt) assembles the adapter and its
[package declarations](package/). The [launcher test](tests/launcher.py)
shows the messages a host sends for this example. For the complete
architecture, see the [SDK guide](../../README.md).

## Build and check the package

From the SDK root, use CMake 3.25 or newer, a C++20 compiler, Ninja and Python
3.9 or newer. Windows and Linux target x86-64. For the macOS preview use an
arm64 compiler and add `-DCMAKE_OSX_DEPLOYMENT_TARGET=15.0` to the configure command.
On Linux, install the [runtime packaging prerequisites](../../docs/package_contract.md#linux-native-runtime-deployment).

```sh
cmake -S examples/desktool -B build/desktool -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/desktool --parallel 4 --target desktool_dev
```

For a copied example, configure its directory with
`-DLOGONOMIC_CLI_ADDON_SDK_SOURCE=/absolute/path/to/sdk`. On Windows run the
commands in an x64 compiler environment. On Varinomics workstations use the
configured build queue for configure and build. The commands above use a
single-configuration generator. Multi-configuration builds place the selected
configuration below both `package/` and `bin/`.

This target assembles the package, checks validation, conformance, launcher
behavior and profile isolation, then prepares a new scratch profile under
the build directory. It runs headlessly. The printed `checks.log` contains the
validator result, individual conformance outcomes and the simulator's
handshake, calls and replies. A failed check stops profile preparation and
retains the log. For an ordinary test run in an existing build use
`ctest --test-dir build/desktool --output-on-failure`.

The command is in `build/desktool/bin`. The complete add-on is in
`build/desktool/package/org.example.desktool`. The demo command lives outside
the package, like a CLI installed by a user. The validator checks the
assembled files. Conformance starts the adapter and checks its advertised
pairs. The launcher test sends a script through the SDK host simulator,
checks discovery with the command present and absent, recognizes a saved
command with arguments and an environment setting, inspects the unmanaged
patch, and separately runs the native command.

## Launch it in Logonomic

The `desktool_dev` target prints a launch command for the new profile.
Supply the path to the Logonomic executable you want to test.
On Linux, run it from the SDK directory with your own paths.

```sh
python3 examples/desktool/develop.py launch /absolute/path/to/desktool-dev-RUN /absolute/path/to/logonomic
```

On Windows use `python` and the path to `logonomic.exe`. On macOS use the
executable inside the application's `Contents/MacOS` directory. The helper
prepends the built DeskTool command to this desktop process's `PATH`, passes
the scratch profile with `--user-data-dir` and records the desktop's stdout
and stderr in the run's `desktop-output.log`.

In **Launcher Configurations**, select **Scan for launchers** or **Rescan**.
Open a local Git repository, expand it in **Repositories** and click
**DeskTool** under **LAUNCHERS**. Its terminal prints `DESKTOOL_READY`.

For another edit, close the desktop instance using that scratch profile,
build `desktool_dev` again, then repeat the original launch command to keep
your test settings. Use the newly printed profile when you want a clean
start. The desktop reads replacement package bytes on restart. You can
remove scratch run directories when you are finished with their profiles
and logs.

For a manually prepared profile, put the command's directory on `PATH` and
set `cli_addons/development_root` in its `settings/Varinomics/Logonomic.ini`
to the absolute **parent** of the assembled package. Use forward slashes on
Windows. Clear the setting and restart that profile to remove its override.
For regular installation, copy the package directory into the active
profile's `cli-addons/packages/`. [Installation and replacement](https://varinomics.com/docs/logonomic/launchers-accounts-and-addons/#install-and-maintain-cli-add-ons)

If the launcher is missing, run
`build/desktool/sdk/logonomic_cli_addon_validate build/desktool/package/org.example.desktool`
(add `.exe` on Windows). It checks the platform, bootstrap and integrity index.
To try an integrity failure, make a separate copy of the assembled package.
Change the fill color `#234b59` to `#234b58` in its `assets/desk.svg` and
validate the copy. The validator reports `digest_mismatch assets/desk.svg`.
Restore the original file to validate it again. To ship your own artwork,
edit the source asset and rebuild the package so the index includes its new bytes.

On desktop builds that log rejected packages, open **System Log** from the
title bar. Set **Level** to **L5 [debug warning]** and enter
`org.example.desktool` in **Filter**. If the host rejected a changed asset,
the entry includes `digest_mismatch` and the asset's package-relative path.
For a valid package with an empty scan, check the parent directory and run
`desktool` from the shell used to start the profile.

## Adapt it to your CLI

Set `desktool_package_id` in [CMakeLists.txt](CMakeLists.txt) to a reverse-domain
name in your namespace. The project version supplies the package release
and the version the example adapter reports in its handshake.
In [adapter.cpp](adapter.cpp), change the executable lookup and recognition
rules to match your CLI. The three [capability sidecars](package/capabilities)
declare the versions advertised by the adapter. Presentation supplies the
display name and original desk icon. Discovery and preparation request only
the host's baseline environment. Preserve the unmanaged result when your
package leaves the saved launcher unchanged.

The [SDK package contract](../../docs/package_contract.md) covers the bootstrap,
integrity index and third-party notices. The [discovery](../../docs/launcher_discovery_1.md),
[preparation](../../docs/launch_preparation_3.md) and
[presentation](../../docs/presentation_1.md) contracts define the three records.
