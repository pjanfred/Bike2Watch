#ifndef SETTINGS_STORE_H
#define SETTINGS_STORE_H

#include "settings.h"

// Loads settings from flash (NVS). On the very first boot ever (nothing
// saved yet), seeds them from the compile-time defaults in config.h and
// persists that seed immediately, so every later boot — and the Settings
// page — always reads from NVS directly.
Bike2WatchSettings loadSettings();

// Persists `settings` to flash (NVS). WiFi/MQTT/password changes take
// effect after the next reboot.
void saveSettings(const Bike2WatchSettings& settings);

// Wipes every persisted setting (WiFi, MQTT, pinned bike, wheel size,
// admin password) from flash. The next loadSettings() call — i.e. the
// next boot — then behaves exactly like a brand new device: reseeded from
// config.h's compile-time defaults. Does not itself reboot; the caller is
// responsible for that.
void factoryResetSettings();

#endif // SETTINGS_STORE_H
