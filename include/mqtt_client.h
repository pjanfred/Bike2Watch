#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include "settings.h"
#include "web_dashboard.h"

// Configures the MQTT client from `settings`. Call once from setup(),
// after loadSettings(). Does nothing (and handleMqtt() stays a no-op) if
// MQTT isn't enabled or no broker host is configured.
void setupMqtt(const Bike2WatchSettings& settings);

// Handles (re)connecting to the broker in the background and publishes
// the current status about once a second. Call every loop() iteration —
// cheap and non-blocking. Only publishes while joined to a real WiFi
// network, not while the bridge is running its own fallback hotspot.
void handleMqtt(const Bike2WatchStatus& status);

// True if MQTT is enabled and a broker host is configured.
bool mqttIsEnabled();

// True if currently connected to the broker.
bool mqttIsConnected();

// Publishes a momentary "pressed" event for the board's physical BOOT
// button (see checkResetButton() in main.cpp) — a normal, short press,
// entirely separate from the long-hold factory reset. Lets Home
// Assistant (or anything else subscribed) react to it, e.g. as an
// automation trigger. No-op if MQTT isn't enabled or not currently
// connected — the button still works locally either way, this is purely
// an additional notification.
void notifyBootButtonPressed();

#endif // MQTT_CLIENT_H
