"""Seal a package-local ELF runtime before its integrity index is written."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
from urllib.parse import quote


BASE_SYSTEM_LIBRARIES = re.compile(
    r"^(ld-linux[^/]*|linux-vdso[^/]*|lib(c|m|dl|pthread|rt|resolv|util)\.so(?:\..*)?)$")


def run(arguments):
    environment = os.environ.copy()
    environment["LC_ALL"] = "C"
    for name in ("LD_LIBRARY_PATH", "LD_PRELOAD", "LD_AUDIT"):
        environment.pop(name, None)
    try:
        result = subprocess.run([str(value) for value in arguments], env=environment,
                                capture_output=True, text=True, check=True)
    except FileNotFoundError as error:
        tool = str(arguments[0])
        packages = {"ldd": "libc-bin", "readelf": "binutils", "dpkg-query": "dpkg"}
        if tool in packages:
            raise ValueError(f"Linux runtime packaging needs {tool}. Install {packages[tool]} in the builder.") from error
        raise
    return result.stdout


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def is_elf(path):
    with path.open("rb") as stream:
        return stream.read(4) == b"\x7fELF"


def elf_files(package):
    result = []
    for path in sorted(package.rglob("*")):
        if path.is_symlink():
            raise ValueError(f"Native package member is not a regular file: {path}")
        if path.is_file() and is_elf(path):
            result.append(path)
    return result


def is_static_executable(binary):
    layout = run(["readelf", "--wide", "--program-headers", "--dynamic", binary])
    if not re.search(r"^  LOAD\s", layout, re.MULTILINE) or re.search(r"^  INTERP\s", layout, re.MULTILINE):
        return False
    if "Elf file type is EXEC " in layout:
        return "There is no dynamic section in this file." in layout and not re.search(r"^  DYNAMIC\s", layout, re.MULTILINE)
    # A static PIE uses its dynamic section for self-relocation. Ordinary
    # shared libraries also lack an interpreter, so require the PIE flag.
    return ("Elf file type is DYN " in layout and
            re.search(r"\(FLAGS_1\)[^\n]*\bPIE\b", layout) is not None and
            not re.search(r"\((?:NEEDED|FILTER|AUXILIARY|AUDIT|DEPAUDIT)\)", layout))


def dependencies(binary):
    if is_static_executable(binary):
        return []
    result = []
    for line in run(["ldd", binary]).splitlines():
        line = line.strip()
        if "=> not found" in line:
            raise ValueError(f"Unresolved native dependency: {binary}: {line}")
        if "=>" in line:
            name, location = line.split("=>", 1)
            name, location = name.strip(), location.strip().split(" (", 1)[0]
        elif line.startswith("/"):
            location = line.split(" (", 1)[0]
            name = Path(location).name
        else:
            continue
        if not BASE_SYSTEM_LIBRARIES.fullmatch(name):
            if Path(name).name != name or not Path(location).is_absolute():
                raise ValueError(f"Invalid native dependency: {binary}: {line}")
            result.append((name, Path(location).resolve(strict=True)))
    return result


def verify(package):
    package = package.resolve(strict=True)
    for binary in elf_files(package):
        for name, source in dependencies(binary):
            if not source.is_relative_to(package):
                raise ValueError(f"Packaged ELF requires an external dependency: {binary}: {name}: {source}")


def controlled_release(path, configuration):
    if configuration != "Release":
        return
    sections = run(["readelf", "--sections", "--wide", path])
    if re.search(r"\.(?:debug\S*|zdebug\S*|gnu_debug\S*|gdb_index|symtab)\s", sections):
        raise ValueError(f"Controlled Release executable contains debugging data: {path}")


def prebuilt_material(path):
    if path is None:
        return {}
    document = json.loads(path.read_text(encoding="utf-8"))
    if document.get("schema_version") != 1 or not isinstance(document.get("inputs"), list):
        raise ValueError(f"Unsupported prebuilt runtime material: {path}")
    inputs = {}
    for row in document["inputs"]:
        source = Path(row["source"]).resolve(strict=True)
        required = ("source_sha256", "binary_package", "binary_version", "source_package",
                    "source_version", "source_url", "notice", "notice_sha256")
        if (not all(isinstance(row.get(field), str) and row[field] for field in required) or
                not isinstance(row.get("acquisition"), dict) or not row["acquisition"]):
            raise ValueError(f"Prebuilt runtime material is incomplete: {source}")
        if source in inputs:
            raise ValueError(f"Duplicate prebuilt runtime material: {source}")
        inputs[source] = row
    return inputs


def runtime_material(source, name, notices, prebuilt=None):
    supplied = (prebuilt or {}).get(source)
    if supplied is not None:
        notice = Path(supplied["notice"])
        if digest(source) != supplied["source_sha256"] or digest(notice) != supplied["notice_sha256"]:
            raise ValueError(f"Prebuilt runtime material differs from its input: {source}")
        fields = ("binary_package", "binary_version", "source_package", "source_version",
                  "source_url", "notice", "notice_sha256", "acquisition")
        provenance = {field: supplied[field] for field in fields}
        provenance["supplier_kind"] = "prebuilt"
        return notice.read_text(encoding="utf-8"), provenance
    # Reuse the provider's existing Debian copyright route. Package ownership
    # also identifies the exact source revision rather than a moving homepage.
    owner = None
    for candidate in dict.fromkeys((str(source), str(source).replace("/usr/", "/", 1))):
        try:
            ownership = run(["dpkg-query", "-S", candidate])
        except subprocess.CalledProcessError:
            continue
        for line in ownership.splitlines():
            if ": " in line:
                owner = line.split(": ", 1)[0]
                break
        if owner:
            break
    if owner is None:
        raise ValueError(f"Runtime has no source-package provenance: {source}. "
                         "Use a library installed by the builder's Debian or Ubuntu package manager.")
    fields = run(["dpkg-query", "-W", "-f=${binary:Package}\t${Version}\t${source:Package}\t${source:Version}",
                  owner]).split("\t")
    if len(fields) != 4 or not all(fields):
        raise ValueError(f"Runtime source-package provenance is incomplete: {source}")
    notice = Path("/usr/share/doc") / owner.split(":", 1)[0] / "copyright"
    if notices is not None and (notices / (name + ".txt")).is_file():
        notice = notices / (name + ".txt")
    if not notice.is_file():
        raise ValueError(f"Missing copyright notice for {owner}: {notice}. "
                         f"Restore the packaged copyright file or supply {name}.txt through RUNTIME_NOTICES.")
    contents = notice.read_text(encoding="utf-8")
    distribution = dict(line.split("=", 1) for line in Path("/etc/os-release").read_text().splitlines()
                        if "=" in line)["ID"].strip('"')
    source_package, source_version = fields[2:]
    if distribution == "ubuntu":
        source_url = f"https://launchpad.net/ubuntu/+source/{quote(source_package)}/{quote(source_version, safe='')}"
    elif distribution == "debian":
        source_url = f"https://snapshot.debian.org/package/{quote(source_package)}/{quote(source_version, safe='')}/"
    else:
        raise ValueError(f"Runtime source-download route is not established for {distribution}: {source}")
    return contents, {"binary_package": fields[0], "binary_version": fields[1],
                      "source_package": source_package, "source_version": source_version,
                      "source_url": source_url, "notice": str(notice), "notice_sha256": digest(notice)}


def write_provenance(package, configuration, observations, material):
    for row in observations:
        row["sha256"] = digest(package / row["path"])
    (package / "notices").mkdir(exist_ok=True)
    (package / "provenance").mkdir(exist_ok=True)
    (package / "notices/linux-runtime.txt").write_text("".join(material), encoding="utf-8")
    report = {"schema_version": 1, "configuration": configuration,
              "vendor_debug_policy": "Supplier attribution does not establish a vendor debugging-data exception; release inspection must assess necessity and available vendor variants.",
              "files": observations}
    (package / "provenance/linux-runtime.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


def record(package, configuration, inputs, executables, notices=None, prebuilt=None):
    """Record a caller's resolved copy map after its deployment-only edits."""
    package = package.resolve(strict=True)
    if (package / "package.files.json").exists():
        raise ValueError("Record native runtimes before indexing; imported generations are immutable")
    observations, material = [], []
    members = set()
    controlled = set(executables)
    source_hashes = []
    for original, relative, expected in inputs:
        source = Path(original).resolve(strict=True)
        target = package / relative
        if (not relative or "\\" in relative or Path(relative).is_absolute() or
                any(part in ("", ".", "..") for part in relative.split("/")) or
                target.is_symlink() or not target.resolve(strict=True).is_relative_to(package)):
            raise ValueError(f"Native package member is not a contained regular file: {relative}")
        if relative in members:
            raise ValueError(f"Duplicate native copy-map member: {relative}")
        members.add(relative)
        if not source.is_file() or not is_elf(source) or not target.is_file() or not is_elf(target):
            raise ValueError(f"Native copy-map member is not ELF: {relative}")
        if digest(source) != expected:
            raise ValueError(f"Native input changed since its copy was selected: {source}")
        source_hashes.append((source, expected))
        row = {"path": relative, "source": str(source), "source_sha256": expected}
        if relative in controlled:
            controlled_release(source, configuration)
            controlled_release(target, configuration)
            row["owner"] = "controlled"
        else:
            contents, provenance = runtime_material(source, target.name, notices, prebuilt)
            row.update(owner="supplied-runtime", **provenance)
            material.append(f"\n--- {target.name}: {provenance['binary_package']} {provenance['binary_version']} ---\n{contents}\n")
        observations.append(row)
    if not controlled or not controlled.issubset(members):
        raise ValueError("Controlled executable members must occur in the native copy map")
    actual = {path.relative_to(package).as_posix() for path in elf_files(package)}
    if members != actual:
        raise ValueError("Native copy map does not cover the package's ELF files")
    verify(package)
    for source, expected in source_hashes:
        if digest(source) != expected:
            raise ValueError(f"Native input changed during provenance recording: {source}")
    write_provenance(package, configuration, observations, material)


def deploy(package, configuration, patchelf, notices=None):
    package = package.resolve(strict=True)
    if (package / "package.files.json").exists():
        raise ValueError("Deploy native runtimes before indexing; imported generations are immutable")
    binaries = elf_files(package)
    libraries = {}
    observations = []
    for binary in binaries:
        controlled_release(binary, configuration)
        observations.append({"path": binary.relative_to(package).as_posix(),
                             "source_sha256": digest(binary), "owner": "controlled"})
        for name, source in dependencies(binary):
            source_hash = digest(source)
            if name in libraries and libraries[name][1] != source_hash:
                raise ValueError(f"Executables require conflicting runtime bytes: {name}")
            libraries[name] = (source, source_hash)
    material = []
    for name, (source, source_hash) in sorted(libraries.items()):
        target = package / "adapter" / name
        if target.exists():
            raise ValueError(f"Runtime destination already belongs to a package member: {target}")
        contents, provenance = runtime_material(source, name, notices)
        material.append(f"\n--- {name}: {provenance['binary_package']} {provenance['binary_version']} ---\n{contents}\n")
        shutil.copyfile(source, target)
        target.chmod(0o755)
        observations.append({"path": target.relative_to(package).as_posix(), "source": str(source),
                             "source_sha256": source_hash, "owner": "supplied-runtime", **provenance})
        if digest(target) != source_hash or digest(source) != source_hash:
            raise ValueError(f"Runtime source changed while copying: {source}")
    for binary in elf_files(package):
        if is_static_executable(binary):
            continue
        relative = os.path.relpath(package / "adapter", binary.parent)
        rpath = "$ORIGIN" if relative == "." else "$ORIGIN/" + relative
        run([patchelf, "--set-rpath", rpath, binary])
    verify(package)
    for source, source_hash in libraries.values():
        if digest(source) != source_hash:
            raise ValueError(f"Runtime input changed during deployment: {source}")
    write_provenance(package, configuration, observations, material)
    return [row["path"] for row in observations if row["owner"] == "supplied-runtime"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    deployment = commands.add_parser("deploy")
    deployment.add_argument("--configuration", required=True)
    deployment.add_argument("--patchelf", type=Path, required=True)
    deployment.add_argument("--runtime-notices", type=Path)
    deployment.add_argument("--index", type=Path, required=True)
    deployment.add_argument("index_arguments", nargs=argparse.REMAINDER)
    verification = commands.add_parser("verify")
    verification.add_argument("--package", type=Path, required=True)
    recording = commands.add_parser("record")
    recording.add_argument("--package", type=Path, required=True)
    recording.add_argument("--configuration", required=True)
    recording.add_argument("--input", nargs=3, action="append", required=True,
                           metavar=("SOURCE", "MEMBER", "SOURCE_SHA256"))
    recording.add_argument("--executable", action="append", required=True)
    recording.add_argument("--runtime-notices", type=Path)
    recording.add_argument("--prebuilt-material", type=Path)
    args = parser.parse_args()
    if args.command == "verify":
        verify(args.package)
        return
    if args.command == "record":
        record(args.package, args.configuration, args.input, args.executable,
               args.runtime_notices, prebuilt_material(args.prebuilt_material))
        return
    if len(args.index_arguments) < 2 or args.index_arguments[0] != "--":
        parser.error("deploy requires -- followed by the normal package index arguments")
    package = Path(args.index_arguments[1])
    resources = deploy(package, args.configuration, args.patchelf, args.runtime_notices)
    arguments = [str(args.index), *args.index_arguments[1:]]
    for relative in resources:
        arguments += ["--third-party", relative, "notices/linux-runtime.txt", "provenance/linux-runtime.json"]
    run(arguments)


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        detail = (error.stderr or error.stdout or "").strip()
        raise SystemExit(f"Linux runtime packaging: {error.cmd[0]} exited {error.returncode}. {detail}".rstrip())
    except (ValueError, OSError) as error:
        raise SystemExit(f"Linux runtime packaging: {error}")
