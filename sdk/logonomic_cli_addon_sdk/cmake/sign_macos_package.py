"""Sign fresh macOS package staging before its first integrity index is written."""

import argparse
import json
from pathlib import Path
import subprocess

from deploy_macos_runtime import digest, inspect_image, sign_image, validate_signing_options


def sign_package(package, identity, team_id, configuration):
    validate_signing_options(identity, team_id, configuration)
    if not identity:
        raise ValueError("Package signing requires a Developer ID Application identity and team ID")
    package = package.resolve(strict=True)
    index = package / "package.files.json"
    if index.exists() or index.is_symlink():
        raise ValueError("Production signing requires fresh package staging without an integrity index")

    native_files = []
    for path in sorted(package.rglob("*")):
        if path.is_symlink():
            raise ValueError(f"Package signing requires regular files: {path}")
        if not path.is_file():
            continue
        with path.open("rb") as source:
            magic = source.read(4)
        if magic in (b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xcf", b"\xca\xfe\xba\xbe",
                     b"\xbe\xba\xfe\xca", b"\xca\xfe\xba\xbf", b"\xbf\xba\xfe\xca"):
            slices = inspect_image(path)
            if any(image["file_type"] == 2 and image["debug_data"] for image in slices):
                raise ValueError(f"Controlled Release executable contains debugging data: {path}")
            native_files.append(path)
    if not native_files:
        raise ValueError("macOS package staging contains no native images")

    for path in native_files:
        sign_image(path, identity, team_id)

    provenance = package / "provenance/macos-runtime.json"
    if provenance.exists():
        report = json.loads(provenance.read_text(encoding="utf-8"))
        for record in report["files"]:
            path = package / record["path"]
            if path not in native_files:
                raise ValueError(f"Native provenance names a member outside the signed closure: {path}")
            record["sha256"] = digest(path)
            record["slices"] = inspect_image(path)
        report["signature"] = "developer-id"
        report["team_id"] = team_id
        provenance.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--signing-identity", required=True)
    parser.add_argument("--team-id", required=True)
    parser.add_argument("--configuration", required=True)
    parser.add_argument("--index", type=Path, required=True)
    parser.add_argument("index_arguments", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    arguments = args.index_arguments
    if arguments and arguments[0] == "--":
        arguments = arguments[1:]
    if not arguments:
        parser.error("The package directory and index arguments must follow --")
    try:
        sign_package(Path(arguments[0]), args.signing_identity, args.team_id, args.configuration)
        subprocess.run([str(args.index), *arguments], check=True)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"macOS package signing failed: {error}\n")


if __name__ == "__main__":
    main()
