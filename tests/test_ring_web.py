import json
from pathlib import Path
import sys
from tempfile import TemporaryDirectory
from threading import Lock
import unittest
from unittest.mock import Mock, patch


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "frontend"))

from deepseek import APIError
from ring_web import RingMonitorSession


class RingWebStatusTests(unittest.TestCase):
    def setUp(self):
        directory = TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.recordings = Path(directory.name).resolve()
        self.device_lock = Lock()

    def app(self):
        return RingMonitorSession(self.recordings / "connect_lab.exe", [], self.recordings,
                                  Mock(), self.device_lock, Mock())

    def event(self, result=None, capture="ready", *, batch=1, times=(), demo=False):
        event = self.recordings / "session-200" / "fall-alerts" / f"event-1-{batch}"
        event.mkdir(parents=True, exist_ok=True)
        (event / "ready").write_text(json.dumps({"version": 1, "demo": demo, "episode": 1, "batch": batch,
                                               "status": capture, "times_ms": times}), encoding="utf-8")
        if result is not None:
            (event / "analysis.json").write_text(json.dumps(result), encoding="utf-8")
        for index in range(len(times)):
            (event / f"frame-{index:03}.jpg").touch()
        return event

    def test_alert_survives_restart_without_final_video(self):
        self.event({"verdict": "suspected_fall", "summary": "Visible fall", "alarm_played": True})
        (self.recordings / "session-300" / "fall-alerts").mkdir(parents=True)
        status = self.app().status()
        self.assertEqual(status["session"], "session-200")
        self.assertEqual(status["videos"], [])
        self.assertEqual(status["events"][0]["analysis_state"], "received")
        self.assertEqual(status["events"][0]["alarm_state"], "played")

    def test_pending_request_is_visible_only_as_in_progress_while_active(self):
        self.event({"verdict": "pending", "analysis_state": "requesting", "alarm_state": "pending"})
        app = self.app()
        app.state = "stopping"
        item = app.status()["events"][0]
        self.assertEqual((item["verdict"], item["analysis_state"], item["alarm_state"]),
                         ("pending", "requesting", "pending"))
        archived = self.app().status()["events"][0]
        self.assertEqual((archived["verdict"], archived["analysis_state"], archived["alarm_state"]),
                         ("cancelled", "unknown", "unknown"))

    def test_completed_reply_and_failed_alarm_are_distinct(self):
        self.event({"verdict": "suspected_fall", "analysis_state": "received", "alarm_state": "failed"})
        item = self.app().status()["events"][0]
        self.assertEqual((item["analysis_state"], item["alarm_state"], item["alarm_played"]),
                         ("received", "failed", False))

    def test_archived_demo_analysis_keeps_the_model_result(self):
        self.event({"verdict": "suspected_fall", "analysis_state": "received", "alarm_state": "skipped",
                    "alarm_reason": "demo", "analyze_demo": True}, demo=True)
        status = self.app().status()
        self.assertTrue(status["demo"])
        self.assertTrue(status["analyze_demo"])
        self.assertEqual(status["events"][0]["analysis_state"], "received")

    @patch("ring_web.shutil.which", return_value="ffmpeg")
    @patch("ring_web.run_recorder", return_value=0)
    def test_demo_analysis_requires_key_and_keeps_native_demo_flag(self, run_recorder, _):
        app = self.app()
        self.addCleanup(app.close)
        app.executable.touch()

        app.start({"demo": True})
        app.thread.join(timeout=2)
        app.client.require_key.assert_not_called()
        self.assertFalse(run_recorder.call_args.kwargs["analyze_demo"])

        with self.assertRaises(APIError):
            app.start({"demo": False, "analyze_demo": True})
        app.client.require_key.side_effect = APIError("Missing key", 503)
        with self.assertRaises(APIError):
            app.start({"demo": True, "analyze_demo": True})
        self.assertEqual(run_recorder.call_count, 1)

        app.client.require_key.side_effect = None
        self.assertTrue(app.start({"demo": True, "analyze_demo": True})["analyze_demo"])
        app.thread.join(timeout=2)
        self.assertIn("--ring-demo", run_recorder.call_args.args[2])
        self.assertTrue(run_recorder.call_args.kwargs["analyze_demo"])

    def test_episode_groups_frames_while_retaining_each_batch_status(self):
        self.event({"verdict": "suspected_fall", "summary": "Visible fall", "analysis_state": "received",
                    "alarm_state": "played"}, times=(-1000, 0))
        self.event({"verdict": "already_alerted", "analysis_state": "skipped", "alarm_state": "skipped",
                    "alarm_reason": "already_alerted"}, batch=2, times=(0, 1000))
        status = self.app().status()
        self.assertEqual(status["warning_count"], 1)
        self.assertEqual(len(status["events"]), 1)
        item = status["events"][0]
        self.assertEqual((item["id"], item["verdict"], item["alarm_state"]),
                         ("event-1", "suspected_fall", "played"))
        self.assertEqual([frame["time_ms"] for frame in item["frames"]], [-1000, 0, 1000])
        self.assertEqual({batch["analysis_state"] for batch in item["batches"]}, {"received", "skipped"})

    @patch("ring_web.play_alarm")
    def test_audition_is_local_and_releases_device_after_failure(self, play_alarm):
        app = self.app()
        with self.device_lock:
            with self.assertRaises(APIError) as busy:
                app.test_alarm()
            self.assertEqual(busy.exception.status, 409)
        play_alarm.assert_not_called()
        play_alarm.side_effect = OSError()
        with self.assertRaises(APIError):
            app.test_alarm()
        play_alarm.side_effect = None
        self.assertEqual(app.test_alarm(), {"played": True})
        app.client.require_key.assert_not_called()


if __name__ == "__main__":
    unittest.main()
