"""Exercise the public package recipe's selected sidecar source and rebuilds."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


PROJECT = r"""
cmake_minimum_required(VERSION 3.25)
project(package_capabilities_fixture LANGUAGES CXX)
include("${SDK_SOURCE}/cmake/package.cmake")
add_executable(logonomic_cli_addon_index IMPORTED GLOBAL)
set_target_properties(logonomic_cli_addon_index PROPERTIES IMPORTED_LOCATION "${INDEX_TOOL}")
add_executable(probe probe.cpp)
logonomic_cli_addon_package_platform(package_platform)
configure_file(manifest.json.in generated/logonomic-cli-addon.json @ONLY)
if(GENERATED_CAPABILITIES)
    set(capabilities "${CMAKE_CURRENT_BINARY_DIR}/generated capabilities")
    file(MAKE_DIRECTORY "${capabilities}")
    configure_file(generated-sidecar.json "${capabilities}/fixture-origin@1.json" COPYONLY)
    logonomic_cli_addon_add_package(probe_package org.example.capabilities-fixture package_directory
        ADAPTER probe CAPABILITIES_DIRECTORY "${capabilities}")
else()
    set(capabilities "${CMAKE_CURRENT_SOURCE_DIR}/package/capabilities")
    logonomic_cli_addon_add_package(probe_package org.example.capabilities-fixture package_directory ADAPTER probe)
endif()
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/package-path-$<CONFIG>.txt" CONTENT "${package_directory}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/capabilities-path.txt" "${capabilities}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/platform.txt" "${package_platform}")
"""

MANIFEST = {
    "bootstrap_version": 1,
    "package_id": "org.example.capabilities-fixture",
    "release_version": "1.0.0",
    "platforms": ["@package_platform@"],
    "adapter": {
        "path": {"@package_platform@": "adapter/probe@CMAKE_EXECUTABLE_SUFFIX@"},
        "argv": [],
        "environment": {},
    },
    "core_protocol_generations": [1],
    "integrity_index": "package.files.json",
}


def sidecar(capability, origin):
    return json.dumps({"capability_id": capability, "version": 1, "payload": {"origin": origin}}).encode()


def run(*arguments):
    subprocess.run([str(argument) for argument in arguments], check=True)


def exercise(options, root, generated):
    source = root / "source"
    build = root / "build"
    for directory in ("assets", "capabilities", "provenance", "notices"):
        (source / "package" / directory).mkdir(parents=True)
    (source / "CMakeLists.txt").write_text(PROJECT, encoding="utf-8")
    (source / "probe.cpp").write_text("int main() { return 0; }\n", encoding="utf-8")
    (source / "manifest.json.in").write_text(json.dumps(MANIFEST), encoding="utf-8")
    original = source / "package/capabilities/fixture-origin@1.json"
    source_only = source / "package/capabilities/fixture-source-only@1.json"
    template = source / "generated-sidecar.json"
    original.write_bytes(sidecar("fixture-origin", "source"))
    source_only.write_bytes(sidecar("fixture-source-only", "source-only"))
    template.write_bytes(sidecar("fixture-origin", "generated"))

    configure = [options.cmake, "-S", source, "-B", build, "-G", options.generator,
                 "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_FASTBUILD_CAPTURE_SYSTEM_ENV=OFF",
                 f"-DSDK_SOURCE={options.sdk_source}", f"-DINDEX_TOOL={options.index_tool}",
                 f"-DGENERATED_CAPABILITIES={'ON' if generated else 'OFF'}"]
    for name, value in (("CMAKE_MAKE_PROGRAM", options.make_program),
                        ("CMAKE_CXX_COMPILER", options.compiler),
                        ("CMAKE_TOOLCHAIN_FILE", options.toolchain)):
        if value:
            configure.append(f"-D{name}={value}")
    for flag, value in (("-A", options.generator_platform), ("-T", options.generator_toolset)):
        if value:
            configure.extend((flag, value))
    run(*configure)
    package = Path((build / "package-path-Release.txt").read_text(encoding="utf-8"))
    selected = Path((build / "capabilities-path.txt").read_text(encoding="utf-8"))
    platform = (build / "platform.txt").read_text(encoding="utf-8")

    def assemble():
        run(options.cmake, "--build", build, "--config", "Release", "--parallel", "1", "--target", "probe_package")

    def verify(expected):
        actual = {path.name: path.read_bytes() for path in (package / "capabilities").iterdir()}
        if actual != expected:
            raise AssertionError(f"Sidecar source/membership mismatch: {actual!r} != {expected!r}")
        run(options.validate_tool, package, "--platform", platform)

    expected = {"fixture-origin@1.json": template.read_bytes() if generated else original.read_bytes()}
    if not generated:
        expected[source_only.name] = source_only.read_bytes()
    assemble()
    verify(expected)
    index = package / "package.files.json"
    unchanged = (index.stat().st_mtime_ns, index.read_bytes())
    assemble()
    if unchanged != (index.stat().st_mtime_ns, index.read_bytes()):
        raise AssertionError("An unchanged package was unnecessarily reassembled")

    changed = template if generated else original
    changed.write_bytes(sidecar("fixture-origin", "changed"))
    expected[original.name] = changed.read_bytes()
    assemble()
    verify(expected)

    added = selected / "fixture-added@1.json"
    added.write_bytes(sidecar("fixture-added", "old-timestamp"))
    os.utime(added, ns=(1_000_000_000, 1_000_000_000))
    expected[added.name] = added.read_bytes()
    assemble()
    verify(expected)
    added.unlink()
    del expected[added.name]
    assemble()
    verify(expected)

    if generated:
        unchanged = (index.stat().st_mtime_ns, index.read_bytes())
        source_only.write_bytes(sidecar("fixture-source-only", "unselected-change"))
        assemble()
        verify(expected)
        if unchanged != (index.stat().st_mtime_ns, index.read_bytes()):
            raise AssertionError("The unselected source sidecars still drive package assembly")
    print(f"{'Generated' if generated else 'Default'} capability source and membership passed", flush=True)


def main():
    parser = argparse.ArgumentParser()
    for name in ("cmake", "sdk-source", "index-tool", "validate-tool", "generator"):
        parser.add_argument(f"--{name}", required=True)
    for name in ("make-program", "compiler", "toolchain", "generator-platform", "generator-toolset"):
        parser.add_argument(f"--{name}", default="")
    options = parser.parse_args()
    # Each nested build is serial. The registered CTest PROCESSORS count and
    # its caller's queue lease cover that one compiler job.
    with tempfile.TemporaryDirectory(prefix="cli-package-capabilities-") as directory:
        root = Path(directory)
        exercise(options, root / "default", False)
        exercise(options, root / "generated", True)


if __name__ == "__main__":
    main()
