#ifndef SETTINGS_H
#define SETTINGS_H

#include <cstdint>
#include <cstring>

// Persisted configuration — stored in flash (NVS) so it survives reboots
// and reflashes. Edited via the web dashboard's Settings page
// (GET/POST /settings); the values in config.h only seed the very first
// boot, before anything has been saved.
struct Bike2WatchSettings {
    char wifiSsid[33]     = "";
    char wifiPassword[65] = "";

    bool     mqttEnabled         = false;
    char     mqttHost[65]        = "";
    uint16_t mqttPort            = 1883;
    char     mqttUser[33]        = "";
    char     mqttPassword[65]    = "";
    char     mqttTopicPrefix[33] = "bike2watch";

    // Optional: pins the bridge to one specific bike instead of connecting
    // to the first FTMS device a scan finds. Empty (the default) means "no
    // pin" — plug & play. Format: "aa:bb:cc:dd:ee:ff", as returned by
    // NimBLEAddress::toString().
    char pinnedBikeMac[18]  = "";
    char pinnedBikeName[32] = "";

    // Wheel circumference in mm, used to turn the bike's real speed into
    // simulated wheel revolutions for the CSC characteristic (see
    // bike2watch.h). Only has to match whatever wheel size the paired
    // Watch/head unit is configured with — it doesn't need to be
    // physically accurate, since an indoor bike has no real wheel. 2096mm
    // (a common 700x25c road wheel) is a reasonable default that matches
    // most head units out of the box.
    uint16_t wheelCircumferenceMm = 2096;

    // Protects the entire web interface — dashboard included — plus
    // network (OTA) flashing from anyone else who can reach the bridge on
    // the network (see isAuthorized() in web_server.cpp). Empty (the
    // default) means no password is required. Username is always "admin".
    char adminPassword[65] = "";

    // Set from the Settings page's "Lite Mode" button; cleared again by
    // holding the BOOT button (see checkResetButton() in main.cpp) or by a
    // factory reset. While true, setup() skips WiFi/hotspot, the web
    // dashboard, MQTT and OTA entirely — only the BLE bridge itself (bike
    // <-> watch) keeps running. See LITE_MODE_EXIT_HOLD_MS in config.h for
    // the BOOT-button hold duration that exits it again.
    bool liteModeEnabled = false;
};

// Copies `src` into the fixed-size field `dst` (size `dstSize`), always
// null-terminating even if `src` is longer than the field can hold. Small
// helper shared by the settings store and the Settings-page form handler.
inline void copyIntoField(char* dst, size_t dstSize, const char* src) {
    strncpy(dst, src, dstSize - 1);
    dst[dstSize - 1] = '\0';
}

#endif // SETTINGS_H
