#!/usr/bin/env python3
"""
Local bridge for an M5StickS3 physical TickTick Focus switch.

The bridge intentionally has no third-party dependencies. It exposes a small
HTTP API for the device, keeps the timing state locally, and delegates TickTick
integration to macOS Shortcuts or configurable shell commands.
"""

from __future__ import annotations

import argparse
import copy
import json
import os
import shlex
import subprocess
import threading
import time
import uuid
from dataclasses import dataclass
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import parse_qs, urlparse


STATE_IDLE = "idle"
STATE_RUNNING = "running"
STATE_PAUSED = "paused"
STATE_ENDING = "ending"
STATE_DONE = "done"


DEFAULT_CONFIG: dict[str, Any] = {
    "host": "0.0.0.0",
    "port": 8787,
    "dry_run": True,
    "allow_local_fallback": True,
    "state_path": "~/Library/Application Support/M5StickTickTickFocus/state.json",
    "security": {
        "token": "",
    },
    "ticktick": {
        "start_shortcut_name": "TickTick Start Stopwatch",
        "start_command": "",
        "pause_command": "",
        "resume_command": "",
        "end_command": "",
        "record_command": "",
        "command_timeout_seconds": 20,
    },
    "pomo": {
        "duration_seconds": 1500,
        "start_shortcut_name": "TickTick Start Countdown",
        "start_command": "",
        "pause_command": "",
        "resume_command": "",
        "end_command": "",
        "command_timeout_seconds": 20,
    },
    "voice": {
        "input_command": "",
        "start_command": "",
        "stop_command": "",
        "command_timeout_seconds": 5,
    },
}


class BridgeError(RuntimeError):
    """Raised for expected bridge failures that should be returned as JSON."""


def utc_now() -> float:
    return time.time()


def iso_from_timestamp(value: float | None) -> str | None:
    if value is None:
        return None
    return datetime.fromtimestamp(value, tz=timezone.utc).astimezone().isoformat()


def deep_merge(base: dict[str, Any], override: dict[str, Any]) -> dict[str, Any]:
    merged = copy.deepcopy(base)
    for key, value in override.items():
        if isinstance(value, dict) and isinstance(merged.get(key), dict):
            merged[key] = deep_merge(merged[key], value)
        else:
            merged[key] = value
    return merged


def load_config(path: str | None, overrides: argparse.Namespace) -> dict[str, Any]:
    config = copy.deepcopy(DEFAULT_CONFIG)
    if path:
        with open(os.path.expanduser(path), "r", encoding="utf-8") as handle:
            config = deep_merge(config, json.load(handle))

    if overrides.host:
        config["host"] = overrides.host
    if overrides.port:
        config["port"] = overrides.port
    if overrides.state_path:
        config["state_path"] = overrides.state_path
    if overrides.dry_run:
        config["dry_run"] = True
    if overrides.live:
        config["dry_run"] = False
    if overrides.token is not None:
        config.setdefault("security", {})["token"] = overrides.token
    return config


def empty_store() -> dict[str, Any]:
    return {
        "current": None,
        "history": [],
        "countdown": None,
        "countdown_history": [],
    }


@dataclass
class CommandResult:
    ok: bool
    action: str
    message: str


class TickTickAdapter:
    def start(self, session: dict[str, Any]) -> CommandResult:
        raise NotImplementedError

    def pause(self, session: dict[str, Any]) -> CommandResult:
        raise NotImplementedError

    def resume(self, session: dict[str, Any]) -> CommandResult:
        raise NotImplementedError

    def end(self, session: dict[str, Any]) -> CommandResult:
        raise NotImplementedError

    def record(self, session: dict[str, Any]) -> CommandResult:
        raise NotImplementedError

    def start_countdown(self, session: dict[str, Any]) -> CommandResult:
        raise NotImplementedError

    def pause_countdown(self, session: dict[str, Any]) -> CommandResult:
        raise NotImplementedError

    def resume_countdown(self, session: dict[str, Any]) -> CommandResult:
        raise NotImplementedError

    def end_countdown(self, session: dict[str, Any]) -> CommandResult:
        raise NotImplementedError


class ConfigurableTickTickAdapter(TickTickAdapter):
    def __init__(self, config: dict[str, Any]):
        self.config = config
        self.ticktick = config["ticktick"]
        self.pomo = config.get("pomo", {})
        self.dry_run = bool(config.get("dry_run", True))
        self.timeout = int(self.ticktick.get("command_timeout_seconds", 20))

    def start(self, session: dict[str, Any]) -> CommandResult:
        command = self.ticktick.get("start_command", "")
        shortcut_name = self.ticktick.get("start_shortcut_name", "")
        if command:
            return self._run_template("start", command, session)
        if shortcut_name:
            return self._run_shortcut(shortcut_name)
        raise BridgeError("No TickTick start shortcut or command configured.")

    def pause(self, session: dict[str, Any]) -> CommandResult:
        return self._run_optional_command("pause", session)

    def resume(self, session: dict[str, Any]) -> CommandResult:
        return self._run_optional_command("resume", session)

    def end(self, session: dict[str, Any]) -> CommandResult:
        return self._run_optional_command("end", session)

    def record(self, session: dict[str, Any]) -> CommandResult:
        command = self.ticktick.get("record_command", "")
        if not command:
            return CommandResult(True, "record", "")
        return self._run_template("record", command, session)

    def start_countdown(self, session: dict[str, Any]) -> CommandResult:
        return self._run_pomo_action("start", session)

    def pause_countdown(self, session: dict[str, Any]) -> CommandResult:
        return self._run_pomo_action("pause", session)

    def resume_countdown(self, session: dict[str, Any]) -> CommandResult:
        return self._run_pomo_action("resume", session)

    def end_countdown(self, session: dict[str, Any]) -> CommandResult:
        return self._run_pomo_action("end", session)

    def _run_optional_command(
        self, action: str, session: dict[str, Any]
    ) -> CommandResult:
        command = self.ticktick.get(f"{action}_command", "")
        if not command:
            raise BridgeError(f"No TickTick {action} command configured.")
        return self._run_template(action, command, session)

    def _run_pomo_action(self, action: str, session: dict[str, Any]) -> CommandResult:
        command = self.pomo.get(f"{action}_command", "")
        shortcut_name = self.pomo.get(f"{action}_shortcut_name", "")
        timeout = int(self.pomo.get("command_timeout_seconds", self.timeout))
        if command:
            return self._run_template(action, command, session, timeout=timeout)
        if shortcut_name:
            return self._run_shortcut(shortcut_name, action=action, timeout=timeout)
        if action == "start":
            raise BridgeError("No TickTick countdown start shortcut or command configured.")
        raise BridgeError(f"No TickTick countdown {action} command configured.")

    def _run_shortcut(
        self, shortcut_name: str, action: str = "start", timeout: int | None = None
    ) -> CommandResult:
        if self.dry_run:
            return CommandResult(True, action, f"dry-run shortcut: {shortcut_name}")
        completed = subprocess.run(
            ["shortcuts", "run", shortcut_name],
            check=False,
            capture_output=True,
            text=True,
            timeout=timeout or self.timeout,
        )
        if completed.returncode != 0:
            detail = (completed.stderr or completed.stdout or "").strip()
            raise BridgeError(
                f"Shortcut '{shortcut_name}' failed with exit "
                f"{completed.returncode}: {detail}"
            )
        return CommandResult(True, action, f"shortcut: {shortcut_name}")

    def _run_template(
        self,
        action: str,
        command_template: str,
        session: dict[str, Any],
        timeout: int | None = None,
    ) -> CommandResult:
        command = command_template.format(**template_context(session))
        if self.dry_run:
            return CommandResult(True, action, f"dry-run command: {command}")
        completed = subprocess.run(
            command,
            shell=True,
            check=False,
            capture_output=True,
            text=True,
            timeout=timeout or self.timeout,
        )
        if completed.returncode != 0:
            detail = (completed.stderr or completed.stdout or "").strip()
            raise BridgeError(
                f"Command for {action} failed with exit "
                f"{completed.returncode}: {detail}"
            )
        detail = (completed.stdout or completed.stderr or "").strip()
        return CommandResult(True, action, detail or f"command: {command}")


def template_context(session: dict[str, Any]) -> dict[str, Any]:
    ended_at = session.get("ended_at") or utc_now()
    elapsed = elapsed_seconds(session, ended_at)
    duration_seconds = int(session.get("duration_seconds") or 0)
    return {
        "session_id": shlex.quote(session["id"]),
        "state": shlex.quote(session["state"]),
        "note": shlex.quote(session.get("note") or ""),
        "started_at_iso": shlex.quote(iso_from_timestamp(session.get("started_at")) or ""),
        "ended_at_iso": shlex.quote(iso_from_timestamp(ended_at) or ""),
        "elapsed_seconds": int(elapsed),
        "elapsed_minutes": max(1, round(elapsed / 60)),
        "duration_seconds": duration_seconds,
        "duration_minutes": max(1, round(duration_seconds / 60)) if duration_seconds else 0,
        "remaining_seconds": int(countdown_remaining_seconds(session, ended_at))
        if duration_seconds
        else 0,
    }


def empty_template_context() -> dict[str, Any]:
    return {
        "session_id": "",
        "state": STATE_IDLE,
        "note": "",
        "started_at_iso": "",
        "ended_at_iso": "",
        "elapsed_seconds": 0,
        "elapsed_minutes": 0,
        "duration_seconds": 0,
        "duration_minutes": 0,
        "remaining_seconds": 0,
    }


def elapsed_seconds(session: dict[str, Any], now: float | None = None) -> float:
    if not session or not session.get("started_at"):
        return 0.0
    now = now or utc_now()
    end = session.get("ended_at") or now
    paused_total = float(session.get("paused_total_seconds") or 0.0)
    if session.get("state") == STATE_PAUSED and session.get("paused_started_at"):
        paused_total += max(0.0, now - float(session["paused_started_at"]))
    return max(0.0, end - float(session["started_at"]) - paused_total)


def countdown_remaining_seconds(session: dict[str, Any], now: float | None = None) -> float:
    duration = float(session.get("duration_seconds") or 0.0)
    if duration <= 0 or not session.get("started_at"):
        return 0.0
    if session.get("state") == STATE_PAUSED:
        return max(0.0, float(session.get("remaining_seconds") or duration))
    if session.get("state") in {STATE_IDLE, STATE_DONE}:
        return max(0.0, float(session.get("remaining_seconds") or 0.0))
    now = now or utc_now()
    elapsed = elapsed_seconds(session, now)
    return max(0.0, duration - elapsed)


class FocusController:
    def __init__(self, config: dict[str, Any], adapter: TickTickAdapter | None = None):
        self.config = config
        self.adapter = adapter or ConfigurableTickTickAdapter(config)
        self.pomo = config.get("pomo", {})
        self.voice = config.get("voice", config.get("key2", {}))
        self.state_path = Path(os.path.expanduser(config["state_path"]))
        self.allow_local_fallback = bool(config.get("allow_local_fallback", True))
        self.lock = threading.RLock()
        self.store = self._load_store()

    def click(self) -> dict[str, Any]:
        with self.lock:
            current = self.store.get("current")
            if not current or current.get("state") == STATE_IDLE:
                return self._start()
            if current["state"] == STATE_RUNNING:
                return self._pause(current)
            if current["state"] == STATE_PAUSED:
                return self._resume(current)
            return self.snapshot("ignored: transition in progress")

    def double_click(self) -> dict[str, Any]:
        with self.lock:
            current = self.store.get("current")
            if not current or current.get("state") == STATE_IDLE:
                return self.snapshot("ignored: no active session")
            return self._end(current)

    def _voice_command_template(self, action: str) -> str:
        action_key = f"{action}_command"
        command_template = (
            self.voice.get(action_key)
            or self.voice.get("input_command")
            or self.voice.get("long_press_command")
            or ""
        ).strip()
        return command_template

    def _run_voice_command(self, action: str) -> dict[str, Any]:
        command_template = self._voice_command_template(action)
        if not command_template:
            return self.snapshot("voice command not configured")
        current = self.store.get("current")
        context = template_context(current) if current else empty_template_context()
        try:
            command = command_template.format(**context)
        except KeyError as exc:
            raise BridgeError(
                f"voice {action} command has an unknown template key: {exc}"
            ) from exc
        if self.config.get("dry_run"):
            return self.voice_snapshot(f"dry-run voice {action} command: {command}")
        completed = subprocess.run(
            command,
            shell=True,
            check=False,
            capture_output=True,
            text=True,
            timeout=int(self.voice.get("command_timeout_seconds", 5)),
        )
        if completed.returncode != 0:
            detail = (completed.stderr or completed.stdout or "").strip()
            raise BridgeError(
                f"voice {action} command failed with exit {completed.returncode}: {detail}"
            )
        detail = (completed.stdout or completed.stderr or "").strip()
        default_message = "voice input started" if action == "start" else "voice input stopped"
        return self.voice_snapshot(detail or default_message)

    def voice_snapshot(self, message: str) -> dict[str, Any]:
        payload = self.snapshot(message)
        payload["last_action"] = message
        payload["last_error"] = ""
        return payload

    def voice_input_start(self) -> dict[str, Any]:
        return self._run_voice_command("start")

    def voice_input_stop(self) -> dict[str, Any]:
        return self._run_voice_command("stop")

    def voice_input(self) -> dict[str, Any]:
        return self.voice_input_start()

    def key2_long_press(self) -> dict[str, Any]:
        return self.voice_input_start()

    def pomo_start(self, duration_seconds: int | None = None) -> dict[str, Any]:
        with self.lock:
            current = self.store.get("countdown")
            if current and current.get("state") == STATE_RUNNING:
                return self.pomo_snapshot("already running")
            if current and current.get("state") == STATE_PAUSED:
                return self._pomo_resume(current)
            duration = self._pomo_duration(duration_seconds)
            session = self._new_countdown_session(duration)
            self.store["countdown"] = session
            try:
                result = self.adapter.start_countdown(session)
                session["last_action"] = result.message
                session["last_error"] = ""
                session["mode"] = "ticktick_countdown"
            except BridgeError as exc:
                if not self.allow_local_fallback:
                    session["state"] = STATE_IDLE
                    session["last_error"] = str(exc)
                    self._save_store()
                    raise
                session["mode"] = "local_countdown"
                session["last_error"] = str(exc)
                session["last_action"] = "local countdown started"
            self._save_store()
            return self.pomo_snapshot("countdown started")

    def pomo_pause(self) -> dict[str, Any]:
        with self.lock:
            current = self.store.get("countdown")
            if not current or current.get("state") != STATE_RUNNING:
                return self.pomo_snapshot("ignored: no running countdown")
            return self._pomo_pause(current)

    def pomo_resume(self) -> dict[str, Any]:
        with self.lock:
            current = self.store.get("countdown")
            if not current or current.get("state") != STATE_PAUSED:
                return self.pomo_snapshot("ignored: no paused countdown")
            return self._pomo_resume(current)

    def pomo_end(self) -> dict[str, Any]:
        with self.lock:
            current = self.store.get("countdown")
            if not current or current.get("state") in {STATE_IDLE, STATE_DONE}:
                self.store["countdown"] = None
                self._save_store()
                return self.pomo_snapshot("countdown reset")
            return self._pomo_end(current, "countdown ended")

    def pomo_snapshot(self, message: str = "ok") -> dict[str, Any]:
        with self.lock:
            self._sync_countdown_completion()
            current = self.store.get("countdown")
            history = self.store.get("countdown_history", [])
            now = utc_now()
            payload = {
                "ok": True,
                "message": message,
                "server_time": iso_from_timestamp(now),
                "state": STATE_IDLE,
                "session_id": None,
                "started_at": None,
                "ended_at": None,
                "duration_seconds": self._pomo_duration(None),
                "remaining_seconds": self._pomo_duration(None),
                "paused_total_seconds": 0,
                "last_action": "",
                "last_error": "",
                "history_count": len(history),
            }
            if current:
                payload.update(
                    {
                        "state": current.get("state", STATE_IDLE),
                        "session_id": current.get("id"),
                        "started_at": iso_from_timestamp(current.get("started_at")),
                        "ended_at": iso_from_timestamp(current.get("ended_at")),
                        "duration_seconds": int(
                            current.get("duration_seconds") or self._pomo_duration(None)
                        ),
                        "remaining_seconds": int(
                            countdown_remaining_seconds(current, now)
                        ),
                        "paused_total_seconds": int(
                            float(current.get("paused_total_seconds") or 0.0)
                        ),
                        "last_action": current.get("last_action") or "",
                        "last_error": current.get("last_error") or "",
                    }
                )
            return payload

    def note(self, text: str = "", session_id: str | None = None) -> dict[str, Any]:
        with self.lock:
            target = self._note_target(session_id)
            if not target:
                target = self._new_session()
                target["state"] = STATE_IDLE
                self.store["current"] = target

            text = (text or "").strip()
            if text:
                existing = target.get("note") or ""
                target["note"] = f"{existing}\n{text}".strip() if existing else text

            target["updated_at"] = utc_now()
            self._save_store()
            return self.snapshot("note saved")

    def snapshot(self, message: str = "ok") -> dict[str, Any]:
        current = self.store.get("current")
        history = self.store.get("history", [])
        session = current or (history[-1] if history else None)
        now = utc_now()
        payload = {
            "ok": True,
            "message": message,
            "server_time": iso_from_timestamp(now),
            "state": STATE_IDLE,
            "session_id": None,
            "started_at": None,
            "ended_at": None,
            "elapsed_seconds": 0,
            "paused_total_seconds": 0,
            "note": "",
            "last_action": "",
            "last_error": "",
            "history_count": len(history),
        }
        if session:
            payload.update(
                {
                    "state": session.get("state", STATE_IDLE),
                    "session_id": session.get("id"),
                    "started_at": iso_from_timestamp(session.get("started_at")),
                    "ended_at": iso_from_timestamp(session.get("ended_at")),
                    "elapsed_seconds": int(elapsed_seconds(session, now)),
                    "paused_total_seconds": int(
                        float(session.get("paused_total_seconds") or 0.0)
                    ),
                    "note": session.get("note") or "",
                    "last_action": session.get("last_action") or "",
                    "last_error": session.get("last_error") or "",
                }
            )
        return payload

    def _start(self) -> dict[str, Any]:
        session = self._new_session()
        self.store["current"] = session
        try:
            result = self.adapter.start(session)
            session["last_action"] = result.message
            session["last_error"] = ""
            session["mode"] = "ticktick"
        except BridgeError as exc:
            if not self.allow_local_fallback:
                session["state"] = STATE_IDLE
                session["last_error"] = str(exc)
                self._save_store()
                raise
            session["mode"] = "local_fallback"
            session["last_error"] = str(exc)
            session["last_action"] = "local timer started"
        self._save_store()
        return self.snapshot("started")

    def _pause(self, session: dict[str, Any]) -> dict[str, Any]:
        try:
            result = self.adapter.pause(session)
            session["last_action"] = result.message
            session["last_error"] = ""
        except BridgeError as exc:
            session["last_error"] = str(exc)
            session["last_action"] = "local timer paused"
        session["state"] = STATE_PAUSED
        session["paused_started_at"] = utc_now()
        session["updated_at"] = utc_now()
        self._save_store()
        return self.snapshot("paused")

    def _resume(self, session: dict[str, Any]) -> dict[str, Any]:
        now = utc_now()
        paused_started_at = session.get("paused_started_at")
        if paused_started_at:
            paused_total = float(session.get("paused_total_seconds") or 0.0)
            session["paused_total_seconds"] = paused_total + max(
                0.0, now - float(paused_started_at)
            )
        try:
            result = self.adapter.resume(session)
            session["last_action"] = result.message
            session["last_error"] = ""
        except BridgeError as exc:
            session["last_error"] = str(exc)
            session["last_action"] = "local timer resumed"
        session["state"] = STATE_RUNNING
        session["paused_started_at"] = None
        session["updated_at"] = now
        self._save_store()
        return self.snapshot("resumed")

    def _end(self, session: dict[str, Any]) -> dict[str, Any]:
        now = utc_now()
        session["state"] = STATE_ENDING
        self._save_store()
        if session.get("paused_started_at"):
            paused_total = float(session.get("paused_total_seconds") or 0.0)
            session["paused_total_seconds"] = paused_total + max(
                0.0, now - float(session["paused_started_at"])
            )
        session["ended_at"] = now
        try:
            result = self.adapter.end(session)
            session["last_action"] = result.message
            session["last_error"] = ""
        except BridgeError as exc:
            session["last_error"] = str(exc)
            session["last_action"] = "local timer ended"

        try:
            record_result = self.adapter.record(session)
            if record_result.message:
                session["last_action"] = f"{session['last_action']}; {record_result.message}"
        except BridgeError as exc:
            session["last_error"] = str(exc)

        session["state"] = STATE_IDLE
        session["paused_started_at"] = None
        session["updated_at"] = now
        self.store["history"].append(copy.deepcopy(session))
        self.store["current"] = None
        self._save_store()
        return self.snapshot("ended")

    def _pomo_pause(self, session: dict[str, Any]) -> dict[str, Any]:
        now = utc_now()
        remaining = countdown_remaining_seconds(session, now)
        try:
            result = self.adapter.pause_countdown(session)
            session["last_action"] = result.message
            session["last_error"] = ""
        except BridgeError:
            session["last_error"] = ""
            session["last_action"] = "local countdown paused"
        session["state"] = STATE_PAUSED
        session["remaining_seconds"] = remaining
        session["paused_started_at"] = now
        session["updated_at"] = now
        self._save_store()
        return self.pomo_snapshot("countdown paused")

    def _pomo_resume(self, session: dict[str, Any]) -> dict[str, Any]:
        now = utc_now()
        remaining = float(session.get("remaining_seconds") or 0.0)
        duration = float(session.get("duration_seconds") or self._pomo_duration(None))
        if remaining <= 0:
            session["state"] = STATE_DONE
            session["remaining_seconds"] = 0
            self._save_store()
            return self.pomo_snapshot("countdown done")
        paused_started_at = session.get("paused_started_at")
        if paused_started_at:
            paused_total = float(session.get("paused_total_seconds") or 0.0)
            session["paused_total_seconds"] = paused_total + max(
                0.0, now - float(paused_started_at)
            )
        session["started_at"] = now - (duration - remaining)
        try:
            result = self.adapter.resume_countdown(session)
            session["last_action"] = result.message
            session["last_error"] = ""
        except BridgeError:
            session["last_error"] = ""
            session["last_action"] = "local countdown resumed"
        session["state"] = STATE_RUNNING
        session["paused_started_at"] = None
        session["updated_at"] = now
        self._save_store()
        return self.pomo_snapshot("countdown resumed")

    def _pomo_end(self, session: dict[str, Any], message: str) -> dict[str, Any]:
        now = utc_now()
        session["ended_at"] = now
        session["remaining_seconds"] = countdown_remaining_seconds(session, now)
        try:
            result = self.adapter.end_countdown(session)
            session["last_action"] = result.message
            session["last_error"] = ""
        except BridgeError:
            session["last_error"] = ""
            session["last_action"] = "local countdown ended"
        session["state"] = STATE_IDLE
        session["paused_started_at"] = None
        session["updated_at"] = now
        self.store["countdown_history"].append(copy.deepcopy(session))
        self.store["countdown"] = None
        self._save_store()
        return self.pomo_snapshot(message)

    def _sync_countdown_completion(self) -> None:
        session = self.store.get("countdown")
        if not session or session.get("state") != STATE_RUNNING:
            return
        now = utc_now()
        if countdown_remaining_seconds(session, now) > 0:
            return
        session["state"] = STATE_DONE
        session["remaining_seconds"] = 0
        session["ended_at"] = now
        session["updated_at"] = now
        session["last_action"] = "local countdown completed"
        self.store["countdown_history"].append(copy.deepcopy(session))
        self._save_store()

    def _pomo_duration(self, duration_seconds: int | None) -> int:
        configured = int(self.pomo.get("duration_seconds") or 1500)
        duration = int(duration_seconds or configured)
        return max(60, min(duration, 24 * 60 * 60))

    def _new_session(self) -> dict[str, Any]:
        now = utc_now()
        return {
            "id": str(uuid.uuid4()),
            "state": STATE_RUNNING,
            "mode": "pending",
            "started_at": now,
            "ended_at": None,
            "paused_started_at": None,
            "paused_total_seconds": 0.0,
            "note": "",
            "created_at": now,
            "updated_at": now,
            "last_action": "",
            "last_error": "",
        }

    def _new_countdown_session(self, duration_seconds: int) -> dict[str, Any]:
        now = utc_now()
        return {
            "id": str(uuid.uuid4()),
            "state": STATE_RUNNING,
            "mode": "pending_countdown",
            "duration_seconds": duration_seconds,
            "remaining_seconds": duration_seconds,
            "started_at": now,
            "ended_at": None,
            "paused_started_at": None,
            "paused_total_seconds": 0.0,
            "created_at": now,
            "updated_at": now,
            "last_action": "",
            "last_error": "",
        }

    def _note_target(self, session_id: str | None = None) -> dict[str, Any] | None:
        current = self.store.get("current")
        if session_id and current and current.get("id") == session_id:
            return current
        history = self.store.get("history") or []
        if session_id:
            for session in reversed(history):
                if session.get("id") == session_id:
                    return session
            return None
        if current:
            return current
        if history:
            return history[-1]
        return None

    def _load_store(self) -> dict[str, Any]:
        if not self.state_path.exists():
            return empty_store()
        with open(self.state_path, "r", encoding="utf-8") as handle:
            data = json.load(handle)
        return deep_merge(empty_store(), data)

    def _save_store(self) -> None:
        self.state_path.parent.mkdir(parents=True, exist_ok=True)
        tmp_path = self.state_path.with_suffix(".tmp")
        with open(tmp_path, "w", encoding="utf-8") as handle:
            json.dump(self.store, handle, ensure_ascii=False, indent=2)
        os.replace(tmp_path, self.state_path)


class FocusRequestHandler(BaseHTTPRequestHandler):
    controller: FocusController
    token: str

    def log_message(self, fmt: str, *args: Any) -> None:
        print(f"[{self.log_date_time_string()}] {fmt % args}")

    def do_GET(self) -> None:
        if not self._authorized():
            self._send_json(401, {"ok": False, "error": "unauthorized"})
            return
        route = urlparse(self.path).path
        if route == "/focus/state":
            self._send_json(200, self.controller.snapshot())
            return
        if route == "/pomo/state":
            self._send_json(200, self.controller.pomo_snapshot())
            return
        if route == "/health":
            self._send_json(200, {"ok": True})
            return
        self._send_json(404, {"ok": False, "error": "not found"})

    def do_POST(self) -> None:
        if not self._authorized():
            self._send_json(401, {"ok": False, "error": "unauthorized"})
            return
        route = urlparse(self.path).path
        try:
            if route == "/focus/click":
                self._send_json(200, self.controller.click())
            elif route == "/focus/double-click":
                self._send_json(200, self.controller.double_click())
            elif route == "/pomo/start":
                payload = self._read_payload()
                duration = (
                    int(payload.get("duration_seconds"))
                    if isinstance(payload, dict) and payload.get("duration_seconds")
                    else None
                )
                self._send_json(200, self.controller.pomo_start(duration))
            elif route == "/pomo/pause":
                self._send_json(200, self.controller.pomo_pause())
            elif route == "/pomo/resume":
                self._send_json(200, self.controller.pomo_resume())
            elif route == "/pomo/end":
                self._send_json(200, self.controller.pomo_end())
            elif route == "/focus/note":
                payload = self._read_payload()
                text = payload.get("text", "") if isinstance(payload, dict) else str(payload)
                session_id = payload.get("session_id") if isinstance(payload, dict) else None
                self._send_json(200, self.controller.note(text, session_id))
            elif route == "/device/voice-input-start":
                self._send_json(200, self.controller.voice_input_start())
            elif route == "/device/voice-input-stop":
                self._send_json(200, self.controller.voice_input_stop())
            elif route == "/device/voice-input":
                self._send_json(200, self.controller.voice_input())
            elif route == "/device/key2-long-press":
                self._send_json(200, self.controller.key2_long_press())
            else:
                self._send_json(404, {"ok": False, "error": "not found"})
        except BridgeError as exc:
            self._send_json(500, {"ok": False, "error": str(exc)})

    def _authorized(self) -> bool:
        if not self.token:
            return True
        header = self.headers.get("X-Focus-Token", "")
        query = parse_qs(urlparse(self.path).query).get("token", [""])[0]
        return header == self.token or query == self.token

    def _read_payload(self) -> Any:
        length = int(self.headers.get("Content-Length", "0"))
        if length <= 0:
            return {}
        raw = self.rfile.read(length).decode("utf-8")
        content_type = self.headers.get("Content-Type", "")
        if "application/json" in content_type:
            return json.loads(raw)
        return raw

    def _send_json(self, status: int, payload: dict[str, Any]) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def build_server(config: dict[str, Any]) -> ThreadingHTTPServer:
    controller = FocusController(config)

    class Handler(FocusRequestHandler):
        pass

    Handler.controller = controller
    Handler.token = config.get("security", {}).get("token", "")
    return ThreadingHTTPServer((config["host"], int(config["port"])), Handler)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="M5StickS3 TickTick Focus bridge")
    parser.add_argument("--config", help="Path to JSON config file")
    parser.add_argument("--host", help="Host/IP to bind")
    parser.add_argument("--port", type=int, help="Port to bind")
    parser.add_argument("--state-path", help="State JSON path")
    parser.add_argument("--token", help="Shared token required from the device")
    parser.add_argument("--dry-run", action="store_true", help="Do not call TickTick")
    parser.add_argument("--live", action="store_true", help="Call configured TickTick actions")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    config = load_config(args.config, args)
    server = build_server(config)
    mode = "dry-run" if config.get("dry_run") else "live"
    print(
        f"M5Stick TickTick Focus bridge listening on "
        f"http://{config['host']}:{config['port']} ({mode})"
    )
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping bridge.")
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
