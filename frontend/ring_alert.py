"""Start Connect ring recording with DeepSeek-assisted fall alerts."""

import argparse
from array import array
from dataclasses import asdict
import io
import json
import math
import os
from pathlib import Path
from queue import Queue
import subprocess
import sys
from tempfile import TemporaryDirectory
from threading import Event, Thread
from time import sleep
import wave

from deepseek import APIError, DeepSeek
from fall_vision import MAX_FRAMES, MAX_IMAGE_BYTES, MAX_JPEG_BYTES, assess_fall

try:
    import winsound
except ImportError:
    winsound = None


FRONTEND = Path(__file__).resolve().parent
DEFAULT_EXE = FRONTEND.parent / "build-student" / "Release" / "connect_lab.exe"
ROOT_LOG = "FALL_ALERT_ROOT="


def play_alarm():
    """Play the same offline warning over the Windows default audio output."""
    if winsound is None:
        raise RuntimeError("声音警报需要 Windows 音频。")
    rate = 24000
    notes = (880, 0, 1175, 0, 880, 0, 1175, 0, 880, 0, 1175)
    pcm = array("h")
    for tone in notes:
        length = int(rate * (0.22 if tone else 0.13))
        if not tone:
            pcm.extend([0] * length)
            continue
        fade = rate * 0.015
        for index in range(length):
            gain = min(1.0, index / fade, (length - 1 - index) / fade)
            pcm.append(round(10000 * gain * math.sin(2 * math.pi * tone * index / rate)))
    with io.BytesIO() as buffer:
        with wave.open(buffer, "wb") as wav:
            wav.setparams((1, 2, rate, 0, "NONE", "not compressed"))
            wav.writeframes(pcm.tobytes())
        winsound.PlaySound(buffer.getvalue(), winsound.SND_MEMORY | winsound.SND_NODEFAULT)


def load_event(marker):
    with marker.open("rb") as handle:
        data = json.loads(handle.read(4097))
    if (not isinstance(data, dict) or data.get("version") != 1
            or type(data.get("demo")) is not bool
            or type(data.get("episode")) is not int or data["episode"] < 1
            or data.get("status") not in ("ready", "insufficient", "failed")):
        raise ValueError("抽帧任务格式无效。")
    return data


def load_frames(directory, times):
    if (not isinstance(times, list) or not 3 <= len(times) <= MAX_FRAMES
            or not all(type(value) is int for value in times)
            or times != sorted(set(times))):
        raise ValueError("缺少足够的按时序排列的画面。")
    frames = []
    remaining = MAX_IMAGE_BYTES
    for index, timestamp in enumerate(times):
        path = directory / f"frame-{index:03}.jpg"
        with path.open("rb") as handle:
            photo = handle.read(min(MAX_JPEG_BYTES, remaining) + 1)
        if len(photo) > min(MAX_JPEG_BYTES, remaining):
            raise ValueError("抽帧图片过大。")
        remaining -= len(photo)
        frames.append((timestamp, photo))
    return frames


def write_result(directory, result):
    temporary = directory / "analysis.json.tmp"
    temporary.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    for attempt in range(3):
        try:
            temporary.replace(directory / "analysis.json")
            return
        except PermissionError:
            if attempt == 2:
                raise
            sleep(0.05)


def save_result(directory, result, on_line):
    try:
        write_result(directory, result)
    except OSError:
        line = f"RING_ALERT_RESULT_SAVE_FAILED={directory.name}"
        print(line, file=sys.stderr, flush=True)
        if on_line:
            on_line(line)


def analyze_event(marker, client, notified, cancel, *, analyze_demo=False, stop_alarm=None, on_line=None):
    result = {"verdict": "pending", "summary": "", "analysis_state": "queued",
              "alarm_state": "pending", "alarm_played": False}

    def skipped(verdict, summary, reason):
        return {**result, "verdict": verdict, "summary": summary, "analysis_state": "skipped",
                "alarm_state": "skipped", "alarm_reason": reason}

    def request_started():
        if cancel.is_set():
            raise InterruptedError
        result["analysis_state"] = "requesting"
        save_result(marker.parent, result, on_line)

    def response_received():
        result["analysis_state"] = "received"
        save_result(marker.parent, result, on_line)

    try:
        event = load_event(marker)
        episode = event["episode"]
        result.update(episode=episode, batch=event.get("batch"))
        if event["demo"]:
            if not analyze_demo:
                return skipped("demo", "模拟心率，本轮图片保留在本机。", "demo")
            result.update(analyze_demo=True, alarm_state="skipped", alarm_reason="demo")
        if episode in notified:
            return skipped("already_alerted", "同一次心率预警已播放过警报声。", "already_alerted")
        if event["status"] == "failed":
            return skipped("error", "抽帧失败，请检查摄像头和录像目录。", "insufficient_frames")
        if event["status"] != "ready":
            return skipped("uncertain", "画面采样不足，请人工查看。", "insufficient_frames")
        if cancel.is_set():
            return skipped("cancelled", "分析已停止。", "stopped")
        frames = load_frames(marker.parent, event.get("times_ms"))
        assessment = assess_fall(client, frames, on_request=request_started, on_response=response_received)
    except InterruptedError:
        return skipped("cancelled", "分析已停止。", "stopped")
    except (APIError, OSError, ValueError, TypeError) as error:
        state = {"queued": "skipped", "requesting": "failed", "received": "received"}[result["analysis_state"]]
        summary = "无法读取本次预警的抽帧。" if isinstance(error, OSError) else str(error)
        return {**result, "verdict": "error", "summary": summary, "analysis_state": state,
                "alarm_state": "skipped", "alarm_reason": "analysis_failed"}

    if result["analysis_state"] == "queued":
        return skipped(assessment.verdict, assessment.summary, "insufficient_frames")
    result.update(asdict(assessment))
    if assessment.verdict != "suspected_fall":
        reason = "no_fall" if assessment.verdict == "no_visible_fall" else "uncertain"
        return {**result, "alarm_state": "skipped", "alarm_reason": reason}
    if event["demo"]:
        return result
    if cancel.is_set() or (stop_alarm is not None and stop_alarm.is_set()):
        return {**result, "alarm_state": "skipped", "alarm_reason": "stopped"}
    print(f"RING_ALERT_POSSIBLE_FALL episode={episode} 疑似摔倒或倒下，请立即查看。", flush=True)
    result["alarm_state"] = "playing"
    save_result(marker.parent, result, on_line)
    # A stop can arrive during the progress write, before playback has started.
    if cancel.is_set() or (stop_alarm is not None and stop_alarm.is_set()):
        return {**result, "alarm_state": "skipped", "alarm_reason": "stopped"}
    try:
        play_alarm()
        result.update(alarm_state="played", alarm_played=True)
        notified.add(episode)
    except (OSError, RuntimeError) as error:
        result.update(alarm_state="failed", alarm_error=str(error))
        print(f"RING_ALERT_SOUND_ERROR episode={episode} 警报播放失败，请查看现场和 Windows 音频输出。", flush=True)
    return result


def watch_events(root, client, finished, cancel, *, analyze_demo=False, stop_alarm=None, on_line=None):
    handled = set()
    notified = set()
    while not cancel.is_set():
        # A completed recording can publish its final batch between two directory reads.
        recording_finished = finished.is_set()
        try:
            markers = sorted(
                (marker for marker in root.glob("event-*/ready") if marker not in handled),
                key=lambda marker: marker.stat().st_mtime_ns,
            )
        except OSError:
            if recording_finished:
                return
            cancel.wait(0.25)
            continue
        for marker in markers:
            if cancel.is_set():
                return
            handled.add(marker)
            result = analyze_event(marker, client, notified, cancel, analyze_demo=analyze_demo,
                                   stop_alarm=stop_alarm, on_line=on_line)
            verdict = result["verdict"]
            print(f"RING_ALERT_RESULT={verdict} event={marker.parent.name} alarm={result['alarm_played']} "
                  f"analysis={result['analysis_state']} alarm_state={result['alarm_state']} "
                  f"reason={result.get('alarm_reason', '-')}", flush=True)
            if verdict in ("uncertain", "error"):
                print(f"需要人工查看。{result['summary']}", flush=True)
            save_result(marker.parent, result, on_line)
        if recording_finished and not markers:
            return
        cancel.wait(0.25)


def relay_output(stream, output, roots, on_line=None, on_root=None):
    root_found = False
    try:
        for raw in stream:
            line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
            print(line, flush=True)
            if on_line:
                on_line(line)
            if line.startswith(ROOT_LOG) and not root_found:
                root = Path(line[len(ROOT_LOG):]).resolve()
                if root.name != "fall-alerts" or not root.parent.name.startswith("session-") or root.parent.parent != output:
                    print("RING_ALERT_ERROR=录像程序返回的事件目录无效。", file=sys.stderr, flush=True)
                    continue
                roots.put(root)
                root_found = True
                if on_root:
                    on_root(root)
    finally:
        if not root_found:
            roots.put(None)


def run_recorder(executable, output, options, client, *, analyze_demo=False, cancel=None, shutdown=None,
                 on_line=None, on_root=None):
    finished = Event()
    controlled = cancel is not None
    if cancel is None:
        cancel = Event()
    if shutdown is None:
        shutdown = Event()
    roots = Queue()

    def monitor():
        root = roots.get()
        if root is not None:
            watch_events(root, client, finished, shutdown, analyze_demo=analyze_demo,
                         stop_alarm=cancel, on_line=on_line)

    with TemporaryDirectory(prefix="connect-ring-alert-") as directory:
        stop_file = Path(directory) / "stop"
        command = [str(executable), "--mode", "mic", "--video", "--ring", "--ring-auto-highlight",
                   "--fall-alert", "--output", str(output), "--stop-file", str(stop_file), *options]
        if "--seconds" not in options and "--continuous" not in options:
            command.append("--continuous")
        env = os.environ.copy()
        env.pop("DEEPSEEK_API_KEY", None)
        with subprocess.Popen(command, stdin=subprocess.DEVNULL if controlled else None, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, env=env,
                              creationflags=getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)) as recorder:
            reader = Thread(target=relay_output, args=(recorder.stdout, output, roots, on_line, on_root), daemon=True)
            worker = Thread(target=monitor, daemon=True)
            reader.start()
            worker.start()

            def save_and_stop(graceful=False):
                stop_file.touch(exist_ok=True)
                print("正在停止录制并保存片段…", flush=True)
                try:
                    # While the service stays up, allow all native video exports to finish.
                    if graceful:
                        while recorder.poll() is None and not shutdown.wait(0.2):
                            pass
                    return recorder.wait(timeout=60)
                except subprocess.TimeoutExpired:
                    recorder.terminate()
                    recorder.wait()
                    print("录制程序停止超时，最后一段文件可能未完成。", file=sys.stderr, flush=True)
                    return 1

            try:
                if controlled:
                    while recorder.poll() is None and not cancel.wait(0.2):
                        pass
                    status = recorder.returncode if recorder.returncode is not None else save_and_stop(graceful=True)
                else:
                    status = recorder.wait()
            except KeyboardInterrupt:
                cancel.set()
                shutdown.set()
                status = save_and_stop()
            finally:
                reader.join()
                finished.set()
            try:
                while worker.is_alive() and not shutdown.is_set():
                    worker.join(timeout=0.2)
                if worker.is_alive():
                    worker.join(timeout=1)
            except KeyboardInterrupt:
                cancel.set()
                shutdown.set()
            return status


def main():
    parser = argparse.ArgumentParser(
        description="录制戒指心率和音箱视频，在心率预警时抽帧并播报疑似跌倒。",
        epilog="可附加录制参数，例如 --ring-baseline 75 或 --seconds 40。"
    )
    parser.add_argument("--exe", type=Path, default=DEFAULT_EXE, help="音箱录制程序")
    parser.add_argument("--output", type=Path, default=Path("recordings"), help="本地录像输出目录")
    parser.add_argument("--key-file", type=Path, default=FRONTEND / "deepseek.env", help="DeepSeek 本机配置文件")
    parser.add_argument("--test-alarm", action="store_true", help="仅试听 Windows 输出警报")
    args, options = parser.parse_known_args()
    try:
        if args.test_alarm:
            play_alarm()
            return 0
        if "--stop-file" in options or "--ring-monitor" in options:
            parser.error("录像启动器负责停止和捕获画面，请去掉 --stop-file 或 --ring-monitor。")
        executable = args.exe.resolve()
        if not executable.is_file():
            raise FileNotFoundError(f"找不到 {executable}，请先编译 Windows Release 版。")
        client = DeepSeek(args.key_file.resolve())
        if "--ring-demo" not in options:
            client.require_key()
            print("心率预警时会将视频抽出的 JPEG 帧发给 DeepSeek。警报通过 Windows 默认输出播放。", flush=True)
        else:
            print("模拟心率只生成本机抽帧，不调用 DeepSeek。", flush=True)
        return run_recorder(executable, args.output.resolve(), options, client)
    except (APIError, OSError, RuntimeError) as error:
        print(f"RING_ALERT_ERROR={error}", file=sys.stderr, flush=True)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
