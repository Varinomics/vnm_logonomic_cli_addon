"""Check profile isolation and explicit launch without starting a desktop."""

from contextlib import redirect_stdout
from io import StringIO
import os
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
from unittest import TestCase, main
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import develop


class Development_profile_test(TestCase):
    def test_checks_isolate_profiles_and_launch_only_the_selected_one(self):
        with tempfile.TemporaryDirectory(prefix="desktool check, ") as directory:
            build = Path(directory).resolve()
            command = build / "bin" / "desktool"
            command.parent.mkdir()
            command.touch()
            package = build / "package" / "org.example.desktool"
            options = SimpleNamespace(build=build, ctest=Path("ctest"), package=package, command=command,
                                      config="Release")
            other_profile = build / "other-profile"
            other_profile.mkdir()
            with redirect_stdout(StringIO()), patch.object(develop.subprocess, "run") as child:
                child.return_value.returncode = 17
                self.assertEqual(develop.prepare(options), 17)
                self.assertFalse(list(build.glob("desktool-dev-*/profile")))
                child.return_value.returncode = 0
                for _ in range(2):
                    self.assertEqual(develop.prepare(options), 0)
                profiles = list(build.glob("desktool-dev-*/profile"))
                self.assertEqual(len(profiles), 2)
                settings = profiles[0] / "settings" / "Varinomics" / "Logonomic.ini"
                self.assertIn(f'development_root="{package.parent.as_posix()}"', settings.read_text(encoding="utf-8"))
                self.assertEqual(child.call_count, 3)
                desktop = build / "desktop"
                with self.assertRaises(ValueError):
                    develop.launch(SimpleNamespace(run=other_profile, desktop=desktop))
                self.assertFalse(list(other_profile.iterdir()))
                self.assertEqual(develop.launch(SimpleNamespace(run=profiles[0].parent, desktop=desktop)), 0)
                args, kwargs = child.call_args
                self.assertEqual(args[0], [desktop, "--user-data-dir", profiles[0]])
                self.assertEqual(kwargs["env"]["PATH"].split(os.pathsep)[0], str(command.parent))


if __name__ == "__main__":
    main()
