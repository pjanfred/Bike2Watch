#ifndef WEB_DASHBOARD_H
#define WEB_DASHBOARD_H

#include <cstdint>
#include <cstdio>

// Snapshot of everything the web dashboard needs to render. Rebuilt by
// main.cpp's loop() every iteration from the same state that drives the
// BLE server, so the dashboard always shows exactly what's being sent to
// the Watch — never a separately-tracked copy that could drift.
struct Bike2WatchStatus {
    bool bikeConnected  = false;
    bool watchConnected = false;

    bool hasSpeed      = false;
    bool hasCadence    = false;
    bool hasPower      = false;
    bool hasResistance = false;

    float   speedKmh        = 0.0f;
    float   cadenceRpm      = 0.0f;
    int16_t power            = 0;
    int16_t resistanceLevel = 0;  // device-specific unit/scale, no fixed physical unit

    // Whether the connected bike exposed an FTMS Control Point we can use
    // to request a target resistance level (see ftms_control.h) — the
    // dashboard only shows the resistance picker when this is true.
    bool canSetResistance = false;
    // Range/step to offer in that picker, same unitless scale as
    // resistanceLevel above. Sourced from the bike's optional "Supported
    // Resistance Level Range" characteristic when it exposed one; a
    // generic fallback range otherwise (see main.cpp's loop()).
    float resistanceMin  = 0.0f;
    float resistanceMax  = 100.0f;
    float resistanceStep = 1.0f;

    uint32_t wheelRevs = 0;
    uint16_t crankRevs = 0;

    uint32_t uptimeSec = 0;

    const char* deviceName = "";
    const char* wifiMode   = "";   // "WiFi", "Hotspot", or "connecting"
    const char* ipAddress  = "";

    bool mqttEnabled   = false;
    bool mqttConnected = false;

    // Set only when a bike is pinned by MAC (see ble_devices.h) but isn't
    // the one currently connected — lets the dashboard show "Selected
    // device not found" instead of a plain, unexplained "not connected".
    // Both stay false/empty for the default, unpinned plug & play setup.
    bool        pinnedDeviceMissing = false;
    const char* pinnedDeviceName    = "";
};

// Renders `status` as a compact JSON object into `buf` (size `bufSize`).
// Returns the number of bytes written (excluding the null terminator), or
// 0 if `buf` was too small to hold the result. Header-only and
// Arduino-free by design, so the native unit tests can exercise it
// directly without any hardware or emulator.
inline size_t buildStatusJson(const Bike2WatchStatus& status, char* buf, size_t bufSize) {
    // pinnedDeviceName ultimately comes from a BLE advertisement sent by
    // some nearby device — untrusted input that could contain a `"` or
    // `\` to break out of the JSON string. Escape it before embedding;
    // every other field here is app-internal and never needs this.
    char escapedName[64];
    size_t escapedLen = 0;
    for (const char* p = status.pinnedDeviceName; *p && escapedLen + 2 < sizeof(escapedName); ++p) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            escapedName[escapedLen++] = '\\';
            escapedName[escapedLen++] = (char)c;
        } else if (c >= 0x20) {
            escapedName[escapedLen++] = (char)c;
        }
        // control characters (c < 0x20) are silently dropped
    }
    escapedName[escapedLen] = '\0';

    int written = snprintf(buf, bufSize,
        "{"
        "\"bikeConnected\":%s,"
        "\"watchConnected\":%s,"
        "\"hasSpeed\":%s,"
        "\"hasCadence\":%s,"
        "\"hasPower\":%s,"
        "\"hasResistance\":%s,"
        "\"speedKmh\":%.2f,"
        "\"cadenceRpm\":%.1f,"
        "\"power\":%d,"
        "\"resistanceLevel\":%d,"
        "\"canSetResistance\":%s,"
        "\"resistanceMin\":%.1f,"
        "\"resistanceMax\":%.1f,"
        "\"resistanceStep\":%.1f,"
        "\"wheelRevs\":%lu,"
        "\"crankRevs\":%u,"
        "\"uptimeSec\":%lu,"
        "\"deviceName\":\"%s\","
        "\"wifiMode\":\"%s\","
        "\"ipAddress\":\"%s\","
        "\"mqttEnabled\":%s,"
        "\"mqttConnected\":%s,"
        "\"pinnedDeviceMissing\":%s,"
        "\"pinnedDeviceName\":\"%s\""
        "}",
        status.bikeConnected  ? "true" : "false",
        status.watchConnected ? "true" : "false",
        status.hasSpeed      ? "true" : "false",
        status.hasCadence    ? "true" : "false",
        status.hasPower      ? "true" : "false",
        status.hasResistance ? "true" : "false",
        status.speedKmh,
        status.cadenceRpm,
        status.power,
        status.resistanceLevel,
        status.canSetResistance ? "true" : "false",
        status.resistanceMin,
        status.resistanceMax,
        status.resistanceStep,
        (unsigned long)status.wheelRevs,
        status.crankRevs,
        (unsigned long)status.uptimeSec,
        status.deviceName,
        status.wifiMode,
        status.ipAddress,
        status.mqttEnabled   ? "true" : "false",
        status.mqttConnected ? "true" : "false",
        status.pinnedDeviceMissing ? "true" : "false",
        escapedName);

    if (written < 0 || (size_t)written >= bufSize) return 0;
    return (size_t)written;
}

#endif // WEB_DASHBOARD_H
