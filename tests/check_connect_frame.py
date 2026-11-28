"""Check startup exposure and dark capture with connect_frame_simulated on Windows."""

import os
from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory


def check_frame(executable, scenario, output, expected):
    env = os.environ.copy()
    env["CONNECT_LAB_TEST_SCENARIO"] = scenario
    result = subprocess.run([str(executable), "--output", str(output), "--timeout", "3"],
                            env=env, capture_output=True, timeout=10)
    assert result.returncode == expected, (result.stdout, result.stderr)
    if expected == 0:
        assert b"SNAPSHOT_SAVED=" in result.stdout
        # BMP is uncompressed. Check a pixel away from the isolated bright spot.
        bitmap = output.read_bytes()
        offset = int.from_bytes(bitmap[10:14], "little")
        assert bitmap[offset + 3:offset + 6] == bytes([48, 48, 48])
    else:
        assert b"SNAPSHOT_DARK_FRAME:" in result.stderr
        assert not output.exists()
    print(f"{scenario}: expected result observed (simulation only)")


if __name__ == "__main__":
    executable = Path(sys.argv[1]).resolve()
    with TemporaryDirectory(prefix="connect-frame-test-") as directory:
        check_frame(executable, "camera_warmup", Path(directory) / "visible.bmp", 0)
        check_frame(executable, "dark_camera", Path(directory) / "dark.bmp", 3)
