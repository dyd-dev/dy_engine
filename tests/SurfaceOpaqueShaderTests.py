"""Compile actual Surface SPIR-V and verify discard/early-test contracts."""
import argparse
import pathlib
import shutil
import struct
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("--output", required=True)
args = parser.parse_args()
output = pathlib.Path(args.output)
output.mkdir(parents=True, exist_ok=True)
source = pathlib.Path(__file__).resolve().parents[1] / "src/Extends/AdvancedRendering/Shaders/surface_ps.glsl"
for opaque, owner in ((0, 0), (1, 0), (0, 1), (1, 1)):
    binary = output / f"surface-{opaque}-owner-{owner}.spv"
    subprocess.run([shutil.which("glslangValidator"), "-V", "-S", "frag",
                    f"-DSURFACE_OPAQUE={opaque}", f"-DSURFACE_OWNER={owner}",
                    f"-DSURFACE_FULL_OUTPUTS={owner}", str(source), "-o", str(binary)], check=True)
    words = struct.unpack("<" + "I" * (binary.stat().st_size // 4), binary.read_bytes())
    instructions = []
    index = 5
    while index < len(words):
        count = words[index] >> 16
        assert count
        instructions.append(words[index:index + count])
        index += count
    kill = any((word[0] & 65535) in (252, 4416, 5380) for word in instructions)
    early = any((word[0] & 65535) == 16 and word[2] == 9 for word in instructions)
    assert kill != bool(opaque), f"opaque={opaque}: unexpected kill={kill}"
    assert early == bool(opaque), f"opaque={opaque}: unexpected EarlyFragmentTests={early}"
print("PASS Surface opaque has no kill and early tests; MASK retains kill and late coverage")
