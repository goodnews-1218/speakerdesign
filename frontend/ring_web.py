"""Expose one Connect ring recording session to the local dashboard."""

from datetime import datetime, timezone
import json
from os import fstat
from pathlib import Path
import re
import shutil
from threading import Event, Lock, Thread
from time import monotonic, sleep, time

from deepseek import APIError
from fall_vision import MAX_FRAMES
from ring_alert import load_event, play_alarm, run_recorder


ACTIVE_STATES = {"starting", "running", "stopping"}
SESSION_NAME = re.compile(r"session-[0-9]+")
EVENT_NAME = re.compile(r"event-([0-9]+)-([0-9]+)")
VIDEO_NAME = re.compile(r"video\.mp4|highlight-ring-[0-9]+\.mp4|segments/segment-[0-9]+\.mp4")
FRAME_NAME = re.compile(r"fall-alerts/(event-[0-9]+-[0-9]+)/frame-([0-9]{3})\.jpg")
PREVIEW = "fall-alerts/preview.jpg"
EVENT_VERDICTS = {"suspected_fall", "no_visible_fall", "uncertain", "error", "demo",
                  "already_alerted", "cancelled", "suspected_immobility", "acknowledged"}
ANALYSIS_STATES = {"queued", "requesting", "received", "failed", "skipped", "unknown"}
ALARM_STATES = {"pending", "confirming", "playing", "played", "failed", "skipped", "unknown"}
VOICE_STATES = {"prompting", "listening", "transcribing", "acknowledged", "no_reply", "unconfirmed",
                "unavailable", "stopped"}
# Keep a confirmed suspicion visible even if a later batch is skipped or inconclusive.
VERDICT_PRIORITY = ("already_alerted", "acknowledged", "cancelled", "pending", "no_visible_fall", "demo",
                    "error", "uncertain", "suspected_immobility", "suspected_fall")
PULSE_LINES = {
    "RING_BPM", "RING_DEMO_BPM", "RING_RECOVERED_BPM", "RING_HELD_BPM", "RING_LAST_BPM",
}


def validate_baseline(value):
    if not isinstance(value, str):
        raise APIError("请输入自动校准、一个心率值或一个心率范围。", 400)
    value = value.strip().lower()
    if value == "auto":
        return value
    if not re.fullmatch(r"[0-9]{1,3}(?:\.[0-9]{1,2})?(?::[0-9]{1,3}(?:\.[0-9]{1,2})?)?", value):
        raise APIError("心率基线可填写 75 或 70:80。", 400)
    numbers = [float(number) for number in value.split(":")]
    if not 35 <= numbers[0] <= numbers[-1] <= 220:
        raise APIError("心率范围须在 35 至 220 BPM 内，且下限不能大于上限。", 400)
    return value


def display_baseline(value):
    if value in ("auto", "CALIBRATING"):
        return value
    try:
        numbers = [float(number) for number in value.split("..")]
        low, high = numbers[0], numbers[-1]
        return f"{low:g}" if low == high else f"{low:g}–{high:g}"
    except ValueError:
        return None


def regular_file(root, relative):
    candidate = root.joinpath(*relative.split("/"))
    try:
        if candidate != candidate.resolve(strict=True) or not candidate.is_file():
            return None
        return candidate
    except (OSError, RuntimeError):
        return None


def media_url(session, relative):
    return f"/api/ring/media/{session.name}/{relative}"


def read_json_file(path, limit=16384):
    try:
        with path.open("rb") as handle:
            data = handle.read(limit + 1)
        result = json.loads(data) if len(data) <= limit else None
        return result if isinstance(result, dict) else None
    except (OSError, ValueError):
        return None


def event_states(result, capture, verdict, active):
    analysis = result.get("analysis_state")
    if not isinstance(analysis, str) or analysis not in ANALYSIS_STATES:
        if verdict in ("suspected_fall", "no_visible_fall"):
            analysis = "received"
        elif verdict in ("demo", "already_alerted") or capture in ("failed", "insufficient"):
            analysis = "skipped"
        else:
            analysis = "queued" if verdict == "pending" else "unknown"
    if not active and analysis == "requesting":
        analysis = "unknown"

    alarm = result.get("alarm_state")
    if not isinstance(alarm, str) or alarm not in ALARM_STATES:
        if result.get("alarm_played") is True:
            alarm = "played"
        elif result.get("alarm_error"):
            alarm = "failed"
        elif verdict in ("no_visible_fall", "uncertain", "demo", "already_alerted", "error"):
            alarm = "skipped"
        else:
            alarm = "pending" if verdict == "pending" else "unknown"
    if not active and alarm in ("pending", "confirming", "playing"):
        alarm = "unknown"
    return analysis, alarm


def list_events(session, active):
    directories = []
    alerts = session / "fall-alerts"
    try:
        for entry in alerts.iterdir():
            match = EVENT_NAME.fullmatch(entry.name)
            if match:
                directories.append((int(match[1]), int(match[2]), entry.name))
    except OSError:
        return [], 0, False, False
    events = {}
    episodes = set()
    demo = False
    analyze_demo = False
    for episode, _, event_id in sorted(directories, key=lambda item: (-item[0], item[1])):
        relative = f"fall-alerts/{event_id}"
        marker = regular_file(session, f"{relative}/ready")
        if not marker:
            continue
        episodes.add(episode)
        if episode not in events and len(events) == 24:
            continue
        try:
            job = load_event(marker)
            created_at = datetime.fromtimestamp(marker.stat().st_mtime, tz=timezone.utc).isoformat()
        except (OSError, ValueError):
            continue
        demo = demo or job["demo"]
        result_path = regular_file(session, f"{relative}/analysis.json")
        result = read_json_file(result_path) if result_path else {}
        if result is None:
            result = {"verdict": "error", "summary": "分析结果暂时无法读取。"}
        analyze_demo = analyze_demo or (job["demo"] and result.get("analyze_demo") is True)
        capture = job["status"]
        summary = result.get("summary", "")
        verdict = result.get("verdict")
        if not isinstance(verdict, str) or verdict not in EVENT_VERDICTS:
            verdict = "pending" if active else "cancelled"
            if not active:
                summary = "监测已结束，此批画面未完成分析。"
        analysis, alarm = event_states(result, capture, verdict, active)
        if capture == "failed" and not summary:
            summary = "摄像头抽帧失败，无法分析。"
        elif capture == "insufficient" and not summary:
            summary = "可用画面不足，无法分析。"
        summary = summary[:300] if isinstance(summary, str) else ""
        alarm_reason = result.get("alarm_reason")
        voice = result.get("voice_state")
        if not isinstance(voice, str) or voice not in VOICE_STATES:
            voice = None
        elif not active and voice in ("prompting", "listening", "transcribing"):
            voice = "stopped"
        states = {"verdict": verdict, "summary": summary, "capture_state": capture,
                  "analysis_state": analysis, "alarm_state": alarm, "voice_state": voice,
                  "urgent": result.get("urgent") is True and verdict == "suspected_fall" and voice == "no_reply",
                  "alarm_reason": alarm_reason if isinstance(alarm_reason, str) else None}
        event = events.setdefault(episode, {"id": f"event-{episode}", "created_at": created_at,
                                           "alarm_played": False, "frames": {}, "batches": []})
        if ("verdict" not in event or
                (states["urgent"], VERDICT_PRIORITY.index(verdict)) >=
                (event["urgent"], VERDICT_PRIORITY.index(event["verdict"]))):
            event.update(states)
        event["alarm_played"] |= alarm == "played" or verdict == "already_alerted"
        frame_count = 0
        times = job.get("times_ms")
        for index, timestamp in enumerate(times[:MAX_FRAMES] if isinstance(times, list) else []):
            frame = f"{relative}/frame-{index:03}.jpg"
            if type(timestamp) is int and regular_file(session, frame):
                frame_count += 1
                event["frames"].setdefault(timestamp, {"url": media_url(session, frame),
                                                       "time_ms": timestamp})
        event["batches"].append({"id": event_id, "created_at": created_at, "frame_count": frame_count,
                                 "alarm_played": alarm == "played", **states})
    for event in events.values():
        event["frames"] = [frame for _, frame in sorted(event["frames"].items())]
        if event["alarm_played"]:
            event.update(alarm_state="played", alarm_reason=None)
    return list(events.values()), len(episodes), demo, analyze_demo


def list_videos(session):
    found = []
    for pattern in ("video.mp4", "highlight-ring-*.mp4", "segments/segment-*.mp4"):
        paths = sorted(session.glob(pattern),
                       key=lambda path: int(re.search(r"[0-9]+$", path.stem)[0])
                       if re.search(r"[0-9]+$", path.stem) else 0, reverse=True)
        for path in paths:
            name = path.relative_to(session).as_posix()
            if VIDEO_NAME.fullmatch(name) and regular_file(session, name):
                found.append({"name": name, "url": media_url(session, name)})
            if len(found) == 40:
                return found
    return found


class RingMonitorSession:
    def __init__(self, executable, device_args, recordings, client, device_lock, stop_playback):
        self.executable = executable
        self.device_args = device_args
        self.recordings = recordings.resolve()
        self.client = client
        self.device_lock = device_lock
        self.stop_playback = stop_playback
        self.lock = Lock()
        self.closed = False
        self.shutdown = Event()
        self.cancel = None
        self.thread = None
        self.session = self.latest_recording()
        self.state = "stopped" if self.session else "idle"
        self.bpm = None
        self.baseline = None
        self.pulse_state = None
        self.warning_count = 0
        self.demo = False
        self.analyze_demo = False
        self.error = None
        self.capture_error = None
        self.result_save_errors = set()

    def latest_recording(self):
        candidates = sorted((path for path in self.recordings.glob("session-*")
                             if SESSION_NAME.fullmatch(path.name)),
                            key=lambda path: int(path.name.split("-")[1]), reverse=True)
        for path in candidates:
            alerts = path / "fall-alerts"
            if not alerts.is_dir() or alerts.is_symlink() or path.is_symlink():
                continue
            if (any(regular_file(path, f"fall-alerts/{marker.parent.name}/ready")
                    for marker in alerts.glob("event-*/ready") if EVENT_NAME.fullmatch(marker.parent.name))
                    or list_videos(path)):
                return path
        return None

    def status(self):
        with self.lock:
            session = self.session
            save_errors = self.result_save_errors.copy()
            reply = {"state": self.state, "bpm": self.bpm, "baseline": self.baseline,
                     "pulse_state": self.pulse_state, "warning_count": self.warning_count,
                     "demo": self.demo, "analyze_demo": self.analyze_demo,
                     "error": self.error, "capture_error": self.capture_error,
                     "session": session.name if session else None,
                     "preview_url": None, "events": [], "videos": []}
        if session:
            events, saved_warnings, saved_demo, saved_analyze_demo = list_events(session, reply["state"] in ACTIVE_STATES)
            reply["events"] = events
            saved_results = {batch["id"] for item in reply["events"] for batch in item["batches"]
                             if batch["verdict"] not in ("pending", "cancelled") and batch["alarm_state"] != "playing"}
            if not reply["error"] and save_errors - saved_results:
                reply["error"] = "部分预警结果未能写入录像目录。请查看启动窗口中的 RING_ALERT_RESULT。"
            reply["warning_count"] = max(reply["warning_count"], saved_warnings)
            reply["demo"] = reply["demo"] or saved_demo
            reply["analyze_demo"] = reply["analyze_demo"] or saved_analyze_demo
            reply["videos"] = list_videos(session)
            preview = regular_file(session, PREVIEW)
            if preview and reply["state"] == "running":
                try:
                    modified = preview.stat()
                    if time() - modified.st_mtime < 6:
                        reply["preview_url"] = f"/api/ring/preview/{session.name}"
                except OSError:
                    pass
        return reply

    def preview_frames(self, name):
        last_stamp = None
        last_update = monotonic()
        while monotonic() - last_update < 5:
            with self.lock:
                session = self.session
                if self.closed or self.state != "running" or not session or session.name != name:
                    return
            path = regular_file(session, PREVIEW)
            if path is not None:
                try:
                    with path.open("rb") as source:
                        stamp = fstat(source.fileno()).st_mtime_ns
                        frame = source.read(512 * 1024 + 1) if stamp != last_stamp else b""
                    if frame.startswith(b"\xff\xd8") and frame.endswith(b"\xff\xd9") and len(frame) <= 512 * 1024:
                        last_stamp = stamp
                        last_update = monotonic()
                        yield frame
                except OSError:
                    pass
            sleep(0.04)

    def start(self, payload):
        baseline = validate_baseline(payload.get("baseline", "auto"))
        demo = payload.get("demo", False)
        analyze_demo = payload.get("analyze_demo", False)
        if type(demo) is not bool:
            raise APIError("演示模式设置无效。", 400)
        if type(analyze_demo) is not bool or (analyze_demo and not demo):
            raise APIError("请开启模拟心率后再启用模拟预警分析。", 400)
        with self.lock:
            if self.closed:
                raise APIError("服务正在关闭，请重新启动。", 503)
            if self.state in ACTIVE_STATES:
                raise APIError("戒指监测正在运行或保存录像。", 409)
            if not self.executable.is_file():
                raise APIError("找不到 Connect 录像程序，请先编译 Windows Release 版。", 503)
            if not shutil.which("ffmpeg"):
                raise APIError("录像需要 FFmpeg。请确认 ffmpeg -version 能在终端运行，随后重启本地页面服务。", 503)
            if not demo or analyze_demo:
                self.client.require_key()
            if not self.device_lock.acquire(blocking=False):
                raise APIError("Connect 正在录音或拍照，请等当前操作结束后再启动监测。", 409)
            try:
                self.stop_playback()
                self.cancel = Event()
                self.session = None
                self.state, self.demo = "starting", demo
                self.analyze_demo = analyze_demo
                self.bpm, self.warning_count, self.error = None, 0, None
                self.capture_error = None
                self.result_save_errors.clear()
                self.baseline = display_baseline(baseline.replace(":", ".."))
                self.pulse_state = None
                self.thread = Thread(target=self.run, args=(baseline, demo, analyze_demo, self.cancel), daemon=True)
                self.thread.start()
            except (OSError, RuntimeError) as error:
                self.device_lock.release()
                self.state, self.error = "error", "无法启动戒指监测。"
                raise APIError(self.error, 503) from error
        return self.status()

    def run(self, baseline, demo, analyze_demo, cancel):
        options = ["--ring-baseline", baseline, "--fall-alert-preview", *self.device_args]
        if demo:
            options.append("--ring-demo")
        error = None
        status = None
        try:
            status = run_recorder(self.executable, self.recordings, options, self.client,
                                  analyze_demo=analyze_demo, cancel=cancel, shutdown=self.shutdown,
                                  on_line=self.on_line, on_root=self.on_root)
        except (OSError, RuntimeError) as failure:
            print(f"RING_WEB_ERROR={type(failure).__name__}", flush=True)
            error = "无法运行录像程序，请检查设备、录像目录和编译结果。"
        finally:
            with self.lock:
                self.device_lock.release()
                if error or (status != 0 and (not cancel.is_set() or self.session)):
                    self.state = "error"
                    self.error = error or self.error or "戒指录像未完整结束，请查看启动窗口。"
                else:
                    self.state = "stopped"
                    if cancel.is_set() and not self.session:
                        self.error = None

    def on_root(self, root):
        if SESSION_NAME.fullmatch(root.parent.name):
            with self.lock:
                self.session = root.parent
                if self.state == "starting":
                    self.state = "running"

    def on_line(self, line):
        key, _, value = line.partition("=")
        with self.lock:
            if key in PULSE_LINES:
                bpm = value.partition(" ")[0]
                if re.fullmatch(r"[0-9]{1,3}", bpm):
                    self.bpm = int(bpm)
            elif key == "RING_STATUS":
                self.pulse_state = value
                if value in ("UNAVAILABLE", "WAITING_FOR_PULSE"):
                    self.bpm = None
            elif key == "RING_BASELINE":
                self.baseline = display_baseline(value)
            elif key == "RING_HIGHLIGHT_TRIGGERED":
                self.warning_count += 1
                self.capture_error = None
            elif key == "FALL_ALERT_ERROR" and self.warning_count:
                self.capture_error = "摄像头取帧或保存失败，请检查镜头和本机录像目录。"
            elif key == "RING_ALERT_RESULT_SAVE_FAILED" and EVENT_NAME.fullmatch(value):
                self.result_save_errors.add(value)
            elif key == "RING_ERROR":
                self.pulse_state, self.bpm = "UNAVAILABLE", None
                self.error = "戒指数据读取失败，请检查 BioScope 是否正在采集。"
            elif "Unknown option" in line and "--fall-alert-preview" in line:
                self.error = "当前录像程序不支持网页预览，请重新编译 Windows Release 版。"
            elif line.startswith("LAB_ERROR:") and ("Connection timeout" in line or "OPEN_ERROR=" in line):
                self.error = "音箱连接失败。请确认 USB 已连接，并从可正常录制的 Windows PowerShell 窗口重启网页后端。"
            elif key in ("OPEN_ERROR", "CREATE_ERROR", "VIDEO_EXPORT_ERROR") or line.startswith((
                    "LAB_ERROR:", "OUTPUT_ERROR:", "VIDEO_ERROR:", "VIDEO_EXPORT_ERROR:")
            ):
                self.error = "Connect 录像出错，请检查 USB、镜头盖和录像日志。"

    def stop(self):
        with self.lock:
            if self.state in ACTIVE_STATES:
                self.state = "stopping"
                self.cancel.set()
        return self.status()

    def test_alarm(self):
        if not self.device_lock.acquire(blocking=False):
            raise APIError("请先停止监测或结束音箱录音，再试听警报。", 409)
        try:
            self.stop_playback()
            play_alarm()
        except (OSError, RuntimeError) as error:
            raise APIError("Windows 播放警报声失败，请检查默认声音输出。", 503) from error
        finally:
            self.device_lock.release()
        return {"played": True}

    def close(self):
        with self.lock:
            self.closed = True
            if self.cancel:
                self.cancel.set()
            self.shutdown.set()
            thread = self.thread
        if thread:
            thread.join(timeout=65)

    def media(self, request):
        name, _, relative = request.partition("/")
        with self.lock:
            session = self.session
        if not session or name != session.name:
            return None
        if VIDEO_NAME.fullmatch(relative):
            content_type = "video/mp4"
        elif relative == PREVIEW:
            content_type = "image/jpeg"
        else:
            match = FRAME_NAME.fullmatch(relative)
            if not match or int(match[2]) >= MAX_FRAMES or not regular_file(session, f"fall-alerts/{match[1]}/ready"):
                return None
            content_type = "image/jpeg"
        path = regular_file(session, relative)
        return (path, content_type) if path else None
