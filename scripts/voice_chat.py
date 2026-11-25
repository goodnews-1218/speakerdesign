"""Use the Connect microphone for turn-based DeepSeek voice chat on Windows."""

import argparse
from collections import deque
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import tomllib
from urllib.error import HTTPError, URLError
from urllib.request import HTTPRedirectHandler, Request, build_opener
import wave
import winsound


ROOT = Path(__file__).resolve().parents[1]
LOCAL_CONFIG = Path(__file__).with_name("deepseek.local.toml")
TTS_DEPENDENCIES = ROOT / "build-student" / "voice-deps"
TTS_MODEL = ROOT / "build-student" / "voice-models" / "kokoro-int8-multi-lang-v1_1" / "model.int8.onnx"
MANDARIN_SPEAKER = 3  # Kokoro v1.1-zh: zf_001.
SYSTEM_PROMPT = "你是一个中文语音助手。请用自然的口语简洁回答，默认一到三句，不使用 Markdown。"


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def api_key():
    if value := os.environ.get("DEEPSEEK_API_KEY", "").strip():
        return value
    if LOCAL_CONFIG.is_file():
        with LOCAL_CONFIG.open("rb") as file:
            value = tomllib.load(file).get("api_key", "")
        if isinstance(value, str) and value.strip():
            return value.strip()
    raise ValueError(f"请填写 {LOCAL_CONFIG} 里的 api_key")


def ask_deepseek(text, key, history, opener):
    messages = [{"role": "system", "content": SYSTEM_PROMPT}, *history,
                {"role": "user", "content": text}]
    request = Request(
        "https://api.deepseek.com/chat/completions",
        data=json.dumps({"model": "deepseek-flash", "thinking": {"type": "disabled"},
                         "messages": messages, "max_tokens": 256, "stream": False},
                        ensure_ascii=False).encode("utf-8"),
        headers={"Authorization": f"Bearer {key}", "Content-Type": "application/json"},
    )
    with opener.open(request, timeout=15) as response:
        reply = json.load(response)["choices"][0]["message"]["content"]
    if not isinstance(reply, str) or not reply.strip():
        raise ValueError("DeepSeek returned an empty reply")
    reply = reply.strip()
    history.extend((messages[-1], {"role": "assistant", "content": reply}))
    return reply


def load_voice(dependencies, model):
    sys.path.insert(0, str(dependencies.resolve()))
    import sherpa_onnx

    directory = model.parent
    config = sherpa_onnx.OfflineTtsConfig(
        model=sherpa_onnx.OfflineTtsModelConfig(
            kokoro=sherpa_onnx.OfflineTtsKokoroModelConfig(
                model=str(model),
                voices=str(directory / "voices.bin"),
                tokens=str(directory / "tokens.txt"),
                data_dir=str(directory / "espeak-ng-data"),
                lexicon=",".join(str(directory / name)
                                 for name in ("lexicon-us-en.txt", "lexicon-zh.txt")),
            ),
            num_threads=8,
        )
    )
    if not config.validate():
        raise ValueError(f"Kokoro 语音包不完整，请检查 {directory}")
    return sherpa_onnx.OfflineTts(config)


def speak(text, voice):
    import numpy as np

    generated = voice.generate(text, sid=MANDARIN_SPEAKER)
    if len(generated.samples) == 0:
        raise ValueError("Kokoro 未生成语音")
    pcm = (np.clip(generated.samples, -1, 1) * 32767).astype("<i2")
    with io.BytesIO() as audio:
        with wave.open(audio, "wb") as wav:
            wav.setparams((1, 2, generated.sample_rate, 0, "NONE", "not compressed"))
            wav.writeframes(pcm.tobytes())
        winsound.PlaySound(audio.getvalue(), winsound.SND_MEMORY | winsound.SND_NODEFAULT)


def microphone_command(executable):
    return [str(executable), "--voice-chat-input", "--mode", "mic",
            "--vid", "0x2207", "--pid", "0x0017"]


def microphone_control(process, command):
    try:
        process.stdin.write(command)
        process.stdin.flush()
    except (BrokenPipeError, OSError):
        pass


def converse(executable, key, voice):
    history = deque(maxlen=12)
    opener = build_opener(NoRedirect())
    microphone_env = os.environ.copy()
    microphone_env.pop("DEEPSEEK_API_KEY", None)
    with subprocess.Popen(
        microphone_command(executable), cwd=ROOT, env=microphone_env,
        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NEW_PROCESS_GROUP,
    ) as microphone:
        try:
            for raw in microphone.stdout:
                line = raw.decode("utf-8", errors="replace").strip()
                if line == "VOICE_CHAT_READY":
                    print("可以说话了。每句话说完稍停一下，结束按 Ctrl+C。", flush=True)
                elif line.startswith("VOICE_CHAT_TEXT="):
                    text = line.partition("=")[2].strip()
                    try:
                        if text:
                            print(f"你：{text}", flush=True)
                            reply = ask_deepseek(text, key, history, opener)
                            print(f"DeepSeek：{reply}", flush=True)
                            speak(reply, voice)
                            time.sleep(0.25)
                    except HTTPError as error:
                        print(f"DeepSeek 请求失败（HTTP {error.code}）。", file=sys.stderr, flush=True)
                    except (URLError, TimeoutError, ValueError, KeyError, IndexError, OSError,
                            RuntimeError, TypeError) as error:
                        print(f"本轮对话失败（{type(error).__name__}）。", file=sys.stderr, flush=True)
                    finally:
                        microphone_control(microphone, b"R")
                    print("可以继续说话。", flush=True)
                elif line:
                    print(line, flush=True)
            return microphone.wait()
        except KeyboardInterrupt:
            print("\n正在结束对话…", flush=True)
            microphone_control(microphone, b"Q")
            try:
                microphone.wait(timeout=10)
            except subprocess.TimeoutExpired:
                microphone.terminate()
                microphone.wait()
            return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, default=ROOT / "build-student/Release/connect_lab.exe")
    parser.add_argument("--tts-deps", type=Path, default=TTS_DEPENDENCIES)
    parser.add_argument("--tts-model", type=Path, default=TTS_MODEL)
    args = parser.parse_args()
    try:
        key = api_key()
        if not args.exe.is_file():
            raise FileNotFoundError(f"找不到 {args.exe}，请先编译 Windows Release 版")
        voice = load_voice(args.tts_deps, args.tts_model.resolve())
    except (ImportError, OSError, ValueError, RuntimeError) as error:
        print(f"无法启动语音对话：{error}", file=sys.stderr)
        return 2
    print("启动语音对话。识别后的文字将发给 DeepSeek，音频在本机处理。", flush=True)
    return converse(args.exe.resolve(), key, voice)


if __name__ == "__main__":
    raise SystemExit(main())
