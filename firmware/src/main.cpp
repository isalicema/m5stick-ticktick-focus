#include <Arduino.h>
#include <ArduinoOTA.h>
#include <ctype.h>
#include <HTTPClient.h>
#include <M5Unified.h>
#include <math.h>
#include <WiFi.h>

#include "config.h"

static const uint8_t DISPLAY_BRIGHTNESS_SLEEP_LEVEL = 0;

enum class TimerMode {
  Stopwatch
};

enum class BridgeState {
  Idle,
  Running,
  Paused,
  Ending,
  Offline,
  Error
};

enum class ScreenMode {
  Focus,
  Voice,
  Dashboard,
  Hourglass
};

enum class CountdownState {
  Ready,
  Running,
  Paused,
  Done,
  Offline,
  Error
};

enum class HourglassPose {
  Unknown,
  Upright,
  Inverted,
  Flat
};

struct ButtonTracker {
  explicit ButtonTracker(int pin) : pin(pin) {}

  int pin;
  bool stablePressed = false;
  bool lastRawPressed = false;
  bool longPressReported = false;
  unsigned long lastChangeMs = 0;
  unsigned long pressedAtMs = 0;

  void begin() {
    pinMode(pin, INPUT_PULLUP);
    bool raw = readRaw();
    stablePressed = raw;
    lastRawPressed = raw;
    lastChangeMs = millis();
  }

  bool readRaw() const {
    return digitalRead(pin) == LOW;
  }

  void update() {
    bool raw = readRaw();
    unsigned long now = millis();
    if (raw != lastRawPressed) {
      lastRawPressed = raw;
      lastChangeMs = now;
    }
    if ((now - lastChangeMs) >= DEBOUNCE_MS && raw != stablePressed) {
      stablePressed = raw;
      if (stablePressed) {
        pressedAtMs = now;
        longPressReported = false;
      }
    }
  }

  bool wasReleased() const {
    return !stablePressed && lastRawPressed == stablePressed;
  }

  bool consumeLongPress() {
    if (!stablePressed || longPressReported) {
      return false;
    }
    if ((millis() - pressedAtMs) >= KEY_LONG_PRESS_MS) {
      longPressReported = true;
      return true;
    }
    return false;
  }
};

ButtonTracker key1(KEY1_PIN);
ButtonTracker key2(KEY2_PIN);

TimerMode timerMode = TimerMode::Stopwatch;
ScreenMode screenMode = ScreenMode::Focus;
BridgeState currentState = BridgeState::Offline;
String currentStateText = "offline";
String lastMessage = "booting";
String lastError = "";
CountdownState countdownState = CountdownState::Ready;
String countdownStateText = "idle";
String countdownMessage = "flip to start";
String countdownError = "";
uint32_t countdownDurationSeconds = POMO_COUNTDOWN_SECONDS;
uint32_t countdownRemainingSeconds = POMO_COUNTDOWN_SECONDS;
unsigned long lastCountdownSyncMs = 0;
HourglassPose rawHourglassPose = HourglassPose::Unknown;
HourglassPose candidateHourglassPose = HourglassPose::Unknown;
HourglassPose stableHourglassPose = HourglassPose::Unknown;
unsigned long hourglassPoseChangedAtMs = 0;
unsigned long lastHourglassActionMs = 0;
bool hourglassImuAvailable = false;
bool countdownDoneSoundPlayed = false;

uint32_t bridgeElapsedSeconds = 0;
unsigned long lastElapsedSyncMs = 0;
uint32_t displayElapsedSeconds = 0;
unsigned long lastFrameMs = 0;
bool key1WasPressed = false;
bool key2WasPressed = false;
bool pendingSingleClick = false;
bool pendingKey2SingleClick = false;
bool voiceListening = false;
unsigned long firstClickAtMs = 0;
unsigned long firstKey2ClickAtMs = 0;
unsigned long lastStatePollMs = 0;
unsigned long lastWifiAttemptMs = 0;
unsigned long harvestStartedAtMs = 0;
unsigned long lastInteractionMs = 0;
bool displayDimmed = false;
bool displaySleeping = false;
bool key1WakeOnly = false;
bool key2WakeOnly = false;
uint8_t currentDisplayBrightness = 255;
uint8_t currentScreenRotation = TOMATO_SCREEN_ROTATION;
bool clockSynced = false;
uint32_t clockSecondsOfDay = 0;
unsigned long lastClockSyncMs = 0;

void drawStatus();
int32_t batteryLevel = -1;
int32_t batteryVoltageMv = -1;
bool batteryLevelEstimated = false;
bool batteryCharging = false;
unsigned long lastBatteryReadMs = 0;
bool otaReady = false;
bool otaActive = false;
uint8_t otaProgress = 0;
String otaStatusText = "OTA ready";
unsigned long lastOtaDrawMs = 0;
M5Canvas canvas(&M5.Display);

struct SoundStep {
  int frequency;
  uint16_t durationMs;
  uint16_t gapMs;
};

const SoundStep HARVEST_MELODY[] = {
    {988, 70, 22},
    {1319, 80, 24},
    {1568, 90, 28},
    {2093, 130, 0},
};

const SoundStep START_MELODY[] = {
    {784, 55, 18},
    {1175, 80, 0},
};

const SoundStep PAUSE_MELODY[] = {
    {784, 70, 20},
    {523, 95, 0},
};

const SoundStep RESUME_MELODY[] = {
    {659, 50, 14},
    {988, 70, 0},
};

const SoundStep BOOT_MELODY[] = {
    {1175, 45, 12},
    {1568, 55, 0},
};

bool harvestSoundPlaying = false;
uint8_t harvestSoundIndex = 0;
unsigned long nextHarvestSoundMs = 0;
const SoundStep* activeMelody = nullptr;
size_t activeMelodyLength = 0;
uint8_t activeMelodyIndex = 0;
unsigned long nextSoundStepMs = 0;

String endpointUrl(const char* path) {
  String base = String(BRIDGE_BASE_URL);
  if (base.endsWith("/")) {
    base.remove(base.length() - 1);
  }
  return base + path;
}

void beep(int frequency, int durationMs) {
  if (!SOUND_ENABLED) {
    return;
  }
  M5.Speaker.tone(frequency, durationMs, 0, true);
}

bool isActiveFocusState(BridgeState state) {
  return state == BridgeState::Running || state == BridgeState::Paused || state == BridgeState::Ending;
}

bool shouldDimDisplay() {
  if (!DISPLAY_PROTECTION_ENABLED) {
    return false;
  }
  if (currentState == BridgeState::Running || countdownState == CountdownState::Running || harvestStartedAtMs > 0) {
    return false;
  }
  return millis() - lastInteractionMs >= DISPLAY_DIM_AFTER_MS;
}

void updateDisplayBrightness() {
  if (displaySleeping) {
    displayDimmed = false;
    if (currentDisplayBrightness != DISPLAY_BRIGHTNESS_SLEEP_LEVEL) {
      currentDisplayBrightness = DISPLAY_BRIGHTNESS_SLEEP_LEVEL;
      M5.Display.setBrightness(DISPLAY_BRIGHTNESS_SLEEP_LEVEL);
    }
    return;
  }

  if (screenMode == ScreenMode::Dashboard) {
    displayDimmed = false;
    if (currentDisplayBrightness != DISPLAY_BRIGHTNESS_DASHBOARD) {
      currentDisplayBrightness = DISPLAY_BRIGHTNESS_DASHBOARD;
      M5.Display.setBrightness(DISPLAY_BRIGHTNESS_DASHBOARD);
    }
    return;
  }

  bool shouldDim = shouldDimDisplay();
  uint8_t brightness = shouldDim ? DISPLAY_BRIGHTNESS_DIM : DISPLAY_BRIGHTNESS_ACTIVE;
  if (shouldDim == displayDimmed && currentDisplayBrightness == brightness) {
    return;
  }
  displayDimmed = shouldDim;
  if (currentDisplayBrightness != brightness) {
    currentDisplayBrightness = brightness;
    M5.Display.setBrightness(brightness);
  }
}

void markInteraction() {
  if (displaySleeping) {
    return;
  }
  lastInteractionMs = millis();
  if (screenMode != ScreenMode::Dashboard && (displayDimmed || currentDisplayBrightness != DISPLAY_BRIGHTNESS_ACTIVE)) {
    displayDimmed = false;
    currentDisplayBrightness = DISPLAY_BRIGHTNESS_ACTIVE;
    M5.Display.setBrightness(DISPLAY_BRIGHTNESS_ACTIVE);
  }
}

void enterDisplaySleep() {
  pendingSingleClick = false;
  pendingKey2SingleClick = false;
  key1WakeOnly = false;
  key2WakeOnly = false;
  displaySleeping = true;
  displayDimmed = false;
  currentDisplayBrightness = DISPLAY_BRIGHTNESS_SLEEP_LEVEL;
  M5.Display.setBrightness(DISPLAY_BRIGHTNESS_SLEEP_LEVEL);
}

void wakeDisplay() {
  displaySleeping = false;
  displayDimmed = false;
  lastInteractionMs = millis();
  updateDisplayBrightness();
  lastFrameMs = 0;
  drawStatus();
  beep(880, 45);
}

void triggerHarvestSound() {
  if (!SOUND_ENABLED || !HARVEST_SOUND_ENABLED) {
    return;
  }
  harvestSoundPlaying = true;
  harvestSoundIndex = 0;
  nextHarvestSoundMs = 0;
}

void triggerMelody(const SoundStep* melody, size_t length) {
  if (!SOUND_ENABLED || melody == nullptr || length == 0) {
    return;
  }
  activeMelody = melody;
  activeMelodyLength = length;
  activeMelodyIndex = 0;
  nextSoundStepMs = 0;
}

void triggerStateSound(BridgeState before, BridgeState after) {
  if (after == BridgeState::Paused) {
    triggerMelody(PAUSE_MELODY, sizeof(PAUSE_MELODY) / sizeof(PAUSE_MELODY[0]));
  } else if (after == BridgeState::Running && before == BridgeState::Paused) {
    triggerMelody(RESUME_MELODY, sizeof(RESUME_MELODY) / sizeof(RESUME_MELODY[0]));
  } else if (after == BridgeState::Running) {
    triggerMelody(START_MELODY, sizeof(START_MELODY) / sizeof(START_MELODY[0]));
  } else {
    beep(1200, 55);
  }
}

void updateMelody() {
  if (activeMelody == nullptr) {
    return;
  }
  unsigned long now = millis();
  if (now < nextSoundStepMs) {
    return;
  }
  if (activeMelodyIndex >= activeMelodyLength) {
    activeMelody = nullptr;
    activeMelodyLength = 0;
    activeMelodyIndex = 0;
    return;
  }
  const SoundStep& step = activeMelody[activeMelodyIndex++];
  M5.Speaker.tone(step.frequency, step.durationMs, 0, true);
  nextSoundStepMs = now + step.durationMs + step.gapMs;
}

void updateHarvestSound() {
  if (!harvestSoundPlaying) {
    return;
  }
  unsigned long now = millis();
  if (now < nextHarvestSoundMs) {
    return;
  }
  if (harvestSoundIndex >= sizeof(HARVEST_MELODY) / sizeof(HARVEST_MELODY[0])) {
    harvestSoundPlaying = false;
    return;
  }
  const SoundStep& step = HARVEST_MELODY[harvestSoundIndex++];
  M5.Speaker.tone(step.frequency, step.durationMs, 0, true);
  nextHarvestSoundMs = now + step.durationMs + step.gapMs;
}

String extractJsonString(const String& body, const char* key) {
  String needle = String("\"") + key + "\":";
  int keyIndex = body.indexOf(needle);
  if (keyIndex < 0) {
    return "";
  }
  int valueStart = body.indexOf('"', keyIndex + needle.length());
  if (valueStart < 0) {
    return "";
  }
  int valueEnd = body.indexOf('"', valueStart + 1);
  if (valueEnd < 0) {
    return "";
  }
  return body.substring(valueStart + 1, valueEnd);
}

uint32_t extractJsonUInt(const String& body, const char* key) {
  String needle = String("\"") + key + "\":";
  int keyIndex = body.indexOf(needle);
  if (keyIndex < 0) {
    return 0;
  }
  int valueStart = keyIndex + needle.length();
  while (valueStart < (int)body.length() && isspace(body[valueStart])) {
    valueStart++;
  }
  int valueEnd = valueStart;
  while (valueEnd < (int)body.length() && isdigit(body[valueEnd])) {
    valueEnd++;
  }
  if (valueEnd == valueStart) {
    return 0;
  }
  return body.substring(valueStart, valueEnd).toInt();
}

BridgeState parseState(const String& state) {
  if (state == "idle") return BridgeState::Idle;
  if (state == "running") return BridgeState::Running;
  if (state == "paused") return BridgeState::Paused;
  if (state == "ending") return BridgeState::Ending;
  return BridgeState::Error;
}

CountdownState parseCountdownState(const String& state) {
  if (state == "idle") return CountdownState::Ready;
  if (state == "running") return CountdownState::Running;
  if (state == "paused") return CountdownState::Paused;
  if (state == "done") return CountdownState::Done;
  return CountdownState::Error;
}

uint16_t colorForState(BridgeState state) {
  switch (state) {
    case BridgeState::Idle:
      return TFT_DARKGREY;
    case BridgeState::Running:
      return TFT_GREEN;
    case BridgeState::Paused:
      return TFT_ORANGE;
    case BridgeState::Ending:
      return TFT_BLUE;
    case BridgeState::Offline:
      return TFT_RED;
    case BridgeState::Error:
    default:
  return TFT_MAGENTA;
  }
}

float clamp01(float value) {
  if (value < 0.0f) return 0.0f;
  if (value > 1.0f) return 1.0f;
  return value;
}

float lerp(float from, float to, float amount) {
  return from + (to - from) * amount;
}

uint8_t lerpByte(uint8_t from, uint8_t to, float amount) {
  return (uint8_t)roundf(lerp(from, to, clamp01(amount)));
}

uint16_t rgb565(uint8_t red, uint8_t green, uint8_t blue) {
  return M5.Display.color565(red, green, blue);
}

uint16_t blendColor(uint16_t from, uint16_t to, float amount) {
  amount = clamp01(amount);
  uint8_t r1 = ((from >> 11) & 0x1F) * 255 / 31;
  uint8_t g1 = ((from >> 5) & 0x3F) * 255 / 63;
  uint8_t b1 = (from & 0x1F) * 255 / 31;
  uint8_t r2 = ((to >> 11) & 0x1F) * 255 / 31;
  uint8_t g2 = ((to >> 5) & 0x3F) * 255 / 63;
  uint8_t b2 = (to & 0x1F) * 255 / 31;
  return rgb565(lerpByte(r1, r2, amount), lerpByte(g1, g2, amount), lerpByte(b1, b2, amount));
}

uint16_t dimColor(uint16_t color, float factor) {
  factor = clamp01(factor);
  uint8_t red = ((color >> 11) & 0x1F) * 255 / 31;
  uint8_t green = ((color >> 5) & 0x3F) * 255 / 63;
  uint8_t blue = (color & 0x1F) * 255 / 31;
  return rgb565(red * factor, green * factor, blue * factor);
}

String labelForState(BridgeState state) {
  switch (state) {
    case BridgeState::Idle:
      return "READY";
    case BridgeState::Running:
      return "FOCUS";
    case BridgeState::Paused:
      return "PAUSED";
    case BridgeState::Ending:
      return "HARVEST";
    case BridgeState::Offline:
      return "OFFLINE";
    case BridgeState::Error:
    default:
      return "ERROR";
  }
}

String labelForCountdownState(CountdownState state) {
  switch (state) {
    case CountdownState::Ready:
      return "POMO";
    case CountdownState::Running:
      return "SAND";
    case CountdownState::Paused:
      return "PAUSED";
    case CountdownState::Done:
      return "DONE";
    case CountdownState::Offline:
      return "OFFLINE";
    case CountdownState::Error:
    default:
      return "ERROR";
  }
}

uint16_t colorForCountdownState(CountdownState state) {
  switch (state) {
    case CountdownState::Ready:
      return rgb565(178, 146, 86);
    case CountdownState::Running:
      return rgb565(238, 186, 84);
    case CountdownState::Paused:
      return TFT_ORANGE;
    case CountdownState::Done:
      return rgb565(96, 224, 168);
    case CountdownState::Offline:
      return TFT_RED;
    case CountdownState::Error:
    default:
      return TFT_MAGENTA;
  }
}

String labelForHourglassPose(HourglassPose pose) {
  switch (pose) {
    case HourglassPose::Upright:
      return "UP";
    case HourglassPose::Inverted:
      return "FLIP";
    case HourglassPose::Flat:
      return "FLAT";
    case HourglassPose::Unknown:
    default:
      return "MOVE";
  }
}

uint32_t currentDisplayElapsedSeconds() {
  if (currentState != BridgeState::Running || lastElapsedSyncMs == 0) {
    return bridgeElapsedSeconds;
  }
  return bridgeElapsedSeconds + ((millis() - lastElapsedSyncMs) / 1000);
}

uint32_t currentCountdownRemainingSeconds() {
  if (countdownState != CountdownState::Running || lastCountdownSyncMs == 0) {
    return countdownRemainingSeconds;
  }
  uint32_t elapsed = (millis() - lastCountdownSyncMs) / 1000;
  if (elapsed >= countdownRemainingSeconds) {
    return 0;
  }
  return countdownRemainingSeconds - elapsed;
}

void updateCountdownCompletion() {
  if (countdownState != CountdownState::Running) {
    if (countdownState != CountdownState::Done) {
      countdownDoneSoundPlayed = false;
    }
    return;
  }
  if (currentCountdownRemainingSeconds() > 0) {
    return;
  }
  countdownRemainingSeconds = 0;
  countdownState = CountdownState::Done;
  countdownStateText = "done";
  countdownMessage = "done";
  countdownError = "";
  if (!countdownDoneSoundPlayed) {
    countdownDoneSoundPlayed = true;
    triggerHarvestSound();
  }
  drawStatus();
}

int parseTwoDigits(const String& value, int index) {
  if (index < 0 || index + 1 >= (int)value.length()) {
    return -1;
  }
  if (!isdigit(value[index]) || !isdigit(value[index + 1])) {
    return -1;
  }
  return (value[index] - '0') * 10 + (value[index + 1] - '0');
}

bool updateClockFromServerTime(const String& serverTime) {
  int timeMarker = serverTime.indexOf('T');
  if (timeMarker < 0) {
    timeMarker = serverTime.indexOf(' ');
  }
  int hour = parseTwoDigits(serverTime, timeMarker + 1);
  int minute = parseTwoDigits(serverTime, timeMarker + 4);
  int second = parseTwoDigits(serverTime, timeMarker + 7);
  if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) {
    return false;
  }
  clockSecondsOfDay = hour * 3600UL + minute * 60UL + second;
  lastClockSyncMs = millis();
  clockSynced = true;
  return true;
}

uint32_t currentClockSecondsOfDay() {
  if (!clockSynced) {
    return 0;
  }
  return (clockSecondsOfDay + ((millis() - lastClockSyncMs) / 1000)) % 86400UL;
}

String formatClockTime() {
  if (!clockSynced) {
    return "--:--";
  }
  uint32_t seconds = currentClockSecondsOfDay();
  uint8_t hour = seconds / 3600UL;
  uint8_t minute = (seconds % 3600UL) / 60UL;
  char buffer[6];
  snprintf(buffer, sizeof(buffer), "%02u:%02u", hour, minute);
  return String(buffer);
}

int32_t interpolateBatteryPercent(int32_t mv, int32_t lowMv, int32_t highMv, int32_t lowPercent, int32_t highPercent) {
  if (mv <= lowMv) {
    return lowPercent;
  }
  if (mv >= highMv) {
    return highPercent;
  }
  return lowPercent + ((mv - lowMv) * (highPercent - lowPercent)) / (highMv - lowMv);
}

int32_t estimateBatteryLevelFromVoltage(int32_t mv) {
  if (mv <= 0) {
    return -1;
  }
  if (mv <= 3200) {
    return 1;
  }
  if (mv < 3300) {
    return interpolateBatteryPercent(mv, 3200, 3300, 1, 5);
  }
  if (mv < 3600) {
    return interpolateBatteryPercent(mv, 3300, 3600, 5, 18);
  }
  if (mv < 3700) {
    return interpolateBatteryPercent(mv, 3600, 3700, 18, 32);
  }
  if (mv < 3800) {
    return interpolateBatteryPercent(mv, 3700, 3800, 32, 52);
  }
  if (mv < 3900) {
    return interpolateBatteryPercent(mv, 3800, 3900, 52, 72);
  }
  if (mv < 4050) {
    return interpolateBatteryPercent(mv, 3900, 4050, 72, 90);
  }
  if (mv < 4200) {
    return interpolateBatteryPercent(mv, 4050, 4200, 90, 100);
  }
  return 100;
}

void refreshBatteryStatus(bool force = false) {
  unsigned long now = millis();
  if (!force && lastBatteryReadMs > 0 && now - lastBatteryReadMs < BATTERY_REFRESH_MS) {
    return;
  }
  int32_t voltageMv = M5.Power.getBatteryVoltage();
  batteryVoltageMv = voltageMv > 0 ? voltageMv : -1;
  batteryLevelEstimated = false;

  int32_t level = M5.Power.getBatteryLevel();
  if (level <= 0 && batteryVoltageMv > 0) {
    batteryLevel = estimateBatteryLevelFromVoltage(batteryVoltageMv);
    batteryLevelEstimated = true;
  } else if (level < 0) {
    batteryLevel = -1;
  } else if (level == 0) {
    batteryLevel = -1;
  } else {
    if (level > 100) {
      level = 100;
    }
    batteryLevel = level;
  }
  batteryCharging = static_cast<int>(M5.Power.isCharging()) == 1;
  lastBatteryReadMs = now;
}

BridgeState displayState() {
  if (harvestStartedAtMs > 0) {
    if (millis() - harvestStartedAtMs < HARVEST_ANIMATION_MS) {
      return BridgeState::Ending;
    }
    harvestStartedAtMs = 0;
  }
  return currentState;
}

class TomatoAnimator {
 public:
  void begin() {
    canvas.setColorDepth(16);
    canvas.createSprite(M5.Display.width(), M5.Display.height());
    canvas.setTextDatum(middle_center);
    canvas.setFont(&fonts::Font2);
  }

  void draw(BridgeState state, uint32_t elapsedSeconds, const String& message, const String& error) {
    displayElapsedSeconds = elapsedSeconds;
    unsigned long now = millis();
    float seconds = now / 1000.0f;
    float stage = stageProgress(elapsedSeconds);
    float pulse = pulseAmount(state, elapsedSeconds, seconds);

    canvas.setFont(&fonts::Font2);
    canvas.setTextDatum(middle_center);
    canvas.fillScreen(rgb565(8, 10, 12));
    int shiftX = protectionShiftX();
    int shiftY = protectionShiftY();
    drawTopLabel(state, shiftX, shiftY);
    drawTomato(state, stage, pulse, elapsedSeconds, seconds, shiftX, shiftY);
    drawStageLine(state, elapsedSeconds, shiftX, shiftY);
    drawBottomText(state, message, error, shiftX, shiftY);
    canvas.pushSprite(0, 0);
  }

  void drawVoice(bool listening, const String& message, const String& error) {
    unsigned long now = millis();
    float seconds = now / 1000.0f;
    float pulse = listening ? 0.5f + 0.5f * sinf(seconds * 4.2f) : 0.0f;
    canvas.setFont(&fonts::Font2);
    canvas.setTextDatum(middle_center);
    canvas.fillScreen(rgb565(7, 10, 14));
    int shiftX = protectionShiftX();
    int shiftY = protectionShiftY();
    drawVoiceTopLabel(listening, shiftX, shiftY);
    drawVoiceGlyph(listening, pulse, shiftX, shiftY);
    drawVoiceBottomText(listening, message, error, shiftX, shiftY);
    canvas.pushSprite(0, 0);
  }

  void drawDashboard(const String& timeText, int32_t batteryPercent, bool charging, bool synced, const String& networkText, bool wifiConnected) {
    canvas.setTextDatum(middle_center);
    canvas.fillScreen(dashboardBg());
    int shiftX = protectionShiftX();
    int shiftY = protectionShiftY();
    drawDashboardTopLabel(wifiConnected, shiftX, shiftY);
    drawDashboardClock(timeText, synced, shiftX, shiftY);
    drawDashboardBattery(batteryPercent, batteryVoltageMv, batteryLevelEstimated, charging, shiftX, shiftY);
    drawDashboardNetwork(networkText, shiftX, shiftY);
    canvas.pushSprite(0, 0);
  }

  void drawHourglass(
      CountdownState state,
      uint32_t remainingSeconds,
      uint32_t durationSeconds,
      HourglassPose pose,
      const String& message,
      const String& error) {
    unsigned long now = millis();
    float seconds = now / 1000.0f;
    float progress = durationSeconds == 0 ? 0.0f : 1.0f - (remainingSeconds / (float)durationSeconds);
    progress = clamp01(progress);

    canvas.setFont(&fonts::Font2);
    canvas.setTextDatum(middle_center);
    canvas.fillScreen(hourglassBg());
    int shiftX = protectionShiftX();
    int shiftY = protectionShiftY();
    drawHourglassTopLabel(state, pose, shiftX, shiftY);
    drawHourglassBody(state, progress, seconds, shiftX, shiftY);
    drawHourglassTime(remainingSeconds, state, shiftX, shiftY);
    drawHourglassBottomText(state, message, error, shiftX, shiftY);
    canvas.pushSprite(0, 0);
  }

  void drawOta(uint8_t progress, const String& statusText) {
    canvas.setTextDatum(middle_center);
    canvas.setFont(&fonts::Font2);
    canvas.setTextSize(1);
    canvas.fillScreen(rgb565(3, 5, 8));
    int cx = canvas.width() / 2;
    int cy = canvas.height() / 2;

    canvas.setTextColor(rgb565(100, 214, 190), rgb565(3, 5, 8));
    canvas.drawString("OTA", cx, topLabelY());

    canvas.setFont(&fonts::Font4);
    canvas.setTextColor(rgb565(198, 236, 230), rgb565(3, 5, 8));
    canvas.drawString(String(progress) + "%", cx, cy - 8);

    int w = canvas.width() - 36;
    int x = (canvas.width() - w) / 2;
    int y = cy + 32;
    canvas.fillRoundRect(x, y, w, 6, 3, rgb565(24, 34, 40));
    canvas.fillRoundRect(x, y, w * progress / 100, 6, 3, rgb565(90, 220, 174));

    canvas.setFont(&fonts::Font2);
    canvas.setTextSize(1);
    canvas.setTextColor(rgb565(116, 146, 154), rgb565(3, 5, 8));
    canvas.drawString(statusText, cx, bottomTextY());
    canvas.pushSprite(0, 0);
  }

 private:
  float stageProgress(uint32_t elapsedSeconds) {
    float progress = elapsedSeconds / (float)STOPWATCH_DEEP_WORK_SECONDS;
    return clamp01(progress);
  }

  float pulseAmount(BridgeState state, uint32_t elapsedSeconds, float seconds) {
    if (state == BridgeState::Idle || state == BridgeState::Offline || state == BridgeState::Error) {
      return 0.0f;
    }
    if (state == BridgeState::Ending) {
      return 0.14f * sinf(seconds * 16.0f);
    }
    float pace = elapsedSeconds < 300 ? 7.2f : (elapsedSeconds < 1800 ? 5.4f : 3.6f);
    float amount = state == BridgeState::Paused ? TOMATO_MAX_PULSE * 0.18f : TOMATO_MAX_PULSE;
    return amount * (0.5f + 0.5f * sinf(seconds * pace));
  }

  uint16_t tomatoColor(BridgeState state, float stage, uint32_t elapsedSeconds) {
    uint16_t young = rgb565(104, 156, 72);
    uint16_t forming = rgb565(206, 84, 50);
    uint16_t ripe = rgb565(222, 36, 42);
    uint16_t deep = rgb565(170, 22, 35);

    uint16_t color = stage < 0.32f
                         ? blendColor(young, forming, stage / 0.32f)
                         : blendColor(forming, ripe, (stage - 0.32f) / 0.68f);
    if (elapsedSeconds > STOPWATCH_DEEP_WORK_SECONDS) {
      float extra = clamp01((elapsedSeconds - STOPWATCH_DEEP_WORK_SECONDS) / 7200.0f);
      color = blendColor(color, deep, extra * 0.45f);
    }
    if (state == BridgeState::Paused) {
      color = dimColor(color, 0.62f);
    }
    if (state == BridgeState::Offline || state == BridgeState::Error) {
      color = rgb565(82, 86, 88);
    }
    if (state == BridgeState::Idle) {
      color = rgb565(94, 42, 38);
    }
    return color;
  }

  void drawTopLabel(BridgeState state, int shiftX, int shiftY) {
    canvas.setTextSize(canvas.height() > canvas.width() ? 2 : 1);
    canvas.setTextColor(colorForState(state), rgb565(8, 10, 12));
    canvas.drawString(labelForState(state), canvas.width() / 2 + shiftX, topLabelY() + shiftY);
  }

  void drawTomato(BridgeState state, float stage, float pulse, uint32_t elapsedSeconds, float seconds, int shiftX, int shiftY) {
    int cx = canvas.width() / 2 + shiftX;
    int cy = tomatoCenterY() + shiftY;
    float minRadius = tomatoMinRadius();
    float maxRadius = tomatoMaxRadius();
    float baseRadius = lerp(minRadius, maxRadius, stage);
    if (state == BridgeState::Idle) {
      baseRadius = minRadius;
    }
    if (state == BridgeState::Offline || state == BridgeState::Error) {
      baseRadius = baseRadius * 0.78f;
      float offlineMin = minRadius * 1.28f;
      if (baseRadius < offlineMin) {
        baseRadius = offlineMin;
      }
    }
    float radius = baseRadius * (1.0f + pulse);
    if (state == BridgeState::Ending) {
      radius = baseRadius * (1.08f + 0.08f * sinf(seconds * 18.0f));
    }

    int shadowY = cy + radius * 0.82f;
    canvas.fillEllipse(cx, shadowY, radius * 0.9f, 6, rgb565(17, 18, 18));

    drawMilestoneGlow(cx, cy, radius, elapsedSeconds, state);

    uint16_t body = tomatoColor(state, stage, elapsedSeconds);
    canvas.fillEllipse(cx, cy + 2, radius * 1.04f, radius * 0.92f, dimColor(body, 0.78f));
    canvas.fillEllipse(cx - radius * 0.13f, cy, radius, radius * 0.9f, body);
    canvas.fillEllipse(cx + radius * 0.25f, cy + 4, radius * 0.82f, radius * 0.75f, dimColor(body, 0.86f));

    drawLeaf(cx, cy - radius * 0.78f, radius, state, stage);
    drawHighlights(cx, cy, radius, state, stage);
  }

  void drawMilestoneGlow(int cx, int cy, float radius, uint32_t elapsedSeconds, BridgeState state) {
    if (state != BridgeState::Running && state != BridgeState::Paused && state != BridgeState::Ending) {
      return;
    }
    uint32_t milestoneCount = elapsedSeconds / STOPWATCH_MILESTONE_SECONDS;
    if (milestoneCount == 0) {
      return;
    }
    uint8_t rings = milestoneCount > 4 ? 4 : milestoneCount;
    for (uint8_t i = 0; i < rings; i++) {
      float ringRadius = radius + 7 + i * 5;
      uint16_t color = dimColor(rgb565(255, 130, 68), state == BridgeState::Paused ? 0.25f : 0.42f - i * 0.06f);
      canvas.drawEllipse(cx, cy + 1, ringRadius * 1.06f, ringRadius * 0.92f, color);
    }
  }

  void drawLeaf(int cx, int leafY, float radius, BridgeState state, float stage) {
    uint16_t leaf = state == BridgeState::Offline || state == BridgeState::Error
                        ? rgb565(72, 78, 72)
                        : blendColor(rgb565(66, 126, 60), rgb565(36, 104, 54), stage);
    if (state == BridgeState::Paused) {
      leaf = dimColor(leaf, 0.58f);
    }
    int spread = (int)(radius * 0.46f);
    int height = (int)(radius * 0.24f);
    if (spread < 8) spread = 8;
    if (height < 7) height = 7;
    canvas.fillTriangle(cx, leafY - height, cx - spread, leafY + height / 2, cx - 3, leafY + 3, leaf);
    canvas.fillTriangle(cx, leafY - height - 2, cx + spread, leafY + height / 2, cx + 3, leafY + 3, leaf);
    canvas.fillTriangle(cx, leafY - height - 3, cx - spread / 2, leafY - 2, cx + spread / 2, leafY - 2, dimColor(leaf, 1.12f));
  }

  void drawHighlights(int cx, int cy, float radius, BridgeState state, float stage) {
    if (state == BridgeState::Offline || state == BridgeState::Error) {
      return;
    }
    uint16_t highlight = state == BridgeState::Paused ? rgb565(118, 94, 88) : rgb565(255, 184, 156);
    int highRx = (int)(radius * 0.13f);
    int highRy = (int)(radius * 0.2f);
    if (highRx < 3) highRx = 3;
    if (highRy < 2) highRy = 2;
    canvas.fillEllipse(cx - radius * 0.36f, cy - radius * 0.28f, highRx, highRy, highlight);
    if (stage > 0.55f) {
      canvas.drawArc(cx + radius * 0.12f, cy + radius * 0.12f, radius * 0.5f, radius * 0.5f, 20, 85, dimColor(highlight, 0.68f));
    }
  }

  void drawStageLine(BridgeState state, uint32_t elapsedSeconds, int shiftX, int shiftY) {
    int x = canvas.width() / 8;
    if (x < 12) x = 12;
    int y = stageLineY() + shiftY;
    int w = canvas.width() - 48;
    w = canvas.width() - (x * 2);
    x += shiftX;
    int h = 4;
    float progress = stageProgress(elapsedSeconds);
    canvas.fillRoundRect(x, y, w, h, 2, rgb565(30, 34, 36));
    uint16_t line = state == BridgeState::Paused ? rgb565(130, 86, 54) : rgb565(218, 72, 46);
    if (state == BridgeState::Offline || state == BridgeState::Error) {
      line = rgb565(78, 82, 84);
    }
    int fillWidth = (int)(w * progress);
    if (fillWidth < 4) fillWidth = 4;
    canvas.fillRoundRect(x, y, fillWidth, h, 2, line);
    for (int i = 1; i <= 3; i++) {
      int markX = x + (w * i) / 3;
      canvas.drawFastVLine(markX, y - 2, h + 4, rgb565(62, 66, 68));
    }
  }

  void drawBottomText(BridgeState state, const String& message, const String& error, int shiftX, int shiftY) {
    String text = bottomStatusText(state, message, error);
    canvas.setTextSize(1);
    canvas.setTextColor(state == BridgeState::Error ? TFT_RED : TFT_DARKGREY, rgb565(8, 10, 12));
    drawWrappedCenterText(text, canvas.width() / 2 + shiftX, bottomTextY() + shiftY, bottomLineChars(), bottomLineCount(), bottomLineHeight());
  }

  void drawVoiceTopLabel(bool listening, int shiftX, int shiftY) {
    canvas.setTextSize(canvas.height() > canvas.width() ? 2 : 1);
    canvas.setTextColor(listening ? rgb565(94, 230, 160) : rgb565(94, 210, 230), rgb565(7, 10, 14));
    canvas.drawString(listening ? "LISTEN" : "VOICE", canvas.width() / 2 + shiftX, topLabelY() + shiftY);
  }

  void drawVoiceGlyph(bool listening, float pulse, int shiftX, int shiftY) {
    int cx = canvas.width() / 2 + shiftX;
    int cy = tomatoCenterY() + shiftY + 4;
    int micW = canvas.width() > 130 ? 34 : 28;
    int micH = canvas.height() > canvas.width() ? 58 : 40;
    uint16_t body = listening ? rgb565(92, 230, 170) : rgb565(76, 126, 142);
    uint16_t bodyDim = listening ? rgb565(32, 92, 74) : rgb565(28, 48, 58);
    uint16_t glow = dimColor(listening ? rgb565(78, 230, 170) : rgb565(48, 84, 98), listening ? 0.3f + pulse * 0.35f : 0.22f);

    if (listening) {
      for (int i = 0; i < 3; i++) {
        int radius = (int)(micH * 0.58f + i * 9 + pulse * 5);
        canvas.drawEllipse(cx, cy + 1, radius, radius * 0.72f, dimColor(glow, 0.72f - i * 0.16f));
      }
    } else {
      canvas.drawEllipse(cx, cy + 1, micH * 0.58f, micH * 0.42f, glow);
    }

    canvas.fillRoundRect(cx - micW / 2, cy - micH / 2, micW, micH, micW / 2, bodyDim);
    canvas.fillRoundRect(cx - micW / 2 + 3, cy - micH / 2 + 3, micW - 6, micH - 6, (micW - 6) / 2, body);
    canvas.fillRoundRect(cx - 4, cy + micH / 2 - 2, 8, 20, 4, rgb565(84, 156, 178));
    canvas.drawFastHLine(cx - 22, cy + micH / 2 + 18, 44, rgb565(84, 156, 178));

    int waveBaseY = cy + micH / 2 + 34;
    if (listening) {
      for (int i = 0; i < 5; i++) {
        int barH = 6 + (int)(18 * (0.5f + 0.5f * sinf((millis() / 280.0f) + i * 0.9f)));
        int x = cx - 28 + i * 14;
        canvas.fillRoundRect(x, waveBaseY - barH / 2, 6, barH, 3, rgb565(72, 214, 174));
      }
    } else {
      canvas.fillRoundRect(cx - 24, waveBaseY - 2, 48, 4, 2, rgb565(48, 84, 98));
    }
  }

  void drawVoiceBottomText(bool listening, const String& message, const String& error, int shiftX, int shiftY) {
    String text = "Hold K1";
    if (error.length() > 0) {
      text = "Voice error";
    } else if (listening) {
      text = "Speak";
    } else if (message == "voice input stopped") {
      text = "Done";
    } else if (message == "voice mode") {
      text = "Hold K1";
    }
    canvas.setTextSize(1);
    canvas.setTextColor(error.length() > 0 ? TFT_RED : rgb565(122, 142, 150), rgb565(7, 10, 14));
    drawWrappedCenterText(text, canvas.width() / 2 + shiftX, bottomTextY() + shiftY, bottomLineChars(), bottomLineCount(), bottomLineHeight());
  }

  uint16_t dashboardBg() const {
    return rgb565(2, 4, 6);
  }

  uint16_t hourglassBg() const {
    return rgb565(6, 7, 9);
  }

  void drawDashboardTopLabel(bool wifiConnected, int shiftX, int shiftY) {
    uint16_t bg = dashboardBg();
    canvas.setFont(&fonts::Font2);
    canvas.setTextSize(1);
    canvas.setTextColor(rgb565(64, 86, 98), bg);
    canvas.drawString("CLOCK", canvas.width() / 2 + shiftX, topLabelY() + shiftY);
    uint16_t dot = wifiConnected ? rgb565(62, 180, 132) : rgb565(112, 60, 66);
    canvas.fillCircle(canvas.width() - 15 + shiftX, topLabelY() + shiftY, 3, dot);
  }

  void drawDashboardClock(const String& timeText, bool synced, int shiftX, int shiftY) {
    uint16_t bg = dashboardBg();
    int centerX = canvas.width() / 2 + shiftX;
    int centerY = canvas.height() > canvas.width() ? canvas.height() / 2 - 20 : canvas.height() / 2 - 6;
    canvas.setFont(&fonts::Font4);
    canvas.setTextSize(1);
    canvas.setTextColor(synced ? rgb565(190, 224, 232) : rgb565(82, 98, 106), bg);
    canvas.drawString(timeText, centerX, centerY + shiftY);
    canvas.setFont(&fonts::Font2);
    canvas.setTextSize(1);
    canvas.setTextColor(rgb565(62, 86, 96), bg);
    canvas.drawString(synced ? "24H" : "SYNC", centerX, centerY + 34 + shiftY);
  }

  String batteryVoltageText(int32_t voltageMv) {
    if (voltageMv <= 0) {
      return "";
    }
    int decivolts = (voltageMv + 50) / 100;
    return String(decivolts / 10) + "." + String(decivolts % 10) + "V";
  }

  void drawDashboardBattery(int32_t percent, int32_t voltageMv, bool estimated, bool charging, int shiftX, int shiftY) {
    uint16_t bg = dashboardBg();
    int w = canvas.width() > 130 ? 76 : 66;
    int h = 22;
    int x = (canvas.width() - w) / 2 + shiftX;
    int y = canvas.height() > canvas.width() ? canvas.height() - 72 + shiftY : canvas.height() - 34 + shiftY;
    uint16_t outline = rgb565(72, 92, 102);
    uint16_t fill = batteryColor(percent);

    canvas.drawRoundRect(x, y, w, h, 4, outline);
    canvas.fillRoundRect(x + w + 2, y + 7, 4, 8, 2, outline);
    if (percent >= 0) {
      int fillWidth = (w - 6) * percent / 100;
      if (fillWidth > 0 && fillWidth < 3) {
        fillWidth = 3;
      }
      canvas.fillRoundRect(x + 3, y + 3, fillWidth, h - 6, 3, fill);
    } else {
      canvas.drawFastHLine(x + 10, y + h / 2, w - 20, rgb565(54, 66, 72));
    }

    String label = percent >= 0 ? String(percent) + "%" : "--%";
    String voltage = batteryVoltageText(voltageMv);
    if (estimated && percent >= 0) {
      label = "~" + label;
      if (voltage.length() > 0) {
        label += " " + voltage;
      }
    }
    if (charging) {
      label += " CHG";
    }
    canvas.setFont(&fonts::Font2);
    canvas.setTextSize(1);
    canvas.setTextColor(percent >= 0 ? rgb565(132, 162, 172) : rgb565(82, 98, 106), bg);
    canvas.drawString(label, canvas.width() / 2 + shiftX, y + h + 17);
  }

  void drawDashboardNetwork(const String& networkText, int shiftX, int shiftY) {
    uint16_t bg = dashboardBg();
    canvas.setFont(&fonts::Font2);
    canvas.setTextSize(1);
    canvas.setTextColor(rgb565(48, 64, 72), bg);
    canvas.drawString(networkText, canvas.width() / 2 + shiftX, canvas.height() - 12 + shiftY);
  }

  uint16_t batteryColor(int32_t percent) const {
    if (percent < 0) {
      return rgb565(54, 66, 72);
    }
    if (percent < 20) {
      return rgb565(214, 70, 72);
    }
    if (percent < 45) {
      return rgb565(218, 150, 64);
    }
    return rgb565(76, 190, 138);
  }

  void drawHourglassTopLabel(CountdownState state, HourglassPose pose, int shiftX, int shiftY) {
    uint16_t bg = hourglassBg();
    canvas.setFont(&fonts::Font2);
    canvas.setTextSize(canvas.height() > canvas.width() ? 2 : 1);
    canvas.setTextColor(colorForCountdownState(state), bg);
    canvas.drawString(labelForCountdownState(state), canvas.width() / 2 + shiftX, topLabelY() + shiftY);
    canvas.setTextSize(1);
    canvas.setTextColor(rgb565(92, 112, 118), bg);
    canvas.drawString(labelForHourglassPose(pose), canvas.width() - 18 + shiftX, topLabelY() + shiftY);
  }

  void drawHourglassBody(CountdownState state, float progress, float seconds, int shiftX, int shiftY) {
    int cx = canvas.width() / 2 + shiftX;
    int cy = (canvas.height() > canvas.width() ? canvas.height() / 2 - 2 : canvas.height() / 2) + shiftY;
    int halfW = canvas.width() > 130 ? 32 : 28;
    int halfH = canvas.height() > canvas.width() ? 48 : 36;
    int topY = cy - halfH;
    int bottomY = cy + halfH;
    int chamberTopY = topY + 11;
    int chamberBottomY = bottomY - 11;
    int neckW = 5;
    uint16_t frame = state == CountdownState::Offline || state == CountdownState::Error
                         ? rgb565(76, 78, 80)
                         : rgb565(138, 128, 104);
    uint16_t glass = state == CountdownState::Offline || state == CountdownState::Error
                         ? rgb565(30, 32, 34)
                         : rgb565(21, 28, 32);
    uint16_t glassEdge = state == CountdownState::Offline || state == CountdownState::Error
                             ? rgb565(98, 102, 104)
                             : rgb565(176, 166, 132);
    uint16_t shine = state == CountdownState::Offline || state == CountdownState::Error
                         ? rgb565(80, 84, 86)
                         : rgb565(204, 224, 218);
    uint16_t sand = state == CountdownState::Paused ? rgb565(168, 124, 70) : rgb565(232, 178, 82);
    if (state == CountdownState::Done) {
      sand = rgb565(106, 220, 156);
    }
    uint16_t sandHighlight = blendColor(
        sand,
        state == CountdownState::Done ? rgb565(220, 255, 232) : rgb565(255, 236, 170),
        0.28f);

    float topFill = 1.0f - progress;
    float bottomFill = progress;
    if (state == CountdownState::Ready || state == CountdownState::Offline || state == CountdownState::Error) {
      topFill = 0.0f;
      bottomFill = 1.0f;
    }
    if (state == CountdownState::Done) {
      topFill = 0.0f;
      bottomFill = 1.0f;
    }

    canvas.fillEllipse(cx, bottomY + 9, halfW + 6, 7, rgb565(2, 3, 4));

    canvas.fillTriangle(cx - halfW + 8, chamberTopY, cx + halfW - 8, chamberTopY, cx + neckW, cy, glass);
    canvas.fillTriangle(cx - halfW + 8, chamberTopY, cx - neckW, cy, cx + neckW, cy, glass);
    canvas.fillTriangle(cx - neckW, cy, cx + neckW, cy, cx + halfW - 8, chamberBottomY, glass);
    canvas.fillTriangle(cx - neckW, cy, cx - halfW + 8, chamberBottomY, cx + halfW - 8, chamberBottomY, glass);

    if (topFill > 0.02f) {
      int topH = cy - chamberTopY;
      int surfaceY = cy - (int)(topH * topFill);
      int surfaceW = neckW + (int)((halfW - 12 - neckW) * topFill);
      canvas.fillTriangle(cx - surfaceW, surfaceY, cx + surfaceW, surfaceY, cx + neckW, cy - 2, dimColor(sand, 0.86f));
      canvas.fillTriangle(cx - surfaceW, surfaceY, cx - neckW, cy - 2, cx + neckW, cy - 2, dimColor(sand, 0.86f));
      canvas.fillEllipse(cx, surfaceY, surfaceW, 3, sand);
      canvas.drawFastHLine(cx - surfaceW + 3, surfaceY - 1, max(2, surfaceW * 2 - 6), sandHighlight);
    }

    if (bottomFill > 0.02f) {
      int bottomH = chamberBottomY - cy;
      int surfaceY = chamberBottomY - (int)(bottomH * bottomFill);
      int bottomMaxW = halfW - 14;
      int levelFromNeck = surfaceY - (cy + 2);
      if (levelFromNeck < 0) {
        levelFromNeck = 0;
      }
      int surfaceW = neckW + (int)((bottomMaxW - neckW) * (levelFromNeck / (float)bottomH));
      if (surfaceW > bottomMaxW) {
        surfaceW = bottomMaxW;
      }
      canvas.fillTriangle(cx - neckW, cy + 2, cx + neckW, cy + 2, cx + surfaceW, surfaceY, dimColor(sand, 0.82f));
      canvas.fillTriangle(cx - neckW, cy + 2, cx - surfaceW, surfaceY, cx + surfaceW, surfaceY, dimColor(sand, 0.82f));
      canvas.fillTriangle(cx - surfaceW, surfaceY, cx + surfaceW, surfaceY, cx + bottomMaxW, chamberBottomY, sand);
      canvas.fillTriangle(cx - surfaceW, surfaceY, cx - bottomMaxW, chamberBottomY, cx + bottomMaxW, chamberBottomY, sand);
      canvas.fillEllipse(cx, surfaceY, surfaceW, 3, sandHighlight);
      canvas.fillEllipse(cx, chamberBottomY - 2, bottomMaxW, 4, dimColor(sand, 0.92f));
    }

    canvas.fillRoundRect(cx - halfW - 7, topY - 5, halfW * 2 + 14, 10, 5, dimColor(frame, 0.68f));
    canvas.fillRoundRect(cx - halfW - 4, topY - 8, halfW * 2 + 8, 8, 4, frame);
    canvas.fillRoundRect(cx - halfW - 7, bottomY - 5, halfW * 2 + 14, 10, 5, dimColor(frame, 0.68f));
    canvas.fillRoundRect(cx - halfW - 4, bottomY, halfW * 2 + 8, 8, 4, frame);

    canvas.drawLine(cx - halfW + 5, topY + 4, cx - neckW, cy - 2, glassEdge);
    canvas.drawLine(cx + halfW - 5, topY + 4, cx + neckW, cy - 2, dimColor(glassEdge, 0.82f));
    canvas.drawLine(cx - neckW, cy + 2, cx - halfW + 5, bottomY - 4, dimColor(glassEdge, 0.82f));
    canvas.drawLine(cx + neckW, cy + 2, cx + halfW - 5, bottomY - 4, glassEdge);
    canvas.drawLine(cx - halfW + 9, topY + 8, cx - 8, cy - 6, shine);
    canvas.drawLine(cx - 8, cy + 6, cx - halfW + 10, bottomY - 10, dimColor(shine, 0.66f));
    canvas.drawEllipse(cx, cy, 8, 5, dimColor(glassEdge, 0.74f));
    canvas.fillEllipse(cx, cy, 3, 2, dimColor(sand, state == CountdownState::Ready ? 0.36f : 0.95f));

    if (state == CountdownState::Running && progress < 1.0f) {
      int streamY = cy - 2 + (int)(2.0f * sinf(seconds * 20.0f));
      canvas.drawFastVLine(cx, cy - 12, 25, sandHighlight);
      canvas.fillCircle(cx, streamY, 2, sandHighlight);
    }

    if (state == CountdownState::Done) {
      canvas.drawCircle(cx - halfW + 8, topY + 18, 2, sandHighlight);
      canvas.drawFastHLine(cx + halfW - 14, topY + 25, 7, sandHighlight);
      canvas.drawFastVLine(cx + halfW - 11, topY + 22, 7, sandHighlight);
    }
  }

  void drawHourglassTime(uint32_t remainingSeconds, CountdownState state, int shiftX, int shiftY) {
    uint16_t bg = hourglassBg();
    char buffer[8];
    uint32_t minutes = remainingSeconds / 60;
    uint32_t seconds = remainingSeconds % 60;
    snprintf(buffer, sizeof(buffer), "%02lu:%02lu", (unsigned long)minutes, (unsigned long)seconds);
    canvas.setFont(&fonts::Font2);
    canvas.setTextSize(1);
    canvas.setTextColor(state == CountdownState::Done ? rgb565(106, 220, 156) : rgb565(204, 176, 112), bg);
    int y = canvas.height() > canvas.width() ? canvas.height() - 58 : stageLineY() - 10;
    canvas.drawString(String(buffer), canvas.width() / 2 + shiftX, y + shiftY);
  }

  void drawHourglassBottomText(CountdownState state, const String& message, const String& error, int shiftX, int shiftY) {
    String text = message;
    if (state == CountdownState::Ready && text.length() == 0) {
      text = "Flip to start";
    } else if (state == CountdownState::Running) {
      text = "Sand falling";
    } else if (state == CountdownState::Paused) {
      text = "Flat pause";
    } else if (state == CountdownState::Done) {
      text = "Done";
    }
    if (error.length() > 0) {
      text = "Pomo error";
    }
    canvas.setFont(&fonts::Font2);
    canvas.setTextSize(1);
    canvas.setTextColor(error.length() > 0 ? TFT_RED : rgb565(118, 136, 134), hourglassBg());
    drawWrappedCenterText(text, canvas.width() / 2 + shiftX, bottomTextY() + shiftY, bottomLineChars(), bottomLineCount(), bottomLineHeight());
  }

  String bottomStatusText(BridgeState state, const String& message, const String& error) const {
    if (state == BridgeState::Offline) {
      return "Bridge offline";
    }
    if (state == BridgeState::Error) {
      if (error == "No TickTick record command configured.") {
        return "No TickTick log";
      }
      if (error.length() > 0) {
        return error;
      }
      return "Bridge error";
    }
    if (state == BridgeState::Running) {
      return "Growing";
    }
    if (state == BridgeState::Paused) {
      return "Paused";
    }
    if (state == BridgeState::Ending) {
      return "Harvest";
    }
    if (message.length() > 0 && message != "ok") {
      return message;
    }
    return timerMode == TimerMode::Stopwatch ? "Ready" : "";
  }

  int protectionShiftX() const {
    if (!DISPLAY_PROTECTION_ENABLED || DISPLAY_PROTECTION_SHIFT_PIXELS <= 0) {
      return 0;
    }
    unsigned long phase = (millis() / DISPLAY_PROTECTION_SHIFT_MS) % 4;
    if (phase == 1) return DISPLAY_PROTECTION_SHIFT_PIXELS;
    if (phase == 3) return -DISPLAY_PROTECTION_SHIFT_PIXELS;
    return 0;
  }

  int protectionShiftY() const {
    if (!DISPLAY_PROTECTION_ENABLED || DISPLAY_PROTECTION_SHIFT_PIXELS <= 0) {
      return 0;
    }
    unsigned long phase = (millis() / DISPLAY_PROTECTION_SHIFT_MS) % 4;
    if (phase == 0) return DISPLAY_PROTECTION_SHIFT_PIXELS;
    if (phase == 2) return -DISPLAY_PROTECTION_SHIFT_PIXELS;
    return 0;
  }

  int topLabelY() const {
    return canvas.height() > canvas.width() ? 20 : 12;
  }

  int tomatoCenterY() const {
    return canvas.height() > canvas.width() ? canvas.height() / 2 - 24 : canvas.height() / 2 - 2;
  }

  int stageLineY() const {
    return canvas.height() > canvas.width() ? canvas.height() - 62 : canvas.height() - 16;
  }

  int bottomTextY() const {
    return canvas.height() > canvas.width() ? canvas.height() - 36 : canvas.height() - 5;
  }

  int bottomLineChars() const {
    return canvas.height() > canvas.width() ? 13 : 32;
  }

  int bottomLineCount() const {
    return canvas.height() > canvas.width() ? 2 : 1;
  }

  int bottomLineHeight() const {
    return canvas.height() > canvas.width() ? 18 : 10;
  }

  float tomatoMinRadius() const {
    float radius = canvas.width() * 0.14f;
    if (radius < 15.0f) radius = 15.0f;
    return radius;
  }

  float tomatoMaxRadius() const {
    float byWidth = canvas.width() * 0.31f;
    float byHeight = canvas.height() * 0.21f;
    float radius = byWidth < byHeight ? byWidth : byHeight;
    if (radius > 42.0f) radius = 42.0f;
    if (radius < 26.0f) radius = 26.0f;
    return radius;
  }

  void drawWrappedCenterText(const String& text, int centerX, int startY, int charsPerLine, int maxLines, int lineHeight) {
    int textLength = text.length();
    int index = 0;
    for (int line = 0; line < maxLines && index < textLength; line++) {
      while (index < textLength && text[index] == ' ') {
        index++;
      }
      int end = index + charsPerLine;
      if (end >= textLength) {
        canvas.drawString(text.substring(index), centerX, startY + line * lineHeight);
        return;
      }

      int breakAt = -1;
      for (int cursor = end; cursor > index; cursor--) {
        if (text[cursor] == ' ') {
          breakAt = cursor;
          break;
        }
      }
      if (breakAt < 0 || breakAt == index) {
        breakAt = end;
      }

      String lineText = text.substring(index, breakAt);
      if (line == maxLines - 1 && breakAt < textLength) {
        if (lineText.length() > 1) {
          lineText = lineText.substring(0, lineText.length() - 1) + ".";
        }
      }
      canvas.drawString(lineText, centerX, startY + line * lineHeight);
      index = breakAt;
    }
  }
};

TomatoAnimator tomatoAnimator;

String dashboardNetworkText() {
  if (WiFi.status() != WL_CONNECTED) {
    return "offline";
  }
  return "OTA " + WiFi.localIP().toString();
}

uint8_t normalizedRotation(uint8_t rotation) {
  return rotation & 0x03;
}

uint8_t desiredScreenRotation() {
  if (!otaActive && screenMode == ScreenMode::Hourglass && stableHourglassPose == HourglassPose::Inverted) {
    return normalizedRotation(TOMATO_SCREEN_ROTATION + 2);
  }
  return normalizedRotation(TOMATO_SCREEN_ROTATION);
}

void applyScreenRotation() {
  uint8_t desired = desiredScreenRotation();
  if (currentScreenRotation == desired) {
    return;
  }
  currentScreenRotation = desired;
  M5.Display.setRotation(currentScreenRotation);
}

void drawStatus() {
  if (displaySleeping && !otaActive) {
    return;
  }
  applyScreenRotation();
  if (otaActive) {
    tomatoAnimator.drawOta(otaProgress, otaStatusText);
    return;
  }
  if (screenMode == ScreenMode::Dashboard) {
    refreshBatteryStatus();
    tomatoAnimator.drawDashboard(formatClockTime(), batteryLevel, batteryCharging, clockSynced, dashboardNetworkText(), WiFi.status() == WL_CONNECTED);
    return;
  }
  if (screenMode == ScreenMode::Hourglass) {
    tomatoAnimator.drawHourglass(
        countdownState,
        currentCountdownRemainingSeconds(),
        countdownDurationSeconds,
        stableHourglassPose,
        countdownMessage,
        countdownError);
    return;
  }
  if (screenMode == ScreenMode::Voice) {
    tomatoAnimator.drawVoice(voiceListening, lastMessage, lastError);
    return;
  }
  tomatoAnimator.draw(displayState(), currentDisplayElapsedSeconds(), lastMessage, lastError);
}

void updateAnimationFrame() {
  unsigned long fps = ANIMATION_FPS == 0 ? 1 : ANIMATION_FPS;
  unsigned long frameMs = screenMode == ScreenMode::Dashboard ? DASHBOARD_REFRESH_MS : 1000 / fps;
  if (screenMode == ScreenMode::Hourglass && countdownState != CountdownState::Running) {
    frameMs = DASHBOARD_REFRESH_MS;
  }
  unsigned long now = millis();
  if (now - lastFrameMs < frameMs) {
    return;
  }
  lastFrameMs = now;
  drawStatus();
}

String otaErrorText(ota_error_t error) {
  switch (error) {
    case OTA_AUTH_ERROR:
      return "auth failed";
    case OTA_BEGIN_ERROR:
      return "begin failed";
    case OTA_CONNECT_ERROR:
      return "connect failed";
    case OTA_RECEIVE_ERROR:
      return "receive failed";
    case OTA_END_ERROR:
      return "end failed";
    default:
      return "update failed";
  }
}

void ensureOta() {
  if (!OTA_ENABLED || otaReady || WiFi.status() != WL_CONNECTED) {
    return;
  }

  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPort(OTA_PORT);
  if (String(OTA_PASSWORD).length() > 0) {
    ArduinoOTA.setPassword(OTA_PASSWORD);
  }

  ArduinoOTA.onStart([]() {
    otaActive = true;
    displaySleeping = false;
    otaProgress = 0;
    otaStatusText = ArduinoOTA.getCommand() == U_FLASH ? "firmware" : "filesystem";
    lastOtaDrawMs = 0;
    currentDisplayBrightness = DISPLAY_BRIGHTNESS_ACTIVE;
    M5.Display.setBrightness(DISPLAY_BRIGHTNESS_ACTIVE);
    drawStatus();
    beep(1568, 70);
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    uint8_t nextProgress = total == 0 ? 0 : (uint8_t)((progress * 100UL) / total);
    if (nextProgress > 100) {
      nextProgress = 100;
    }
    if (nextProgress != otaProgress) {
      otaProgress = nextProgress;
      unsigned long now = millis();
      if (now - lastOtaDrawMs >= 120 || otaProgress == 100) {
        lastOtaDrawMs = now;
        drawStatus();
      }
    }
  });

  ArduinoOTA.onEnd([]() {
    otaProgress = 100;
    otaStatusText = "restarting";
    drawStatus();
    beep(2093, 110);
  });

  ArduinoOTA.onError([](ota_error_t error) {
    otaActive = true;
    otaProgress = 0;
    otaStatusText = otaErrorText(error);
    drawStatus();
    beep(440, 180);
  });

  ArduinoOTA.begin();
  otaReady = true;
  otaStatusText = "OTA " + WiFi.localIP().toString();
}

bool ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    ensureOta();
    return true;
  }

  unsigned long now = millis();
  if (now - lastWifiAttemptMs < WIFI_RETRY_MS) {
    return false;
  }

  lastWifiAttemptMs = now;
  currentState = BridgeState::Offline;
  currentStateText = "offline";
  lastMessage = "connecting wifi";
  drawStatus();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  return false;
}

bool callBridge(const char* method, const char* path, const String& body = "") {
  bool pomoRequest = String(path).startsWith("/pomo/");
  if (!ensureWifi()) {
    if (pomoRequest) {
      countdownError = "wifi offline";
      countdownState = CountdownState::Offline;
      countdownStateText = "offline";
    } else {
      lastError = "wifi offline";
      currentState = BridgeState::Offline;
      currentStateText = "offline";
    }
    drawStatus();
    return false;
  }

  HTTPClient http;
  String url = endpointUrl(path);
  http.begin(url);
  http.setTimeout(BRIDGE_HTTP_TIMEOUT_MS);
  if (String(BRIDGE_TOKEN).length() > 0) {
    http.addHeader("X-Focus-Token", BRIDGE_TOKEN);
  }
  if (body.length() > 0) {
    http.addHeader("Content-Type", "application/json");
  }

  int code = 0;
  if (strcmp(method, "GET") == 0) {
    code = http.GET();
  } else {
    code = http.POST(body);
  }

  if (code <= 0) {
    if (pomoRequest) {
      countdownError = "bridge unreachable";
      countdownState = CountdownState::Offline;
      countdownStateText = "offline";
    } else {
      lastError = "bridge unreachable";
      currentState = BridgeState::Offline;
      currentStateText = "offline";
    }
    http.end();
    drawStatus();
    return false;
  }

  String response = http.getString();
  http.end();

  if (code >= 400) {
    if (pomoRequest) {
      countdownError = "http " + String(code);
      countdownState = CountdownState::Error;
      countdownStateText = "error";
    } else {
      lastError = "http " + String(code);
      currentState = BridgeState::Error;
      currentStateText = "error";
    }
    drawStatus();
    return false;
  }

  String state = extractJsonString(response, "state");
  String message = extractJsonString(response, "message");
  String error = extractJsonString(response, "last_error");
  String serverTime = extractJsonString(response, "server_time");
  uint32_t elapsedSeconds = extractJsonUInt(response, "elapsed_seconds");
  uint32_t durationSeconds = extractJsonUInt(response, "duration_seconds");
  uint32_t remainingSeconds = extractJsonUInt(response, "remaining_seconds");

  if (serverTime.length() > 0) {
    updateClockFromServerTime(serverTime);
  }
  if (pomoRequest) {
    if (state.length() > 0) {
      countdownStateText = state;
      countdownState = parseCountdownState(state);
      if (countdownState != CountdownState::Done) {
        countdownDoneSoundPlayed = false;
      }
    }
    if (durationSeconds > 0) {
      countdownDurationSeconds = durationSeconds;
    }
    countdownRemainingSeconds = remainingSeconds;
    lastCountdownSyncMs = millis();
    if (message.length() > 0) {
      countdownMessage = message;
    }
    countdownError = error;
  } else {
    if (state.length() > 0) {
      currentStateText = state;
      currentState = parseState(state);
    }
    bridgeElapsedSeconds = elapsedSeconds;
    lastElapsedSyncMs = millis();
    if (message.length() > 0) {
      lastMessage = message;
    }
    lastError = error;
  }
  drawStatus();
  return true;
}

void startCountdown() {
  countdownDurationSeconds = POMO_COUNTDOWN_SECONDS;
  countdownRemainingSeconds = POMO_COUNTDOWN_SECONDS;
  countdownState = CountdownState::Running;
  countdownStateText = "running";
  countdownMessage = "sand falling";
  countdownError = "";
  lastCountdownSyncMs = millis();
  countdownDoneSoundPlayed = false;
  triggerMelody(START_MELODY, sizeof(START_MELODY) / sizeof(START_MELODY[0]));
  drawStatus();
  callBridge("POST", "/pomo/start", "{\"duration_seconds\":" + String(POMO_COUNTDOWN_SECONDS) + "}");
}

void pauseCountdown() {
  if (countdownState != CountdownState::Running) {
    return;
  }
  countdownRemainingSeconds = currentCountdownRemainingSeconds();
  countdownState = CountdownState::Paused;
  countdownStateText = "paused";
  countdownMessage = "flat pause";
  countdownError = "";
  countdownDoneSoundPlayed = false;
  triggerMelody(PAUSE_MELODY, sizeof(PAUSE_MELODY) / sizeof(PAUSE_MELODY[0]));
  drawStatus();
  callBridge("POST", "/pomo/pause");
}

void resumeCountdown() {
  if (countdownState != CountdownState::Paused) {
    startCountdown();
    return;
  }
  countdownState = CountdownState::Running;
  countdownStateText = "running";
  countdownMessage = "sand falling";
  countdownError = "";
  lastCountdownSyncMs = millis();
  countdownDoneSoundPlayed = false;
  triggerMelody(RESUME_MELODY, sizeof(RESUME_MELODY) / sizeof(RESUME_MELODY[0]));
  drawStatus();
  callBridge("POST", "/pomo/resume");
}

void endCountdown() {
  countdownState = CountdownState::Ready;
  countdownStateText = "idle";
  countdownDurationSeconds = POMO_COUNTDOWN_SECONDS;
  countdownRemainingSeconds = POMO_COUNTDOWN_SECONDS;
  countdownMessage = "flip to start";
  countdownError = "";
  lastCountdownSyncMs = millis();
  countdownDoneSoundPlayed = false;
  beep(1320, 80);
  drawStatus();
  callBridge("POST", "/pomo/end");
}

void handleHourglassSingleClick() {
  markInteraction();
  if (countdownState == CountdownState::Running) {
    pauseCountdown();
  } else if (countdownState == CountdownState::Paused) {
    resumeCountdown();
  } else {
    startCountdown();
  }
}

void handleHourglassPoseAction(HourglassPose pose) {
  if (screenMode != ScreenMode::Hourglass || !HOURGLASS_IMU_ENABLED) {
    return;
  }
  unsigned long now = millis();
  if (now - lastHourglassActionMs < HOURGLASS_TRIGGER_COOLDOWN_MS) {
    return;
  }
  if (pose == HourglassPose::Inverted) {
    if (countdownState == CountdownState::Paused) {
      lastHourglassActionMs = now;
      resumeCountdown();
    } else if (countdownState != CountdownState::Running && countdownState != CountdownState::Done) {
      lastHourglassActionMs = now;
      startCountdown();
    }
  } else if (pose == HourglassPose::Flat && countdownState == CountdownState::Running) {
    lastHourglassActionMs = now;
    pauseCountdown();
  } else if (pose == HourglassPose::Upright && countdownState == CountdownState::Done) {
    lastHourglassActionMs = now;
    endCountdown();
  }
}

HourglassPose detectHourglassPose(float ax, float ay, float az) {
  float axes[3] = {ax, ay, az};
  uint8_t flipAxis = HOURGLASS_GRAVITY_AXIS;
  if (flipAxis > 2) {
    flipAxis = 2;
  }
  float axisValue = axes[flipAxis];
  if (HOURGLASS_GRAVITY_INVERT) {
    axisValue = -axisValue;
  }
  if (axisValue >= HOURGLASS_FLIP_THRESHOLD) {
    return HourglassPose::Upright;
  }
  if (axisValue <= -HOURGLASS_FLIP_THRESHOLD) {
    return HourglassPose::Inverted;
  }
  if (fabsf(az) >= HOURGLASS_FLAT_Z_THRESHOLD) {
    return HourglassPose::Flat;
  }
  return HourglassPose::Unknown;
}

void updateHourglassPose() {
  if (screenMode != ScreenMode::Hourglass || !HOURGLASS_IMU_ENABLED || !hourglassImuAvailable) {
    return;
  }
  if (!M5.Imu.update()) {
    return;
  }
  float ax = 0.0f;
  float ay = 0.0f;
  float az = 0.0f;
  if (!M5.Imu.getAccel(&ax, &ay, &az)) {
    return;
  }
  HourglassPose pose = detectHourglassPose(ax, ay, az);
  rawHourglassPose = pose;
  unsigned long now = millis();
  if (pose != candidateHourglassPose) {
    candidateHourglassPose = pose;
    hourglassPoseChangedAtMs = now;
    return;
  }
  if (pose != stableHourglassPose && now - hourglassPoseChangedAtMs >= HOURGLASS_ORIENTATION_STABLE_MS) {
    stableHourglassPose = pose;
    handleHourglassPoseAction(pose);
    drawStatus();
  }
}

void handleKey1Release() {
  markInteraction();
  if (voiceListening) {
    voiceListening = false;
    screenMode = ScreenMode::Voice;
    lastMessage = "voice input stopped";
    lastError = "";
    triggerMelody(PAUSE_MELODY, sizeof(PAUSE_MELODY) / sizeof(PAUSE_MELODY[0]));
    drawStatus();
    callBridge("POST", "/device/voice-input-stop");
    return;
  }
  if (key1.longPressReported) {
    pendingSingleClick = false;
    return;
  }
  unsigned long now = millis();
  if (pendingSingleClick && (now - firstClickAtMs) <= DOUBLE_CLICK_MS) {
    pendingSingleClick = false;
    if (screenMode == ScreenMode::Hourglass) {
      endCountdown();
      return;
    }
    lastMessage = "harvest";
    if (isActiveFocusState(currentState)) {
      currentState = BridgeState::Ending;
      currentStateText = "ending";
      harvestStartedAtMs = millis();
      triggerHarvestSound();
      drawStatus();
    } else {
      beep(1600, 70);
    }
    callBridge("POST", "/focus/double-click");
    return;
  }

  pendingSingleClick = true;
  firstClickAtMs = now;
}

void handleKey1LongPress() {
  markInteraction();
  pendingSingleClick = false;
  pendingKey2SingleClick = false;
  screenMode = ScreenMode::Voice;
  voiceListening = true;
  lastMessage = "voice input started";
  lastError = "";
  triggerMelody(RESUME_MELODY, sizeof(RESUME_MELODY) / sizeof(RESUME_MELODY[0]));
  drawStatus();
  callBridge("POST", "/device/voice-input-start");
}

void cycleScreenMode() {
  markInteraction();
  if (screenMode == ScreenMode::Focus) {
    screenMode = ScreenMode::Voice;
  } else if (screenMode == ScreenMode::Voice) {
    screenMode = ScreenMode::Dashboard;
    refreshBatteryStatus(true);
  } else if (screenMode == ScreenMode::Dashboard) {
    screenMode = ScreenMode::Hourglass;
    countdownMessage = hourglassImuAvailable ? "flip to start" : "K1 start";
    countdownError = hourglassImuAvailable ? "" : "imu offline";
    stableHourglassPose = HourglassPose::Unknown;
    candidateHourglassPose = HourglassPose::Unknown;
    rawHourglassPose = HourglassPose::Unknown;
    callBridge("GET", "/pomo/state");
  } else {
    screenMode = ScreenMode::Focus;
  }
  lastError = "";
  if (screenMode == ScreenMode::Voice) {
    lastMessage = "voice mode";
    beep(960, 70);
  } else if (screenMode == ScreenMode::Dashboard) {
    lastMessage = "clock mode";
    beep(520, 55);
  } else if (screenMode == ScreenMode::Hourglass) {
    lastMessage = "pomo mode";
    beep(880, 55);
  } else {
    lastMessage = "ok";
    beep(720, 70);
  }
  updateDisplayBrightness();
  drawStatus();
}

void handleKey2Release() {
  if (key2.longPressReported) {
    return;
  }
  if (voiceListening) {
    return;
  }

  unsigned long now = millis();
  if (pendingKey2SingleClick && (now - firstKey2ClickAtMs) <= DOUBLE_CLICK_MS) {
    pendingKey2SingleClick = false;
    markInteraction();
    lastMessage = "screen off";
    lastError = "";
    countdownError = "";
    beep(392, 70);
    enterDisplaySleep();
    return;
  }

  pendingKey2SingleClick = true;
  firstKey2ClickAtMs = now;
}

void flushPendingKey2Click() {
  if (!pendingKey2SingleClick) {
    return;
  }
  if ((millis() - firstKey2ClickAtMs) < DOUBLE_CLICK_MS) {
    return;
  }
  pendingKey2SingleClick = false;
  if (voiceListening) {
    return;
  }
  cycleScreenMode();
}

void flushPendingSingleClick() {
  if (!pendingSingleClick) {
    return;
  }
  if ((millis() - firstClickAtMs) < DOUBLE_CLICK_MS) {
    return;
  }
  pendingSingleClick = false;
  pendingKey2SingleClick = false;
  markInteraction();
  if (screenMode == ScreenMode::Hourglass) {
    handleHourglassSingleClick();
    return;
  }
  lastMessage = "click";
  BridgeState before = currentState;
  callBridge("POST", "/focus/click");
  triggerStateSound(before, currentState);
}

void setup() {
  auto cfg = M5.config();
  cfg.internal_imu = HOURGLASS_IMU_ENABLED;
  M5.begin(cfg);
  currentScreenRotation = normalizedRotation(TOMATO_SCREEN_ROTATION);
  M5.Display.setRotation(currentScreenRotation);
  M5.Display.setFont(&fonts::Font2);
  currentDisplayBrightness = DISPLAY_BRIGHTNESS_ACTIVE;
  M5.Display.setBrightness(DISPLAY_BRIGHTNESS_ACTIVE);
  M5.Speaker.begin();
  M5.Speaker.setVolume(SOUND_VOLUME);
  M5.Speaker.setAllChannelVolume(SOUND_CHANNEL_VOLUME);
  M5.Speaker.setChannelVolume(0, SOUND_CHANNEL_VOLUME);
  tomatoAnimator.begin();
  lastInteractionMs = millis();

  key1.begin();
  key2.begin();
  hourglassImuAvailable = HOURGLASS_IMU_ENABLED && M5.Imu.isEnabled();

  WiFi.setSleep(false);
  refreshBatteryStatus(true);
  ensureWifi();
  drawStatus();
  if (SOUND_BOOT_CHIRP_ENABLED) {
    triggerMelody(BOOT_MELODY, sizeof(BOOT_MELODY) / sizeof(BOOT_MELODY[0]));
  }
}

void loop() {
  M5.update();
  key1.update();
  key2.update();

  if (WiFi.status() == WL_CONNECTED) {
    ensureOta();
    if (otaReady) {
      ArduinoOTA.handle();
    }
  }
  if (otaActive) {
    updateMelody();
    updateHarvestSound();
    delay(4);
    return;
  }

  bool key1Pressed = key1.stablePressed;
  if (!key1WasPressed && key1Pressed && displaySleeping) {
    wakeDisplay();
    key1WakeOnly = true;
    pendingSingleClick = false;
    pendingKey2SingleClick = false;
  }
  if (!key1WakeOnly && key1.consumeLongPress()) {
    handleKey1LongPress();
  }
  if (key1WasPressed && !key1Pressed) {
    if (key1WakeOnly) {
      key1WakeOnly = false;
    } else {
      handleKey1Release();
    }
  }
  key1WasPressed = key1Pressed;

  bool key2Pressed = key2.stablePressed;
  if (!key2WasPressed && key2Pressed && displaySleeping) {
    wakeDisplay();
    key2WakeOnly = true;
    pendingSingleClick = false;
    pendingKey2SingleClick = false;
  }
  if (key2WasPressed && !key2Pressed) {
    if (key2WakeOnly) {
      key2WakeOnly = false;
    } else {
      handleKey2Release();
    }
  }
  key2WasPressed = key2Pressed;

  flushPendingSingleClick();
  flushPendingKey2Click();
  updateHourglassPose();
  updateCountdownCompletion();

  if (WiFi.status() != WL_CONNECTED) {
    ensureWifi();
  } else if (millis() - lastStatePollMs >= STATE_POLL_MS) {
    lastStatePollMs = millis();
    callBridge("GET", screenMode == ScreenMode::Hourglass ? "/pomo/state" : "/focus/state");
  }

  updateDisplayBrightness();
  updateMelody();
  updateHarvestSound();
  updateAnimationFrame();
  delay(8);
}
