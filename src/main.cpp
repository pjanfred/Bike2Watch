#include <Arduino.h>
#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <strings.h>
#include <string>
#include "bike2watch.h"
#include "web_server.h"
#include "settings_store.h"
#include "mqtt_client.h"
#include "ble_devices.h"
#include "ftms_control.h"
#include "ota.h"
#include "config.h"

#include "log.h"

// ── BLE UUIDs ────────────────────────────────────────────────────────
static const NimBLEUUID FTMS_SERVICE_UUID((uint16_t)0x1826);
static const NimBLEUUID FTMS_INDOOR_BIKE_UUID((uint16_t)0x2AD2);
static const NimBLEUUID FTMS_CONTROL_POINT_UUID((uint16_t)0x2AD9);
static const NimBLEUUID FTMS_RESISTANCE_RANGE_UUID((uint16_t)0x2AD6);
static const NimBLEUUID CSC_SERVICE_UUID((uint16_t)0x1816);
static const NimBLEUUID CSC_MEASUREMENT_UUID((uint16_t)0x2A5B);
static const NimBLEUUID CSC_FEATURE_UUID((uint16_t)0x2A5C);
static const NimBLEUUID CP_SERVICE_UUID((uint16_t)0x1818);
static const NimBLEUUID CP_MEASUREMENT_UUID((uint16_t)0x2A63);
static const NimBLEUUID CP_FEATURE_UUID((uint16_t)0x2A65);
static const NimBLEUUID SENSOR_LOCATION_UUID((uint16_t)0x2A5D);

// ── Live bike data (written by the BLE notify callback, read by loop) ─
static volatile bool     g_bikeConnected   = false;
static volatile bool     g_bikeDataReady   = false;
static volatile uint16_t g_speedRaw        = 0;
static volatile uint16_t g_cadenceRaw      = 0;
static volatile int16_t  g_power           = 0;
static volatile int16_t  g_resistanceLevel = 0;
static volatile bool     g_hasSpeed        = false;
static volatile bool     g_hasCadence      = false;
static volatile bool     g_hasPower        = false;
static volatile bool     g_hasResistance   = false;

// ── BLE client (bridge → bike) state ──────────────────────────────────
// The advertised device is only valid inside the scan callback (NimBLE
// frees scan results when the next scan starts), so its address is copied
// here and used to connect later instead of keeping the pointer.
static NimBLEAddress           g_targetAddress;
static volatile bool           g_hasTarget      = false;
static volatile bool           g_wantsToConnect = false;
static NimBLEClient*           g_bikeClient     = nullptr;

// FTMS Control Point on the connected bike, kept around after
// connectToFtms() so requestSetResistance() (see ftms_control.h) can use
// it later. Both this and g_resistanceRange reset to "unavailable" on
// disconnect (BikeClientCallbacks) so a stale pointer from a previous
// connection is never written to.
static NimBLERemoteCharacteristic* g_ftmsControlPoint = nullptr;
static ResistanceRange             g_resistanceRange;

// ── BLE server (bridge → Watch) state ─────────────────────────────────
static NimBLEServer*         g_bleServer      = nullptr;
static NimBLECharacteristic* g_cscMeasurement = nullptr;
static NimBLECharacteristic* g_cpMeasurement  = nullptr;
static volatile bool         g_watchConnected = false;
static CscAccumulator        g_csc;

// ── Timing ─────────────────────────────────────────────────────────
static unsigned long g_lastNotifyMs = 0;
static const unsigned long NOTIFY_INTERVAL_MS = 1000;

// Cooldown before the next scan/reconnect attempt — read/written only
// from bleClientTask(), see below.
static unsigned long g_bleRetryAtMs = 0;
static const unsigned long BLE_RETRY_INTERVAL_MS = 5000;

// Loaded once in setup() and kept in sync with NVS by setPinnedDevice()/
// clearPinnedDevice() and setupWebServer()'s own copy. g_settings.
// pinnedBikeMac == "" is the default and means "no pin" — plug & play.
static Bike2WatchSettings g_settings;
// Guards g_settings.pinnedBikeMac/Name, which the web task writes and the
// NimBLE host task reads (ScanCallbacks::onResult).
static SemaphoreHandle_t g_pinMutex = nullptr;

enum class ScanPurpose { AutoConnect, DiscoveryOnly };
static volatile ScanPurpose g_scanPurpose = ScanPurpose::AutoConnect;
static volatile bool        g_scanning    = false;

// Set by requestImmediateRescan() / requestDiscoveryScanWhileConnected();
// consumed by bleClientTask(). Kept separate from g_wantsToConnect/
// g_bleRetryAtMs so a Settings-page click can never fight with the normal
// reconnect timer for the same variables.
static volatile bool g_manualRescanRequested = false;
static volatile bool g_discoveryScanRequested = false;

// Set by requestSetResistance() (called from the web server's task, i.e.
// on core 1); consumed by bleClientTask() on its own core/task, same
// reason as everything else that talks to g_bikeClient/its
// characteristics only ever happening there — see bleClientTask()'s own
// comment below.
static volatile bool  g_resistanceWritePending = false;
static volatile float g_resistanceWriteTarget  = 0.0f;

static const size_t MAX_DISCOVERED_DEVICES = 8;
static DiscoveredBleDevice g_discoveredDevices[MAX_DISCOVERED_DEVICES];
static size_t              g_discoveredCount = 0;
// Guards g_discoveredDevices/g_discoveredCount: written from the NimBLE
// host task (ScanCallbacks::onResult) and read from loop()/the web
// server's task.
static SemaphoreHandle_t g_discoveredDevicesMutex = nullptr;

// Records (or updates) one FTMS-advertising device in the "recently
// seen" cache. Called from ScanCallbacks::onResult() for *every* matching
// device, independent of whether we go on to connect to it — so the list
// stays useful even when a pin is set and most devices get filtered out
// of the connect decision below.
static void recordDiscoveredDevice(const char* mac, const char* name, int8_t rssi) {
    if (!g_discoveredDevicesMutex) return;
    if (xSemaphoreTake(g_discoveredDevicesMutex, pdMS_TO_TICKS(50)) != pdTRUE) return;

    int slot = -1;
    for (size_t i = 0; i < g_discoveredCount; i++) {
        if (strcasecmp(g_discoveredDevices[i].mac, mac) == 0) { slot = (int)i; break; }
    }
    if (slot < 0) {
        if (g_discoveredCount < MAX_DISCOVERED_DEVICES) {
            slot = (int)g_discoveredCount++;
        } else {
            // Full — drop the oldest entry (index 0) to make room for the
            // newest one, keeping the list useful even with more FTMS
            // devices around than it has slots for.
            for (size_t i = 1; i < MAX_DISCOVERED_DEVICES; i++) {
                g_discoveredDevices[i - 1] = g_discoveredDevices[i];
            }
            slot = MAX_DISCOVERED_DEVICES - 1;
        }
    }
    copyIntoField(g_discoveredDevices[slot].mac, sizeof(g_discoveredDevices[slot].mac), mac);
    copyIntoField(g_discoveredDevices[slot].name, sizeof(g_discoveredDevices[slot].name),
                  (name && name[0]) ? name : "(unknown)");
    g_discoveredDevices[slot].rssi = rssi;

    xSemaphoreGive(g_discoveredDevicesMutex);
}

// ── FTMS notification callback ────────────────────────────────────────
static void onBikeDataNotify(NimBLERemoteCharacteristic* characteristic,
                              uint8_t* data, size_t length, bool isNotify) {
    IndoorBikeData bikeData;
    if (!parseIndoorBikeData(data, length, bikeData)) {
        LOG("FTMS: parse failed (%u bytes)", (unsigned)length);
        return;
    }

    // Only update fields that are present — the bike splits its data
    // across multiple notifications rather than sending every field
    // every time.
    if (bikeData.hasSpeed) {
        g_hasSpeed = true;
        g_speedRaw = bikeData.instantSpeedRaw;
    }
    if (bikeData.hasCadence) {
        g_hasCadence = true;
        g_cadenceRaw = bikeData.instantCadenceRaw;
    }
    if (bikeData.hasPower) {
        g_hasPower = true;
        g_power = bikeData.instantPower;
    }
    if (bikeData.hasResistance) {
        g_hasResistance = true;
        g_resistanceLevel = bikeData.resistanceLevel;
    }
    g_bikeDataReady = true;

    LOG("FTMS: spd=%.1f km/h  cad=%.0f rpm  pwr=%d W  res=%d",
        bikeData.hasSpeed      ? bikeData.speedKmh()      : 0.0f,
        bikeData.hasCadence    ? bikeData.cadenceRpm()    : 0.0f,
        bikeData.hasPower      ? bikeData.instantPower    : 0,
        bikeData.hasResistance ? bikeData.resistanceLevel : 0);
}

// ── BLE scan callbacks ─────────────────────────────────────────────
// Default (no pin set): connect to the first device found that
// advertises the FTMS service (0x1826).
// With a pin set (g_settings.pinnedBikeMac != ""): every FTMS device is
// still recorded in the "recently seen" cache, but the scan only stops
// (and a connect is only attempted) once the pinned MAC specifically
// shows up; other FTMS devices are noted and ignored.
// During a discovery-only scan (Settings/Bike page, while already
// connected): never stops the scan or touches g_targetAddress/
// g_wantsToConnect at all — purely populates the cache for display.
class ScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* device) override {
        if (!device->isAdvertisingService(FTMS_SERVICE_UUID)) return;

        std::string mac  = device->getAddress().toString();
        std::string name = device->haveName() ? device->getName() : "";
        recordDiscoveredDevice(mac.c_str(), name.c_str(), (int8_t)device->getRSSI());

        if (g_scanPurpose == ScanPurpose::DiscoveryOnly) {
            LOG("Scan(discovery): saw FTMS device %s (%s) RSSI=%d",
                mac.c_str(), name.c_str(), device->getRSSI());
            return;
        }

        char pinnedMac[sizeof(g_settings.pinnedBikeMac)];
        xSemaphoreTake(g_pinMutex, portMAX_DELAY);
        copyIntoField(pinnedMac, sizeof(pinnedMac), g_settings.pinnedBikeMac);
        xSemaphoreGive(g_pinMutex);
        if (pinnedMac[0] != '\0' && strcasecmp(pinnedMac, mac.c_str()) != 0) {
            // A device is pinned and this isn't it — keep scanning.
            return;
        }

        LOG("Scan: found FTMS device %s! RSSI=%d", mac.c_str(), device->getRSSI());
        NimBLEDevice::getScan()->stop();
        g_targetAddress  = device->getAddress();
        g_hasTarget      = true;
        g_wantsToConnect = true;
    }
};

// ── BLE client (bike connection) callbacks ────────────────────────────
class BikeClientCallbacks : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient* client) override {
        LOG("Client: connected to FTMS device");
        g_bikeConnected = true;
    }
    void onDisconnect(NimBLEClient* client) override {
        LOG("Client: disconnected from FTMS device — will reconnect");
        g_bikeConnected = false;
        g_bikeDataReady = false;
        // The Control Point characteristic (and any range it reported)
        // belonged to this connection — drop both rather than risk a
        // later requestSetResistance() writing to a stale pointer, or the
        // dashboard offering a range that came from a different bike.
        g_ftmsControlPoint = nullptr;
        g_resistanceRange  = ResistanceRange();
        g_wantsToConnect   = true;
    }
};

// ── BLE server (Watch connection) callbacks ───────────────────────────
class WatchServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* server) override {
        LOG("Server: Watch connected");
        g_watchConnected = true;
    }
    void onDisconnect(NimBLEServer* server) override {
        LOG("Server: Watch disconnected");
        g_watchConnected = false;
        NimBLEDevice::startAdvertising();
    }
};

// ── Connect to the target FTMS device ─────────────────────────────────
static bool connectToFtms() {
    if (!g_hasTarget) return false;

    LOG("Connecting to %s...", g_targetAddress.toString().c_str());

    if (!g_bikeClient) {
        g_bikeClient = NimBLEDevice::createClient();
        g_bikeClient->setClientCallbacks(new BikeClientCallbacks());
    }

    if (!g_bikeClient->connect(g_targetAddress)) {
        LOG("Connection failed!");
        return false;
    }

    NimBLERemoteService* ftmsService = g_bikeClient->getService(FTMS_SERVICE_UUID);
    if (!ftmsService) {
        LOG("FTMS service not found!");
        g_bikeClient->disconnect();
        return false;
    }

    NimBLERemoteCharacteristic* bikeData = ftmsService->getCharacteristic(FTMS_INDOOR_BIKE_UUID);
    if (!bikeData) {
        LOG("Indoor Bike Data characteristic not found!");
        g_bikeClient->disconnect();
        return false;
    }

    // Try acknowledged subscribe first — some devices require a Write
    // Request (rather than Write Command) for the CCCD.
    if (!bikeData->subscribe(true, onBikeDataNotify, true)) {
        LOG("Acknowledged subscribe failed, trying unacknowledged...");
        if (!bikeData->subscribe(true, onBikeDataNotify, false)) {
            LOG("Failed to subscribe to notifications!");
            g_bikeClient->disconnect();
            return false;
        }
    }
    LOG("Subscribed to FTMS Indoor Bike Data");

    // Write to the FTMS Control Point to start data flow. Kept around in
    // g_ftmsControlPoint afterwards (rather than a local variable) so
    // requestSetResistance() (see ftms_control.h) can reuse it later,
    // once the dashboard is showing live data.
    g_ftmsControlPoint = ftmsService->getCharacteristic(FTMS_CONTROL_POINT_UUID);
    if (g_ftmsControlPoint) {
        uint8_t requestControl[] = {0x00};  // opcode 0x00: Request Control
        if (g_ftmsControlPoint->writeValue(requestControl, 1, true)) {
            LOG("FTMS Control: requested control");
            uint8_t startOrResume[] = {0x07};  // opcode 0x07: Start or Resume
            if (g_ftmsControlPoint->writeValue(startOrResume, 1, true)) {
                LOG("FTMS Control: sent Start command");
            } else {
                LOG("FTMS Control: Start command failed");
            }
        } else {
            LOG("FTMS Control: request control failed");
        }
    } else {
        LOG("FTMS Control Point not found (OK, may not be required)");
    }

    // Optional "Supported Resistance Level Range" characteristic — lets
    // the dashboard's resistance picker offer the bike's real min/max/step
    // instead of a generic guess. Many bikes don't expose it, in which
    // case g_resistanceRange simply stays at whatever
    // BikeClientCallbacks::onDisconnect() reset it to ("unavailable") and
    // the dashboard falls back to a generic range (see loop() below).
    NimBLERemoteCharacteristic* rangeChar = ftmsService->getCharacteristic(FTMS_RESISTANCE_RANGE_UUID);
    if (rangeChar && rangeChar->canRead()) {
        std::string rangeBytes = rangeChar->readValue();
        if (rangeBytes.length() >= 6) {
            const uint8_t* data = reinterpret_cast<const uint8_t*>(rangeBytes.data());
            auto readS16 = [](const uint8_t* p) -> int16_t {
                return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
            };
            // Bluetooth FTMS spec: min/max/increment are each a signed
            // 16-bit little-endian value with resolution 0.1 (same
            // resolution the Control Point's "Set Target Resistance
            // Level" command uses below) — so divide by 10, not 1, unlike
            // the plain resistanceLevel field in Indoor Bike Data.
            g_resistanceRange.available = true;
            g_resistanceRange.min  = readS16(data)     / 10.0f;
            g_resistanceRange.max  = readS16(data + 2) / 10.0f;
            g_resistanceRange.step = readS16(data + 4) / 10.0f;
            LOG("FTMS: resistance range %.1f..%.1f, step %.1f",
                g_resistanceRange.min, g_resistanceRange.max, g_resistanceRange.step);
        } else {
            LOG("FTMS: resistance range characteristic returned %d bytes (expected 6)",
                (int)rangeBytes.length());
        }
    } else {
        LOG("FTMS Resistance Range characteristic not found (OK, dashboard will use a generic range)");
    }

    return true;
}

// Marks the scan as finished so bleScanInProgress() (and with it, the
// Settings page's spinner/disabled button) reflects reality even for a
// discovery-only scan that runs its full 10s without ever calling stop().
static void onScanComplete(NimBLEScanResults results) {
    g_scanning = false;

    // Only pause here if this scan ended *without* finding a device to
    // connect to (g_wantsToConnect is only set true by
    // ScanCallbacks::onResult, above) — otherwise this would delay the
    // very connect attempt the scan was just for. The pause used to be
    // set at the moment a scan *started* rather than when one actually
    // finished — since a scan runs up to 10s and the pause was only 5s,
    // it had already elapsed before the scan even completed, so the next
    // one began immediately with no real gap. With no bike around, that
    // meant scanning back-to-back continuously instead of in the intended
    // 10s-scan/5s-pause pattern (see startScan()'s comment on scan duty
    // cycle) — starving WiFi (the web dashboard/settings pages) of any
    // guaranteed clear airtime for as long as no bike was found.
    if (!g_wantsToConnect) {
        g_bleRetryAtMs = millis() + BLE_RETRY_INTERVAL_MS;
    }
}

// ── Scan for any FTMS-capable device ──────────────────────────────────
// Caller is expected to have set g_scanPurpose beforehand.
static void startScan() {
    LOG("Scanning for FTMS devices (%s)...",
        g_scanPurpose == ScanPurpose::DiscoveryOnly ? "discovery" : "auto-connect");
    static ScanCallbacks scanCallbacks;  // reused — avoids leaking one per scan cycle
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setAdvertisedDeviceCallbacks(&scanCallbacks, false);
    scan->setActiveScan(true);
    // Window close to interval (originally 100/99, ~99% BLE radio duty
    // cycle) starves WiFi of airtime on the ESP32's single shared 2.4GHz
    // radio — the web server/dashboard would slow to a crawl or become
    // unreachable for the whole time no bike is connected, since that's
    // exactly when this runs on a loop (10s scan + 5s pause, repeatedly,
    // in bleClientTask()). 100/15 (~15% duty cycle) leaves WiFi most of
    // the airtime while still scanning aggressively enough to find a
    // device within the 10s window in virtually all cases — FTMS devices
    // advertise repeatedly, so a smaller window just means catching one a
    // little later within the same scan, not missing it.
    scan->setInterval(100);
    scan->setWindow(15);
    g_scanning = true;
    scan->start(10, onScanComplete, false);
}

// ── BLE client task ────────────────────────────────────────────────
// Scanning, connecting, service discovery, and CCCD/Control-Point writes
// are all synchronous NimBLE calls that can each block for hundreds of
// milliseconds up to several seconds — worse, and more often, while a
// bike is flaky or out of range and every reconnect attempt fails. That
// used to happen inside loop(), which also drives the web server; the
// bridge's HTTP dashboard would freeze for the duration of every attempt.
// Running this on its own FreeRTOS task (pinned away from loop()'s core)
// means the BLE client can block as long as it needs to without ever
// stalling the web server, MQTT, or OTA handling in loop().
static void bleClientTask(void* pvParameters) {
    for (;;) {
        // A target resistance was requested from the dashboard
        // (requestSetResistance(), see ftms_control.h) — perform the
        // actual (blocking) GATT write here, never on the web server's
        // task. Drop it silently if the bike disconnected (or never had a
        // Control Point) in the meantime, rather than firing a stale
        // request once some later bike connects.
        if (g_resistanceWritePending) {
            g_resistanceWritePending = false;
            if (g_bikeConnected && g_ftmsControlPoint) {
                float level = g_resistanceWriteTarget;
                // FTMS Control Point "Set Target Resistance Level" (opcode
                // 0x04): sint16, little-endian, resolution 0.1 — i.e. the
                // wire value is the resistance level (same unitless scale
                // as resistanceLevel/g_resistanceRange elsewhere) times 10.
                long wireValue = (long)(level * 10.0f + (level >= 0.0f ? 0.5f : -0.5f));
                if (wireValue < INT16_MIN) wireValue = INT16_MIN;
                if (wireValue > INT16_MAX) wireValue = INT16_MAX;
                uint8_t command[3] = {
                    0x04,
                    (uint8_t)(wireValue & 0xFF),
                    (uint8_t)((wireValue >> 8) & 0xFF),
                };
                bool ok = g_ftmsControlPoint->writeValue(command, sizeof(command), true);
                LOG("FTMS Control: set resistance %.1f (raw=%ld) -> %s",
                    level, wireValue, ok ? "sent" : "failed");
            }
        }

        // Discovery-only scan requested from the Settings/Bike page while
        // a bike is connected — the user already confirmed the warning
        // dialog. Runs independently of the g_bikeConnected branch below
        // and never touches g_wantsToConnect/g_targetAddress (see
        // ScanCallbacks::onResult), so it can't interfere with an
        // existing connection or trigger a reconnect to a different
        // device.
        if (g_discoveryScanRequested && !NimBLEDevice::getScan()->isScanning()) {
            g_discoveryScanRequested = false;
            g_scanPurpose = ScanPurpose::DiscoveryOnly;
            startScan();
        }

        if (!g_bikeConnected) {
            unsigned long now = millis();

            if (g_manualRescanRequested) {
                // "Scan again now" while not connected — just skips the
                // rest of the current cooldown so the button feels
                // responsive; everything else about the reconnect flow
                // below is unchanged.
                g_manualRescanRequested = false;
                g_bleRetryAtMs = 0;
            }

            if (g_wantsToConnect && g_hasTarget) {
                if (now >= g_bleRetryAtMs) {
                    g_wantsToConnect = false;
                    if (!connectToFtms()) {
                        LOG("Connect failed, will retry in 5s");
                        g_bleRetryAtMs   = now + BLE_RETRY_INTERVAL_MS;
                        g_wantsToConnect = true;
                    }
                }
            } else if (!NimBLEDevice::getScan()->isScanning() && now >= g_bleRetryAtMs) {
                // The pause before the *next* scan is set in
                // onScanComplete() once this one actually finishes, not
                // here — see its comment for why that distinction matters.
                g_scanPurpose = ScanPurpose::AutoConnect;
                startScan();
            }
        } else {
            // Connected — clear a stale manual-rescan request rather than
            // let it fire the instant the bike disconnects; a fresh click
            // is needed once actually disconnected.
            g_manualRescanRequested = false;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ── BLE server setup ───────────────────────────────────────────────
static void setupBleServer() {
    g_bleServer = NimBLEDevice::createServer();
    g_bleServer->setCallbacks(new WatchServerCallbacks());

    // ── CSC service ────────────────────────────────────────────────
    NimBLEService* cscService = g_bleServer->createService(CSC_SERVICE_UUID);

    g_cscMeasurement = cscService->createCharacteristic(
        CSC_MEASUREMENT_UUID, NIMBLE_PROPERTY::NOTIFY);

    NimBLECharacteristic* cscFeatureChar = cscService->createCharacteristic(
        CSC_FEATURE_UUID, NIMBLE_PROPERTY::READ);
    uint16_t cscFeatures = 0x0003;  // wheel + crank revolution data supported
    cscFeatureChar->setValue(cscFeatures);

    uint8_t sensorLocation = 0x00;  // "Other" — no real sensor location to report

    NimBLECharacteristic* cscSensorLocationChar = cscService->createCharacteristic(
        SENSOR_LOCATION_UUID, NIMBLE_PROPERTY::READ);
    cscSensorLocationChar->setValue(&sensorLocation, 1);

    cscService->start();

    // ── Cycling Power service ─────────────────────────────────────
    NimBLEService* cpService = g_bleServer->createService(CP_SERVICE_UUID);

    g_cpMeasurement = cpService->createCharacteristic(
        CP_MEASUREMENT_UUID, NIMBLE_PROPERTY::NOTIFY);

    NimBLECharacteristic* cpFeatureChar = cpService->createCharacteristic(
        CP_FEATURE_UUID, NIMBLE_PROPERTY::READ);
    uint32_t cpFeatures = 0x00000000;  // no optional Cycling Power features supported
    cpFeatureChar->setValue(cpFeatures);

    NimBLECharacteristic* cpSensorLocationChar = cpService->createCharacteristic(
        SENSOR_LOCATION_UUID, NIMBLE_PROPERTY::READ);
    cpSensorLocationChar->setValue(&sensorLocation, 1);

    cpService->start();

    // ── Advertising ────────────────────────────────────────────────
    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    advertising->addServiceUUID(CSC_SERVICE_UUID);
    advertising->addServiceUUID(CP_SERVICE_UUID);
    advertising->setScanResponse(true);
    advertising->start();
    LOG("Server: advertising CSC + CP services");
}

// ── Status LED ─────────────────────────────────────────────────────
// Lite Mode overlay: two quick blinks at the start of every 3s cycle,
// independent of the bike/watch connection status below — makes Lite Mode
// visible at a glance without permanently hiding the normal connection
// pattern, which still shows for the rest of each cycle.
static const unsigned long LITE_MODE_LED_CYCLE_MS      = 3000;
static const unsigned long LITE_MODE_LED_OVERLAY_MS    = 400;
static const unsigned long LITE_MODE_LED_BLINK_STEP_MS = 100;

// A plain digitalWrite() only lights a classic ESP32 board's GPIO2 LED.
// Boards whose status LED is an addressable WS2812 (e.g. the
// esp32-s3-devkitc-1 environment's GPIO48) need the single-wire protocol
// driven by neopixelWrite() instead -- see LED_IS_RGB in config.h.
static void setLed(bool on) {
#if LED_IS_RGB
    uint8_t v = on ? 24 : 0; // dim -- full brightness white is eye-wateringly bright up close
    neopixelWrite(LED_PIN, v, v, v);
#else
    digitalWrite(LED_PIN, on ? HIGH : LOW);
#endif
}

static void updateLed() {
    unsigned long now = millis();
    if (g_settings.liteModeEnabled) {
        unsigned long cyclePos = now % LITE_MODE_LED_CYCLE_MS;
        if (cyclePos < LITE_MODE_LED_OVERLAY_MS) {
            setLed((cyclePos / LITE_MODE_LED_BLINK_STEP_MS) % 2);
            return;
        }
    }
    if (g_bikeConnected && g_watchConnected) {
        setLed(true);
    } else if (g_bikeConnected) {
        setLed((now / 125) % 2);
    } else {
        setLed((now / 500) % 2);
    }
}

// ── BOOT button: factory reset (long hold) + MQTT event (short press) ─
// Holding RESET_BUTTON_PIN down for RESET_BUTTON_HOLD_MS wipes all
// settings and reboots — same effect as the Danger Zone button on the
// Settings page, but reachable without the network: the point is
// recovering a bridge whose WiFi credentials are wrong, whose admin
// password was forgotten, or that's otherwise unreachable over HTTP. This
// always fires on reaching the threshold while still held, Lite Mode or
// not, and clears liteModeEnabled along with everything else.
//
// A normal short press (released well before that threshold) instead
// publishes a momentary MQTT event (see notifyBootButtonPressed() in
// mqtt_client.h) so the same physical button can drive a Home Assistant
// automation — pausing a workout, toggling a light, whatever — without
// needing a second button wired up. Meaningless while Lite Mode is active
// (MQTT isn't reachable — see setup()), so a short press is a no-op there
// instead; see the LITE_MODE_EXIT_HOLD_MS branch below for what a longer
// hold does in that state instead.
//
// g_resetButtonHeldSinceMs is 0 whenever the button is up; set once on
// the falling edge (press) and checked every loop() tick against
// RESET_BUTTON_HOLD_MS. BOOT_BUTTON_DEBOUNCE_MS filters out mechanical
// bounce right at the press edge being misread as a short press on its
// own; a bounce that momentarily reads "not pressed" mid-hold just
// restarts the hold timer, a minor inconvenience (hold it a little more
// firmly) rather than a false trigger either way.
static unsigned long g_resetButtonHeldSinceMs = 0;
static const unsigned long BOOT_BUTTON_DEBOUNCE_MS = 30;

static void checkResetButton() {
    bool pressed = digitalRead(RESET_BUTTON_PIN) == LOW;
    if (!pressed) {
        if (g_resetButtonHeldSinceMs != 0) {
            unsigned long heldMs = millis() - g_resetButtonHeldSinceMs;
            if (g_settings.liteModeEnabled) {
                // Exiting Lite Mode is evaluated on release, not the
                // instant the hold reaches LITE_MODE_EXIT_HOLD_MS — firing
                // (and rebooting) immediately at that mark would cut the
                // hold short and make it impossible to keep holding to
                // RESET_BUTTON_HOLD_MS for a factory reset instead. A
                // release before LITE_MODE_EXIT_HOLD_MS does nothing (see
                // the comment above).
                if (heldMs >= LITE_MODE_EXIT_HOLD_MS && heldMs < RESET_BUTTON_HOLD_MS) {
                    LOG("Reset button: held %lums -> exiting Lite Mode", heldMs);
                    g_settings = updateSettings([](Bike2WatchSettings& s) { s.liteModeEnabled = false; });
                    setLed(true);
                    delay(300);
                    ESP.restart();
                }
            } else if (heldMs >= BOOT_BUTTON_DEBOUNCE_MS && heldMs < RESET_BUTTON_HOLD_MS) {
                LOG("Reset button: short press (%lums) -> MQTT boot_button event", heldMs);
                notifyBootButtonPressed();
            }
        }
        g_resetButtonHeldSinceMs = 0;
        return;
    }
    if (g_resetButtonHeldSinceMs == 0) {
        g_resetButtonHeldSinceMs = millis();
        LOG("Reset button: held down — keep holding for %lus to factory reset",
            (unsigned long)(RESET_BUTTON_HOLD_MS / 1000));
        return;
    }

    unsigned long heldMs = millis() - g_resetButtonHeldSinceMs;
    if (heldMs >= RESET_BUTTON_HOLD_MS) {
        LOG("Reset button: held %lums, factory resetting...", heldMs);
        factoryResetSettings();
        setLed(true);
        delay(300);
        ESP.restart();
    } else {
        // Once the hold has gone on for a bit (not just a stray brush of
        // the button), a fast blink overrides updateLed()'s normal
        // pattern for the rest of this loop() tick — visual confirmation
        // something is being armed. Must run after updateLed() in loop()
        // to win for this frame. Starts at LITE_MODE_EXIT_HOLD_MS while
        // Lite Mode is active (nothing is armed before that point in this
        // state — see above), at a flat 1s otherwise.
        unsigned long blinkStartMs = g_settings.liteModeEnabled ? LITE_MODE_EXIT_HOLD_MS : 1000;
        if (heldMs >= blinkStartMs) {
            setLed((heldMs / 100) % 2);
        }
    }
}

// ── Arduino entry points ───────────────────────────────────────────
void setup() {
    Serial.begin(115200);
#if !LED_IS_RGB
    pinMode(LED_PIN, OUTPUT);
#endif
    pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);
    LOG("%s starting...", DEVICE_NAME);

    g_discoveredDevicesMutex = xSemaphoreCreateMutex();
    g_pinMutex = xSemaphoreCreateMutex();

    NimBLEDevice::init(DEVICE_NAME);
    setupBleServer();

    g_settings = loadSettings();
    // Wheel circumference lives in settings (see include/settings.h)
    // rather than being fixed at compile time — WHEEL_CIRCUMFERENCE_MM
    // from config.h only seeds it on the very first boot (see
    // settings_store.cpp), same as WiFi/MQTT.
    g_csc.wheelCircumferenceM = g_settings.wheelCircumferenceMm / 1000.0f;
    if (g_settings.pinnedBikeMac[0] != '\0') {
        LOG("BLE: pinned to %s (%s)", g_settings.pinnedBikeMac, g_settings.pinnedBikeName);
    }

    // Scanning/connecting runs on its own task (see bleClientTask) so it
    // can never block loop() — and with it the web server. Pinned to core
    // 0, away from loop()'s core 1, for genuine concurrency. Runs
    // regardless of Lite Mode — the BLE bridge (bike <-> watch) is the one
    // thing Lite Mode never touches.
    xTaskCreatePinnedToCore(bleClientTask, "bleClient", 4096, nullptr, 1, nullptr, 0);

    if (g_settings.liteModeEnabled) {
        // See Bike2WatchSettings::liteModeEnabled in settings.h: WiFi/hotspot,
        // the web dashboard, MQTT and OTA all depend on a WiFi connection
        // that Lite Mode deliberately never brings up, so none of the
        // setup*() calls below have anything to attach to — skipped
        // outright rather than started just to sit idle. Exiting Lite Mode
        // again (BOOT button, see checkResetButton()) reboots back into
        // this same setup() with liteModeEnabled now false.
        LOG("Lite Mode: active -- WiFi/hotspot, web dashboard, MQTT and OTA are all disabled. "
            "Hold the BOOT button for %lus to exit.",
            (unsigned long)(LITE_MODE_EXIT_HOLD_MS / 1000));
    } else {
        setupWebServer(g_settings);
        setupOta(g_settings);
        setupMqtt(g_settings);
    }
}

void loop() {
    // Everything in this block depends on WiFi (web dashboard, MQTT) —
    // none of it was ever started while Lite Mode is active (see setup()),
    // so skip it here too rather than call into a web server/MQTT client
    // that never began. The BLE bridge below (CSC/CP notifications, the
    // status LED, the BOOT button) is completely unaffected either way.
    if (!g_settings.liteModeEnabled) {
        // Keep the web dashboard's snapshot fresh and serve any pending
        // HTTP requests. Cheap — just a struct copy plus a non-blocking poll.
        {
            Bike2WatchStatus status;
            status.bikeConnected   = g_bikeConnected;
            status.watchConnected  = g_watchConnected;
            status.hasSpeed        = g_hasSpeed;
            status.hasCadence      = g_hasCadence;
            status.hasPower        = g_hasPower;
            status.hasResistance   = g_hasResistance;
            status.speedKmh        = g_hasSpeed   ? (g_speedRaw / 100.0f) : 0.0f;
            status.cadenceRpm      = g_hasCadence ? (g_cadenceRaw / 2.0f) : 0.0f;
            status.power           = g_power;
            status.resistanceLevel = g_resistanceLevel;
            status.canSetResistance = ftmsControlPointAvailable();
            {
                ResistanceRange range = getResistanceRange();
                // Generic fallback (0..100, step 1) when the bike didn't
                // report a real range — better than not offering the picker
                // at all, since plenty of trainers use roughly that scale.
                status.resistanceMin  = range.available ? range.min  : 0.0f;
                status.resistanceMax  = range.available ? range.max  : 100.0f;
                status.resistanceStep = range.available ? (range.step > 0.0f ? range.step : 1.0f) : 1.0f;
            }
            status.wheelRevs = g_csc.cumulativeWheelRevs;
            status.crankRevs = g_csc.cumulativeCrankRevs;
            status.uptimeSec = millis() / 1000;
            status.mqttEnabled = mqttIsEnabled();
            status.mqttConnected = mqttIsConnected();
            status.pinnedDeviceMissing = pinnedDeviceMissing();
            status.pinnedDeviceName    = g_settings.pinnedBikeName;
            updateWebStatus(status);
            handleMqtt(status);
        }
        handleWebServerClients();
        handleOta();
    }

    // 1 Hz CSC/CP notifications
    unsigned long now = millis();
    if (now - g_lastNotifyMs >= NOTIFY_INTERVAL_MS) {
        uint32_t deltaMs = now - g_lastNotifyMs;
        g_lastNotifyMs = now;

        if (g_bikeDataReady) {
            float speedKmh   = g_hasSpeed   ? (g_speedRaw / 100.0f) : 0.0f;
            float cadenceRpm = g_hasCadence ? (g_cadenceRaw / 2.0f) : 0.0f;

            updateCsc(g_csc, speedKmh, cadenceRpm, deltaMs);

            if (g_watchConnected) {
                if (g_cscMeasurement) {
                    uint8_t cscBuf[16];
                    size_t cscLen = buildCscMeasurement(cscBuf, g_csc, g_hasSpeed, g_hasCadence);
                    g_cscMeasurement->setValue(cscBuf, cscLen);
                    g_cscMeasurement->notify();
                    LOG("CSC: wheelRevs=%lu crankRevs=%u",
                        (unsigned long)g_csc.cumulativeWheelRevs,
                        (unsigned)g_csc.cumulativeCrankRevs);
                }

                if (g_hasPower && g_cpMeasurement) {
                    uint8_t cpBuf[8];
                    size_t cpLen = buildCpMeasurement(cpBuf, g_power);
                    g_cpMeasurement->setValue(cpBuf, cpLen);
                    g_cpMeasurement->notify();
                    LOG("CP: power=%d W", g_power);
                }
            }
        }
    }

    updateLed();
    checkResetButton();
    delay(10);
}

// ── ble_devices.h implementation ────────────────────────────────────
// See include/ble_devices.h for the contract each function promises.

size_t getDiscoveredDevices(DiscoveredBleDevice* out, size_t maxCount) {
    size_t count = 0;
    if (!g_discoveredDevicesMutex) return 0;
    if (xSemaphoreTake(g_discoveredDevicesMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        count = g_discoveredCount < maxCount ? g_discoveredCount : maxCount;
        for (size_t i = 0; i < count; i++) {
            // Most recently seen first.
            out[i] = g_discoveredDevices[g_discoveredCount - 1 - i];
        }
        xSemaphoreGive(g_discoveredDevicesMutex);
    }
    return count;
}

bool bleScanInProgress() {
    return g_scanning;
}

bool bleBikeConnected() {
    return g_bikeConnected;
}

void requestImmediateRescan() {
    g_manualRescanRequested = true;
}

void requestDiscoveryScanWhileConnected() {
    g_discoveryScanRequested = true;
}

// Pin changes go through updateSettings() (read-modify-write against flash)
// so they don't clobber what the web UI saved since boot, and vice versa.
// Only the pin fields of the cached g_settings are refreshed, under
// g_pinMutex, since the BLE scan callback reads them from another task.
static void applyPin(const char* mac, const char* name) {
    updateSettings([&](Bike2WatchSettings& s) {
        copyIntoField(s.pinnedBikeMac, sizeof(s.pinnedBikeMac), mac);
        copyIntoField(s.pinnedBikeName, sizeof(s.pinnedBikeName), name);
    });
    xSemaphoreTake(g_pinMutex, portMAX_DELAY);
    copyIntoField(g_settings.pinnedBikeMac, sizeof(g_settings.pinnedBikeMac), mac);
    copyIntoField(g_settings.pinnedBikeName, sizeof(g_settings.pinnedBikeName), name);
    xSemaphoreGive(g_pinMutex);
}

void setPinnedDevice(const char* mac, const char* name) {
    applyPin(mac ? mac : "", name ? name : "");
    LOG("BLE: pinned to %s (%s)", g_settings.pinnedBikeMac, g_settings.pinnedBikeName);
    // Deliberately doesn't touch g_targetAddress/g_wantsToConnect/
    // disconnect anything: an existing connection (to a different device,
    // if any) keeps running. The pin is only consulted by
    // ScanCallbacks::onResult on the *next* scan, i.e. after the current
    // connection eventually drops on its own.
}

void clearPinnedDevice() {
    applyPin("", "");
    LOG("BLE: pin cleared, back to auto-connect");
}

const char* getPinnedDeviceMac() {
    return g_settings.pinnedBikeMac;
}

const char* getPinnedDeviceName() {
    return g_settings.pinnedBikeName;
}

bool pinnedDeviceMissing() {
    // Simple and conservative on purpose: "pinned but not currently
    // connected". The bridge already retries every 5s (see
    // bleClientTask), so this simply mirrors that state rather than
    // adding a separate not-found timeout to track. Always false when
    // nothing is pinned, so it can never show up for a plug & play setup.
    return g_settings.pinnedBikeMac[0] != '\0' && !g_bikeConnected;
}

// ── ftms_control.h implementation ───────────────────────────────────
// See include/ftms_control.h for the contract each function promises.

bool ftmsControlPointAvailable() {
    return g_bikeConnected && g_ftmsControlPoint != nullptr;
}

ResistanceRange getResistanceRange() {
    return g_resistanceRange;
}

bool requestSetResistance(float level) {
    if (!g_bikeConnected || !g_ftmsControlPoint) {
        LOG("FTMS Control: set-resistance request dropped, no bike connected");
        return false;
    }
    g_resistanceWriteTarget  = level;
    g_resistanceWritePending = true;
    return true;
}
