"""Install the local Windows voice models and Sherpa-ONNX runtime."""

import hashlib
import importlib.util
from pathlib import Path
import platform
import subprocess
import sys
import tarfile
import tempfile
from urllib.request import urlopen


BUILD = Path(__file__).resolve().parents[1] / "build-student"
RELEASE = "https://github.com/k2-fsa/sherpa-onnx/releases/download"
MODELS = (
    ("tts-models", "kokoro-int8-multi-lang-v1_1",
     "259c9c8bc906d6078e4e0570421885dabb68b539c2f970fcbb890a58eeaf3e9c",
     ("model.int8.onnx", "voices.bin", "tokens.txt", "lexicon-us-en.txt",
      "lexicon-zh.txt", "espeak-ng-data")),
    ("asr-models", "sherpa-onnx-sense-voice-zh-en-ja-ko-yue-int8-2024-07-17",
     "7d1efa2138a65b0b488df37f8b89e3d91a60676e416f515b952358d83dfd347e",
     ("model.int8.onnx", "tokens.txt")),
)


def install_model(tag, name, sha256, required):
    models = BUILD / "voice-models"
    models.mkdir(parents=True, exist_ok=True)
    destination = models / name
    if destination.exists():
        if all((destination / file).exists() for file in required):
            print(f"Already installed: {destination}", flush=True)
            return
        raise OSError(f"Incomplete model directory: {destination}. Move it aside and retry.")

    archive_name = f"{name}.tar.bz2"
    print(f"Downloading {archive_name}...", flush=True)
    with tempfile.TemporaryDirectory(dir=models) as staging_dir:
        staging = Path(staging_dir)
        archive = staging / archive_name
        digest = hashlib.sha256()
        with urlopen(f"{RELEASE}/{tag}/{archive_name}", timeout=45) as response, archive.open("wb") as file:
            while block := response.read(1024 * 1024):
                digest.update(block)
                file.write(block)
        if digest.hexdigest() != sha256:
            raise ValueError(f"SHA-256 mismatch for {archive_name}")
        print(f"SHA-256 verified. Extracting {archive_name}...", flush=True)
        with tarfile.open(archive, "r:bz2") as package:
            package.extractall(staging, filter="data")
        unpacked = staging / name
        if not all((unpacked / file).exists() for file in required):
            raise ValueError(f"Expected model files are missing from {archive_name}")
        unpacked.rename(destination)
    print(f"Installed: {destination}", flush=True)


def install_runtime():
    target = BUILD / "voice-deps"
    runtime = target / "sherpa_onnx"
    sys.path.insert(0, str(target))
    if ((runtime / "include/sherpa-onnx/c-api/c-api.h").is_file()
            and all((runtime / "lib" / name).is_file() for name in
                    ("sherpa-onnx-c-api.lib", "sherpa-onnx-c-api.dll", "onnxruntime.dll"))
            and importlib.util.find_spec("numpy")):
        print(f"Already installed: {target}", flush=True)
        return
    subprocess.run([sys.executable, "-m", "pip", "install", "--upgrade", "--only-binary=:all:",
                    "--target", str(target), "sherpa-onnx==1.13.8", "numpy"], check=True)


def main():
    if sys.platform != "win32" or platform.machine().lower() not in {"amd64", "x86_64"}:
        raise SystemExit("Use Windows x64 Python for the Connect voice setup.")
    if sys.version_info < (3, 12):
        raise SystemExit("Use Python 3.12 or newer.")
    try:
        for model in MODELS:
            install_model(*model)
        install_runtime()
    except (OSError, ValueError, tarfile.TarError, subprocess.CalledProcessError) as error:
        print(f"Voice setup failed: {error}", file=sys.stderr)
        return 1
    print("Voice setup complete. Build the Windows Release target before starting voice chat.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
