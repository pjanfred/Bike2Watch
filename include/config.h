#ifndef CONFIG_H
#define CONFIG_H

// Compile-time defaults. Every setting listed here only ever seeds the
// very first boot (or, for RESET_BUTTON_*/LED_PIN, is a fixed hardware
// choice) — WiFi, MQTT, wheel size and the admin password all move into
// flash (NVS) after that and are edited from the /settings page, so
// reflashing never wipes a device's live configuration. See
// settings_store.cpp for the seed-then-persist logic.

#define DEVICE_NAME "Bike2Watch"

// GPIO2 + a plain digitalWrite() matches the onboard LED wiring of
// classic ESP32 dev boards. The esp32-s3-devkitc-1 PlatformIO environment
// overrides both LED_PIN and LED_IS_RGB via build_flags to match that
// board's onboard addressable WS2812 (GPIO48) instead -- see main.cpp's
// setLed(), which picks digitalWrite() vs. neopixelWrite() based on
// LED_IS_RGB. Boards with neither (or a differently wired LED) can still
// override either macro the same way.
#ifndef LED_PIN
#define LED_PIN 2
#endif
#ifndef LED_IS_RGB
#define LED_IS_RGB 0
#endif

// Identifies which PlatformIO environment (= which board) this binary was
// built for, set via build_flags in platformio.ini. Used by the Firmware
// Update page's "Check for updates" feature (see update_check.h /
// web_server.cpp) to pick the matching .bin asset out of a GitHub release
// that publishes one binary per board. Defaults to "esp32dev" so the
// feature still degrades sensibly if ever built outside PlatformIO
// without the flag, same spirit as the LED_PIN/LED_IS_RGB fallbacks above.
#ifndef BOARD_ENV
#define BOARD_ENV "esp32dev"
#endif

// Wheel circumference in mm — see Bike2WatchSettings::wheelCircumferenceMm in
// settings.h for what it's actually used for.
#define WHEEL_CIRCUMFERENCE_MM 2096

// Hold the factory-reset button down for RESET_BUTTON_HOLD_MS to wipe all
// settings and reboot, same as the Danger Zone button on the Settings
// page — see checkResetButton() in main.cpp. RESET_BUTTON_PIN defaults to
// GPIO0, the "BOOT" button already built into every ESP32 dev board, so
// this works with no extra wiring. GPIO0 also happens to be a boot-mode
// strapping pin, but that only matters at the moment of reset/power-up —
// reading it as a plain input afterwards, once the board has already
// booted normally, doesn't affect anything, which is why "hold BOOT to
// reset" is such a common pattern on ESP32 boards generally.
#define RESET_BUTTON_PIN     0
#define RESET_BUTTON_HOLD_MS 15000

// While Lite Mode is active (see Bike2WatchSettings::liteModeEnabled in
// settings.h), releasing the BOOT button after holding it between
// LITE_MODE_EXIT_HOLD_MS and RESET_BUTTON_HOLD_MS exits Lite Mode and
// reboots back into normal (WiFi-enabled) operation — see
// checkResetButton() in main.cpp. Holding past RESET_BUTTON_HOLD_MS still
// factory-resets as usual, Lite Mode or not.
#define LITE_MODE_EXIT_HOLD_MS 5000

// WiFi / web dashboard
#define WIFI_SSID               ""
#define WIFI_PASSWORD           ""
#define WIFI_AP_SSID             "Bike2Watch"
#define WIFI_AP_PASSWORD         ""   // 8+ characters, or "" for an open network
#define WIFI_CONNECT_TIMEOUT_MS  15000
#define WEB_HOSTNAME             "bike2watch"

// MQTT
#define MQTT_ENABLED       false
#define MQTT_HOST          ""
#define MQTT_PORT          1883
#define MQTT_USER          ""
#define MQTT_PASSWORD      ""
#define MQTT_TOPIC_PREFIX  "bike2watch"

#define HA_DISCOVERY_PREFIX "homeassistant"

// Protects the entire web interface (dashboard included) and network
// (OTA) flashing. Strongly recommended once the bridge is reachable on a
// shared WiFi network — anyone who can reach it could otherwise reflash
// it or read back your MQTT credentials. Username is always "admin".
// Leave empty ("") to disable protection. Once set (here or from the
// Settings page), can also be removed again from the Settings page, or by
// holding the factory-reset button — see RESET_BUTTON_PIN above.
#define ADMIN_PASSWORD ""

#endif // CONFIG_H
