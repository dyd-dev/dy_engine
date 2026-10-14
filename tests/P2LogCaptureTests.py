"""Focused external-consumer tests for the production LogCapture.cmake module.

Run from an MSVC development environment; the only native programs built are
two one-line consumers and a test-only monitor stub. No engine dependency is
downloaded or built. All generated files stay below --work-dir.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time


CASES = {
    "legacy-old": ("3.18", "OLD", True),
    "explicit-old": ("3.20", "OLD", True),
    "new": ("3.20", "NEW", True),
    "disabled": ("3.18", "OLD", False),
}


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def execute(folder, name, command):
    start = time.monotonic()
    result = subprocess.run(list(map(str, command)), capture_output=True, timeout=180)
    (folder / (name + ".stdout.log")).write_bytes(result.stdout)
    (folder / (name + ".stderr.log")).write_bytes(result.stderr)
    record = {"command": list(map(str, command)), "exit": result.returncode,
              "seconds": round(time.monotonic() - start, 3)}
    (folder / (name + ".json")).write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    print(name, "exit", result.returncode, flush=True)
    if result.returncode:
        print(result.stderr.decode(errors="replace")[-4000:], file=sys.stderr)
    require(result.returncode == 0, name + " failed")
    return record


def test_case(args, generator, case):
    version, policy, enabled = CASES[case]
    label = "vs" if generator.startswith("Visual Studio") else "ninja"
    folder = args.work_dir / label / case
    source = folder / "source"
    build = folder / "build"
    (source / "engine").mkdir(parents=True, exist_ok=True)
    (source / "nested").mkdir(exist_ok=True)
    (source / "main.cpp").write_text("int main() { return 0; }\n", encoding="utf-8")
    monitor_source = source / "engine" / "monitor.cpp"
    monitor_source.write_text("int main() { return 1; }\n", encoding="utf-8")
    (source / "engine" / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.20)
include([==[{(args.repo / 'cmake/LogCapture.cmake').as_posix()}]==])
if(TARGET dy_log_monitor)
    set_property(TARGET dy_log_monitor PROPERTY SOURCES "${{CMAKE_CURRENT_SOURCE_DIR}}/monitor.cpp")
    set_target_properties(dy_log_monitor PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${{CMAKE_BINARY_DIR}}/native monitor/$<CONFIG>")
endif()
''', encoding="utf-8")
    (source / "nested" / "CMakeLists.txt").write_text('''add_executable(nested_consumer "${CMAKE_CURRENT_SOURCE_DIR}/../main.cpp")
set_target_properties(nested_consumer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/runtime nested/$<CONFIG>")
''', encoding="utf-8")
    (source / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION {version})
project(P2LogCaptureConsumer LANGUAGES CXX)
cmake_policy(SET CMP0112 {policy})
set(DY_LOG_CRASH_MONITOR {'ON' if enabled else 'OFF'} CACHE BOOL "" FORCE)
add_subdirectory(engine)
add_executable(consumer main.cpp)
set_target_properties(consumer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${{CMAKE_BINARY_DIR}}/runtime root/$<CONFIG>")
add_subdirectory(nested)
cmake_policy(GET CMP0112 actual_policy)
file(WRITE "${{CMAKE_BINARY_DIR}}/consumer-policy.txt" "${{actual_policy}}")
if(DY_LOG_CRASH_MONITOR AND NOT TARGET dy_log_monitor)
    message(FATAL_ERROR "Monitor target is missing with option ON")
elseif(NOT DY_LOG_CRASH_MONITOR AND TARGET dy_log_monitor)
    message(FATAL_ERROR "Monitor target exists with option OFF")
endif()
''', encoding="utf-8")
    configure = [args.cmake, "-S", source, "-B", build, "-G", generator]
    if generator.startswith("Visual Studio"):
        configure += ["-A", "x64"]
    elif args.make_program:
        configure += ["-DCMAKE_MAKE_PROGRAM=" + str(args.make_program)]
    commands = [execute(folder, "configure", configure)]
    require((build / "consumer-policy.txt").read_text() == policy,
            "LogCapture must preserve the external consumer policy")

    def compile_targets(config, step):
        commands.append(execute(folder, config + "-" + step,
            [args.cmake, "--build", build, "--config", config,
             "--target", "consumer", "nested_consumer", "--parallel", "1"]))

    def outputs(config):
        return [build / "runtime root" / config, build / "runtime nested" / config]

    originals = {}
    for config in ["Debug", "Release"]:
        compile_targets(config, "initial")
        runtime = outputs(config)
        for directory, name in zip(runtime, ["consumer", "nested_consumer"]):
            require((directory / (name + ".exe")).is_file(), "Consumer executable missing")
        helper = build / "native monitor" / config / "windows_monitor.exe"
        if enabled:
            require(helper.is_file(), "Native monitor executable missing")
            originals[config] = digest(helper)
            for directory in runtime:
                require(digest(directory / helper.name) == originals[config],
                        "Initial monitor deployment differs from the configuration helper")
        else:
            require(not helper.exists(), "Option OFF must not build the monitor")
            require(all(not (directory / "windows_monitor.exe").exists() for directory in runtime),
                    "Option OFF must not deploy a monitor")

    if enabled:
        monitor_source.write_text("int main() { return 42; }\n", encoding="utf-8")
        for config in ["Debug", "Release"]:
            compile_targets(config, "source-only-change")
            helper = build / "native monitor" / config / "windows_monitor.exe"
            changed = digest(helper)
            require(changed != originals[config], "Monitor source change must rebuild the helper")
            for directory in outputs(config):
                deployed = directory / helper.name
                require(digest(deployed) == changed, "Source-only monitor update was not deployed")
                # Only remove our own generated deployment artifact, never a
                # production source, executable outside the fixture, or tree.
                require(deployed.resolve().is_relative_to(args.work_dir), "Deployment path escaped the fixture")
                deployed.unlink()
            compile_targets(config, "restore-deployment")
            for directory in outputs(config):
                require(digest(directory / helper.name) == changed, "Missing deployed monitor was not restored")
    return {"case": case, "policy": policy, "minimum_version": version,
            "enabled": enabled, "generator": generator, "commands": commands,
            "status": "passed"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--generator", action="append", default=[])
    parser.add_argument("--make-program", type=Path)
    parser.add_argument("--case", action="append", choices=CASES)
    args = parser.parse_args()
    args.repo = args.repo.resolve()
    args.work_dir = args.work_dir.resolve()
    args.work_dir.mkdir(parents=True, exist_ok=True)
    require(os.name == "nt", "The production monitor module requires Windows/MSVC")
    results = []
    try:
        for generator in args.generator or ["Visual Studio 17 2022"]:
            require(generator.startswith("Visual Studio") or generator == "Ninja Multi-Config",
                    "This focused runner requires Visual Studio or Ninja Multi-Config")
            for case in args.case or CASES:
                results.append(test_case(args, generator, case))
    finally:
        (args.work_dir / "results.json").write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    print("PASS:", len(results), "focused policy/option/generator cases", flush=True)


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, subprocess.TimeoutExpired) as error:
        print("FAIL:", error, file=sys.stderr)
        sys.exit(1)
