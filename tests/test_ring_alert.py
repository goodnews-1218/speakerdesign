"""Regressions for the ring alert consumer without a device, cloud, or audio output."""

import base64
from contextlib import contextmanager, redirect_stdout
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

    def consume(self, *, analyze_demo=False, reply="no_reply"):
        finished = Event()
        finished.set()
        with (patch.object(ring_alert, "winsound", self.sound),
              patch.object(ring_alert, "listen_for_reply", return_value=reply) as listening,
              redirect_stdout(io.StringIO())):
            ring_alert.watch_events(self.root, self.client, finished, Event(), analyze_demo=analyze_demo)
        return listening

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
        result = self.result(directory)
        self.assertTrue(result["alarm_played"])
        self.assertTrue(result["urgent"])
        self.assertEqual((result["analysis_state"], result["alarm_state"]), ("received", "played"))
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
            self.assertAlmostEqual(alarm.getnframes() / alarm.getframerate(), 8.27, places=2)
            self.assertTrue(any(alarm.readframes(alarm.getnframes())))

    def test_only_one_alarm_across_batches_of_one_episode(self):
        batches = [self.make_event(1, episode=7), self.make_event(2, episode=7)]
        self.consume()
        results = [self.result(directory) for directory in batches]
        self.assertCountEqual([item["verdict"] for item in results], ["suspected_fall", "already_alerted"])
        self.assertEqual(sum(item["alarm_played"] for item in results), 1)
        self.client.complete.assert_called_once()
        self.sound.PlaySound.assert_called_once()

    def test_priority_audio_covers_confirmation_and_alarm(self):
        event = self.make_event(1)
        audio_steps = []

        @contextmanager
        def reserve_audio():
            audio_steps.append("reserved")
            try:
                yield
            finally:
                audio_steps.append("released")

        def complete(*args, **kwargs):
            audio_steps.append("vision")
            return self.client.complete.return_value

        def listen(*args):
            audio_steps.append("confirmation")
            return "needs_help"

        self.client.complete.side_effect = complete
        self.sound.PlaySound.side_effect = lambda *_: audio_steps.append("alarm")
        finished = Event()
        finished.set()
        with (patch.object(ring_alert, "winsound", self.sound),
              patch.object(ring_alert, "listen_for_reply", side_effect=listen),
              redirect_stdout(io.StringIO())):
            ring_alert.watch_events(self.root, self.client, finished, Event(), priority_audio=reserve_audio)
        self.assertEqual(audio_steps, ["vision", "reserved", "confirmation", "alarm", "released"])
        self.assertTrue(self.result(event)["alarm_played"])

    def test_demo_incomplete_and_duplicate_frames_stay_local(self):
        demo = self.make_event(1, demo=True)
        incomplete = self.make_event(2, status="insufficient", photos=())
        duplicates = self.make_event(3, photos=(PHOTOS[0],) * 3)
        self.consume()
        self.assertEqual(self.result(demo)["verdict"], "demo")
        self.assertEqual(self.result(incomplete)["verdict"], "uncertain")
        self.assertEqual(self.result(duplicates)["verdict"], "uncertain")
        self.assertTrue(all(self.result(event)["analysis_state"] == "skipped"
                            for event in (demo, incomplete, duplicates)))
        self.client.complete.assert_not_called()
        self.sound.PlaySound.assert_not_called()

    def test_demo_opt_in_runs_analysis_voice_and_alarm(self):
        event = self.make_event(1, demo=True)
        incomplete = self.make_event(2, demo=True, status="insufficient", photos=())

        def complete(*_, **__):
            pending = self.result(event)
            self.assertTrue(pending["analyze_demo"])
            self.assertEqual((pending["analysis_state"], pending["alarm_state"]), ("requesting", "pending"))
            return json.dumps(suspected_fall_response())

        self.client.complete.side_effect = complete
        self.consume(analyze_demo=True).assert_called_once()

        self.client.complete.assert_called_once()
        content = self.client.complete.call_args.args[0][1]["content"]
        photos = [base64.b64decode(item["image_url"]["url"].split(",", 1)[1])
                  for item in content if item["type"] == "image_url"]
        self.assertEqual(photos, list(PHOTOS))
        result = self.result(event)
        self.assertEqual((result["verdict"], result["analysis_state"]), ("suspected_fall", "received"))
        self.assertEqual((result["alarm_state"], result["voice_state"]), ("played", "no_reply"))
        self.assertTrue(result["analyze_demo"])
        self.assertTrue(result["alarm_played"])
        skipped = self.result(incomplete)
        self.assertTrue(skipped["analyze_demo"])
        self.assertEqual(skipped["analysis_state"], "skipped")
        self.sound.PlaySound.assert_called_once()

    def test_acknowledged_reply_skips_alarm_for_the_episode(self):
        first = self.make_event(1, episode=4)
        second = self.make_event(2, episode=4)
        self.consume(reply="acknowledged").assert_called_once()
        self.assertCountEqual([self.result(first)["verdict"], self.result(second)["verdict"]],
                              ["suspected_fall", "acknowledged"])
        for marker in (first, second):
            self.assertEqual(self.result(marker)["alarm_reason"], "voice_acknowledged")
        self.sound.PlaySound.assert_not_called()

    def test_seated_slump_checks_voice_and_escalates_on_no_reply(self):
        response = suspected_fall_response()
        response["evidence"][0]["observation"] = "同一人坐在座位上。"
        response["evidence"][1].update(posture="slumped", observation="同一人突然瘫软趴在桌上。")
        self.client.complete.return_value = json.dumps(response)
        event = self.make_event(1)
        self.consume().assert_called_once()
        result = self.result(event)
        self.assertTrue(result["urgent"])
        self.assertTrue(result["alarm_played"])

    def test_failed_voice_check_still_plays_alarm_without_marking_no_reply(self):
        event = self.make_event(1)
        self.consume(reply="unavailable").assert_called_once()
        result = self.result(event)
        self.assertTrue(result["alarm_played"])
        self.assertEqual(result["voice_state"], "unavailable")
        self.assertFalse(result.get("urgent", False))

    def test_explicit_help_plays_alarm_and_records_the_reason(self):
        event = self.make_event(1)
        self.consume(reply="needs_help").assert_called_once()
        result = self.result(event)
        self.assertTrue(result["alarm_played"])
        self.assertEqual(result["voice_state"], "needs_help")
        self.assertEqual(result["alarm_reason"], "voice_needs_help")

    def test_stillness_needs_sustained_post_alert_evidence(self):
        response = {"verdict": "suspected_immobility", "summary": "同一人坐在座位上，未见明显动作。",
                    "evidence": [{"frame": n, "posture": "seated", "observation": "仍坐在原处。"}
                                 for n in range(1, 4)]}
        self.client.complete.return_value = json.dumps(response)
        valid = self.make_event(1, times=(300, 3300, 6300))
        for n, times in enumerate(((-3000, 0, 3000), (0, 2000, 4000), (0, 2000, 7000)), 2):
            self.make_event(n, times=times)
        self.consume(reply="no_reply").assert_called_once()
        self.assertTrue(self.result(valid)["alarm_played"])
        self.assertTrue(self.result(valid)["urgent"])
        for n in range(2, 5):
            self.assertEqual(self.result(self.root / f"event-{n}")["verdict"], "uncertain")

        for evidence in response["evidence"]:
            evidence["posture"] = "upright_or_seated"
        self.client.complete.return_value = json.dumps(response)
        self.assertEqual(assess_fall(self.client, list(zip((300, 3300, 6300), PHOTOS))).verdict, "uncertain")

    def test_voice_sends_only_post_prompt_audio_and_uses_semantic_classification(self):
        mic_path = self.root / "mic.wav"
        sound = io.BytesIO()
        with wave.open(sound, "wb") as wav:
            wav.setparams((1, 2, 16000, 0, "NONE", "not compressed"))
            wav.writeframes(b"\x00\x00" * 100)
        self.client.synthesize.return_value = sound.getvalue()
        cases = (
            ("我沒有受傷，不用幫忙。", '{"intent":"safe"}', "acknowledged"),
            ("我摔倒了，救命！", '{"intent":"needs_help"}', "needs_help"),
            ("我没事，但是我站不起来。", '{"intent":"needs_help"}', "needs_help"),
            ("喂，听见了。", '{"intent":"unclear"}', "unconfirmed"),
            ("喂。", '{"intent":"safe","other":true}', "unconfirmed"),
            ("喂。", APIError("语义判断超时。"), "unavailable"),
            ("   ", None, "no_reply"),
        )
        for transcript, response, expected in cases:
            with self.subTest(transcript=transcript):
                mic_path.write_bytes(sound.getvalue())
                self.client.transcribe.return_value = transcript
                self.client.complete.reset_mock()
                self.client.complete.side_effect = response if isinstance(response, APIError) else None
                self.client.complete.return_value = response
                cancel = Mock()
                cancel.is_set.return_value = False
                new_pcm = b"\x00\x04" * 1000

                def feed(seconds):
                    if seconds == 0.25:
                        with mic_path.open("ab") as audio:
                            audio.write(new_pcm * 4)
                    return False

                cancel.wait.side_effect = feed
                progress = Mock()
                with patch.object(ring_alert, "winsound", self.sound):
                    reply = ring_alert.listen_for_reply(self.client, mic_path, cancel, None, progress)
                self.assertEqual(reply, expected)
                with wave.open(io.BytesIO(self.client.transcribe.call_args.args[0]), "rb") as sent:
                    self.assertEqual(sent.readframes(sent.getnframes()), new_pcm * 128)
                if response is None:
                    self.client.complete.assert_not_called()
                    continue
                progress.assert_called_with("interpreting")
                self.client.complete.assert_called_once()
                messages = self.client.complete.call_args.args[0]
                self.assertEqual([message["role"] for message in messages], ["system", "user"])
                self.assertEqual(json.loads(messages[1]["content"]), {"transcript": transcript})
                self.assertTrue(self.client.complete.call_args.kwargs["json_output"])

    def test_bad_marker_and_upstream_error_do_not_play_alarm(self):
        invalid = self.make_event(1, times=(-500, -500, 500))
        upstream = self.make_event(2)
        self.client.complete.side_effect = APIError("模拟服务暂不可用。")
        self.consume()
        self.assertEqual(self.result(invalid)["verdict"], "error")
        self.assertEqual(self.result(upstream)["verdict"], "error")
        self.assertEqual(self.result(invalid)["analysis_state"], "skipped")
        self.assertEqual(self.result(upstream)["analysis_state"], "failed")
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
        self.assertEqual((result["analysis_state"], result["alarm_state"], result["alarm_reason"]),
                         ("received", "skipped", "stopped"))
        self.sound.PlaySound.assert_not_called()

    def test_progress_distinguishes_model_response_from_audio_failure(self):
        directory = self.make_event(1)

        def complete(*_, **__):
            result = self.result(directory)
            self.assertEqual((result["analysis_state"], result["alarm_state"]), ("requesting", "pending"))
            return json.dumps(suspected_fall_response())

        def play(*_):
            result = self.result(directory)
            self.assertEqual((result["analysis_state"], result["alarm_state"]), ("received", "playing"))
            raise RuntimeError("模拟 Windows 音频错误。")

        self.client.complete.side_effect = complete
        self.sound.PlaySound.side_effect = play
        self.consume()
        result = self.result(directory)
        self.assertEqual((result["analysis_state"], result["alarm_state"]), ("received", "failed"))
        self.assertEqual(result["verdict"], "suspected_fall")
        self.assertIn("音频错误", result["alarm_error"])

    def test_web_stop_drains_batch_published_by_finalizing_recorder(self):
        pending = self.make_event(1, publish=False)
        output = self.root.resolve()
        alerts = output / "session-42" / "fall-alerts"
        alerts.mkdir(parents=True)
        directory = pending.replace(alerts / "event-1-1")
        stop = Event()

        def launch(command, **_):
            stop_file = Path(command[command.index("--stop-file") + 1])
            recorder = Mock(returncode=None, stdout=io.BytesIO(f"FALL_ALERT_ROOT={alerts}\n".encode("utf-8")))
            recorder.__enter__ = Mock(return_value=recorder)
            recorder.__exit__ = Mock(return_value=False)

            def poll():
                if recorder.returncode is None and stop_file.is_file():
                    (directory / "ready.tmp").replace(directory / "ready")
                    recorder.returncode = 0
                return recorder.returncode

            recorder.poll.side_effect = poll
            recorder.wait.side_effect = lambda **_: recorder.returncode
            return recorder

        with patch.object(ring_alert.subprocess, "Popen", side_effect=launch), \
                patch.object(ring_alert, "winsound", self.sound), redirect_stdout(io.StringIO()):
            status = ring_alert.run_recorder(Path("connect_lab"), output, [], self.client,
                                             cancel=stop, on_root=lambda _: stop.set())
        self.assertEqual(status, 0)
        result = self.result(directory)
        self.assertEqual(result["verdict"], "suspected_fall")
        self.assertEqual((result["analysis_state"], result["alarm_state"], result["alarm_reason"]),
                         ("received", "skipped", "stopped"))
        self.client.complete.assert_called_once()
        self.sound.PlaySound.assert_not_called()


if __name__ == "__main__":
    unittest.main()
