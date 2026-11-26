"""Regressions for the ring alert consumer without a device, cloud, or audio output."""

import base64
from contextlib import redirect_stdout
import io
import json
from pathlib import Path
import sys
import tempfile
from threading import Event
import unittest
from unittest.mock import Mock, patch
import wave

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "frontend"))

from deepseek import APIError, DeepSeek
from fall_vision import assess_fall
import ring_alert


# Synthetic 16 x 16 grayscale JPEGs with different brightness and shapes.
PHOTOS = tuple(base64.b64decode(value) for value in (
    "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAgGBgcGBQgHBwcJCQgKDBQNDAsLDBkSEw8UHRofHh0aHBwgJC4nICIsIxwcKD"
    "cpLDAxNDQ0Hyc5PTgyPC4zNDL/wAALCAAQABABAREA/8QAFgABAQEAAAAAAAAAAAAAAAAABgAE/8QAIxAAAQIGAQUBAAAAAAAAAAA"
    "AAQIDAAQFBhESFAcVISJBUv/aAAgBAQAAPwAXbHT2rXZTXJ+QmJJtpt4skPrWFbAJPxJ8ewjNddl1Kz+J3B+Vd5W+nHWpWNdc5"
    "ykfoQcij//Z",
    "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAgGBgcGBQgHBwcJCQgKDBQNDAsLDBkSEw8UHRofHh0aHBwgJC4nICIsIxwcKD"
    "cpLDAxNDQ0Hyc5PTgyPC4zNDL/wAALCAAQABABAREA/8QAFwAAAwEAAAAAAAAAAAAAAAAAAAMFBv/EACMQAAAFBAAHAAAAAAAAAAAA"
    "AAECBAURAAMSExQhIjFRgZH/2gAIAQEAAD8Az9FXWV9QtiM9lUyJlxzXBOF27jIBABHMo+J90l8d0bro4VpsINeWWqOuYiYKH"
    "aB+1//Z",
    "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAgGBgcGBQgHBwcJCQgKDBQNDAsLDBkSEw8UHRofHh0aHBwgJC4nICIsIxwcKD"
    "cpLDAxNDQ0Hyc5PTgyPC4zNDL/wAALCAAQABABAREA/8QAFwAAAwEAAAAAAAAAAAAAAAAAAAEEBv/EAB0QAAEFAQADAAAAAAAAAAA"
    "AAAMAAQIEERJBodH/2gAIAQEAAD8A2iFTXrCMN5TswE7PmSz6lZAMHPB4l3d58e1//9k=",
))
TIMES_MS = (-1500, -500, 500)


def suspected_fall_response():
    return {
        "verdict": "suspected_fall",
        "evidence": [
            {"frame": 1, "posture": "upright_or_seated", "observation": "画面中央的人站立。"},
            {"frame": 3, "posture": "on_floor", "observation": "同一人倒在地板上。"},
        ],
        "summary": "取样画面显示疑似摔倒。",
    }


class RingAlertTests(unittest.TestCase):
    def setUp(self):
        scratch = tempfile.TemporaryDirectory(prefix="connect-ring-alert-test-")
        self.addCleanup(scratch.cleanup)
        self.root = Path(scratch.name)
        self.client = Mock(spec=DeepSeek)
        self.client.json_data.side_effect = DeepSeek.json_data
        self.client.complete.return_value = json.dumps(suspected_fall_response())
        self.sound = Mock(SND_MEMORY=4, SND_NODEFAULT=2)

    def make_event(self, number, episode=None, *, status="ready", demo=False, publish=True,
                   photos=PHOTOS, times=TIMES_MS):
        directory = self.root / f"event-{number}"
        directory.mkdir()
        for index, jpeg in enumerate(photos):
            (directory / f"frame-{index:03}.jpg").write_bytes(jpeg)
        data = {"version": 1, "demo": demo, "status": status,
                "episode": episode if episode is not None else number,
                "batch": number, "times_ms": list(times)}
        (directory / "ready.tmp").write_text(json.dumps(data), encoding="utf-8")
        if publish:
            (directory / "ready.tmp").replace(directory / "ready")
        return directory

    def consume(self):
        finished = Event()
        finished.set()
        with patch.object(ring_alert, "winsound", self.sound), redirect_stdout(io.StringIO()):
            ring_alert.watch_events(self.root, self.client, finished, Event())

    @staticmethod
    def result(directory):
        return json.loads((directory / "analysis.json").read_text(encoding="utf-8"))

    def test_ready_marker_gates_ordered_upload_and_local_wav(self):
        directory = self.make_event(1, publish=False)
        self.consume()
        self.client.complete.assert_not_called()
        self.sound.PlaySound.assert_not_called()
        self.assertFalse((directory / "analysis.json").exists())

        (directory / "ready.tmp").replace(directory / "ready")
        self.consume()
        self.assertTrue(self.result(directory)["alarm_played"])
        self.client.complete.assert_called_once()
        request = self.client.complete.call_args.args[0][1]["content"]
        self.assertEqual([item["type"] for item in request], ["text", "image_url"] * 3)
        for position, (timestamp, photo) in enumerate(zip(TIMES_MS, PHOTOS)):
            self.assertIn(f"{timestamp:+d}", request[2 * position]["text"])
            url = request[2 * position + 1]["image_url"]["url"]
            self.assertEqual(base64.b64decode(url.split(",", 1)[1]), photo)
        self.assertTrue(self.client.complete.call_args.kwargs["json_output"])
        self.sound.PlaySound.assert_called_once()
        data, flags = self.sound.PlaySound.call_args.args
        self.assertEqual(flags, self.sound.SND_MEMORY | self.sound.SND_NODEFAULT)
        with wave.open(io.BytesIO(data), "rb") as alarm:
            self.assertEqual((alarm.getnchannels(), alarm.getsampwidth(), alarm.getframerate()), (1, 2, 24000))
            self.assertTrue(any(alarm.readframes(alarm.getnframes())))

    def test_only_one_alarm_across_batches_of_one_episode(self):
        batches = [self.make_event(1, episode=7), self.make_event(2, episode=7)]
        self.consume()
        results = [self.result(directory) for directory in batches]
        self.assertCountEqual([item["verdict"] for item in results], ["suspected_fall", "already_alerted"])
        self.assertEqual(sum(item["alarm_played"] for item in results), 1)
        self.client.complete.assert_called_once()
        self.sound.PlaySound.assert_called_once()

    def test_demo_incomplete_and_duplicate_frames_stay_local(self):
        demo = self.make_event(1, demo=True)
        incomplete = self.make_event(2, status="insufficient", photos=())
        duplicates = self.make_event(3, photos=(PHOTOS[0],) * 3)
        self.consume()
        self.assertEqual(self.result(demo)["verdict"], "demo")
        self.assertEqual(self.result(incomplete)["verdict"], "uncertain")
        self.assertEqual(self.result(duplicates)["verdict"], "uncertain")
        self.client.complete.assert_not_called()
        self.sound.PlaySound.assert_not_called()

    def test_bad_marker_and_upstream_error_do_not_play_alarm(self):
        invalid = self.make_event(1, times=(-500, -500, 500))
        upstream = self.make_event(2)
        self.client.complete.side_effect = APIError("模拟服务暂不可用。")
        self.consume()
        self.assertEqual(self.result(invalid)["verdict"], "error")
        self.assertEqual(self.result(upstream)["verdict"], "error")
        self.client.complete.assert_called_once()
        self.sound.PlaySound.assert_not_called()

    def test_model_label_requires_ordered_evidence_of_a_change(self):
        response = suspected_fall_response()
        response["evidence"][0]["posture"] = "on_floor"
        self.client.complete.return_value = json.dumps(response)
        directory = self.make_event(1)
        self.consume()
        self.assertEqual(self.result(directory)["verdict"], "uncertain")
        self.sound.PlaySound.assert_not_called()

    def test_evidence_keeps_saved_frame_numbers_after_deduplication(self):
        response = suspected_fall_response()
        response["evidence"][1]["frame"] = 2
        self.client.complete.return_value = json.dumps(response)
        frames = list(zip(TIMES_MS, (PHOTOS[0], PHOTOS[0], PHOTOS[2])))
        result = assess_fall(self.client, frames)
        self.assertEqual(result.verdict, "suspected_fall")
        self.assertEqual([item.frame for item in result.evidence], [1, 3])

    def test_model_cannot_use_boolean_or_out_of_range_frame_numbers(self):
        for invalid_frame in (True, 4):
            with self.subTest(frame=invalid_frame):
                response = suspected_fall_response()
                response["evidence"][0]["frame"] = invalid_frame
                self.client.complete.return_value = json.dumps(response)
                with self.assertRaises(APIError):
                    assess_fall(self.client, list(zip(TIMES_MS, PHOTOS)))

    def test_cancel_while_model_is_running_suppresses_alarm(self):
        directory = self.make_event(1)
        cancel = Event()

        def complete(*_, **__):
            cancel.set()
            return json.dumps(suspected_fall_response())

        self.client.complete.side_effect = complete
        with patch.object(ring_alert, "winsound", self.sound):
            result = ring_alert.analyze_event(directory / "ready", self.client, set(), cancel)
        self.assertFalse(result["alarm_played"])
        self.sound.PlaySound.assert_not_called()


if __name__ == "__main__":
    unittest.main()
