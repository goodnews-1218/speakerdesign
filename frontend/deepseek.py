"""DeepSeek requests for the local Connect service."""

import base64
import http.client
import io
import json
import os
import urllib.error
import urllib.request
import uuid
import wave


MODEL = "deepseek-flash"
API_ORIGIN = "https://api.deepseek.com"
MAX_AUDIO_BYTES = 8 * 1024 * 1024

DESCRIPTION_PROMPT = (
    "你是面向盲人和低视力用户的中文画面描述助手。用自然、简明的普通话回答，方便直接朗读。"
    "先说明主要场景，再描述人物、物体及其在照片中的相对位置，例如画面左侧、中央或右侧。"
    "按用户的问题优先描述相关细节。只说图中能看见的内容，遮挡、模糊或难以判断时明确说明。"
    "不要推测身份、病情、准确距离或路线安全，不要依据单张图片给出过街、避障或操作危险物品的指令。"
    "通常用两到四句完整的话，不用标题、Markdown 或项目符号。"
)


class APIError(Exception):
    def __init__(self, message, status=502):
        super().__init__(message)
        self.status = status


def jpeg_content(jpeg):
    image_url = "data:image/jpeg;base64," + base64.b64encode(jpeg).decode("ascii")
    return {"type": "image_url", "image_url": {"url": image_url}}


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def load_key(path):
    key = os.environ.get("DEEPSEEK_API_KEY", "").strip()
    if key or not path.is_file():
        return key
    for line in path.read_text(encoding="utf-8-sig").splitlines():
        name, separator, value = line.strip().partition("=")
        if separator and name.strip() == "DEEPSEEK_API_KEY":
            value = value.strip()
            if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
                return value[1:-1].strip()
            return value
    return ""


class DeepSeek:
    def __init__(self, key_file):
        self.key_file = key_file
        self.opener = urllib.request.build_opener(NoRedirect())
        self.validated_key = None

    def key(self):
        try:
            key = load_key(self.key_file)
        except (OSError, UnicodeError) as error:
            raise APIError("无法读取 DeepSeek 配置，请检查 deepseek.env 文件。", 503) from error
        if not key.isascii() or any(character.isspace() for character in key):
            raise APIError("DeepSeek API Key 格式不正确，请检查 deepseek.env。", 503)
        return key

    def require_key(self):
        key = self.key()
        if not key:
            raise APIError("请在 frontend/deepseek.env 填入 DEEPSEEK_API_KEY 后再试。", 503)
        return key

    def request(self, route, body, content_type, max_response, timeout=35, accept="application/json"):
        key = self.require_key()
        request = urllib.request.Request(
            API_ORIGIN + route, body,
            {"Authorization": f"Bearer {key}", "Content-Type": content_type, "Accept": accept},
            method="POST",
        )
        try:
            with self.opener.open(request, timeout=timeout) as response:
                data = response.read(max_response + 1)
                if not data or len(data) > max_response:
                    raise APIError("DeepSeek 返回的数据为空或过大。")
                if accept == "*/*" and response.headers.get_content_type() not in (
                    "audio/pcm", "audio/l16", "audio/x-pcm", "application/octet-stream",
                ):
                    raise APIError("DeepSeek 没有返回可播放的语音数据。")
                self.validated_key = key
                return data
        except urllib.error.HTTPError as error:
            messages = {
                400: "DeepSeek 无法处理这次请求，请检查输入内容后重试。",
                401: "DeepSeek API Key 无效，请检查本地配置。",
                402: "DeepSeek 账户余额不足，请到开放平台查看余额。",
                429: "DeepSeek 请求过于频繁，请稍后重试。",
            }
            if error.code in (401, 402) and self.validated_key == key:
                self.validated_key = None
            raise APIError(messages.get(error.code, f"DeepSeek 服务暂不可用（HTTP {error.code}）。")) from error
        except TimeoutError as error:
            raise APIError("DeepSeek 响应超时，请稍后重试。", 504) from error
        except (OSError, http.client.HTTPException) as error:
            raise APIError("电脑无法连接 DeepSeek，请检查网络后重试。") from error

    @staticmethod
    def json_data(data):
        try:
            result = json.loads(data)
        except (ValueError, UnicodeError) as error:
            raise APIError("DeepSeek 返回了无法读取的数据。") from error
        if not isinstance(result, dict):
            raise APIError("DeepSeek 返回的数据格式不正确。")
        return result

    def chat(self, text, jpeg=None):
        messages = [{"role": "user", "content": text}]
        if jpeg is not None:
            messages = [
                {"role": "system", "content": DESCRIPTION_PROMPT},
                {"role": "user", "content": [
                    {"type": "text", "text": text},
                    jpeg_content(jpeg),
                ]},
            ]
        return self.complete(messages, max_tokens=450 if jpeg is not None else 1200)

    def complete(self, messages, *, max_tokens, timeout=38, json_output=False):
        request = {"model": MODEL, "messages": messages, "thinking": {"type": "disabled"},
                   "max_tokens": max_tokens, "stream": False}
        if json_output:
            request["response_format"] = {"type": "json_object"}
            request["temperature"] = 0
        result = self.json_data(self.request(
            "/chat/completions", json.dumps(request, ensure_ascii=False).encode("utf-8"),
            "application/json", 128 * 1024, timeout=timeout,
        ))
        try:
            choice = result["choices"][0]
            answer = choice["message"]["content"].strip()
            finish_reason = choice.get("finish_reason")
        except (AttributeError, IndexError, KeyError, TypeError) as error:
            raise APIError("DeepSeek 没有返回可用的回答。") from error
        if (not answer or (json_output and finish_reason != "stop")
                or finish_reason in ("length", "content_filter", "insufficient_system_resource")):
            raise APIError("DeepSeek 没有返回完整回答，请重试。")
        return answer

    def transcribe(self, audio):
        if not audio or len(audio) > MAX_AUDIO_BYTES:
            raise APIError("请使用不超过 8 MiB 的 WAV 或 Ogg Opus 音频。", 400)
        if audio[:4] == b"RIFF" and audio[8:12] == b"WAVE":
            filename, media_type = "recording.wav", "audio/wav"
        elif audio[:4] == b"OggS":
            filename, media_type = "recording.ogg", "audio/ogg"
        else:
            raise APIError("DeepSeek 语音识别支持 16 位 PCM WAV 或 Ogg Opus。", 400)
        boundary = uuid.uuid4().hex
        header = (f"--{boundary}\r\nContent-Disposition: form-data; name=\"file\"; filename=\"{filename}\"\r\n"
                  f"Content-Type: {media_type}\r\n\r\n").encode("ascii")
        body = header + audio + f"\r\n--{boundary}--\r\n".encode("ascii")
        result = self.json_data(self.request(
            "/v1/tools/asr", body, f"multipart/form-data; boundary={boundary}", 128 * 1024, timeout=18,
        ))
        text = result.get("text")
        if not isinstance(text, str) or not text.strip():
            raise APIError("这次没有识别到语音，请靠近音箱再说一次。", 422)
        return text.strip()

    def synthesize(self, answer):
        body = json.dumps({"text": answer, "format": "pcm"}, ensure_ascii=False).encode("utf-8")
        audio = self.request("/v1/tools/tts", body, "application/json", MAX_AUDIO_BYTES,
                             timeout=20, accept="*/*")
        if len(audio) < 4800 or len(audio) % 2:
            raise APIError("DeepSeek 返回的语音数据无法播放。")
        output = io.BytesIO()
        with wave.open(output, "wb") as wav:
            wav.setnchannels(1)
            wav.setsampwidth(2)
            wav.setframerate(24000)
            wav.writeframes(audio)
        return output.getvalue()
