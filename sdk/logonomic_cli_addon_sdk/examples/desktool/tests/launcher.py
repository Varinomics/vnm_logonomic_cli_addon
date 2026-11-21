"""Exercise the packaged adapter through the SDK host simulator."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile


def check_equal(observed, expected, description):
    if observed != expected:
        raise AssertionError(f"{description}: expected {expected!r}, got {observed!r}")


def exercise(simulator, package, platform, command, working):
    ambient = {"PATH": str(command.parent)}
    if platform == "windows-x86_64":
        ambient["PATHEXT"] = ".EXE"
    saved = {"script_body": 'desktool "two words"', "runner_key": "direct",
             "environment_text": "DESKTOOL_PROJECT=external"}
    discovery = {"platform": platform, "ambient_environment": ambient,
                 "default_home_fallback": str(working)}
    recognition = {"platform": platform, "launcher": saved, "ambient_environment": ambient}
    preparation = {
        "launcher": saved,
        "launcher_argv": ["desktool", "two words"],
        "launch_environment": {"platform": platform,
                               "effective_environment": {**ambient, "DESKTOOL_PROJECT": "external"}},
        "working_directory": str(working), "title_instruction": "", "declared_assets": [],
    }

    steps = []
    requests = [
        ("discover", "launcher-discovery", 1, "discover", discovery),
        ("absent", "launcher-discovery", 1, "discover", {**discovery, "ambient_environment": {"PATH": str(working)}}),
        ("recognize", "launcher-discovery", 1, "recognize", recognition),
        ("unrelated", "launcher-discovery", 1, "recognize",
         {**recognition, "launcher": {**saved, "script_body": "other-tool"}}),
        ("prepare", "launch-preparation", 3, "prepare", preparation),
    ]
    for name, capability, version, operation, payload in requests:
        steps.append({"op": "invoke", "id": name, "capability_id": capability,
                      "version": version, "operation": operation, "payload": payload})
        steps.append({"op": "wait_result", "id": name, "timeout_ms": 5000})
    steps.append({"op": "shutdown"})
    script = working / "script.json"
    script.write_text(json.dumps({"script_version": 1, "steps": steps}), encoding="utf-8")
    simulation = subprocess.run([simulator, package, "--script", script],
                                stdout=subprocess.PIPE, encoding="utf-8")
    transcript = simulation.stdout
    print(transcript, end="", flush=True)
    simulation.check_returncode()
    frames = [json.loads(line[2:]) for line in transcript.splitlines() if line.startswith("< ")]
    results = [frame for frame in frames if frame["type"] == "result"]
    check_equal(len(results), len(requests), transcript)
    for index, result in enumerate(results, start=1):
        check_equal(result["request_id"], f"<r#{index}>", transcript)
        check_equal(result["status"], "success", transcript)
    found, absent, match, unrelated, patch = [result["payload"] for result in results]
    check_equal(len(found["candidates"]), 1, "candidate count")
    candidate = found["candidates"][0]
    check_equal((candidate["candidate_id"], candidate["display_name"], candidate["script_body"],
                 candidate["runner_key"], candidate["environment_text"], candidate["default"]),
                ("desktool", "DeskTool", "desktool", "direct", "", True), "discovered recipe")
    check_equal(absent["candidates"], [], "empty PATH directory")
    check_equal((match["recognized"], match["display_name"]), (True, "DeskTool"), "recognized saved command")
    check_equal(unrelated, {"recognized": False}, "unrelated command")
    check_equal(patch, {"environment": {"set": {}, "unset": []}, "working_directory": "inherit",
                        "disposition": {"version": 1, "prepared": False, "lifecycle_events": False,
                                        "coordination_delivery": False}}, "unmanaged patch")
    check_equal(transcript.splitlines()[-1], "! adapter_exited exit_code=0", transcript)

    marker = subprocess.run([command, "two words"], cwd=working, check=True,
                            capture_output=True, encoding="utf-8").stdout.strip()
    check_equal(marker, "DESKTOOL_READY", "native command output")


if __name__ == "__main__":
    simulator, package, platform, command = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="desktool-launcher-") as temporary:
        exercise(simulator, package, platform, Path(command).resolve(), Path(temporary))
