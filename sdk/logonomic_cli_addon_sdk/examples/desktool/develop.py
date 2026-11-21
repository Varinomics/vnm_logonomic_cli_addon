"""Run DeskTool's native checks and prepare or launch an isolated desktop profile."""

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


def prepare(options):
    run = Path(tempfile.mkdtemp(prefix="desktool-dev-", dir=options.build.resolve()))
    log = run / "checks.log"
    print(f"DeskTool test results and simulator transcript: {log}", flush=True)
    tests = [options.ctest, "--test-dir", options.build, "-V", "--no-tests=error",
             "--output-log", log, "-R", "^desktool_(validation|conformance|launcher|development_profile)$"]
    if options.config:
        tests.extend(["-C", options.config])
    status = subprocess.run(tests).returncode
    if status:
        return status

    profile = run / "profile"
    settings = profile / "settings" / "Varinomics"
    settings.mkdir(parents=True)
    # QSettings quotes comma-separated values and interprets backslash escapes.
    package_root = options.package.resolve().parent.as_posix()
    value = (package_root.replace("\\", "\\\\").replace('"', '\\"')
             .replace("\n", "\\n").replace("\r", "\\r"))
    (settings / "Logonomic.ini").write_text(f'[cli_addons]\ndevelopment_root="{value}"\n', encoding="utf-8")
    (run / "desktool-command").write_text(str(options.command.resolve()), encoding="utf-8")
    print(f"Prepared desktop profile: {profile}")
    arguments = [sys.executable, str(Path(__file__).resolve()), "launch", str(run), "PATH_TO_LOGONOMIC"]
    if os.name == "nt":
        example = "& " + " ".join("'" + argument.replace("'", "''") + "'" for argument in arguments)
    else:
        example = shlex.join(arguments)
    print(f"Replace PATH_TO_LOGONOMIC with your desktop executable to launch this profile:\n{example}")
    return 0


def launch(options):
    run = options.run.resolve()
    marker = run / "desktool-command"
    profile = run / "profile"
    if (not marker.is_file() or profile.resolve() != profile
            or not (profile / "settings" / "Varinomics" / "Logonomic.ini").is_file()):
        raise ValueError("Select the run directory printed by the desktool_dev target.")
    command = Path(marker.read_text(encoding="utf-8"))
    if not command.is_file():
        raise ValueError(f"Rebuild DeskTool. Its command is missing at {command}")
    environment = os.environ.copy()
    environment["PATH"] = os.pathsep.join([str(command.parent), environment.get("PATH", "")])
    output = run / "desktop-output.log"
    print(f"Starting Logonomic with profile {profile}\nDesktop stdout and stderr: {output}", flush=True)
    with output.open("ab") as log:
        return subprocess.run([options.desktop.resolve(), "--user-data-dir", profile],
                              env=environment, stdout=log, stderr=subprocess.STDOUT).returncode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="operation", required=True)
    check = commands.add_parser("prepare", help="test a configured DeskTool build and create a fresh profile")
    check.add_argument("ctest", type=Path)
    check.add_argument("build", type=Path)
    check.add_argument("package", type=Path)
    check.add_argument("command", type=Path)
    check.add_argument("--config", default="")
    start = commands.add_parser("launch", help="start Logonomic with a prepared DeskTool profile")
    start.add_argument("run", type=Path)
    start.add_argument("desktop", type=Path)
    options = parser.parse_args()
    try:
        return prepare(options) if options.operation == "prepare" else launch(options)
    except (OSError, ValueError) as error:
        print(f"DeskTool: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
