# TickTick Shortcuts Notes

TickTick for macOS exposes a `Start Focus` App Intent. V1 uses that as the
most reliable public start path.

Recommended shortcut:

1. Open the Shortcuts app.
2. Create a shortcut named `TickTick Start Stopwatch`.
3. Add TickTick's `Start Focus` action.
4. Set the focus type to Stopwatch / Focus Timer.
5. Run it once manually and confirm TickTick starts a focus session.

The bridge calls it with:

```bash
shortcuts run "TickTick Start Stopwatch"
```

Countdown / Pomodoro mode uses a separate shortcut:

1. Create a shortcut named `TickTick Start Countdown`.
2. Add TickTick's `Start Focus` action.
3. Set the focus type to Countdown / Pomo and the duration to 25 minutes.
4. Run it once manually and confirm TickTick starts a countdown focus session.

The bridge calls it with:

```bash
shortcuts run "TickTick Start Countdown"
```

For pause/resume/end, TickTick's public automation surface is less clear. This
project uses the bundled `ticktick_focus_ui.py` helper to drive TickTick through
macOS UI automation.

Recommended command hook shape:

```json
{
  "ticktick": {
    "pause_command": "python3 mac-bridge/ticktick_focus_ui.py pause",
    "resume_command": "python3 mac-bridge/ticktick_focus_ui.py resume",
    "end_command": "python3 mac-bridge/ticktick_focus_ui.py end",
    "record_command": "shortcuts run 'TickTick Add Focus Record'"
  },
  "pomo": {
    "pause_command": "python3 mac-bridge/ticktick_focus_ui.py pause",
    "resume_command": "python3 mac-bridge/ticktick_focus_ui.py resume",
    "end_command": "python3 mac-bridge/ticktick_focus_ui.py end"
  }
}
```

The helper depends on TickTick's current macOS UI structure, so it may need
small calibration if TickTick changes the Focus window.
