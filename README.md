# M5StickS3 TickTick / Typeless Control Deck

[中文说明](README.zh-CN.md)

Personal V1 firmware and Mac bridge for turning an M5Stack StickS3 /
M5StickS3 into a small Wi-Fi control deck for TickTick Focus, Typeless voice
input, device status, and TickTick countdown.

The project has two parts:

- `firmware/`: PlatformIO firmware for StickS3. It reads KEY1/KEY2, updates
  the screen, handles IMU posture, plays small sounds, and calls the Mac bridge
  over Wi-Fi.
- `mac-bridge/`: a local HTTP bridge that owns state and talks to TickTick,
  Typeless, and macOS Shortcuts or command hooks.

Local secrets are intentionally ignored:

- `firmware/include/config.h`
- `mac-bridge/config.local.json`
- PlatformIO build output under `.pio/`

Use the checked-in example config files as templates.

## Current Interaction Model

KEY2 cycles through four screens:

1. Focus tomato screen
2. Typeless voice input screen
3. Low-brightness clock / battery / OTA dashboard
4. Hourglass countdown screen

Global controls:

- KEY2 single click: switch to the next screen.
- KEY2 double click: turn the display off. Wi-Fi, bridge polling, stopwatch,
  and countdown state continue running.
- After display-off, the first press of any key only wakes the display and does
  not also trigger start, pause, screen switching, or voice input.
- KEY2 long press: intentionally unused.

KEY1 controls depend on the active screen:

- Focus tomato screen:
  - single click: start / pause / resume TickTick stopwatch Focus
  - double click: end the current Focus session and play the harvest animation
- Typeless screen:
  - long press: start Typeless voice input
  - release after long press: stop Typeless voice input
- Hourglass screen:
  - single click: start / pause / resume countdown
  - double click: end / reset countdown

The power/reset button is deliberately not used for business logic.

## Focus Tomato Stopwatch

The Focus screen is for long stopwatch-style work, not a 25-minute countdown.
It uses `elapsed_seconds` from the bridge but keeps animation smooth locally
between network polls.

Animation rules:

- 0-5 minutes: a small green-red tomato forms and pulses quickly.
- 5-30 minutes: the tomato grows larger and turns orange-red.
- 30-90 minutes: the tomato becomes round, deep red, leafy, and glossy.
- After 90 minutes: the tomato stops growing in size and enters deep-work mode.
  It gains subtle glow rings every 30 minutes and breathes more slowly.
- Paused: the tomato keeps its current maturity, dims, and breathes lightly.
- Ending: the tomato pops with a short harvest highlight and melody.
- Offline or error: the tomato turns gray but keeps its shape.

TickTick start uses the macOS Shortcut `TickTick Start Stopwatch`. Pause,
resume, and end currently use configurable macOS UI automation hooks.

## Typeless Voice Input

The Typeless screen is independent from TickTick records.

- The microphone icon stays quiet until KEY1 is actually held.
- KEY1 long press calls `/device/voice-input-start`.
- KEY1 release calls `/device/voice-input-stop`.
- The current setup uses the Mac microphone through Typeless, not the StickS3
  microphone.

The bridge uses `TypelessKeySender.app`, a small native helper, to send the
configured Typeless shortcut. In the current setup both start and stop use:

```text
Ctrl + Cmd + Shift + Space
```

The older KEY2 long-press recording-note idea was intentionally abandoned.

## Dashboard

The dashboard is a low-brightness screen for checking device state:

- 24-hour local time
- battery percentage
- battery icon
- charging marker
- OTA address, shown as `OTA <device-ip>`

Time comes from the Mac bridge's `server_time` response and then advances
locally with `millis()` between bridge polls.

The dashboard is dim, but it is not the same as display-off. Display-off is
triggered by KEY2 double click and sets screen brightness to 0.

## Hourglass Countdown

The hourglass screen is for TickTick countdown / Pomo mode. It is separate from
the stopwatch tomato screen.

- Default duration: 25 minutes (`POMO_COUNTDOWN_SECONDS = 1500`)
- Start endpoint: `POST /pomo/start`
- Bridge shortcut: `TickTick Start Countdown`

V1 behavior:

- Starting the countdown is synced to TickTick through Shortcuts.
- Pause / resume / end are managed by the local bridge first, avoiding fragile
  TickTick desktop UI automation for those actions.

IMU posture mapping:

- X axis controls upright / inverted.
- Z axis controls flat.
- Y axis is ignored for hourglass logic.
- X-axis flip shows `FLIP` and starts or resumes countdown.
- Z-axis flat pose shows `FLAT` and pauses countdown.
- The hourglass screen rotates 180 degrees in `FLIP` pose, so the display stays
  readable while the device is upside down.

KEY1 single click and double click mirror the Focus screen's start/pause/resume
and end/reset pattern.

## Mac Bridge Quick Start

Dry-run bridge:

```bash
python3 mac-bridge/focus_bridge.py --dry-run --state-path /tmp/m5stick-focus-state.json
```

Simulate device actions:

```bash
curl -s -X POST http://127.0.0.1:8787/focus/click
curl -s -X POST http://127.0.0.1:8787/focus/double-click
curl -s -X POST http://127.0.0.1:8787/device/voice-input-start
curl -s -X POST http://127.0.0.1:8787/device/voice-input-stop
curl -s -X POST http://127.0.0.1:8787/pomo/start -H 'Content-Type: application/json' -d '{"duration_seconds":1500}'
curl -s -X POST http://127.0.0.1:8787/pomo/pause
curl -s -X POST http://127.0.0.1:8787/pomo/resume
curl -s -X POST http://127.0.0.1:8787/pomo/end
curl -s http://127.0.0.1:8787/focus/state
curl -s http://127.0.0.1:8787/pomo/state
```

Live bridge:

```bash
python3 mac-bridge/focus_bridge.py --config mac-bridge/config.local.json --live
```

The local bridge is designed to run as a macOS LaunchAgent:

```text
com.machiwhale.m5stick-focus-bridge
```

## Shortcuts Setup

Create these macOS Shortcuts:

- `TickTick Start Stopwatch`
  - TickTick action: Start Focus / Stopwatch
- `TickTick Start Countdown`
  - TickTick action: Start Pomo / Countdown, 25 minutes

Then copy the bridge config template:

```bash
cp mac-bridge/config.example.json mac-bridge/config.local.json
```

Set `dry_run` to `false` and adjust local command paths as needed.

## Firmware Setup

Copy the firmware config template:

```bash
cp firmware/include/config.example.h firmware/include/config.h
```

Edit local Wi-Fi and bridge values:

```cpp
static const char* WIFI_SSID = "your-wifi";
static const char* WIFI_PASSWORD = "your-password";
static const char* BRIDGE_BASE_URL = "http://your-mac-lan-ip:8787";
static const char* BRIDGE_TOKEN = "";
```

Build and upload over USB:

```bash
cd firmware
pio run -e m5stack-sticks3
pio run -e m5stack-sticks3 -t upload --upload-port /dev/cu.usbmodem2101
```

Build and upload over OTA:

```bash
cd firmware
pio run -e m5stack-sticks3-ota -t upload
```

If `m5stick-focus.local` cannot be resolved, switch to the dashboard screen and
use the shown IP:

```bash
pio run -e m5stack-sticks3-ota -t upload --upload-port <device-ip>
```

The PlatformIO environment uses `M5Unified`, `M5PM1`, `ArduinoOTA`,
`HTTPClient`, and `WiFi`.

## Optional Token

For LAN use, set the same token in the bridge and firmware:

```json
{
  "security": {
    "token": "replace-with-a-long-random-string"
  }
}
```

The firmware sends it as `X-Focus-Token`.

## Validation

Current V1 has been validated with:

- PlatformIO build
- USB upload
- Wi-Fi OTA upload
- bridge unit tests
- unplugged Wi-Fi control of TickTick and Typeless
- TickTick stopwatch start / pause / resume / end
- Typeless long-press start and release stop
- dashboard time / battery / OTA display
- hourglass X-axis flip start, Z-axis flat pause, auto-rotate UI, and TickTick
  countdown start
- KEY2 double-click display-off and first-key wake behavior

Run bridge tests:

```bash
python3 -m unittest mac-bridge/test_focus_bridge.py
```

## Files

- `firmware/src/main.cpp`: StickS3 firmware.
- `firmware/include/config.example.h`: clean firmware config template.
- `firmware/platformio.ini`: PlatformIO build config.
- `mac-bridge/focus_bridge.py`: local HTTP service and TickTick / Typeless
  controller.
- `mac-bridge/ticktick_focus_ui.py`: macOS UI automation helper for TickTick
  Focus controls.
- `mac-bridge/test_focus_bridge.py`: state-machine tests.
- `mac-bridge/config.example.json`: bridge configuration template.
- `mac-bridge/SHORTCUTS.md`: notes for TickTick macOS Shortcuts.
- `mac-bridge/send_typeless_key.c`: source for the native Typeless key helper.
