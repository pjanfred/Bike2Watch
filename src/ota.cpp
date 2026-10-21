#include <Arduino.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <cstring>
#include "ota.h"
#include "config.h"

#if DEBUG_LOG
  #define LOG(fmt, ...) Serial.printf(fmt "\n", ##__VA_ARGS__)
#else
  #define LOG(fmt, ...)
#endif

void setupOta(const Bike2WatchSettings& settings) {
    ArduinoOTA.setHostname(WEB_HOSTNAME);
    if (strlen(settings.adminPassword) > 0) {
        ArduinoOTA.setPassword(settings.adminPassword);
    }

    ArduinoOTA.onStart([]() {
        LOG("OTA: update starting (%s)",
            ArduinoOTA.getCommand() == U_FLASH ? "firmware" : "filesystem");
    });
    ArduinoOTA.onEnd([]() {
        LOG("OTA: update complete, rebooting");
    });
    ArduinoOTA.onProgress([](unsigned int bytesDone, unsigned int bytesTotal) {
        static unsigned long lastLogMs = 0;
        if (millis() - lastLogMs > 1000) {
            lastLogMs = millis();
            LOG("OTA: %u%%", bytesTotal ? (bytesDone * 100) / bytesTotal : 0);
        }
    });
    ArduinoOTA.onError([](ota_error_t error) {
        LOG("OTA: error [%u]", error);
    });

    ArduinoOTA.begin();
    LOG("OTA: network flashing ready (hostname '%s')", WEB_HOSTNAME);
}

void handleOta() {
    ArduinoOTA.handle();
}
