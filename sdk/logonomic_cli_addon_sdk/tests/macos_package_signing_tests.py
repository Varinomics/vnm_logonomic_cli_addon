"""Apple signing-policy and SDK seal-order checks; native tools are mocked.

Oracle: developer.apple.com/documentation/xcode/creating-distribution-signed-code-for-the-mac
and developer.apple.com/documentation/security/resolving-common-notarization-issues
require Developer ID, hardened runtime, a secure timestamp and no debugger
entitlement. The SDK index seals the final bytes.
"""

import json
from pathlib import Path
import plistlib
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "cmake"))
import deploy_macos_runtime as runtime
import sign_macos_package as package_signer


class Macos_package_signing_tests(unittest.TestCase):
    identity = "Developer ID Application: Example Software (ABCDE12345)"
    team = "ABCDE12345"

    def setUp(self):
        self.commands = []
        self.details = "TeamIdentifier=ABCDE12345\nTimestamp=Oct 16, 2026\nflags=0x10000(runtime)\n"
        self.entitlements = plistlib.dumps({}).decode("utf-8")

    def native_command(self, arguments):
        self.commands.append(arguments)
        if arguments[0] == "/usr/bin/lipo":
            return "arm64 x86_64\n"
        if "--verbose=4" in arguments:
            return self.details
        if "--entitlements" in arguments:
            return self.entitlements
        return ""

    def test_production_signs_and_verifies_every_slice(self):
        with mock.patch.object(runtime, "run", side_effect=self.native_command):
            runtime.sign_image(Path("adapter/example"), self.identity, self.team)
        sign = self.commands[0]
        self.assertIn("--timestamp", sign)
        self.assertEqual(sign[sign.index("--options") + 1], "runtime")
        self.assertEqual(sign[sign.index("--sign") + 1], self.identity)
        verify = self.commands[1]
        self.assertIn("--all-architectures", verify)
        requirement = verify[verify.index("-R") + 1]
        self.assertIn("anchor apple generic", requirement)
        self.assertIn("certificate leaf[field.1.2.840.113635.100.6.1.13] exists", requirement)
        self.assertIn('certificate leaf[subject.OU] = "ABCDE12345"', requirement)
        inspected = [item[item.index("--arch") + 1] for item in self.commands if "--verbose=4" in item]
        self.assertEqual(inspected, ["arm64", "x86_64"])

    def test_invalid_signature_properties_are_rejected(self):
        for details, reason in (
                (self.details.replace("ABCDE12345", "OTHER12345"), "team ID"),
                (self.details.replace("Timestamp=Oct 16, 2026", "Signed Time=Oct 16, 2026"), "timestamp"),
                (self.details.replace("Timestamp=Oct 16, 2026", "Timestamp=none"), "timestamp"),
                (self.details.replace("flags=0x10000", "flags=0x0"), "hardened runtime")):
            with self.subTest(reason=reason):
                self.details = details
                with mock.patch.object(runtime, "run", side_effect=self.native_command):
                    with self.assertRaisesRegex(ValueError, reason):
                        runtime.sign_image(Path("adapter/example"), self.identity, self.team)

    def test_debugger_entitlement_is_rejected(self):
        for key in ("com.apple.security.get-task-allow", "get-task-allow"):
            self.entitlements = plistlib.dumps({key: True}).decode("utf-8")
            with mock.patch.object(runtime, "run", side_effect=self.native_command):
                with self.assertRaisesRegex(ValueError, "debugger"):
                    runtime.sign_image(Path("adapter/example"), self.identity, self.team)

    def test_signature_failure_stops_verification(self):
        with mock.patch.object(runtime, "run", side_effect=ValueError("codesign failed")) as native:
            with self.assertRaisesRegex(ValueError, "codesign failed"):
                runtime.sign_image(Path("adapter/example"), self.identity, self.team)
        self.assertEqual(native.call_count, 1)

    def test_development_retains_ad_hoc_signing(self):
        with mock.patch.object(runtime, "run", side_effect=self.native_command):
            runtime.sign_image(Path("adapter/example"))
        self.assertEqual(len(self.commands), 2)
        self.assertIn("--timestamp=none", self.commands[0])
        self.assertEqual(self.commands[0][self.commands[0].index("--sign") + 1], "-")

    def test_options_require_production_identity_team_and_release(self):
        for identity, team, configuration in (
                (self.identity, None, "Release"), (None, self.team, "Release"),
                ("-", self.team, "Release"), (self.identity, "invalid", "Release"),
                (self.identity, self.team, "RelWithDebInfo")):
            with self.subTest(identity=identity, team=team, configuration=configuration):
                with self.assertRaises(ValueError):
                    runtime.validate_signing_options(identity, team, configuration)

    def create_package(self, directory):
        package = Path(directory)
        (package / "adapter").mkdir()
        binary = package / "adapter/example"
        binary.write_bytes(struct.pack("<8I", 0xFEEDFACF, 0x0100000C, 0, 2, 0, 0, 0, 0))
        return package, binary

    def test_native_ownership_report_is_identical_across_build_directories(self):
        reports = []
        with tempfile.TemporaryDirectory() as directory, mock.patch.object(runtime, "run", side_effect=self.native_command):
            for build in ("standalone", "embedded"):
                root = Path(directory) / build
                root.mkdir()
                package, binary = self.create_package(root)
                source = root / "release" / "example"
                source.parent.mkdir()
                source.write_bytes(binary.read_bytes())
                runtime.deploy(SimpleNamespace(package=package, executable=["adapter/example"],
                    input=[(source, "adapter/example")], configuration="Release", minimum_macos=None))
                reports.append((package / "provenance/macos-runtime.json").read_bytes())
                row = json.loads(reports[-1])["files"][0]
                self.assertEqual(row["owner"], "controlled")
                self.assertEqual(row["source_sha256"], runtime.digest(source))
                self.assertEqual(row["sha256"], runtime.digest(binary))
        self.assertEqual(reports[0], reports[1])

    def test_signing_precedes_provenance_and_first_index(self):
        with tempfile.TemporaryDirectory() as directory:
            package, binary = self.create_package(directory)
            (package / "provenance").mkdir()
            (package / "provenance/macos-runtime.json").write_text(json.dumps({
                "signature": "ad-hoc", "files": [{"path": "adapter/example", "sha256": runtime.digest(binary)}]}))
            original = binary.read_bytes()
            index_called = []

            def sign(path, identity, team):
                self.assertFalse((package / "package.files.json").exists())
                path.write_bytes(path.read_bytes() + b"signed")

            def index(arguments, check):
                self.assertEqual(arguments, ["index-tool", str(package)])
                self.assertTrue(check)
                self.assertNotEqual(binary.read_bytes(), original)
                report = json.loads((package / "provenance/macos-runtime.json").read_text())
                self.assertEqual(report["signature"], "developer-id")
                self.assertEqual(report["team_id"], self.team)
                self.assertEqual(report["files"][0]["sha256"], runtime.digest(binary))
                index_called.append(True)

            arguments = ["sign_macos_package.py", "--signing-identity", self.identity,
                         "--team-id", self.team, "--configuration", "Release",
                         "--index", "index-tool", "--", str(package)]
            with mock.patch.object(sys, "argv", arguments), \
                    mock.patch.object(package_signer, "sign_image", side_effect=sign), \
                    mock.patch.object(subprocess, "run", side_effect=index):
                package_signer.main()
            self.assertEqual(index_called, [True])

    def test_sealed_package_is_not_mutated(self):
        with tempfile.TemporaryDirectory() as directory:
            package, binary = self.create_package(directory)
            original = binary.read_bytes()
            (package / "package.files.json").write_text("existing seal")
            with mock.patch.object(package_signer, "sign_image") as sign:
                with self.assertRaisesRegex(ValueError, "fresh package staging"):
                    package_signer.sign_package(package, self.identity, self.team, "Release")
            sign.assert_not_called()
            self.assertEqual(binary.read_bytes(), original)

    def test_signing_failure_never_writes_an_index(self):
        with tempfile.TemporaryDirectory() as directory:
            package, binary = self.create_package(directory)
            arguments = ["sign_macos_package.py", "--signing-identity", self.identity,
                         "--team-id", self.team, "--configuration", "Release",
                         "--index", "index-tool", "--", str(package)]
            with mock.patch.object(sys, "argv", arguments), \
                    mock.patch.object(package_signer, "sign_image", side_effect=ValueError("wrong team")), \
                    mock.patch.object(subprocess, "run") as index, \
                    mock.patch.object(sys, "stderr"):
                with self.assertRaises(SystemExit) as result:
                    package_signer.main()
            self.assertEqual(result.exception.code, 1)
            index.assert_not_called()
            self.assertFalse((package / "package.files.json").exists())


if __name__ == "__main__":
    unittest.main()
