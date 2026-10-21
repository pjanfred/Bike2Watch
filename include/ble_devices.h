#ifndef BLE_DEVICES_H
#define BLE_DEVICES_H

#include <cstdint>
#include <cstddef>

// Everything the web server needs to build the optional "Bike" (device
// selection) page. Implemented in main.cpp, which already owns all BLE
// client/server state — this header exposes only the narrow slice of it
// web_server.cpp actually needs, rather than letting it reach into
// main.cpp's internals directly.
//
// The default, unconfigured behaviour — connect to the first FTMS device
// a scan finds — is unaffected by any of this: everything here is opt-in,
// and none of it changes what happens if nobody ever opens the page.

// One entry in the "recently seen FTMS devices" cache. Populated
// passively whenever a scan already happens for another reason (the
// regular auto-reconnect scan while no bike is connected), or explicitly
// via requestDiscoveryScanWhileConnected().
struct DiscoveredBleDevice {
    char   mac[18];   // "aa:bb:cc:dd:ee:ff"
    char   name[32];  // "(unknown)" if the device advertised no name
    int8_t rssi;
};

// Copies up to `maxCount` cached devices into `out`, most recently seen
// first. Returns the number actually written. Safe to call from any task
// at any time (internally synchronized).
size_t getDiscoveredDevices(DiscoveredBleDevice* out, size_t maxCount);

// True while a scan (auto-connect or discovery) is currently running —
// lets the web UI show a spinner and disable the "scan again" button.
bool bleScanInProgress();

// True if the bridge is currently connected to the FTMS bike.
bool bleBikeConnected();

// Skips the remaining reconnect cooldown and scans right away. Only has
// an observable effect while no bike is connected (the regular reconnect
// loop already scans periodically in that state) — harmless no-op
// otherwise. Used by the Bike page's "Scan again now" button when no
// confirmation dialog is needed.
void requestImmediateRescan();

// Requests a discovery-only scan even though a bike is currently
// connected — used by "Scan again now" once the user has confirmed the
// warning dialog (scanning alongside an active connection can cause brief
// notification gaps). Only ever updates the discovered-devices cache
// above; never touches the existing connection and never attempts to
// connect to anything.
void requestDiscoveryScanWhileConnected();

// Pins the bridge to one specific bike by MAC address (format
// "aa:bb:cc:dd:ee:ff", as found in DiscoveredBleDevice::mac). Persists to
// flash immediately and takes effect on the *next* (re)connect attempt —
// an existing connection to a different device, if any, keeps running
// until it disconnects on its own.
void setPinnedDevice(const char* mac, const char* name);

// Clears the pinned device. The bridge goes back to connecting to the
// first FTMS device a scan finds — the default, plug & play behaviour.
void clearPinnedDevice();

// "" if no device is currently pinned.
const char* getPinnedDeviceMac();
const char* getPinnedDeviceName();

// True if a device is pinned but the bridge isn't currently connected to
// it — lets the dashboard show "Selected device not found" instead of a
// plain, unexplained "not connected". Always false when nothing is
// pinned, so it never fires for the default plug & play setup.
bool pinnedDeviceMissing();

#endif // BLE_DEVICES_H
