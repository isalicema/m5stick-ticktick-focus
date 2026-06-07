#pragma once

// Copy this file to config.h and edit the values for your network.

static const char* WIFI_SSID = "YOUR_WIFI_NAME";
static const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// Use your Mac's LAN IP address, for example:
// http://192.168.1.23:8787
static const char* BRIDGE_BASE_URL = "http://192.168.1.23:8787";

// Leave empty unless mac-bridge/config.json sets security.token.
static const char* BRIDGE_TOKEN = "";

static const bool OTA_ENABLED = true;
static const char* OTA_HOSTNAME = "m5stick-focus";
static const uint16_t OTA_PORT = 3232;
static const char* OTA_PASSWORD = "";

static const int KEY1_PIN = 11;
static const int KEY2_PIN = 12;

static const uint8_t TOMATO_SCREEN_ROTATION = 0;

static const unsigned long DEBOUNCE_MS = 35;
static const unsigned long DOUBLE_CLICK_MS = 380;
static const unsigned long KEY_LONG_PRESS_MS = 900;
static const unsigned long STATE_POLL_MS = 3000;
static const unsigned long WIFI_RETRY_MS = 5000;
static const unsigned long BRIDGE_HTTP_TIMEOUT_MS = 7000;

static const unsigned long STOPWATCH_DEEP_WORK_SECONDS = 5400;
static const unsigned long STOPWATCH_MILESTONE_SECONDS = 1800;
static const unsigned long ANIMATION_FPS = 20;
static const float TOMATO_MAX_PULSE = 0.08f;

static const unsigned long POMO_COUNTDOWN_SECONDS = 1500;
static const bool HOURGLASS_IMU_ENABLED = true;
static const uint8_t HOURGLASS_GRAVITY_AXIS = 0;
static const bool HOURGLASS_GRAVITY_INVERT = true;
static const float HOURGLASS_FLIP_THRESHOLD = 0.62f;
static const float HOURGLASS_FLAT_Z_THRESHOLD = 0.75f;
static const unsigned long HOURGLASS_ORIENTATION_STABLE_MS = 650;
static const unsigned long HOURGLASS_TRIGGER_COOLDOWN_MS = 2500;

static const bool HARVEST_SOUND_ENABLED = true;
static const unsigned long HARVEST_ANIMATION_MS = 900;
static const bool SOUND_ENABLED = true;
static const bool SOUND_BOOT_CHIRP_ENABLED = true;
static const uint8_t SOUND_VOLUME = 128;
static const uint8_t SOUND_CHANNEL_VOLUME = 255;

static const bool DISPLAY_PROTECTION_ENABLED = true;
static const uint8_t DISPLAY_BRIGHTNESS_ACTIVE = 150;
static const uint8_t DISPLAY_BRIGHTNESS_DIM = 48;
static const uint8_t DISPLAY_BRIGHTNESS_DASHBOARD = 18;
static const unsigned long DISPLAY_DIM_AFTER_MS = 600000;
static const unsigned long DISPLAY_PROTECTION_SHIFT_MS = 45000;
static const int DISPLAY_PROTECTION_SHIFT_PIXELS = 2;
static const unsigned long DASHBOARD_REFRESH_MS = 1000;
static const unsigned long BATTERY_REFRESH_MS = 10000;
