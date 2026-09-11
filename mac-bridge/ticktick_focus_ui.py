#!/usr/bin/env python3
"""
UI automation helpers for TickTick Focus controls.

TickTick exposes a reliable Shortcuts action for starting Focus, but
pause/resume/end are only visible inside the app UI. This script clicks the
large Focus action buttons and verifies the result through TickTick's local
stopwatch preference state.
"""

from __future__ import annotations

import argparse
import plistlib
import subprocess
import sys
import time
from pathlib import Path


PREFERENCES_PATH = Path.home() / "Library/Preferences/com.TickTick.task.mac.plist"
ACTIONS = {"pause", "resume", "end"}

CONFIRM_BUTTONS = [
    "结束并保存",
    "End and Save",
    "完成并保存",
    "Complete and Save",
    "保存",
    "Save",
    "确认",
    "Confirm",
    "结束",
    "End",
    "Finish",
    "完成",
    "Done",
]

STATE_EXPECTATIONS = {
    "pause": {"paused"},
    "resume": {"running"},
    "end": {"idle", None},
}


class TickTickUIError(RuntimeError):
    pass


def stopwatch_state() -> str | None:
    if not PREFERENCES_PATH.exists():
        return None
    try:
        data = plistlib.loads(PREFERENCES_PATH.read_bytes())
    except Exception:
        return None
    return data.get("focus__stopwatch_state")


def run_osascript(script: str, *args: str) -> str:
    completed = subprocess.run(
        ["osascript", "-e", script, *args],
        check=False,
        capture_output=True,
        text=True,
        timeout=10,
    )
    if completed.returncode != 0:
        detail = (completed.stderr or completed.stdout or "").strip()
        if "不允许辅助访问" in detail or "not allowed assistive access" in detail.lower():
            raise TickTickUIError(
                "macOS Accessibility permission is required for /usr/bin/osascript."
            )
        raise TickTickUIError(detail or f"osascript exited {completed.returncode}")
    return (completed.stdout or "").strip()


def window_metrics() -> tuple[int, int, int, int]:
    script = r'''
tell application "System Events"
  tell process "TickTick"
    set p to position of window 1
    set s to size of window 1
    return (item 1 of p as text) & "|" & (item 2 of p as text) & "|" & (item 1 of s as text) & "|" & (item 2 of s as text)
  end tell
end tell
'''
    parts = run_osascript(script).split("|")
    if len(parts) != 4:
        raise TickTickUIError(f"Unable to read TickTick window metrics: {parts!r}")
    return tuple(int(float(part)) for part in parts)  # type: ignore[return-value]


def button_metrics() -> list[dict[str, int]]:
    script = r'''
set output to ""
tell application "System Events"
  tell process "TickTick"
    set elems to entire contents of window 1
    repeat with i from 1 to count of elems
      set e to item i of elems
      try
        if (role of e as text) is "AXButton" then
          set p to position of e
          set s to size of e
          set output to output & i & "|" & (item 1 of p as text) & "|" & (item 2 of p as text) & "|" & (item 1 of s as text) & "|" & (item 2 of s as text) & linefeed
        end if
      end try
    end repeat
  end tell
end tell
return output
'''
    buttons: list[dict[str, int]] = []
    for line in run_osascript(script).splitlines():
        parts = line.split("|")
        if len(parts) != 5:
            continue
        index, x, y, width, height = (int(float(part)) for part in parts)
        buttons.append(
            {"index": index, "x": x, "y": y, "width": width, "height": height}
        )
    return buttons


def focus_action_buttons() -> list[dict[str, int]]:
    wx, wy, ww, wh = window_metrics()
    candidates = []
    for button in button_metrics():
        x = button["x"]
        y = button["y"]
        width = button["width"]
        height = button["height"]
        if not (90 <= width <= 220 and 35 <= height <= 80):
            continue
        if not (wy + wh * 0.50 <= y <= wy + wh * 0.92):
            continue
        if not (wx + ww * 0.18 <= x <= wx + ww * 0.58):
            continue
        candidates.append(button)
    candidates.sort(key=lambda item: (item["y"], item["x"]))
    if not candidates:
        raise TickTickUIError("Unable to find TickTick Focus action buttons.")
    return candidates


def click_button(index: int) -> None:
    script = r'''
on run argv
  set targetIndex to (item 1 of argv) as integer
  tell application "TickTick" to activate
  delay 0.12
  tell application "System Events"
    tell process "TickTick"
      set frontmost to true
      set elems to entire contents of window 1
      click item targetIndex of elems
    end tell
  end tell
end run
'''
    run_osascript(script, str(index))


def click_focus_action(mode: str) -> None:
    script = r'''
on run argv
  set modeName to item 1 of argv
  tell application "TickTick" to activate
  delay 0.08
  tell application "System Events"
    tell process "TickTick"
      set frontmost to true
      if modeName is "secondary" then
        my clickSecondaryWhenVisible()
      else
        set primaryIndex to my primaryActionIndex()
        set elems to entire contents of window 1
        click item primaryIndex of elems
      end if
    end tell
  end tell
end run

on actionIndexes()
  tell application "System Events"
    tell process "TickTick"
      set winPos to position of window 1
      set winSize to size of window 1
      set wx to item 1 of winPos
      set wy to item 2 of winPos
      set ww to item 1 of winSize
      set wh to item 2 of winSize
      set primaryIndex to 0
      set primaryY to 99999
      set secondaryIndex to 0
      set secondaryY to -1
      set candidateCount to 0
      set elems to entire contents of window 1
      repeat with i from 1 to count of elems
        set e to item i of elems
        try
          if (role of e as text) is "AXButton" then
            set p to position of e
            set s to size of e
            set bx to item 1 of p
            set buttonY to item 2 of p
            set bw to item 1 of s
            set bh to item 2 of s
            if bw ≥ 90 and bw ≤ 220 and bh ≥ 35 and bh ≤ 80 then
              if buttonY ≥ (wy + wh * 0.5) and buttonY ≤ (wy + wh * 0.92) then
                if bx ≥ (wx + ww * 0.18) and bx ≤ (wx + ww * 0.58) then
                  set candidateCount to candidateCount + 1
                  if buttonY < primaryY then
                    set primaryY to buttonY
                    set primaryIndex to i
                  end if
                  if buttonY > secondaryY then
                    set secondaryY to buttonY
                    set secondaryIndex to i
                  end if
                end if
              end if
            end if
          end if
        end try
      end repeat
      return {candidateCount, primaryIndex, secondaryIndex}
    end tell
  end tell
end actionIndexes

on primaryActionIndex()
  set resultInfo to my actionIndexes()
  if item 1 of resultInfo < 1 then error "Unable to find TickTick Focus action button."
  return item 2 of resultInfo
end primaryActionIndex

on clickSecondaryWhenVisible()
  tell application "System Events"
    tell process "TickTick"
      set resultInfo to my actionIndexes()
      if item 1 of resultInfo < 2 then
        if item 1 of resultInfo < 1 then error "Unable to find TickTick Focus action button."
        set elems to entire contents of window 1
        click item (item 2 of resultInfo) of elems
        repeat 14 times
          delay 0.12
          set resultInfo to my actionIndexes()
          if item 1 of resultInfo ≥ 2 then exit repeat
        end repeat
      end if
      if item 1 of resultInfo < 2 then error "TickTick secondary Focus action button is not visible."
      set elems to entire contents of window 1
      click item (item 3 of resultInfo) of elems
    end tell
  end tell
end clickSecondaryWhenVisible
'''
    run_osascript(script, mode)


def click_focus_action_fast(mode: str) -> None:
    """Click a direct Focus button without walking the full AX tree.

    TickTick's calendar pane can add hundreds of accessibility nodes. Repeatedly
    scanning ``entire contents`` then makes a simple action exceed the
    AppleScript timeout. The Focus controls are direct children of the window,
    so inspect only that small button list and select the upper/lower action by
    geometry. This also avoids a blind coordinate click and localized labels.
    """
    script = r'''
on run argv
  set modeName to item 1 of argv
  tell application "TickTick" to activate
  delay 0.12
  tell application "System Events"
    tell process "TickTick"
      set frontmost to true
      set actionClicked to false
      repeat 15 times
        set winPos to position of window 1
        set winSize to size of window 1
        set wx to item 1 of winPos
        set wy to item 2 of winPos
        set ww to item 1 of winSize
        set wh to item 2 of winSize
        set primaryButton to missing value
        set primaryY to 99999
        set secondaryButton to missing value
        set secondaryY to -1
        set candidateCount to 0
        repeat with candidateButton in buttons of window 1
          try
            set p to position of candidateButton
            set s to size of candidateButton
            set bx to item 1 of p
            set buttonY to item 2 of p
            set bw to item 1 of s
            set bh to item 2 of s
            if bw >= 90 and bw <= 220 and bh >= 35 and bh <= 80 then
              if buttonY >= (wy + wh * 0.5) and buttonY <= (wy + wh * 0.95) then
                if bx >= (wx + ww * 0.18) and bx <= (wx + ww * 0.58) then
                  set candidateCount to candidateCount + 1
                  if buttonY < primaryY then
                    set primaryY to buttonY
                    set primaryButton to candidateButton
                  end if
                  if buttonY > secondaryY then
                    set secondaryY to buttonY
                    set secondaryButton to candidateButton
                  end if
                end if
              end if
            end if
          end try
        end repeat
        if modeName is "secondary" and candidateCount >= 2 then
          click secondaryButton
          set actionClicked to true
          exit repeat
        else if modeName is not "secondary" and candidateCount >= 1 then
          click primaryButton
          set actionClicked to true
          exit repeat
        end if
        delay 0.1
      end repeat
      if not actionClicked then error "Unable to find direct TickTick Focus action button."
    end tell
  end tell
end run
'''
    run_osascript(script, mode)


def click_primary_action_button() -> None:
    click_focus_action("primary")


def click_secondary_action_button() -> None:
    click_focus_action("secondary")


def click_confirmation_buttons() -> str:
    script = r'''
on run argv
  tell application "System Events"
    tell process "TickTick"
      repeat with buttonName in argv
        try
          click (first button of entire contents of front window whose name is buttonName)
          return buttonName as text
        end try
      end repeat
    end tell
  end tell
  return ""
end run
'''
    return run_osascript(script, *CONFIRM_BUTTONS)


def click_compact_confirmation_button() -> str:
    """Confirm TickTick's long-focus duration sheet when it is present.

    Sessions longer than one hour open a small window containing hour/minute
    fields and one confirmation button. Its button currently has no accessible
    name, so localized title matching cannot find it. Restrict the fallback to
    a compact auxiliary window to avoid ever clicking an arbitrary control in
    the main TickTick window.
    """
    script = r'''
tell application "System Events"
  tell process "TickTick"
    if (count of windows) < 1 then return ""
    set frontWindow to front window
    set windowSize to size of frontWindow
    if (item 1 of windowSize) > 600 or (item 2 of windowSize) > 500 then return ""
    if (count of text fields of frontWindow) < 2 then return ""
    try
      click button 1 of frontWindow
      return "compact confirmation"
    end try
  end tell
end tell
return ""
'''
    return run_osascript(script)


def wait_for_state(expected: set[str | None], timeout: float = 2.5) -> str | None:
    deadline = time.time() + timeout
    last_state = stopwatch_state()
    while time.time() < deadline:
        last_state = stopwatch_state()
        if last_state in expected:
            return last_state
        time.sleep(0.2)
    return last_state


def wait_for_secondary_button(timeout: float = 2.0) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            if len(focus_action_buttons()) >= 2:
                return True
        except TickTickUIError:
            pass
        time.sleep(0.15)
    return False


def click_action_and_wait(
    mode: str,
    expected: set[str | None],
    *,
    accessibility_fallback: bool = True,
    verify_timeout: float = 3.0,
) -> str | None:
    click_focus_action_fast(mode)
    state = wait_for_state(expected, timeout=verify_timeout)
    if state in expected or not accessibility_fallback:
        return state

    # Window layouts outside TickTick's standard split view retain the older
    # accessibility-based locator. Only fall back after the fast click has had
    # enough time to produce an authoritative preference-state transition.
    click_focus_action(mode)
    return wait_for_state(expected, timeout=8.0)


def perform_quick(action: str) -> str:
    if action in {"pause", "resume"}:
        # Pause/resume must be verified just like End. A fire-and-forget AX
        # click can time out while walking TickTick's large calendar tree and
        # leave the watch showing a state that the Mac never entered.
        return perform_and_wait(action)

    if action == "end":
        # Ending is a two-stage transition while running. Always verify it,
        # even for legacy configs that do not yet pass --wait.
        return perform_and_wait(action)

    raise TickTickUIError(f"Unsupported action: {action}")


def perform_and_wait(action: str) -> str:
    expected = STATE_EXPECTATIONS[action]
    before = stopwatch_state()

    if action in {"pause", "resume"}:
        # Pause/resume share one toggle button. Never issue a blind fallback
        # click when the preference file is slow to publish its new state: a
        # second click would undo the first transition. Wait longer and fail
        # closed; the controller will preserve its previous state on failure.
        state = click_action_and_wait(
            "primary",
            expected,
            accessibility_fallback=False,
            verify_timeout=15.0,
        )
        if state in expected:
            return f"{action}: {before or 'unknown'} -> {state or 'unknown'}"
        raise TickTickUIError(
            f"{action} did not reach expected state. before={before!r}, after={state!r}"
        )

    if action == "end":
        if before == "running":
            paused = click_action_and_wait("primary", {"paused"})
            if paused != "paused":
                raise TickTickUIError(
                    f"end could not pause before ending. before={before!r}, after={paused!r}"
                )

        # A long stopwatch session opens a separate duration-confirmation
        # sheet after End. Give that branch a chance before falling back to the
        # expensive full accessibility-tree locator.
        state = click_action_and_wait(
            "secondary", expected, accessibility_fallback=False
        )
        if state in expected:
            return f"{action}: {before or 'unknown'} -> {state or 'unknown'}"

        clicked = click_compact_confirmation_button()
        if not clicked:
            clicked = click_confirmation_buttons()
        state = wait_for_state(expected, timeout=8.0)
        if state in expected:
            suffix = f", confirmed {clicked}" if clicked else ""
            return f"{action}: {before or 'unknown'} -> {state or 'unknown'}{suffix}"

        # The window-relative point may not apply to a custom TickTick layout.
        # Use the older AX locator once, after ruling out the known long-focus
        # confirmation sheet, rather than repeatedly walking a large calendar.
        click_secondary_action_button()
        state = wait_for_state(expected, timeout=8.0)
        if state in expected:
            return f"{action}: {before or 'unknown'} -> {state or 'unknown'}"

        clicked = click_compact_confirmation_button()
        if not clicked:
            clicked = click_confirmation_buttons()
        state = wait_for_state(expected, timeout=8.0)
        if state in expected:
            suffix = f", confirmed {clicked}" if clicked else ""
            return f"{action}: {before or 'unknown'} -> {state or 'unknown'}{suffix}"

        raise TickTickUIError(
            f"end did not reach expected state. before={before!r}, after={state!r}"
        )

    raise TickTickUIError(f"Unsupported action: {action}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Control TickTick Focus UI")
    parser.add_argument("action", choices=sorted(ACTIONS))
    parser.add_argument(
        "--wait",
        action="store_true",
        help="Wait for TickTick's local stopwatch state to confirm the action.",
    )
    args = parser.parse_args()

    try:
        result = perform_and_wait(args.action) if args.wait else perform_quick(args.action)
        print(result)
    except TickTickUIError as exc:
        print(str(exc), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
