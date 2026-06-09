#!/usr/bin/env python3

import tempfile
import unittest
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

from focus_bridge import (
    BridgeError,
    CommandResult,
    DEFAULT_CONFIG,
    FocusController,
    deep_merge,
)


class FakeAdapter:
    def __init__(self, fail_actions=None, messages=None):
        self.actions = []
        self.fail_actions = set(fail_actions or [])
        self.messages = messages or {}

    def _action(self, action, session):
        self.actions.append(action)
        if action in self.fail_actions:
            raise BridgeError(f"{action} failed")
        return CommandResult(True, action, self.messages.get(action, f"{action} ok"))

    def start(self, session):
        return self._action("start", session)

    def pause(self, session):
        return self._action("pause", session)

    def resume(self, session):
        return self._action("resume", session)

    def end(self, session):
        return self._action("end", session)

    def record(self, session):
        return self._action("record", session)

    def start_countdown(self, session):
        return self._action("start_countdown", session)

    def pause_countdown(self, session):
        return self._action("pause_countdown", session)

    def resume_countdown(self, session):
        return self._action("resume_countdown", session)

    def end_countdown(self, session):
        return self._action("end_countdown", session)


class FocusControllerTests(unittest.TestCase):
    def make_controller(self, adapter, config_extra=None):
        temp_dir = tempfile.TemporaryDirectory()
        self.addCleanup(temp_dir.cleanup)
        state_path = str(Path(temp_dir.name) / "state.json")
        config = deep_merge(DEFAULT_CONFIG, {"state_path": state_path})
        if config_extra:
            config = deep_merge(config, config_extra)
        return FocusController(config, adapter)

    def test_click_pause_resume_and_double_click_end(self):
        adapter = FakeAdapter()
        controller = self.make_controller(adapter)

        self.assertEqual(controller.click()["state"], "running")
        self.assertEqual(controller.click()["state"], "paused")
        self.assertEqual(controller.click()["state"], "running")
        ended = controller.double_click()

        self.assertEqual(ended["state"], "idle")
        self.assertEqual(adapter.actions, ["start", "pause", "resume", "end", "record"])
        self.assertEqual(ended["history_count"], 1)

    def test_local_fallback_keeps_timer_usable(self):
        adapter = FakeAdapter(fail_actions={"start", "pause", "resume", "end", "record"})
        controller = self.make_controller(adapter)

        started = controller.click()
        paused = controller.click()
        resumed = controller.click()
        ended = controller.double_click()

        self.assertEqual(started["state"], "running")
        self.assertEqual(paused["state"], "paused")
        self.assertEqual(resumed["state"], "running")
        self.assertEqual(ended["state"], "idle")
        self.assertIn("record failed", ended["last_error"])

    def test_note_attaches_to_latest_session(self):
        adapter = FakeAdapter()
        controller = self.make_controller(adapter)

        controller.click()
        controller.double_click()
        noted = controller.note("Reviewed the first bridge prototype")

        self.assertEqual(noted["state"], "idle")
        self.assertIn("Reviewed", noted["note"])

    def test_note_can_attach_to_finished_session_by_id(self):
        adapter = FakeAdapter()
        controller = self.make_controller(adapter)

        started = controller.click()
        first_session_id = started["session_id"]
        controller.double_click()
        controller.click()
        noted = controller.note("第一段录音备注", session_id=first_session_id)

        self.assertEqual(noted["state"], "running")
        self.assertEqual(controller.store["history"][-1]["note"], "第一段录音备注")
        self.assertEqual((controller.store["current"] or {}).get("note"), "")

    def test_voice_input_start_and_stop_run_independent_commands(self):
        adapter = FakeAdapter()
        controller = self.make_controller(
            adapter,
            {
                "voice": {
                    "start_command": "\"/absolute/path/TypelessKeySender\" ctrl-cmd-shift-space",
                    "stop_command": "\"/absolute/path/TypelessKeySender\" ctrl-cmd-shift-space",
                }
            },
        )

        controller.click()
        controller.store["current"]["last_error"] = "old focus error"
        adapter.actions.clear()
        started = controller.voice_input_start()
        stopped = controller.voice_input_stop()

        self.assertIn("dry-run voice start command: \"/absolute/path/TypelessKeySender\"", started["message"])
        self.assertIn("dry-run voice stop command: \"/absolute/path/TypelessKeySender\"", stopped["message"])
        self.assertEqual(started["last_error"], "")
        self.assertEqual(stopped["last_error"], "")
        self.assertEqual(adapter.actions, [])

    def test_pomo_start_pause_resume_and_end(self):
        adapter = FakeAdapter()
        controller = self.make_controller(
            adapter,
            {
                "pomo": {
                    "duration_seconds": 1500,
                    "start_shortcut_name": "TickTick Start Countdown",
                }
            },
        )

        started = controller.pomo_start()
        paused = controller.pomo_pause()
        resumed = controller.pomo_resume()
        ended = controller.pomo_end()

        self.assertEqual(started["state"], "running")
        self.assertEqual(started["duration_seconds"], 1500)
        self.assertLessEqual(started["remaining_seconds"], 1500)
        self.assertEqual(paused["state"], "paused")
        self.assertEqual(resumed["state"], "running")
        self.assertEqual(ended["state"], "idle")
        self.assertEqual(
            adapter.actions,
            ["start_countdown", "pause_countdown", "resume_countdown", "end_countdown"],
        )


if __name__ == "__main__":
    unittest.main()
