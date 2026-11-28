"""Serve Connect Studio and its hardware and DeepSeek APIs on loopback."""

import argparse
import base64
import binascii
from collections import OrderedDict
from contextlib import contextmanager
from email import policy
from email.parser import BytesParser
from http.client import HTTPConnection, HTTPException
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import re
import subprocess
from tempfile import TemporaryDirectory
from threading import Lock
from time import monotonic
from urllib.parse import urlsplit
import uuid

from deepseek import APIError, DeepSeek, MAX_AUDIO_BYTES, MODEL
from recording import Recorder, RecordingError
from ring_web import RingMonitorSession

try:
    import winsound
except ImportError:
    winsound = None


FRONTEND = Path(__file__).resolve().parent
RING_INTEGRATION_PAGE = FRONTEND.parent / "ring-integration" / "戒指音箱联动测试.html"
RING_BRIDGE_PORT = 8765
DEFAULT_BIN = FRONTEND.parent / "build-student" / "Release"
MAX_JPEG = 12 * 1024 * 1024
DEFAULT_COMMAND = "描述一下你看到的画面"


class MediaCache:
    def __init__(self):
        self.lock = Lock()
        self.entries = OrderedDict()

    def prune(self):
        while self.entries and next(iter(self.entries.values()))[0] <= monotonic():
            self.entries.popitem(last=False)

    def put(self, suffix, content_type, body):
        name = uuid.uuid4().hex + suffix
        with self.lock:
            self.prune()
            while len(self.entries) >= 8:
                self.entries.popitem(last=False)
            self.entries[name] = (monotonic() + 300, content_type, body)
        return "/api/media/" + name

    def get(self, name):
        with self.lock:
            self.prune()
            entry = self.entries.get(name)
            return entry[1:] if entry else None


class Playback:
    def __init__(self):
        self.lock = Lock()
        self.generation = 0
        self.safety_generation = 0
        self.safety_active = False
        self.directory = TemporaryDirectory(prefix="connect-playback-") if winsound else None

    def stop(self):
        with self.lock:
            return self._stop()

    def _stop(self):
        self.generation += 1
        if self.directory and not self.safety_active:
            target = Path(self.directory.name) / "response.wav"
            if target.exists():
                try:
                    winsound.PlaySound(None, 0)
                except (OSError, RuntimeError):
                    pass
                try:
                    target.unlink(missing_ok=True)
                except OSError:
                    pass
        return self.generation

    @contextmanager
    def safety_audio(self):
        with self.lock:
            self.safety_generation = self._stop()
            self.safety_active = True
        try:
            yield
        finally:
            with self.lock:
                self.safety_active = False

    def safety_interrupted(self, generation):
        with self.lock:
            return self.safety_active or generation < self.safety_generation

    def play(self, wav, generation):
        with self.lock:
            if not winsound or self.safety_active or generation != self.generation:
                return False
            target = Path(self.directory.name) / "response.wav"
            target.write_bytes(wav)
            winsound.PlaySound(str(target), winsound.SND_FILENAME | winsound.SND_ASYNC | winsound.SND_NODEFAULT)
            return True

    def close(self):
        self.stop()
        if self.directory:
            self.directory.cleanup()


class ConnectStudio:
    def __init__(self, bin_dir, device_args, key_file):
        self.frame_exe = bin_dir / "connect_frame.exe"
        self.device_args = device_args
        self.device_lock = Lock()
        self.describe_lock = Lock()
        self.ring_measure_lock = Lock()
        self.device_connected = None
        self.recorder = Recorder(bin_dir / "connect_lab.exe", device_args, self.device_lock)
        self.deepseek = DeepSeek(key_file)
        self.media = MediaCache()
        self.playback = Playback()
        self.ring = RingMonitorSession(bin_dir / "connect_lab.exe", device_args,
                                       FRONTEND.parent / "recordings", self.deepseek,
                                       self.device_lock, self.playback.stop,
                                       priority_audio=self.playback.safety_audio)

    def status(self):
        key = self.deepseek.key()
        return {
            "device": {"connected": self.device_connected is True, "checked": self.device_connected is not None,
                       "name": "Connect 音箱"},
            "model": {"connected": bool(key and key == self.deepseek.validated_key), "configured": bool(key),
                      "name": MODEL},
        }

    def capture(self):
        if not self.frame_exe.is_file():
            raise APIError("请先编译 connect_frame.exe，或开启电脑摄像头后再试。", 503)
        if not self.device_lock.acquire(blocking=False):
            raise APIError("音箱正在录音或拍照，请等它结束再试。", 409)
        try:
            with TemporaryDirectory(prefix="connect-picture-") as directory:
                photo = Path(directory) / "scene.jpg"
                process = subprocess.run(
                    [str(self.frame_exe), "--output", str(photo), "--timeout", "8", *self.device_args],
                    cwd=self.frame_exe.parent, capture_output=True, timeout=20, check=False,
                    creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
                )
                if process.returncode or not photo.is_file():
                    self.device_connected = False
                    if process.stderr:
                        print(process.stderr.decode("utf-8", errors="replace").strip()[:1200], flush=True)
                    raise APIError("没有收到音箱画面。请检查 USB、镜头盖和摄像头是否被占用。", 503)
                self.device_connected = True
                if photo.stat().st_size > MAX_JPEG:
                    raise APIError("音箱拍摄的照片过大。", 502)
                return check_jpeg(photo.read_bytes(), 502)
        except subprocess.TimeoutExpired as error:
            self.device_connected = False
            raise APIError("等待音箱拍照超时，请检查 USB 后重试。", 504) from error
        except OSError as error:
            self.device_connected = False
            raise APIError("无法启动音箱拍照程序，请检查编译结果和依赖文件。", 503) from error
        finally:
            self.device_lock.release()

    def measure_ring(self):
        if not self.ring_measure_lock.acquire(blocking=False):
            raise APIError("戒指正在测量，请等待当前测量结束。", 409)
        bridge = HTTPConnection("127.0.0.1", RING_BRIDGE_PORT, timeout=65)
        try:
            bridge.request("POST", "/measure", body=b"", headers={
                "Origin": f"http://127.0.0.1:{RING_BRIDGE_PORT}", "X-Ring-Measure": "1",
            })
            response = bridge.getresponse()
            body = response.read(4097)
            if len(body) > 4096:
                raise APIError("戒指测量服务返回的数据过大。", 502)
            if response.status != 200:
                raise APIError(body.decode("utf-8", errors="replace").strip()[:300] or "戒指测量失败。", 503)
            result = json.loads(body)
            fields = ("heart", "oxygen", "temperature")
            if not isinstance(result, dict) or any(
                field not in result or (result[field] is not None and type(result[field]) not in (int, float))
                for field in fields
            ):
                raise APIError("戒指测量服务返回的格式不正确。", 502)
            return {field: result[field] for field in fields}
        except TimeoutError as error:
            raise APIError("等待戒指测量超时，请检查佩戴和蓝牙连接。", 504) from error
        except (OSError, HTTPException) as error:
            raise APIError("无法连接 Windows 戒指测量服务，请先运行 RingBridge.exe。", 503) from error
        except (ValueError, UnicodeError) as error:
            raise APIError("戒指测量服务返回了无法读取的数据。", 502) from error
        finally:
            bridge.close()
            self.ring_measure_lock.release()

    def describe(self, payload):
        self.deepseek.require_key()
        command = question(payload, fallback=DEFAULT_COMMAND)
        if not self.describe_lock.acquire(blocking=False):
            raise APIError("上一张照片还在处理，请稍后再试。", 409)
        try:
            playback_generation = self.playback.stop()
            browser_image = "image" in payload
            monitor_frame = None if browser_image else self.ring.current_camera_frame()
            if browser_image:
                photo = decode_browser_photo(payload["image"])
            elif monitor_frame is not None:
                photo = check_jpeg(monitor_frame, 502)
                self.device_connected = True
            else:
                photo = self.capture()
            answer = self.deepseek.chat(command, photo)
            result = {
                "answer": answer, "model": MODEL, "audio_url": None, "played": False,
                "image_source": "browser" if browser_image else "connect",
                "image_url": self.media.put(".jpg", "image/jpeg", photo),
            }
            try:
                wav = self.deepseek.synthesize(answer)
                result["audio_url"] = self.media.put(".wav", "audio/wav", wav)
                result["audio_duration_ms"] = round((len(wav) - 44) / 48)
                if monitor_frame is not None:
                    result["played"] = self.playback.play(wav, playback_generation)
                elif self.device_lock.acquire(blocking=False):
                    try:
                        result["played"] = self.playback.play(wav, playback_generation)
                    finally:
                        self.device_lock.release()
            except APIError as error:
                result["audio_error"] = str(error)
            except (OSError, RuntimeError):
                result["audio_error"] = "Windows 未能播放，可以在页面上重播回答。"
            if self.playback.safety_interrupted(playback_generation):
                result.update(played=False, audio_deferred=True)
            return result
        finally:
            self.describe_lock.release()

    def close(self):
        try:
            self.ring.close()
        finally:
            try:
                self.recorder.close()
            finally:
                self.playback.close()


def check_jpeg(data, status=400):
    if not data.startswith(b"\xff\xd8\xff") or not data.endswith(b"\xff\xd9"):
        raise APIError("没有获取到完整 JPEG 照片，请重新拍照。", status)
    return data


def decode_browser_photo(value):
    if not isinstance(value, str) or not value.startswith("data:image/jpeg;base64,"):
        raise APIError("浏览器照片必须使用 JPEG 格式。", 400)
    try:
        data = base64.b64decode(value.partition(",")[2], validate=True)
    except (ValueError, binascii.Error) as error:
        raise APIError("无法读取浏览器拍摄的照片。", 400) from error
    if len(data) > MAX_JPEG:
        raise APIError("照片过大，请重新拍照。", 413)
    return check_jpeg(data)


def question(payload, fallback=""):
    value = payload.get("text", fallback)
    if not isinstance(value, str) or not value.strip() or len(value) > 12000:
        raise APIError("请输入有效的问题，最长 12000 字。", 400)
    return value.strip()


def session_id(payload):
    value = payload.get("session_id")
    if not isinstance(value, str) or not re.fullmatch(r"[a-zA-Z0-9_-]{1,100}", value):
        raise APIError("录音会话标识无效，请开启新对话再试。", 400)
    return value


def requested_range(value, size):
    if value is None:
        return 0, size
    match = re.fullmatch(r"bytes=([0-9]{0,19})-([0-9]{0,19})", value)
    if not match or not size or not (match[1] or match[2]):
        raise ValueError("Invalid range")
    if not match[1]:
        start, end = max(0, size - int(match[2])), size
    else:
        start = int(match[1])
        end = min(size, int(match[2]) + 1) if match[2] else size
    if start >= end:
        raise ValueError("Invalid range")
    return start, end


class FrontendHandler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(FRONTEND / "dist"), **kwargs)

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        super().end_headers()

    def allowed_host(self):
        port = self.server.server_port
        return self.headers.get("Host") in (f"127.0.0.1:{port}", f"localhost:{port}")

    def reply(self, body, content_type, status=200, send_body=True):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if send_body:
            try:
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                pass

    def json_reply(self, payload, status=200, send_body=True):
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.reply(body, "application/json; charset=utf-8", status, send_body)

    def stream_ring_media(self, path, content_type, send_body):
        try:
            file = path.open("rb")
        except OSError:
            self.json_reply({"error": "录像或图像暂时无法读取。"}, 404, send_body)
            return
        with file:
            size = os.fstat(file.fileno()).st_size
            range_header = self.headers.get("Range")
            try:
                start, end = requested_range(range_header, size)
            except ValueError:
                self.send_response(416)
                self.send_header("Content-Range", f"bytes */{size}")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            self.send_response(206 if range_header else 200)
            self.send_header("Content-Type", content_type)
            self.send_header("Cross-Origin-Resource-Policy", "same-origin")
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Length", str(end - start))
            if range_header:
                self.send_header("Content-Range", f"bytes {start}-{end - 1}/{size}")
            self.end_headers()
            if send_body:
                try:
                    file.seek(start)
                    remaining = end - start
                    while remaining:
                        data = file.read(min(remaining, 256 * 1024))
                        if not data:
                            break
                        self.wfile.write(data)
                        remaining -= len(data)
                except OSError:
                    pass

    def stream_ring_preview(self, name, send_body):
        frames = self.server.app.ring.preview_frames(name)
        first = next(frames, None)
        if first is None:
            self.json_reply({"error": "当前没有实时画面。"}, 404, send_body)
            return
        self.close_connection = True
        self.connection.settimeout(3)
        self.send_response(200)
        self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=connect-frame")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        self.send_header("Connection", "close")
        self.end_headers()
        if not send_body:
            return
        try:
            frame = first
            while frame is not None:
                header = f"--connect-frame\r\nContent-Type: image/jpeg\r\nContent-Length: {len(frame)}\r\n\r\n"
                self.wfile.write(header.encode("ascii"))
                self.wfile.write(frame)
                self.wfile.write(b"\r\n")
                frame = next(frames, None)
            self.wfile.write(b"--connect-frame--\r\n")
        except OSError:
            pass

    def read_body(self, maximum):
        try:
            count = int(self.headers.get("Content-Length", "0"))
        except ValueError as error:
            raise APIError("请求长度无效。", 400) from error
        if not 0 < count <= maximum:
            raise APIError("请求为空或文件过大。", 413 if count > maximum else 400)
        data = self.rfile.read(count)
        if len(data) != count:
            raise APIError("没有收到完整请求。", 400)
        return data

    def read_json(self, maximum=64 * 1024):
        if self.headers.get_content_type() != "application/json":
            raise APIError("请求需要使用 application/json 格式。", 415)
        try:
            value = json.loads(self.read_body(maximum))
        except (ValueError, UnicodeError) as error:
            raise APIError("无法读取请求内容。", 400) from error
        if not isinstance(value, dict):
            raise APIError("请求内容格式不正确。", 400)
        return value

    def read_audio(self):
        if self.headers.get_content_type() != "multipart/form-data":
            raise APIError("请上传音频文件。", 415)
        content_type = str(self.headers["Content-Type"])
        if not content_type.isascii() or "\n" in content_type or "\r" in content_type:
            raise APIError("音频文件请求格式不正确。", 400)
        headers = f"Content-Type: {content_type}\r\nMIME-Version: 1.0\r\n\r\n".encode("ascii")
        message = BytesParser(policy=policy.default).parsebytes(headers + self.read_body(MAX_AUDIO_BYTES + 16384))
        for part in message.iter_parts():
            if part.get_param("name", header="content-disposition") == "audio":
                return part.get_payload(decode=True)
        raise APIError("没有收到音频文件。", 400)

    def get_api(self, path, send_body=True):
        if not self.allowed_host():
            self.json_reply({"error": "请通过本机地址访问服务。"}, 403, send_body)
            return
        app = self.server.app
        try:
            if path == "/api/status":
                self.json_reply(app.status(), send_body=send_body)
                return
            if path == "/api/ring/status":
                self.json_reply(app.ring.status(), send_body=send_body)
                return
            if path == "/api/ring/history" or path.startswith("/api/ring/history/"):
                cursor = path.removeprefix("/api/ring/history").removeprefix("/")
                if cursor and (not cursor.isascii() or not cursor.isdecimal() or len(cursor) > 20):
                    raise APIError("预警历史游标无效。", 400)
                self.json_reply(app.ring.history(int(cursor) if cursor else None), send_body=send_body)
                return
            if path.startswith("/api/ring/preview/"):
                self.stream_ring_preview(path.removeprefix("/api/ring/preview/"), send_body)
                return
            if path.startswith("/api/ring/media/"):
                entry = app.ring.media(path.removeprefix("/api/ring/media/"))
                if entry:
                    self.stream_ring_media(*entry, send_body)
                    return
            if path.startswith("/api/media/"):
                entry = app.media.get(path.removeprefix("/api/media/"))
                if entry:
                    content_type, body = entry
                    self.reply(body, content_type, send_body=send_body)
                    return
            self.json_reply({"error": "接口或临时媒体已失效。"}, 404, send_body)
        except APIError as error:
            self.json_reply({"error": str(error)}, error.status, send_body)

    def do_GET(self):
        path = urlsplit(self.path).path
        if path == "/ring-integration/":
            self.ring_integration_page()
        elif path.startswith("/api/"):
            self.get_api(path)
        else:
            super().do_GET()

    def do_HEAD(self):
        path = urlsplit(self.path).path
        if path == "/ring-integration/":
            self.ring_integration_page(send_body=False)
        elif path.startswith("/api/"):
            self.get_api(path, send_body=False)
        else:
            super().do_HEAD()

    def ring_integration_page(self, send_body=True):
        if not self.allowed_host():
            self.send_error(403)
            return
        try:
            page = RING_INTEGRATION_PAGE.read_bytes()
        except OSError:
            self.send_error(404)
            return
        self.reply(page, "text/html; charset=utf-8", send_body=send_body)

    def do_POST(self):
        origin = self.headers.get("Origin")
        if not self.allowed_host() or (origin and origin != "http://" + self.headers["Host"]):
            self.json_reply({"error": "请从本机 Connect Studio 页面发起操作。"}, 403)
            return
        try:
            self.json_reply(self.post_api(urlsplit(self.path).path))
        except (APIError, RecordingError) as error:
            self.json_reply({"error": str(error)}, error.status)
        except (OSError, RuntimeError) as error:
            print("本机服务错误：", type(error).__name__, flush=True)
            self.json_reply({"error": "电脑端处理失败，请重新尝试。"}, 500)

    def post_api(self, path):
        app = self.server.app
        if path == "/api/ring/start":
            return app.ring.start(self.read_json(4096))
        if path == "/api/ring/stop":
            self.read_json(4096)
            return app.ring.stop()
        if path == "/api/ring/test-alarm":
            self.read_json(4096)
            return app.ring.test_alarm()
        if path == "/api/ring/measure":
            self.read_json(4096)
            return app.measure_ring()
        if path == "/api/describe":
            return app.describe(self.read_json(17 * 1024 * 1024))
        if path == "/api/chat":
            return {"answer": app.deepseek.chat(question(self.read_json())), "model": MODEL}
        if path == "/api/transcribe":
            return {"text": app.deepseek.transcribe(self.read_audio())}
        if path == "/api/playback/stop":
            self.read_json()
            app.playback.stop()
            return {"playing": False}
        if path == "/api/recording/start":
            app.deepseek.require_key()
            session = session_id(self.read_json())
            app.playback.stop()
            try:
                app.recorder.start(session)
            except RecordingError as error:
                if error.status != 409:
                    app.device_connected = False
                raise
            app.device_connected = True
            return {"recording": True}
        if path == "/api/recording/stop":
            payload = self.read_json()
            discard = payload.get("discard") is True
            audio = app.recorder.stop(session_id(payload), discard=discard)
            transcript = "" if discard else app.deepseek.transcribe(audio)
            return {"recording": False, "transcript": transcript}
        raise APIError("接口不存在。", 404)


def device_id(value):
    try:
        number = int(value, 0)
    except ValueError as error:
        raise argparse.ArgumentTypeError("请输入有效的 VID 或 PID。") from error
    if not 1 <= number <= 0xFFFF:
        raise argparse.ArgumentTypeError("VID 和 PID 必须在 1 到 65535 之间。")
    return number


def main():
    parser = argparse.ArgumentParser(description="Start the Connect Studio local service.")
    parser.add_argument("--port", type=int, default=4173)
    parser.add_argument("--open", action="store_true", help="Open the local page in your default browser")
    parser.add_argument("--bin-dir", type=Path, default=DEFAULT_BIN, help="Directory with both native executables")
    parser.add_argument("--vid", type=device_id, help="Override the Connect USB vendor ID")
    parser.add_argument("--pid", type=device_id, help="Override the Connect USB product ID")
    args = parser.parse_args()
    if (args.vid is None) != (args.pid is None):
        parser.error("--vid and --pid must be set together")
    device_args = [] if args.vid is None else ["--vid", hex(args.vid), "--pid", hex(args.pid)]
    with ThreadingHTTPServer(("127.0.0.1", args.port), FrontendHandler) as server:
        server.app = ConnectStudio(args.bin_dir.resolve(), device_args, FRONTEND / "deepseek.env")
        url = f"http://127.0.0.1:{server.server_port}"
        print(f"Connect Studio: {url}", flush=True)
        try:
            if args.open:
                import webbrowser
                webbrowser.open(url)
            server.serve_forever()
        except KeyboardInterrupt:
            pass
        finally:
            server.app.close()


if __name__ == "__main__":
    main()
