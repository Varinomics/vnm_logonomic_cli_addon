"""Relocate a regular-file Mach-O closure without changing its source inputs."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import struct
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def checked(data, offset, size):
    if offset < 0 or size < 0 or offset + size > len(data):
        raise ValueError("Mach-O range is outside its image")
    return data[offset:offset + size]


def slices(data):
    magic = checked(data, 0, 4)
    formats = {
        b"\xca\xfe\xba\xbe": (">", False), b"\xbe\xba\xfe\xca": ("<", False),
        b"\xca\xfe\xba\xbf": (">", True), b"\xbf\xba\xfe\xca": ("<", True),
    }
    if magic not in formats:
        return [data]
    endian, wide = formats[magic]
    count = struct.unpack(endian + "I", checked(data, 4, 4))[0]
    entry_size = 32 if wide else 20
    checked(data, 8, count * entry_size)
    result = []
    for index in range(count):
        entry = checked(data, 8 + index * entry_size, entry_size)
        offset, size = struct.unpack_from(endian + ("QQ" if wide else "II"), entry, 8)
        result.append(checked(data, offset, size))
    return result


def inspect_image(path):
    result = []
    for data in slices(path.read_bytes()):
        magic = checked(data, 0, 4)
        if magic not in (b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xcf"):
            raise ValueError(f"Unsupported native image in {path}")
        endian = "<" if magic == b"\xcf\xfa\xed\xfe" else ">"
        header = struct.unpack(endian + "8I", checked(data, 0, 32))
        commands_end = 32 + header[5]
        checked(data, 32, header[5])
        image = {"cpu_type": header[1], "file_type": header[3], "dependencies": [],
                 "rpaths": [], "identity": None, "minimum_macos": None, "debug_data": False}
        offset = 32
        for _ in range(header[4]):
            command, size = struct.unpack(endian + "II", checked(data, offset, 8))
            if size < 8 or offset + size > commands_end:
                raise ValueError(f"Malformed load command in {path}")
            body = checked(data, offset, size)
            if command in (0xC, 0xD, 0x80000018, 0x8000001F, 0x20, 0x80000023, 0x8000001C):
                name_offset = struct.unpack(endian + "I", checked(body, 8, 4))[0]
                name = checked(body, name_offset, size - name_offset)
                if b"\0" not in name:
                    raise ValueError(f"Unterminated native path in {path}")
                name = name.split(b"\0", 1)[0].decode("utf-8")
                if command == 0xD:
                    image["identity"] = name
                elif command == 0x8000001C:
                    image["rpaths"].append(name)
                else:
                    image["dependencies"].append(name)
            elif command == 0x19:
                segment = checked(body, 8, 16).split(b"\0", 1)[0]
                image["debug_data"] |= segment == b"__DWARF"
                count = struct.unpack(endian + "I", checked(body, 64, 4))[0]
                for index in range(count):
                    flags = struct.unpack(endian + "I", checked(body, 72 + index * 80 + 64, 4))[0]
                    image["debug_data"] |= bool(flags & 0x02000000)
            elif command == 0x2:
                symbols, count = struct.unpack(endian + "II", checked(body, 8, 8))
                for index in range(count):
                    image["debug_data"] |= bool(checked(data, symbols + index * 16 + 4, 1)[0] & 0xE0)
            elif command in (0x24, 0x32):
                if command == 0x32 and struct.unpack(endian + "I", checked(body, 8, 4))[0] != 1:
                    raise ValueError(f"Native input is not a macOS image: {path}")
                version = struct.unpack(endian + "I", checked(body, 12 if command == 0x32 else 8, 4))[0]
                image["minimum_macos"] = [version >> 16, (version >> 8) & 255, version & 255]
            offset += size
        if offset != commands_end:
            raise ValueError(f"Native load-command size disagrees with its header: {path}")
        result.append(image)
    if not result or not any(image["cpu_type"] == 0x0100000C for image in result):
        raise ValueError(f"Native runtime has no ARM64 image: {path}")
    return result


def system_dependency(name):
    return name.startswith(("/System/Library/", "/usr/lib/"))


def run(arguments):
    result = subprocess.run(arguments, capture_output=True, text=True)
    if result.returncode:
        raise ValueError(f"{arguments[0]} failed for {arguments[-1]}: {result.stderr.strip()}")
    return result.stdout + result.stderr


def validate_signing_options(identity, team_id, configuration):
    if not identity and not team_id:
        return
    if not identity or not team_id:
        raise ValueError("Production signing requires both signing identity and team ID")
    if identity == "-" or not identity.strip():
        raise ValueError("Production signing requires a Developer ID Application identity")
    if re.fullmatch(r"[A-Z0-9]{10}", team_id) is None:
        raise ValueError("Production signing requires a ten-character Apple team ID")
    if configuration != "Release":
        raise ValueError("Production signing requires a Release configuration")


def sign_image(path, identity=None, team_id=None):
    if not identity:
        run(["/usr/bin/codesign", "--force", "--sign", "-", "--timestamp=none", str(path)])
        run(["/usr/bin/codesign", "--verify", "--strict", "--all-architectures", str(path)])
        return

    run(["/usr/bin/codesign", "--force", "--sign", identity,
         "--options", "runtime", "--timestamp", str(path)])
    requirement = (
        'anchor apple generic and certificate leaf[field.1.2.840.113635.100.6.1.13] exists '
        f'and certificate leaf[subject.OU] = "{team_id}"')
    run(["/usr/bin/codesign", "--verify", "--strict", "--all-architectures",
         "-R", requirement, str(path)])
    architectures = run(["/usr/bin/lipo", "-archs", str(path)]).split()
    if not architectures:
        raise ValueError(f"Signed native image has no architectures: {path}")
    for architecture in architectures:
        command = ["/usr/bin/codesign", "--display", "--arch", architecture]
        details = run([*command, "--verbose=4", str(path)])
        if f"TeamIdentifier={team_id}" not in details.splitlines():
            raise ValueError(f"Signed native image has the wrong Apple team ID: {path}: {architecture}")
        timestamp = re.search(r"^Timestamp=(.+)$", details, re.MULTILINE)
        if timestamp is None or timestamp[1].strip().lower() in ("none", "not set"):
            raise ValueError(f"Signed native image has no secure timestamp: {path}: {architecture}")
        flags = re.search(r"\bflags=0x([0-9a-fA-F]+)\b", details)
        if flags is None or not int(flags[1], 16) & 0x10000:
            raise ValueError(f"Signed native image has no hardened runtime: {path}: {architecture}")
        entitlements = run([*command, "--entitlements", "-", "--xml", str(path)])
        start = entitlements.find("<?xml")
        if start != -1:
            end = entitlements.find("</plist>", start)
            values = plistlib.loads(entitlements[start:end + len("</plist>")].encode("utf-8"))
            if values.get("com.apple.security.get-task-allow") or values.get("get-task-allow"):
                raise ValueError(f"Signed native image permits debugger attachment: {path}: {architecture}")


def deploy(args):
    package = args.package.resolve(strict=True)
    executables = set(args.executable)
    inputs = dict((relative, Path(source).resolve(strict=True)) for source, relative in args.input)
    runtimes = {Path(relative).name: relative for relative in inputs if relative not in executables}
    if len(runtimes) != len(inputs) - len(executables):
        raise ValueError("Runtime filenames must be unique within the package")
    observations = []
    for relative, source in inputs.items():
        target = package / relative
        if target.is_symlink() or not target.resolve(strict=True).is_relative_to(package):
            raise ValueError(f"Native package member is not a contained regular file: {relative}")
        before = inspect_image(source)
        controlled = relative in executables
        if controlled and (len(before) != 1 or before[0]["file_type"] != 2):
            raise ValueError(f"Controlled executable is not a thin ARM64 image: {relative}")
        if not controlled and any(image["file_type"] != 6 for image in before):
            raise ValueError(f"Runtime dependency is not a shared library: {relative}")
        if controlled and args.configuration == "Release" and any(image["debug_data"] for image in before):
            raise ValueError(f"Controlled Release executable contains debugging data: {relative}")
        if args.minimum_macos:
            minimum = [int(part) for part in args.minimum_macos.split(".")]
            minimum += [0] * (3 - len(minimum))
            for image in before:
                if image["cpu_type"] == 0x0100000C and (
                        image["minimum_macos"] is None or image["minimum_macos"] > minimum):
                    raise ValueError(f"Runtime exceeds the selected macOS deployment target: {relative}")
        source_hash = digest(source)
        if digest(target) != source_hash:
            raise ValueError(f"Staged native input differs from its source: {relative}")
        command = ["/usr/bin/install_name_tool"]
        dependencies = sorted({name for image in before for name in image["dependencies"]})
        for name in dependencies:
            if system_dependency(name):
                continue
            dependency = runtimes.get(Path(name).name)
            if dependency is None:
                raise ValueError(f"Native dependency is outside the resolved closure: {relative}: {name}")
            replacement = "@loader_path/" + os.path.relpath(package / dependency, target.parent)
            if name != replacement:
                command += ["-change", name, replacement]
        for rpath in sorted({name for image in before for name in image["rpaths"]}):
            command += ["-delete_rpath", rpath]
        if not controlled:
            command += ["-id", "@loader_path/" + target.name]
        if len(command) > 1:
            run([*command, str(target)])
        sign_image(target)
        after = inspect_image(target)
        for image in after:
            if image["rpaths"]:
                raise ValueError(f"Staged native runtime retains an external loader search path: {relative}")
            for name in image["dependencies"]:
                if system_dependency(name):
                    continue
                if not name.startswith("@loader_path/"):
                    raise ValueError(f"Staged native dependency is not relocatable: {relative}: {name}")
                dependency = target.parent / name.removeprefix("@loader_path/")
                if dependency.is_symlink() or not dependency.resolve(strict=True).is_relative_to(package):
                    raise ValueError(f"Staged dependency escapes the package: {relative}: {name}")
        observations.append({"path": relative, "source_sha256": source_hash,
                             "sha256": digest(target), "owner": "controlled" if controlled else "supplied-runtime",
                             "slices": after})
        if digest(source) != source_hash:
            raise ValueError(f"Native source changed during packaging: {source}")
    report = {"schema_version": 1, "configuration": args.configuration, "signature": "ad-hoc",
              "vendor_debug_policy": "Required supplied runtimes retain their original debugging data; standalone symbols are omitted.",
              "files": observations}
    (package / "provenance").mkdir(exist_ok=True)
    (package / "provenance/macos-runtime.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--configuration", required=True)
    parser.add_argument("--minimum-macos")
    parser.add_argument("--input", nargs=2, action="append", required=True, metavar=("SOURCE", "MEMBER"))
    parser.add_argument("--executable", action="append", required=True)
    deploy(parser.parse_args())


if __name__ == "__main__":
    main()
