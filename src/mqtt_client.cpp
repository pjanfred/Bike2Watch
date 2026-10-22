#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include "mqtt_client.h"
#include "ftms_control.h"
#include "config.h"

#include "log.h"

static WiFiClient     g_wifiClient;
static PubSubClient   g_mqttClient(g_wifiClient);
static Bike2WatchSettings g_settings;
static char           g_clientId[40];
static unsigned long  g_lastReconnectAttemptMs = 0;
static unsigned long  g_lastPublishMs           = 0;

static const unsigned long RECONNECT_INTERVAL_MS     = 5000;
// Cap for the backoff below — a broker that's merely restarting should
// still be picked back up within a minute of it coming back.
static const unsigned long RECONNECT_INTERVAL_MAX_MS = 60000;
static const unsigned long PUBLISH_INTERVAL_MS        = 1000;

// Grows on every failed connect attempt (doubling, capped at
// RECONNECT_INTERVAL_MAX_MS) and resets to RECONNECT_INTERVAL_MS on
// success — see handleMqtt(). A wrong host/port would otherwise retry
// every RECONNECT_INTERVAL_MS forever, and each attempt blocks loop()
// (BLE notifications, the web dashboard) for as long as the connect/
// socket timeouts in setupMqtt() allow.
static unsigned long g_reconnectIntervalMs = RECONNECT_INTERVAL_MS;

static void buildClientId() {
    uint64_t chipId = ESP.getEfuseMac();
    snprintf(g_clientId, sizeof(g_clientId), "bike2watch-%08X%08X",
             (uint32_t)(chipId >> 32), (uint32_t)chipId);
}

bool mqttIsEnabled() {
    return g_settings.mqttEnabled && strlen(g_settings.mqttHost) > 0;
}

bool mqttIsConnected() {
    return g_mqttClient.connected();
}

static String availabilityTopic() {
    return String(g_settings.mqttTopicPrefix) + "/available";
}

// Command topic for setting the bike's target resistance over MQTT —
// publish a plain number (e.g. "50" or "12.5") to it. Same unitless scale
// as the resistance_level state topic / the dashboard's resistance card.
static String resistanceSetTopic() {
    return String(g_settings.mqttTopicPrefix) + "/resistance_level/set";
}

// PubSubClient message callback — only one command topic exists right
// now, so no real dispatch logic is needed, just a suffix check. Forwards
// to requestSetResistance() (see ftms_control.h), which queues the actual
// BLE write on the BLE client's own task, same as the dashboard's
// resistance picker — this callback runs from handleMqtt()'s call to
// g_mqttClient.loop() (i.e. loop()'s task), so it must never touch the
// BLE client directly, same reasoning as web_server.cpp's /resistance
// handler.
static void onMqttMessage(char* topic, byte* payload, unsigned int length) {
    if (resistanceSetTopic() != topic) return;

    if (length == 0 || length >= 16) {
        LOG("MQTT: ignoring resistance_level/set payload of implausible length %u", length);
        return;
    }
    char text[16];
    memcpy(text, payload, length);
    text[length] = '\0';

    char* parseEnd = nullptr;
    float level = strtof(text, &parseEnd);
    if (parseEnd == text) {
        LOG("MQTT: ignoring non-numeric resistance_level/set payload '%s'", text);
        return;
    }

    if (requestSetResistance(level)) {
        LOG("MQTT: resistance_level/set -> %.1f (queued)", level);
    } else {
        LOG("MQTT: resistance_level/set -> %.1f dropped, no bike connected", level);
    }
}

// ── Home Assistant MQTT Discovery ───────────────────────────────────
// Announces the bridge and its entities so they show up in Home
// Assistant automatically, with no manual YAML. Published (retained) once
// per successful broker connection — see:
// https://www.home-assistant.io/integrations/mqtt/#mqtt-discovery

static String haDeviceJson() {
    String json = "{\"identifiers\":[\"";
    json += g_clientId;
    json += "\"],\"name\":\"";
    json += DEVICE_NAME;
    json += "\",\"manufacturer\":\"Bike2Watch\",\"model\":\"ESP32 Bike2Watch\"";
    // Lets Home Assistant show a "Visit" link to the bridge's own web
    // dashboard right on its device page — no separate IP topic/sensor
    // needed. Safe to read here unconditionally: haDeviceJson() is only
    // ever called from publishDiscoveryEntities(), which only runs after
    // mqttReconnect() succeeds, which itself only happens once
    // handleMqtt() has already confirmed WiFi.status() == WL_CONNECTED —
    // so WiFi.localIP() is always a real address at this point, never
    // 0.0.0.0.
    json += ",\"configuration_url\":\"http://";
    json += WiFi.localIP().toString();
    json += "/\"";
    json += "}";
    return json;
}

static void publishSensorDiscovery(const char* objectId, const char* name,
                                    const char* unit, const char* deviceClass,
                                    const char* icon, const char* stateSuffix) {
    char configTopic[128];
    snprintf(configTopic, sizeof(configTopic), "%s/sensor/%s/%s/config",
             HA_DISCOVERY_PREFIX, g_clientId, objectId);

    String payload = "{";
    payload += "\"name\":\"" + String(name) + "\",";
    payload += "\"unique_id\":\"" + String(g_clientId) + "_" + objectId + "\",";
    payload += "\"state_topic\":\"" + String(g_settings.mqttTopicPrefix) + "/" + stateSuffix + "\",";
    payload += "\"availability_topic\":\"" + availabilityTopic() + "\",";
    if (unit)        payload += "\"unit_of_measurement\":\"" + String(unit) + "\",";
    if (deviceClass) payload += "\"device_class\":\"" + String(deviceClass) + "\",";
    payload += "\"state_class\":\"measurement\",";
    if (icon)        payload += "\"icon\":\"" + String(icon) + "\",";
    payload += "\"device\":" + haDeviceJson();
    payload += "}";

    g_mqttClient.publish(configTopic, payload.c_str(), true /* retained */);
}

static void publishBinarySensorDiscovery(const char* objectId, const char* name,
                                          const char* deviceClass, const char* stateSuffix) {
    char configTopic[128];
    snprintf(configTopic, sizeof(configTopic), "%s/binary_sensor/%s/%s/config",
             HA_DISCOVERY_PREFIX, g_clientId, objectId);

    String payload = "{";
    payload += "\"name\":\"" + String(name) + "\",";
    payload += "\"unique_id\":\"" + String(g_clientId) + "_" + objectId + "\",";
    payload += "\"state_topic\":\"" + String(g_settings.mqttTopicPrefix) + "/" + stateSuffix + "\",";
    payload += "\"availability_topic\":\"" + availabilityTopic() + "\",";
    payload += "\"payload_on\":\"true\",";
    payload += "\"payload_off\":\"false\",";
    if (deviceClass) payload += "\"device_class\":\"" + String(deviceClass) + "\",";
    payload += "\"device\":" + haDeviceJson();
    payload += "}";

    g_mqttClient.publish(configTopic, payload.c_str(), true /* retained */);
}

// A momentary Home Assistant "event" entity (HA 2023.8+) — unlike a
// sensor/binary_sensor, it has no persistent state to show, just fires
// whenever a message lands on state_topic, which is exactly what a
// button press is. Used for the board's physical BOOT button (see
// notifyBootButtonPressed() below) so a normal short press can drive a
// Home Assistant automation, separate from the long-hold factory reset.
static void publishEventDiscovery(const char* objectId, const char* name,
                                   const char* deviceClass, const char* stateSuffix,
                                   const char* const* eventTypes, size_t eventTypeCount) {
    char configTopic[128];
    snprintf(configTopic, sizeof(configTopic), "%s/event/%s/%s/config",
             HA_DISCOVERY_PREFIX, g_clientId, objectId);

    String payload = "{";
    payload += "\"name\":\"" + String(name) + "\",";
    payload += "\"unique_id\":\"" + String(g_clientId) + "_" + objectId + "\",";
    payload += "\"state_topic\":\"" + String(g_settings.mqttTopicPrefix) + "/" + stateSuffix + "\",";
    payload += "\"availability_topic\":\"" + availabilityTopic() + "\",";
    if (deviceClass) payload += "\"device_class\":\"" + String(deviceClass) + "\",";
    payload += "\"event_types\":[";
    for (size_t i = 0; i < eventTypeCount; i++) {
        if (i > 0) payload += ",";
        payload += "\"" + String(eventTypes[i]) + "\"";
    }
    payload += "],";
    payload += "\"device\":" + haDeviceJson();
    payload += "}";

    g_mqttClient.publish(configTopic, payload.c_str(), true /* retained */);
}

// A settable Home Assistant "number" entity (shows as a numeric input,
// not just a read-only sensor) — its command_topic is the same one
// onMqttMessage() above listens on, so setting it in the HA UI reaches
// requestSetResistance() the same way a manual `mosquitto_pub` or the
// dashboard's resistance picker does. Its state_topic reuses the existing
// resistance_level state (the bike's live-reported value) rather than
// tracking a separate "last commanded" value — same simplification the
// dashboard picker makes; there's no confirmation from the bike either
// way (see ftms_control.h). min/max are generic, not the connected
// bike's actual range (that's only known once a bike is connected, which
// may happen before or after this runs, and MQTT discovery doesn't get a
// live status snapshot to read it from) — "mode":"box" keeps this a
// plain number input rather than a slider, so the generic bounds don't
// visually imply a hard limit that might not match the real bike.
static void publishNumberDiscovery(const char* objectId, const char* name,
                                    const char* stateSuffix, const String& commandTopic,
                                    float min, float max, float step) {
    char configTopic[128];
    snprintf(configTopic, sizeof(configTopic), "%s/number/%s/%s/config",
             HA_DISCOVERY_PREFIX, g_clientId, objectId);

    String payload = "{";
    payload += "\"name\":\"" + String(name) + "\",";
    payload += "\"unique_id\":\"" + String(g_clientId) + "_" + objectId + "\",";
    payload += "\"state_topic\":\"" + String(g_settings.mqttTopicPrefix) + "/" + stateSuffix + "\",";
    payload += "\"command_topic\":\"" + commandTopic + "\",";
    payload += "\"availability_topic\":\"" + availabilityTopic() + "\",";
    payload += "\"min\":" + String(min, 1) + ",";
    payload += "\"max\":" + String(max, 1) + ",";
    payload += "\"step\":" + String(step, 1) + ",";
    payload += "\"mode\":\"box\",";
    payload += "\"icon\":\"mdi:knob\",";
    payload += "\"device\":" + haDeviceJson();
    payload += "}";

    g_mqttClient.publish(configTopic, payload.c_str(), true /* retained */);
}

static void publishDiscoveryEntities() {
    publishSensorDiscovery("speed", "Speed", "km/h", nullptr, "mdi:speedometer", "speed_kmh");
    publishSensorDiscovery("cadence", "Cadence", "rpm", nullptr, "mdi:rotate-right", "cadence_rpm");
    publishSensorDiscovery("power", "Power", "W", "power", nullptr, "power_watt");
    // No fixed unit_of_measurement — FTMS resistance level is a
    // device-specific scale, not a standardized physical unit (unlike
    // speed/cadence/power above).
    publishSensorDiscovery("resistance", "Resistance", nullptr, nullptr, "mdi:knob", "resistance_level");
    publishNumberDiscovery("resistance_target", "Target Resistance", "resistance_level",
                            resistanceSetTopic(), -50.0f, 100.0f, 1.0f);
    publishBinarySensorDiscovery("bike_connected", "Bike Connected", "connectivity", "bike_connected");
    publishBinarySensorDiscovery("watch_connected", "Watch Connected", "connectivity", "watch_connected");
    static const char* const bootButtonEvents[] = {"pressed"};
    publishEventDiscovery("boot_button", "Boot Button", "button", "boot_button",
                           bootButtonEvents, 1);
    LOG("MQTT: Home Assistant discovery published (device '%s')", DEVICE_NAME);
}

void setupMqtt(const Bike2WatchSettings& settings) {
    g_settings = settings;
    if (!mqttIsEnabled()) {
        LOG("MQTT: disabled");
        return;
    }
    buildClientId();
    g_mqttClient.setServer(g_settings.mqttHost, g_settings.mqttPort);
    g_mqttClient.setBufferSize(768);
    g_mqttClient.setCallback(onMqttMessage);
    // mqttReconnect() runs synchronously in loop() (see handleMqtt()), so
    // a slow/failing connect attempt freezes BLE notifications and the web
    // dashboard for as long as it blocks. The library defaults (3s TCP
    // connect, 15s waiting for the broker's CONNACK) assume a client on a
    // normal desktop network, not a single-threaded embedded loop — with a
    // wrong host/port (TCP connects to something that isn't an MQTT broker
    // and never replies) that's up to 15s frozen per attempt. Tightened
    // here so a bad config still fails fast instead of stalling everything.
    g_wifiClient.setTimeout(2);        // seconds; caps the raw TCP connect()
    g_mqttClient.setSocketTimeout(5);  // seconds; caps waiting for the CONNACK
    LOG("MQTT: configured for %s:%u (client id %s, topic prefix '%s')",
        g_settings.mqttHost, g_settings.mqttPort, g_clientId, g_settings.mqttTopicPrefix);
}

static bool mqttReconnect() {
    LOG("MQTT: connecting to %s:%u...", g_settings.mqttHost, g_settings.mqttPort);

    String willTopic = availabilityTopic();
    bool connected;
    if (strlen(g_settings.mqttUser) > 0) {
        connected = g_mqttClient.connect(g_clientId, g_settings.mqttUser, g_settings.mqttPassword,
                                          willTopic.c_str(), 0, true, "offline");
    } else {
        connected = g_mqttClient.connect(g_clientId, willTopic.c_str(), 0, true, "offline");
    }

    if (connected) {
        LOG("MQTT: connected");
        g_mqttClient.publish(willTopic.c_str(), "online", true /* retained */);
        publishDiscoveryEntities();

        String commandTopic = resistanceSetTopic();
        g_mqttClient.subscribe(commandTopic.c_str());
        LOG("MQTT: subscribed to %s", commandTopic.c_str());
    } else {
        LOG("MQTT: connect failed, rc=%d", g_mqttClient.state());
    }
    return connected;
}

static void publishTopic(const char* suffix, const String& value) {
    char topic[96];
    snprintf(topic, sizeof(topic), "%s/%s", g_settings.mqttTopicPrefix, suffix);
    g_mqttClient.publish(topic, value.c_str());
}

void notifyBootButtonPressed() {
    // No queuing for "publish once reconnected" — a button press is only
    // meaningful in the moment. Silently dropped if the broker isn't
    // connected right now, same as every other best-effort publish here.
    if (!mqttIsConnected()) return;
    publishTopic("boot_button", "{\"event_type\":\"pressed\"}");
}

void handleMqtt(const Bike2WatchStatus& status) {
    if (!mqttIsEnabled()) return;
    // The broker is only reachable while we're a station on a real
    // network — not while the bridge is serving its own fallback hotspot.
    if (WiFi.getMode() != WIFI_STA && WiFi.getMode() != WIFI_AP_STA) return;
    if (WiFi.status() != WL_CONNECTED) return;

    if (!g_mqttClient.connected()) {
        unsigned long now = millis();
        if (now - g_lastReconnectAttemptMs >= g_reconnectIntervalMs) {
            g_lastReconnectAttemptMs = now;
            if (mqttReconnect()) {
                g_reconnectIntervalMs = RECONNECT_INTERVAL_MS;
            } else {
                // Back off (instead of hammering a broken config every
                // RECONNECT_INTERVAL_MS) — each attempt still briefly
                // blocks loop() (bounded by the connect/socket timeouts
                // in setupMqtt()), so a wrong host/port should retry
                // rarely, not constantly.
                g_reconnectIntervalMs = min(g_reconnectIntervalMs * 2, RECONNECT_INTERVAL_MAX_MS);
            }
        }
        return;
    }

    g_mqttClient.loop();

    unsigned long now = millis();
    if (now - g_lastPublishMs < PUBLISH_INTERVAL_MS) return;
    g_lastPublishMs = now;

    // 640, not 512 — same reasoning as handleStatus() in web_server.cpp:
    // the resistance-control status fields pushed the worst case close to
    // the old buffer size.
    char statusJson[640];
    if (buildStatusJson(status, statusJson, sizeof(statusJson)) > 0) {
        char topic[96];
        snprintf(topic, sizeof(topic), "%s/status", g_settings.mqttTopicPrefix);
        g_mqttClient.publish(topic, statusJson);
    }

    // Individual topics too — these are what the Home Assistant discovery
    // entities above subscribe to, so they don't need to parse JSON.
    publishTopic("speed_kmh", status.hasSpeed ? String(status.speedKmh, 1) : String("0"));
    publishTopic("cadence_rpm", status.hasCadence ? String(status.cadenceRpm, 0) : String("0"));
    publishTopic("power_watt", status.hasPower ? String(status.power) : String("0"));
    publishTopic("resistance_level", status.hasResistance ? String(status.resistanceLevel) : String("0"));
    publishTopic("bike_connected", status.bikeConnected ? "true" : "false");
    publishTopic("watch_connected", status.watchConnected ? "true" : "false");
}
