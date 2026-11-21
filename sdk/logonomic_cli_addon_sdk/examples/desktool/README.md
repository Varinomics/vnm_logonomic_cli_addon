# A native launcher add-on

DeskTool is a small command that prints `DESKTOOL_READY`. Its Logonomic add-on
discovers the executable on the supplied `PATH`, recognizes a saved Direct
invocation and leaves its arguments, environment and working directory intact.
You can run it without an account.

## Build and check the package

From the SDK root, use CMake 3.25 or newer, a C++20 compiler, Ninja and Python
3.9 or newer. Windows and Linux target x86-64. For the macOS preview use an
arm64 compiler and add `-DCMAKE_OSX_DEPLOYMENT_TARGET=15.0` to the configure command.
The Linux package recipe needs Python 3.11+, patchelf, binutils and the Debian
or Ubuntu runtime packages' ownership and copyright records.

```sh
cmake -S examples/desktool -B build/desktool -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/desktool --parallel 4
ctest --test-dir build/desktool --output-on-failure
```

For a copied example, configure its directory with
`-DLOGONOMIC_CLI_ADDON_SDK_SOURCE=/absolute/path/to/sdk`. On Windows run the
commands in an x64 compiler environment. On Varinomics workstations use the
configured build queue for configure and build. The commands above use a
single-configuration generator. Multi-configuration builds place the selected
configuration below both `package/` and `bin/`.

The command is in `build/desktool/bin`. The complete add-on is in
`build/desktool/package/org.example.desktool`. The demo command lives outside
the package, like a CLI installed by a user. The validator checks the
assembled files. Conformance starts the adapter and checks its advertised
pairs. The launcher test sends a script through the SDK host simulator,
checks discovery with the command present and absent, recognizes a saved
command with arguments and an environment setting, inspects the unmanaged
patch, and separately runs the native command.

## Launch it in Logonomic

1. Put the absolute path of `build/desktool/bin` first on the `PATH` of the
   shell that will start Logonomic. Run `desktool` in that shell and check for
   `DESKTOOL_READY`.
2. Choose an absolute directory for a separate profile. Create or edit
   `settings/Varinomics/Logonomic.ini` inside it and set the package **parent**
   directory, using forward slashes on Windows.

   ```ini
   [cli_addons]
   development_root=/absolute/path/to/build/desktool/package
   ```

3. Start your installed Logonomic from that shell with
   `--user-data-dir <absolute-profile-directory>`. If that profile is open,
   close it before restarting. In **Launcher Configurations**, select **Scan
   for launchers** (or **Rescan**). Open a local Git repository in the profile.
   Expand it in **Repositories** and click **DeskTool** under **LAUNCHERS**.
   The terminal prints `DESKTOOL_READY`.
4. After editing the package, build it again and restart that profile so the
   host reads the new generation. To remove the override, clear
   `development_root` and restart. For regular installation, copy the complete
   package directory into the active profile's `cli-addons/packages/`.
   [Installation and replacement](https://varinomics.com/docs/logonomic/launchers-accounts-and-addons/#install-and-maintain-cli-add-ons)

If the launcher is missing, run
`build/desktool/sdk/logonomic_cli_addon_validate build/desktool/package/org.example.desktool`
(add `.exe` on Windows). It checks the platform, bootstrap and integrity index.
To try an integrity failure, make a separate copy of the assembled package.
Change the fill color `#234b59` to `#234b58` in its `assets/desk.svg` and
validate the copy. The validator reports `digest_mismatch assets/desk.svg`.
Restore the original file to validate it again. To ship your own artwork,
edit the source asset and rebuild the package so the index includes its new bytes.
For a valid package with an empty scan, check the parent directory and run
`desktool` from the shell used to start the profile.

## Adapt it to your CLI

Use a reverse-domain package ID in a namespace you control when distributing
your add-on. Change the ID in the bootstrap template and CMake package call.
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
