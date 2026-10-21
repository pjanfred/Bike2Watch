#include <Preferences.h>
#include "settings_store.h"
#include "config.h"

// NVS namespace name (max 15 characters, an ESP-IDF limit).
static const char* NVS_NAMESPACE = "bike2watch";

Bike2WatchSettings loadSettings() {
    Bike2WatchSettings s;

    // Seeds from the compile-time ADMIN_PASSWORD; the prefs.getString()
    // call below overwrites this once "adminPass" actually exists in NVS
    // (i.e. after saveSettings() has run at least once), so a password set
    // later via the web UI always wins.
    copyIntoField(s.adminPassword, sizeof(s.adminPassword), ADMIN_PASSWORD);

    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, /*readOnly=*/true);
    bool hasSavedSettings = prefs.isKey("wifiSsid");
    if (hasSavedSettings) {
        prefs.getString("wifiSsid", s.wifiSsid, sizeof(s.wifiSsid));
        prefs.getString("wifiPass", s.wifiPassword, sizeof(s.wifiPassword));
        s.mqttEnabled = prefs.getBool("mqttEn", false);
        prefs.getString("mqttHost", s.mqttHost, sizeof(s.mqttHost));
        s.mqttPort = prefs.getUShort("mqttPort", 1883);
        prefs.getString("mqttUser", s.mqttUser, sizeof(s.mqttUser));
        prefs.getString("mqttPass", s.mqttPassword, sizeof(s.mqttPassword));
        prefs.getString("mqttTopic", s.mqttTopicPrefix, sizeof(s.mqttTopicPrefix));
        // getString() leaves the buffer untouched if the key doesn't exist
        // in NVS, so this stays at the struct's own default (empty — no
        // pin set, plug & play) in that case.
        prefs.getString("pinMac", s.pinnedBikeMac, sizeof(s.pinnedBikeMac));
        prefs.getString("pinName", s.pinnedBikeName, sizeof(s.pinnedBikeName));
        // getUShort()'s default parameter (the compile-time value) kicks
        // in if "wheelCirc" doesn't exist in NVS.
        s.wheelCircumferenceMm = prefs.getUShort("wheelCirc", WHEEL_CIRCUMFERENCE_MM);
        // Only overwrites s.adminPassword if "adminPass" exists in NVS —
        // otherwise it stays at the ADMIN_PASSWORD fallback set above.
        prefs.getString("adminPass", s.adminPassword, sizeof(s.adminPassword));
        // getBool()'s default parameter (false) kicks in if "liteMode"
        // doesn't exist in NVS.
        s.liteModeEnabled = prefs.getBool("liteMode", false);
    }
    prefs.end();

    if (!hasSavedSettings) {
        // First boot ever — seed from config.h and persist it immediately,
        // so every future boot (and the Settings page) reads from NVS.
        copyIntoField(s.wifiSsid, sizeof(s.wifiSsid), WIFI_SSID);
        copyIntoField(s.wifiPassword, sizeof(s.wifiPassword), WIFI_PASSWORD);
        s.mqttEnabled = MQTT_ENABLED;
        copyIntoField(s.mqttHost, sizeof(s.mqttHost), MQTT_HOST);
        s.mqttPort = MQTT_PORT;
        copyIntoField(s.mqttUser, sizeof(s.mqttUser), MQTT_USER);
        copyIntoField(s.mqttPassword, sizeof(s.mqttPassword), MQTT_PASSWORD);
        copyIntoField(s.mqttTopicPrefix, sizeof(s.mqttTopicPrefix), MQTT_TOPIC_PREFIX);
        s.wheelCircumferenceMm = WHEEL_CIRCUMFERENCE_MM;
        // s.adminPassword was already seeded from ADMIN_PASSWORD above.
        saveSettings(s);
    }

    return s;
}

void saveSettings(const Bike2WatchSettings& s) {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, /*readOnly=*/false);
    prefs.putString("wifiSsid", s.wifiSsid);
    prefs.putString("wifiPass", s.wifiPassword);
    prefs.putBool("mqttEn", s.mqttEnabled);
    prefs.putString("mqttHost", s.mqttHost);
    prefs.putUShort("mqttPort", s.mqttPort);
    prefs.putString("mqttUser", s.mqttUser);
    prefs.putString("mqttPass", s.mqttPassword);
    prefs.putString("mqttTopic", s.mqttTopicPrefix);
    prefs.putString("pinMac", s.pinnedBikeMac);
    prefs.putString("pinName", s.pinnedBikeName);
    prefs.putUShort("wheelCirc", s.wheelCircumferenceMm);
    prefs.putString("adminPass", s.adminPassword);
    prefs.putBool("liteMode", s.liteModeEnabled);
    prefs.end();
}

void factoryResetSettings() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, /*readOnly=*/false);
    prefs.clear();
    prefs.end();
}
