from contextlib import closing
import json
from pathlib import Path
import sqlite3
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

    def event(self, result=None, capture="ready", *, batch=1, times=(), demo=False,
              session=200, episode=1, onset_ms=None):
        event = self.recordings / f"session-{session}" / "fall-alerts" / f"event-{episode}-{batch}"
        event.mkdir(parents=True, exist_ok=True)
        (event / "ready").write_text(json.dumps({"version": 1, "demo": demo, "episode": episode, "batch": batch,
                                               "status": capture, "times_ms": times, "onset_ms": onset_ms}),
                                     encoding="utf-8")
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

    def test_current_camera_frame_only_uses_fresh_running_preview(self):
        preview = self.event().parent / "preview.jpg"
        frame = b"\xff\xd8\xffpreview\xff\xd9"
        preview.write_bytes(frame)
        app = self.app()
        self.assertIsNone(app.current_camera_frame())

        app.state = "running"
        self.assertEqual(app.current_camera_frame(), frame)
        with patch("ring_web.time", return_value=preview.stat().st_mtime + 10):
            self.assertIsNone(app.current_camera_frame())

        preview.write_bytes(frame[:-2])
        self.assertIsNone(app.current_camera_frame())

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

    def test_urgent_result_remains_visible_across_batches(self):
        self.event({"verdict": "suspected_fall", "voice_state": "no_reply", "urgent": True,
                    "alarm_state": "failed"})
        self.event({"verdict": "suspected_fall", "voice_state": "unavailable", "urgent": True,
                    "alarm_state": "played"}, batch=2)
        item = self.app().status()["events"][0]
        self.assertTrue(item["urgent"])
        self.assertEqual(item["voice_state"], "no_reply")
        self.assertFalse(item["batches"][1]["urgent"])
        self.assertEqual(item["alarm_state"], "played")

    def test_history_pages_survive_restart_and_keep_archived_frames_accessible(self):
        for session in range(200, 218):
            self.event({"verdict": "suspected_immobility", "voice_state": "no_reply", "urgent": True,
                        "evidence": [{"frame": 1, "observation": "坐在桌前。"},
                                     {"frame": 2, "observation": "保持坐姿。"}]},
                       session=session, times=(-8000, -3000), onset_ms=-6000)
        page = self.app().history()
        self.assertEqual(len(page["events"]), 16)
        app = self.app()
        older = app.history(page["next_before"])
        self.assertEqual([event["session_id"] for event in older["events"]], ["session-201", "session-200"])
        self.assertIsNone(older["next_before"])
        event = older["events"][1]
        self.assertTrue(event["urgent"])
        self.assertEqual(event["before"], ["坐在桌前。"])
        self.assertEqual(event["after"], ["保持坐姿。"])
        self.assertIsNotNone(app.media("session-200/fall-alerts/event-1-1/frame-000.jpg"))
        self.assertIsNone(app.media("session-200/../session-217/fall-alerts/event-1-1/frame-000.jpg"))

    def test_event_replay_links_survive_restart_without_crossing_episodes(self):
        first = self.event({"verdict": "suspected_fall"}, times=(-2000, 1000))
        second = self.event({"verdict": "no_visible_fall"}, episode=2)
        self.event({"verdict": "suspected_fall"}, demo=True, session=201)
        root = first.parents[1]
        (root / "video.mp4").touch()
        for clip, episode in ((2, 1), (3, 1), (5, 2)):
            name = f"highlight-ring-{clip}.mp4"
            (root / name).touch()
            (root / (name + ".event")).write_text(f"{episode}\n", encoding="ascii")
        (root / "highlight-ring-4.mp4").touch()

        self.assertEqual(len(self.app().history()["events"]), 2)
        self.assertTrue((self.recordings / "ring-events.sqlite3").is_file())
        for batch in (first, second):
            (batch / "ready").unlink()
        restarted = self.app()
        self.assertEqual(len(restarted.history()["events"]), 2)
        first_detail = restarted.event_detail("session-200/event-1")
        second_detail = restarted.event_detail("session-200/event-2")
        self.assertEqual([item["name"] for item in first_detail["videos"]],
                         ["video.mp4", "highlight-ring-2.mp4", "highlight-ring-3.mp4"])
        self.assertEqual([item["name"] for item in second_detail["videos"]],
                         ["video.mp4", "highlight-ring-5.mp4"])
        self.assertEqual(len(first_detail["event"]["frames"]), 2)
        with self.assertRaises(APIError) as missing:
            restarted.event_detail("session-201/event-1")
        self.assertEqual(missing.exception.status, 404)

    def test_combined_history_includes_simulated_events_and_details_across_pages(self):
        for session in range(200, 218):
            self.event({"verdict": "uncertain"}, session=session, demo=session % 2 == 0)
        app = self.app()
        page = app.history(testing=None)
        older = app.history(page["next_before"], testing=None)
        events = page["events"] + older["events"]
        self.assertEqual([item["session_id"] for item in events],
                         [f"session-{session}" for session in reversed(range(200, 218))])
        self.assertEqual([item["demo"] for item in events], [False, True] * 9)
        self.assertIsNone(older["next_before"])
        for event in older["events"]:
            key = f"{event['session_id']}/{event['id']}"
            self.assertEqual(app.event_detail(key, testing=None)["event"], event)

    def test_legacy_clips_only_match_an_unambiguous_archived_episode(self):
        root = self.event(session=200).parents[1]
        clip = root / "highlight-ring-1.mp4"
        clip.touch()
        app = self.app()
        self.assertEqual([item["name"] for item in app.event_detail("session-200/event-1")["videos"]],
                         [clip.name])

        self.event(session=200, episode=2)
        app = self.app()
        self.assertEqual(app.event_detail("session-200/event-1")["videos"], [])
        self.assertEqual(app.event_detail("session-200/event-2")["videos"], [])

    def test_legacy_archive_classifies_simulated_records_without_source_files(self):
        app = self.app()
        with closing(sqlite3.connect(app.archive.path)) as db, db:
            db.execute("""CREATE TABLE ring_event (
                session_id TEXT NOT NULL, event_id TEXT NOT NULL, session_number INTEGER NOT NULL,
                payload TEXT NOT NULL, PRIMARY KEY (session_id, event_id))""")
            db.executemany("INSERT INTO ring_event VALUES (?, ?, ?, ?)", (
                ("session-200", "event-1", 200, '{"demo": false, "verdict": "uncertain"}'),
                ("session-201", "event-1", 201, '{"demo": true, "verdict": "suspected_fall"}'),
                ("session-202", "event-1", 202, '{"verdict": "demo"}'),
            ))
        self.assertEqual(len(app.history()["events"]), 1)
        self.assertEqual(len(app.history(testing=True)["events"]), 2)
        self.assertEqual(len(app.history(testing=None)["events"]), 3)

    def test_completed_session_adds_playback_after_a_live_event(self):
        event = self.event({"verdict": "uncertain"})
        app = self.app()
        app.state = "running"
        self.assertEqual(app.event_detail("session-200/event-1")["videos"], [])
        root = event.parents[1]
        (root / "video.mp4").touch()
        app.state = "stopped"
        detail = app.event_detail("session-200/event-1")
        self.assertEqual([item["name"] for item in detail["videos"]], ["video.mp4"])

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
