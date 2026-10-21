#ifndef OTA_H
#define OTA_H

#include "settings.h"

// Enables network flashing via PlatformIO/Arduino IDE, e.g.:
//   pio run -t upload --upload-port bike2watch.local
// Call once from setup(), after WiFi is up. Protected by
// settings.adminPassword, if set (see Bike2WatchSettings in settings.h).
void setupOta(const Bike2WatchSettings& settings);

// Services pending OTA requests. Call every loop() iteration.
void handleOta();

#endif // OTA_H
