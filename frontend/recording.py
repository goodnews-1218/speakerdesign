"""Run the existing Windows SDK recorder for one browser session at a time."""

import io
import os
import subprocess
import threading
import time
import wave
from dataclasses import dataclass, field
from pathlib import Path
from tempfile import TemporaryDirectory


class RecordingError(Exception):
    def __init__(self, message: str, status: int = 503):
        super().__init__(message)
        self.status = status


@dataclass
class RecordingSession:
    session_id: str
    startup: threading.Event = field(default_factory=threading.Event)
    finished: threading.Event = field(default_factory=threading.Event)
    stop_requested: threading.Event = field(default_factory=threading.Event)
    discard: threading.Event = field(default_factory=threading.Event)
    started: bool = False
    claimed: bool = False
    audio: bytes | None = None
    error: RecordingError | None = None
    expiry: threading.Timer | None = None


class Recorder:
    def __init__(self, executable: Path, device_args: list[str], device_lock):
        self.executable = Path(executable).resolve()
        self.device_args = tuple(device_args)
        self.device_lock = device_lock
        self._mutex = threading.Lock()
        self._session: RecordingSession | None = None
        self._closed = False

    @property
    def active(self) -> bool:
        """Track the process from launch until it has stopped using the device."""
        with self._mutex:
            return self._session is not None and not self._session.finished.is_set()

    def start(self, session_id: str) -> None:
        if not isinstance(session_id, str) or not session_id.strip() or len(session_id) > 128:
            raise RecordingError("录音会话编号无效。", 400)
        with self._mutex:
            if self._closed:
                raise RecordingError("录音服务正在关闭。")
            if self._session and not self._session.finished.is_set():
                raise RecordingError("音箱正在录音，请先结束当前录音。", 409)
            if not self.executable.is_file():
                raise RecordingError("找不到 Connect 录音程序，请先编译项目。")
            if not self.device_lock.acquire(blocking=False):
                raise RecordingError("音箱正在拍照或录音，请等当前操作结束。", 409)
            if self._session and self._session.expiry:
                self._session.expiry.cancel()
            session = RecordingSession(session_id)
            self._session = session
            try:
                threading.Thread(target=self._record, args=(session,), daemon=True).start()
            except RuntimeError as error:
                self._session = None
                self.device_lock.release()
                raise RecordingError("无法启动音箱录音。") from error

        if not session.startup.wait(21):
            session.discard.set()
            session.stop_requested.set()
            raise RecordingError("音箱录音启动超时，请检查 USB 连接。", 504)
        if session.error:
            raise session.error
        if not session.started or session.discard.is_set():
            raise RecordingError("录音已取消。", 409)

    def stop(self, session_id: str, discard: bool = False) -> bytes | None:
        with self._mutex:
            session = self._session
            if session is None or session.session_id != session_id or session.claimed:
                raise RecordingError("此会话没有可结束的录音，请重新开始。", 409)
            session.claimed = True
            if discard:
                session.discard.set()
            session.stop_requested.set()

        if not session.finished.wait(10):
            session.discard.set()
            raise RecordingError("音箱结束录音超时，请稍后重试。", 504)
        self._expire(session)
        if discard:
            return None
        if session.error:
            raise session.error
        if session.audio is None:
            raise RecordingError("录音已取消。", 409)
        return session.audio

    def close(self) -> None:
        with self._mutex:
            self._closed = True
            session = self._session
            if session:
                session.discard.set()
                session.stop_requested.set()
        if session:
            session.finished.wait(11)
            self._expire(session)

    def _expire(self, session: RecordingSession) -> None:
        with self._mutex:
            if self._session is session:
                self._session = None
            if session.expiry:
                session.expiry.cancel()

    def _record(self, session: RecordingSession) -> None:
        try:
            with TemporaryDirectory(prefix="connect-rec-") as temporary:
                directory = Path(temporary)
                self._capture(session, directory)
                if not session.discard.is_set():
                    session.audio = self._read_wav(directory)
        except RecordingError as error:
            session.error = error
        except OSError:
            session.error = RecordingError("无法运行录音程序或读取录音，请检查构建结果和临时目录权限。")
        finally:
            self.device_lock.release()
            with self._mutex:
                session.expiry = threading.Timer(25, self._expire, args=(session,))
                session.expiry.daemon = True
                session.expiry.start()
                session.finished.set()
                session.startup.set()

    def _capture(self, session: RecordingSession, directory: Path) -> None:
        if session.discard.is_set():
            return
        stop_file = directory / "stop-requested"
        log_path = directory / "capture.log"
        command = [
            str(self.executable), "--mode", "mic", "--seconds", "12",
            "--connect-timeout", "8", "--output", str(directory),
            "--stop-file", str(stop_file), *self.device_args,
        ]
        environment = os.environ.copy()
        environment.pop("DEEPSEEK_API_KEY", None)
        flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
        with log_path.open("wb") as log, subprocess.Popen(
            command, cwd=self.executable.parent, stdin=subprocess.DEVNULL,
            stdout=log, stderr=subprocess.STDOUT, env=environment, creationflags=flags,
        ) as process:
            stop_deadline = None
            natural_deadline = time.monotonic() + 33
            try:
                while process.poll() is None:
                    now = time.monotonic()
                    if session.stop_requested.is_set() or now >= natural_deadline:
                        if stop_deadline is None:
                            stop_file.touch()
                            stop_deadline = now + 8
                        elif now >= stop_deadline:
                            raise RecordingError("音箱录音程序停止响应。", 504)
                    if not session.started and "Recording active;" in self._log(log_path):
                        session.started = True
                        session.startup.set()
                    time.sleep(0.04)
            finally:
                if process.poll() is None:
                    process.kill()

        if session.discard.is_set():
            return
        capture_log = self._log(log_path)
        if process.returncode == 0 and "LAB_RESULT=PASS" in capture_log.splitlines():
            return
        if "Unknown option: --stop-file" in capture_log:
            raise RecordingError("录音程序版本过旧，请重新编译 Connect 项目。")
        if "Connection timeout" in capture_log or "OPEN_ERROR=" in capture_log:
            raise RecordingError("未能连接 Connect 音箱，请检查 USB 连接和设备设置。")
        if not session.started:
            raise RecordingError("音箱未确认开始录音，请确认设备可用且未被其他程序占用。")
        raise RecordingError("没有收到完整的麦克风音频，请检查硬件连接后重试。")

    @staticmethod
    def _log(path: Path) -> str:
        with path.open("rb") as source:
            return source.read(64 * 1024).decode("utf-8", errors="replace")

    @staticmethod
    def _read_wav(directory: Path) -> bytes:
        recordings = list(directory.glob("session-*/mic.wav"))
        if len(recordings) != 1:
            raise RecordingError("没有收到麦克风音频，请重试。")
        with recordings[0].open("rb") as source:
            audio = source.read(512 * 1024 + 1)
        if len(audio) > 512 * 1024:
            raise RecordingError("麦克风录音文件超过预期大小，请重试。")
        try:
            with wave.open(io.BytesIO(audio), "rb") as recording:
                frames = recording.getnframes()
                if (recording.getnchannels(), recording.getsampwidth(), recording.getframerate()) != (1, 2, 16000):
                    raise RecordingError("麦克风音频格式不正确。")
                if not frames or len(recording.readframes(frames)) != frames * 2:
                    raise RecordingError("麦克风录音为空或文件不完整，请重试。")
        except (wave.Error, EOFError) as error:
            raise RecordingError("无法读取麦克风录音，请重试。") from error
        return audio
