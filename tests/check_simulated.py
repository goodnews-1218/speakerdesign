"""Optional maintainer check. Build connect_lab_simulated first; never hardware evidence."""
import os
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

exe = str(Path(sys.argv[1]).resolve())
for scenario, expected in [("normal", 0), ("no_device", 1), ("no_audio", 1),
                           ("reject_start", 1), ("one_ack", 1),
                           ("wrong_format", 1), ("disconnect", 1)]:
    env = os.environ.copy()
    env["CONNECT_LAB_TEST_SCENARIO"] = scenario
    with tempfile.TemporaryDirectory(prefix="connect-lab-test-") as directory:
        result = subprocess.run([exe, "--seconds", "1", "--connect-timeout", "1",
                                 "--output", directory], capture_output=True, text=True,
                                env=env, timeout=15)
        assert result.returncode == expected, (scenario, result.stdout, result.stderr)
        assert "SIMULATED SDK" in result.stdout
        assert "CLEANUP camera=0 close=0 destroy=0" in result.stdout
        if scenario == "normal":
            files = list(Path(directory).rglob("*.wav"))
            assert len(files) == 2
            for file in files:
                with wave.open(str(file), "rb") as audio:
                    assert (audio.getframerate(), audio.getnchannels(), audio.getsampwidth()) == (16000, 1, 2)
                    data = audio.readframes(audio.getnframes())
                    assert data and all(byte == 0x12 for byte in data)
        print(scenario + ": expected result observed (simulation only)")
