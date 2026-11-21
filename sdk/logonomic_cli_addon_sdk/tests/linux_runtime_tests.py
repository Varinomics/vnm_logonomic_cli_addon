"""Regressions for the package closure and final provider export boundaries."""

import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch


SDK = Path(__file__).resolve().parents[1]
specification = importlib.util.spec_from_file_location("linux_runtime", SDK / "cmake/deploy_linux_runtime.py")
runtime = importlib.util.module_from_spec(specification)
specification.loader.exec_module(runtime)


class Linux_runtime(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix="cli-linux-runtime-")
        self.addCleanup(self.scratch.cleanup)
        self.root = Path(self.scratch.name)
        self.package = self.root / "package"
        (self.package / "adapter").mkdir(parents=True)

    def image(self, relative, contents=b"input"):
        path = self.package / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"\x7fELF" + contents)
        return path

    def test_missing_dependency_is_not_an_empty_closure(self):
        with patch.object(runtime, "run", return_value="libcurl.so.4 => not found\n"):
            with self.assertRaisesRegex(ValueError, "Unresolved native dependency"):
                runtime.dependencies(Path("grok_quota_bridge"))

    def test_imported_generation_is_refused_without_mutation(self):
        image = self.image("adapter/adapter")
        index = self.package / "package.files.json"
        index.write_bytes(b"sealed index")
        with self.assertRaisesRegex(ValueError, "imported generations are immutable"):
            runtime.deploy(self.package, "Release", Path("patchelf"))
        self.assertEqual(image.read_bytes(), b"\x7fELFinput")
        self.assertEqual(index.read_bytes(), b"sealed index")

    def test_base_system_boundary_keeps_compiler_and_http_libraries(self):
        output = (f"libc.so.6 => {self.root / 'libc.so.6'} (0x01)\n"
                  f"libstdc++.so.6 => {self.root / 'libstdc++.so.6'} (0x02)\n"
                  f"libcurl.so.4 => {self.root / 'libcurl.so.4'} (0x03)\n")
        with patch.object(runtime, "run", return_value=output), patch.object(Path, "resolve", lambda path, **_: path):
            self.assertEqual([name for name, _ in runtime.dependencies(Path("adapter"))],
                             ["libstdc++.so.6", "libcurl.so.4"])

    def test_external_library_dependency_is_checked_without_an_executable(self):
        self.image("adapter/libcurl.so.4")
        with patch.object(runtime, "dependencies", return_value=[("libssl.so.3", self.root / "libssl.so.3")]):
            with self.assertRaisesRegex(ValueError, "external dependency"):
                runtime.verify(self.package)

    def test_conflicting_soname_inputs_fail_before_any_copy(self):
        first = self.image("adapter/adapter")
        second = self.image("helpers/helper")
        one, two = self.root / "one", self.root / "two"
        one.write_bytes(b"first runtime")
        two.write_bytes(b"different runtime")
        def dependencies(binary):
            return [("libsqlite3.so.0", one if binary == first else two)]
        with patch.object(runtime, "controlled_release"), patch.object(runtime, "dependencies", dependencies):
            with self.assertRaisesRegex(ValueError, "conflicting runtime bytes"):
                runtime.deploy(self.package, "Release", Path("patchelf"))
        self.assertFalse((self.package / "adapter/libsqlite3.so.0").exists())
        self.assertEqual(second.read_bytes(), b"\x7fELFinput")

    def test_controlled_release_rejects_debug_data(self):
        with patch.object(runtime, "run", return_value="[30] .debug_info PROGBITS"):
            with self.assertRaisesRegex(ValueError, "debugging data"):
                runtime.controlled_release(Path("adapter"), "Release")

    def copied_input(self, name, contents=b"original"):
        source = self.root / name
        source.write_bytes(b"\x7fELF" + contents)
        relative = "adapter/" + name
        self.image(relative, contents + b"deployed-rpath")
        return str(source), relative, runtime.digest(source)

    def supplier_material(self, source):
        notice = self.root / "supplier-notice.txt"
        notice.write_text("Supplier copyright and license\n", encoding="utf-8")
        return {"source": str(source), "source_sha256": runtime.digest(source),
                "binary_package": "Qt online installer qtbase", "binary_version": "6.12.0",
                "source_package": "qtbase", "source_version": "6.12.0",
                "source_url": "https://code.qt.io/cgit/qt/qtbase.git/tag/?h=v6.12.0",
                "notice": str(notice), "notice_sha256": runtime.digest(notice),
                "acquisition": {"provider": "qt-online-installer", "host": "linux", "target": "desktop",
                                "version": "6.12.0", "architecture": "linux_gcc_64", "archive": "qtbase"}}

    def test_copy_map_records_original_and_deployed_hashes_with_exact_owners(self):
        program = self.copied_input("kimi_adapter")
        library = self.copied_input("libQt6Core.so.6")
        source = Path(library[0])
        supplied = self.supplier_material(source)
        with patch.object(runtime, "run", return_value=""), \
                patch.object(runtime, "dependencies", return_value=[]):
            runtime.record(self.package, "Release", [program, library], [program[1]],
                           prebuilt={source: supplied})
        report = json.loads((self.package / "provenance/linux-runtime.json").read_text())
        self.assertEqual(report["schema_version"], 1)
        self.assertEqual(report["configuration"], "Release")
        controlled, vendor = report["files"]
        self.assertEqual(controlled["owner"], "controlled")
        self.assertNotIn("binary_package", controlled)
        self.assertEqual(vendor["owner"], "supplied-runtime")
        self.assertEqual(vendor["supplier_kind"], "prebuilt")
        self.assertEqual(vendor["source_sha256"], library[2])
        self.assertEqual(vendor["sha256"], runtime.digest(self.package / library[1]))
        self.assertNotEqual(vendor["source_sha256"], vendor["sha256"])
        self.assertNotIn("vendor_exception", vendor)

    def test_prebuilt_material_is_bound_to_original_and_notice_bytes(self):
        library = self.copied_input("libicuuc.so.73")
        source = Path(library[0])
        supplied = self.supplier_material(source)
        path = self.root / "material.json"
        path.write_text(json.dumps({"schema_version": 1, "inputs": [supplied]}))
        material = runtime.prebuilt_material(path)
        self.assertEqual(runtime.runtime_material(source, source.name, None, material)[1]["source_version"],
                         "6.12.0")
        source.write_bytes(b"changed vendor input")
        with self.assertRaisesRegex(ValueError, "differs from its input"):
            runtime.runtime_material(source, source.name, None, material)
        source.write_bytes(b"\x7fELForiginal")
        Path(supplied["notice"]).write_text("changed notice")
        with self.assertRaisesRegex(ValueError, "differs from its input"):
            runtime.runtime_material(source, source.name, None, material)

    def test_installed_runtime_retains_exact_dpkg_source_identity(self):
        library = self.copied_input("libcurl.so.4")
        source = Path(library[0])
        notices = self.root / "notices"
        notices.mkdir()
        (notices / (source.name + ".txt")).write_text("Installed package copyright\n")
        original_read = Path.read_text
        def read_text(path, *args, **kwargs):
            return "ID=ubuntu\n" if path.as_posix() == "/etc/os-release" else original_read(path, *args, **kwargs)
        with patch.object(runtime, "run", side_effect=[
                f"libcurl4t64:amd64: {source}\n",
                "libcurl4t64:amd64\t8.5.0-2ubuntu10.16\tcurl\t8.5.0-2ubuntu10.16"]), \
                patch.object(Path, "read_text", read_text):
            _, facts = runtime.runtime_material(source, source.name, notices)
        self.assertEqual(facts["binary_package"], "libcurl4t64:amd64")
        self.assertEqual(facts["source_package"], "curl")
        self.assertEqual(facts["source_version"], "8.5.0-2ubuntu10.16")
        self.assertEqual(facts["source_url"], "https://launchpad.net/ubuntu/+source/curl/8.5.0-2ubuntu10.16")

    def test_unmatched_prebuilt_material_does_not_establish_supplier_identity(self):
        library = self.copied_input("libicuuc.so.73")
        with patch.object(runtime, "run", side_effect=subprocess.CalledProcessError(1, "dpkg-query")):
            with self.assertRaisesRegex(ValueError, "no source-package provenance"):
                runtime.runtime_material(Path(library[0]), "libicuuc.so.73", None, {})

    def test_copy_map_requires_all_elf_members_and_unchanged_originals(self):
        program = self.copied_input("kimi_helper")
        with patch.object(runtime, "controlled_release"):
            self.image("adapter/unmapped-runtime")
            with self.assertRaisesRegex(ValueError, "does not cover"):
                runtime.record(self.package, "Release", [program], [program[1]])
            Path(program[0]).write_bytes(b"\x7fELFchanged")
            with self.assertRaisesRegex(ValueError, "changed since its copy"):
                runtime.record(self.package, "Release", [program], [program[1]])

    def test_copy_map_cannot_reclassify_an_indexed_generation(self):
        program = self.copied_input("kimi_quota_probe")
        (self.package / "package.files.json").write_text("sealed")
        with self.assertRaisesRegex(ValueError, "imported generations are immutable"):
            runtime.record(self.package, "Release", [program], [program[1]])

    @unittest.skipUnless(os.name == "posix" and (SDK.parents[1] / ".github/scripts/run_cli_addon_provider.py").exists(),
                         "hosted provider driver belongs to the full Linux SDK owner checkout")
    def test_provider_refuses_archive_when_exported_closure_fails(self):
        driver_path = SDK.parents[1] / ".github/scripts/run_cli_addon_provider.py"
        specification = importlib.util.spec_from_file_location("provider_driver", driver_path)
        driver = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(driver)
        workspace = self.root / "workspace"
        workspace.mkdir()
        commands = []
        def run(command, **kwargs):
            commands.append([str(argument) for argument in command])
            if "verify" in command:
                raise RuntimeError("exported native closure failed")
            if "--platform" in command:
                return "OK logonomic.cli.grok " + "a" * 64 + " package\n"
            return ""
        environment = {"GITHUB_ACTIONS": "true", "RUNNER_ENVIRONMENT": "github-hosted",
                       "PACKAGE_PLATFORM": "linux-x86_64", "CLI_ADDON_TOOLCHAIN": "retail"}
        with patch.dict(os.environ, environment), patch.object(driver, "run", run), \
                patch.object(driver, "compiler", return_value="g++"), \
                patch.object(driver, "executable", side_effect=lambda name: name), \
                patch.object(driver, "verify_compilers"), patch.object(driver, "verify_cache"):
            with self.assertRaisesRegex(RuntimeError, "exported native closure failed"):
                driver.contracts(workspace, "grok")
        self.assertTrue(any("verify" in command for command in commands))
        self.assertFalse(list(workspace.rglob("*.tar")))
        self.assertFalse((workspace / "ci-output/artifact-set.json").exists())


if __name__ == "__main__":
    unittest.main()
