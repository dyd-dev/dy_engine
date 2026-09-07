"""Run the Debug LoadModel smoke and require its Vulkan validation results."""

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys


EXPECTED = {
    "VULKAN_VALIDATION_CAPTURE_ENABLED": 1,
    "VULKAN_VALIDATION_ERROR_COUNT": 0,
    "VULKAN_VALIDATION_VUID_COUNT": 0,
    "VULKAN_DEVICE_LOST": 0,
}


def validate_output(output):
    # Device destruction can report errors after LoadModel prints its counters.
    if "VUID-" in output or "Validation Error" in output:
        raise ValueError("Vulkan validation reported an error, including during shutdown")
    for name, value in EXPECTED.items():
        if re.findall(rf"^{name}=(\d+)$", output, re.MULTILINE) != [str(value)]:
            raise ValueError(f"Expected exactly one {name}={value}")


def smoke(build):
    build = build.resolve()
    paths = json.loads((build / "ci-targets-Debug.json").read_text(encoding="utf-8"))
    binary = Path(paths["LoadModel"]).resolve()
    if not binary.is_relative_to(build) or not binary.is_file():
        raise ValueError("LoadModel executable is missing or outside the build directory")
    model = binary.parent / "Models/Duck/glTF/Duck.gltf"
    if not model.is_file():
        raise ValueError("The smoke model was not deployed beside LoadModel")
    log = build / "ci-checks/Debug/LoadModel-smoke.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    try:
        with log.open("w", encoding="utf-8") as output:
            result = subprocess.run([str(binary), str(model), "--smoke-seconds=3"],
                                    cwd=binary.parent, stdout=output, stderr=subprocess.STDOUT,
                                    timeout=120)
    except subprocess.TimeoutExpired as error:
        print(log.read_text(encoding="utf-8", errors="replace"))
        raise ValueError("LoadModel smoke timed out after 120 seconds") from error
    output = log.read_text(encoding="utf-8", errors="replace")
    print(output, end="")
    if result.returncode:
        raise ValueError(f"LoadModel smoke exited {result.returncode}")
    validate_output(output)
    print("Vulkan LoadModel smoke passed: model loaded, validation active, no errors or device loss")


def self_test():
    success = "\n".join(f"{name}={value}" for name, value in EXPECTED.items()) + "\n"
    validate_output(success)
    invalid_outputs = []
    for name, value in EXPECTED.items():
        invalid_outputs.extend((success.replace(f"{name}={value}\n", ""),
                                success.replace(f"{name}={value}", f"{name}={1 - value}"),
                                success + f"{name}={value}\n"))
    invalid_outputs.extend((success + "Vulkan validation: VUID-vkDestroyDevice-device-05137\n",
                            success + "Vulkan validation: Validation Error: device cleanup failed\n"))
    for invalid in invalid_outputs:
        try:
            validate_output(invalid)
        except ValueError:
            continue
        raise AssertionError(f"Invalid smoke result accepted:\n{invalid}")
    print("Vulkan smoke log self-check passed: missing, failing, duplicate and shutdown errors rejected")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build/ci"))
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    try:
        if args.self_test:
            self_test()
        else:
            smoke(args.build_dir)
    except (ValueError, KeyError, OSError, subprocess.SubprocessError) as error:
        print(f"Vulkan smoke failed: {error}", file=sys.stderr)
        sys.exit(1)
