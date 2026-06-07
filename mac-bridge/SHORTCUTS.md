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

For pause/resume/end, TickTick's public automation surface is less clear. Keep
those as configurable commands until a reliable UI automation or TickTick CLI/MCP
focus-control command is confirmed on the target machine.

Example command hook shape:

```json
{
  "ticktick": {
    "pause_command": "shortcuts run 'TickTick Pause Focus'",
    "resume_command": "shortcuts run 'TickTick Resume Focus'",
    "end_command": "shortcuts run 'TickTick End Focus'",
    "record_command": "shortcuts run 'TickTick Add Focus Record'"
  }
}
```

The exact shortcut internals are intentionally outside this repository because
they depend on what TickTick exposes on the installed macOS version.
