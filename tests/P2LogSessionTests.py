"""Check real LogSession path selection in separate, controlled processes."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import uuid


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    binary = args.binary.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    # Inherit the workspace ACL instead of tempfile's owner-only Windows ACL.
    output = args.output.resolve() / ("paths-" + uuid.uuid4().hex[:12])
    output.mkdir()
    cases = ["unset", "empty", "home", "override", "external", "disabled"]
    if sys.platform.startswith("linux"):
        cases.append("xdg")
    results = []
    for case in cases:
        folder = output / case
        cwd, temporary = folder / "cwd", folder / "temporary"
        cwd.mkdir(parents=True)
        temporary.mkdir()
        env = os.environ.copy()
        for key in ("HOME", "XDG_STATE_HOME", "LOCALAPPDATA", "DY_LOG_DIR",
                    "DY_LOG_SESSION_ID", "DY_LOG_SESSION_DIR", "DY_LOG_AUTO",
                    "DY_LOG_CRASH", "DY_LOG_MONITOR"):
            env.pop(key, None)
        env.update(TMPDIR=str(temporary), TMP=str(temporary), TEMP=str(temporary),
                   DY_LOG_CRASH="0")
        home, explicit, external = folder / "home", folder / "explicit", folder / "external"
        default_suffix = Path("dy_engine") / "logs" / binary.stem
        expected = temporary / default_suffix
        active = True
        if case == "empty":
            env.update(HOME="", XDG_STATE_HOME="", LOCALAPPDATA="")
        elif case == "home":
            if sys.platform == "win32":
                env["LOCALAPPDATA"] = str(home)
                expected = home / default_suffix
            else:
                env["HOME"] = str(home)
                expected = home / ("Library/Logs" if sys.platform == "darwin" else ".local/state") / default_suffix
        elif case == "xdg":
            env.update(HOME=str(home), XDG_STATE_HOME=str(folder / "xdg"))
            expected = folder / "xdg" / default_suffix
        elif case == "override":
            env.update(DY_LOG_DIR=str(explicit), HOME=str(home),
                       XDG_STATE_HOME=str(folder / "xdg"), LOCALAPPDATA=str(home))
            expected = explicit
        elif case == "external":
            env.update(DY_LOG_SESSION_ID="p2-external", DY_LOG_SESSION_DIR=str(external),
                       DY_LOG_DIR=str(explicit))
            active = False
            expected = external
        elif case == "disabled":
            env.update(DY_LOG_AUTO="0", DY_LOG_DIR=str(explicit))
            active = False
        process = subprocess.run([str(binary)], cwd=cwd, env=env, capture_output=True,
                                 text=True, encoding="utf-8", timeout=20)
        (folder / "stdout.log").write_text(process.stdout, encoding="utf-8")
        (folder / "stderr.log").write_text(process.stderr, encoding="utf-8")
        failures = []
        lines = process.stdout.splitlines()
        if process.returncode != 0 or len(lines) != 2:
            failures.append("session process did not complete its state report")
        else:
            if lines[0] != ("1" if active else "0"):
                failures.append("unexpected automatic output ownership")
            actual = Path(lines[1]) if lines[1] else None
            if active:
                if actual is None or actual.parent != expected:
                    failures.append(f"expected session root {expected}, got {actual}")
                if actual is not None:
                    metadata = actual / "session.json"
                    if not metadata.is_file() or not json.loads(metadata.read_text())["shutdown_recorded"]:
                        failures.append("orderly shutdown metadata is missing")
                    log = actual / "engine-00000001.log"
                    if not log.is_file() or log.read_text() != "P2 log session path check\n":
                        failures.append("session log contents are missing")
            elif case == "external":
                if actual != expected or external.exists() or explicit.exists():
                    failures.append("external session ownership or directory changed")
            elif actual is not None or explicit.exists():
                failures.append("disabled auto logging created a session")
        results.append({"case": case, "exit": process.returncode, "failures": failures})
    report = {"platform": sys.platform, "binary": str(binary), "output": str(output),
              "results": results}
    (args.output / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    return int(any(result["failures"] for result in results))


if __name__ == "__main__":
    sys.exit(main())
