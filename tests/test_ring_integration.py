from io import BytesIO
import json
from pathlib import Path
import sys
from tempfile import TemporaryDirectory
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "frontend"))

from deepseek import APIError
from serve import ConnectStudio, FrontendHandler


class RingIntegrationTests(unittest.TestCase):
    def setUp(self):
        directory = TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.sound = Mock(SND_FILENAME=1, SND_ASYNC=2, SND_NODEFAULT=4)
        audio = patch("serve.winsound", self.sound)
        audio.start()
        self.addCleanup(audio.stop)
        self.app = ConnectStudio(Path(directory.name), [], Path(directory.name) / "key.env")
        self.addCleanup(self.app.close)
        self.app.deepseek = Mock()
        self.app.deepseek.chat.return_value = "桌面上有一个杯子。"
        self.app.deepseek.synthesize.return_value = b"W" * 92

    def test_description_reuses_monitor_frame_while_recorder_owns_camera(self):
        frame = b"\xff\xd8\xff\xff\xd9"
        with self.app.device_lock, patch.object(self.app.ring, "current_camera_frame", return_value=frame):
            result = self.app.describe({})
        self.app.deepseek.chat.assert_called_once_with("描述一下你看到的画面", frame)
        self.assertEqual(result["image_source"], "connect")
        self.assertTrue(result["played"])

    @patch("serve.subprocess.run")
    def test_dark_native_frame_reports_camera_connected_and_skips_deepseek(self, run):
        self.app.frame_exe.touch()
        run.return_value = SimpleNamespace(returncode=3, stderr=b"SNAPSHOT_DARK_FRAME: No usable light")
        with self.assertRaisesRegex(APIError, "画面太暗"):
            self.app.describe({})
        self.assertTrue(self.app.device_connected)
        self.app.deepseek.chat.assert_not_called()
        self.app.deepseek.synthesize.assert_not_called()

    def test_alert_reserves_audio_and_interrupts_pending_description(self):
        playback = self.app.playback
        generation = playback.stop()
        self.assertTrue(playback.play(b"old", generation))
        with playback.safety_audio():
            playback.stop()
            self.assertFalse(playback.play(b"new", playback.generation))
            self.assertTrue(playback.safety_interrupted(playback.generation))
        self.sound.PlaySound.assert_called_with(None, 0)
        self.assertEqual(self.sound.PlaySound.call_count, 2)
        self.assertTrue(playback.safety_interrupted(generation))
        self.assertFalse(playback.safety_interrupted(playback.stop()))

    @patch("serve.HTTPConnection")
    def test_measurement_only_requests_the_local_ring_bridge(self, connection):
        bridge = connection.return_value
        response = bridge.getresponse.return_value
        response.status = 200
        response.read.return_value = json.dumps({"heart": 73, "oxygen": 98, "temperature": 35.41}).encode()
        self.assertEqual(self.app.measure_ring(), {"heart": 73, "oxygen": 98, "temperature": 35.41})
        connection.assert_called_once_with("127.0.0.1", 8765, timeout=65)
        bridge.request.assert_called_once_with("POST", "/measure", body=b"", headers={
            "Origin": "http://127.0.0.1:8765", "X-Ring-Measure": "1",
        })
        with self.app.ring_measure_lock, self.assertRaises(APIError) as raised:
            self.app.measure_ring()
        self.assertEqual(raised.exception.status, 409)

    def http(self, request):
        socket = Mock()
        socket.makefile.return_value = BytesIO(request)
        server = SimpleNamespace(server_port=4173, app=self.app)
        with patch.object(FrontendHandler, "log_message"):
            FrontendHandler(socket, ("127.0.0.1", 12345), server)
        response = b"".join(args[0] for args, _ in socket.sendall.call_args_list)
        return response.split(b"\r\n\r\n", 1)

    def test_frontend_serves_the_ring_test_page(self):
        request = b" /ring-integration/ HTTP/1.1\r\nHost: 127.0.0.1:4173\r\n\r\n"
        headers, body = self.http(b"GET" + request)
        self.assertIn(b"200 OK", headers)
        self.assertIn(b"/api/describe", body)
        self.assertIn(b"/api/ring/measure", body)
        self.assertEqual(self.http(b"HEAD" + request)[1], b"")

    def test_frontend_measurement_requires_the_same_origin(self):
        request = (b"POST /api/ring/measure HTTP/1.1\r\nHost: 127.0.0.1:4173\r\n"
                   b"Content-Type: application/json\r\nContent-Length: 2\r\nOrigin: http://%s\r\n\r\n{}")
        with patch.object(self.app, "measure_ring", return_value={"heart": 73}) as measure:
            self.assertIn(b"403", self.http(request % b"127.0.0.1:8765")[0])
            measure.assert_not_called()
            headers, body = self.http(request % b"127.0.0.1:4173")
            self.assertIn(b"200 OK", headers)
            self.assertEqual(json.loads(body), {"heart": 73})
            measure.assert_called_once_with()


if __name__ == "__main__":
    unittest.main()
