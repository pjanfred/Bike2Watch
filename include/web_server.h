#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "web_dashboard.h"
#include "settings.h"

// Connects to the configured WiFi (falling back to the bridge's own
// hotspot if that fails, or if no WiFi is configured yet), starts mDNS,
// and starts the HTTP server (dashboard, Settings, Bike and
// Firmware-Update pages). Call once from setup(), after loadSettings().
void setupWebServer(const Bike2WatchSettings& settings);

// Services pending HTTP requests. Call once per loop() iteration — cheap
// and non-blocking when there's nothing to do.
void handleWebServerClients();

// Publishes the latest bridge status so the next dashboard request serves
// fresh data. Call from loop() every iteration — it's just a struct copy.
void updateWebStatus(const Bike2WatchStatus& status);

#endif // WEB_SERVER_H
