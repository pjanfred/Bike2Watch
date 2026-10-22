# Bike2Watch

Firmware for an ESP32 that bridges an indoor bike's Bluetooth **FTMS**
(Fitness Machine Service) data to devices that only understand the older
**CSC** (Cycling Speed & Cadence) and **Cycling Power** BLE profiles — for
example sports watches (Garmin, etc.) that can pair with a "bike sensor"
but have no FTMS support.

The bridge acts as a BLE *client* to your bike and a BLE *server* to your
watch at the same time, translates the data in between, and adds a small
WiFi web dashboard, optional MQTT / Home Assistant integration, and
over-the-air firmware updates on top.

```
 Indoor Bike  --FTMS (BLE)-->  ESP32 (Bike2Watch)  --CSC + Power (BLE)-->  Watch
                                       |
                                       +--> Web dashboard (WiFi)
                                       +--> MQTT / Home Assistant (optional)
```

If this project is useful to you, consider supporting its development:

[![Buy Me A Coffee](https://cdn.buymeacoffee.com/buttons/v2/default-yellow.png)](https://buymeacoffee.com/pjanfred)

## Features

- **BLE protocol translation** — connects to any FTMS-capable indoor bike
  (no fixed MAC address, just scans for the FTMS service) and re-exposes
  speed, cadence and power as standard CSC + Cycling Power characteristics
  that watches and head units already know how to pair with.
- **Optional device selection** — the default stays plug & play (connects
  to the first FTMS device found); the Bike page optionally lets you pin
  the bridge to one specific bike by name/MAC if several are ever in
  range. Opening the page never starts a scan on its own — it only shows
  devices already seen — and rescanning while a bike is connected
  requires explicit confirmation, since that can briefly interrupt it.
- **Web dashboard** — a single-page live view (speed, cadence, power,
  connection status) served over WiFi, polling once a second. If the
  connected bike supports it, tapping the Resistance card lets you send
  it a new target resistance level directly (FTMS Control Point) — see
  "Setting a target resistance" below. Respects your device's light/dark
  mode automatically.
- **WiFi with AP fallback + captive portal** — connects to your configured
  network; if that fails (or none is configured yet) the bridge opens its
  own hotspot so you can always reach the Settings page. Joining that
  hotspot pops up a "Sign in to network" page automatically on most
  phones/laptops, taking you straight to `/settings`. The Settings page
  can scan for nearby networks and fill in the SSID for you, and a "Test
  Wi-Fi" button checks new credentials before you commit to them.
- **MQTT + Home Assistant auto-discovery** — optionally publishes status
  and metrics to an MQTT broker, with Home Assistant MQTT Discovery so
  entities show up automatically, no manual YAML required. Also
  subscribes to a command topic to set the target resistance over MQTT —
  see "Setting a target resistance" below.
- **OTA updates** — flash new firmware over the network: the Firmware
  Update page (`/update`) can check GitHub directly for the latest release
  and install it with two clicks (no manual download step), drag-and-drop
  a `.bin` yourself, or use `pio run -t upload --upload-port
  <hostname>.local`.
- **Persistent settings** — WiFi, MQTT, wheel size and the admin password
  are stored in flash (NVS) and edited from the web UI; `config.h` only
  seeds the very first boot.
- **Optional password protection** for the entire web interface —
  dashboard included — plus network (OTA) flashing, editable (and
  removable) from the Settings page.
- **Two ways to factory reset** — a "Danger Zone" button on the Settings
  page, or holding the board's physical BOOT button for 15 seconds. The
  latter works even if the bridge is completely unreachable over the
  network.
- **Lite Mode** — an "Activate Lite Mode" button on the Settings page
  turns WiFi off entirely (station connection and fallback hotspot both),
  leaving a pure BLE bridge with no dashboard, MQTT or network OTA. The
  bike-to-watch bridging itself is unaffected. Exit again by holding the
  BOOT button for 5–15 seconds — see "Lite Mode" below.

## Hardware

- An ESP32 dev board (tested against the `esp32dev` PlatformIO board
  definition). The factory-reset button (see below) uses the board's
  built-in BOOT button — no extra components needed for a working setup.
  An `esp32-s3-devkitc-1` environment is also provided for ESP32-S3 dev
  boards — the BLE/WiFi/OTA code is identical, only the board ID differs.
  Its onboard status LED is an addressable WS2812 on GPIO48 rather than a
  plain LED on GPIO2, so that environment overrides `LED_PIN`/`LED_IS_RGB`
  in `platformio.ini` to drive it correctly; a board wired differently can
  override either the same way — see `include/config.h`.
- An indoor bike (or trainer) that advertises the standard Bluetooth FTMS
  service (`0x1826`) with Indoor Bike Data (`0x2AD2`).
- A watch or head unit that can pair with a CSC and/or Cycling Power
  sensor over BLE.

## Getting started

This is a [PlatformIO](https://platformio.org/) project.

```bash
# Build
pio run -e esp32dev

# Flash over USB
pio run -e esp32dev -t upload

# Run the native unit tests (no hardware required)
pio test -e native
```

On first boot the bridge has no WiFi configured, so it starts its own
hotspot (default SSID `Bike2Watch`, see `WIFI_AP_SSID` in
`include/config.h`). Connect to it — on most phones/laptops a "Sign in to
network" page pops up on its own and takes you straight to the Settings
page; if it doesn't, open `http://192.168.4.1/settings` manually. Set your
WiFi network and, optionally, an MQTT broker. The bridge reboots and joins
your network; from then on the dashboard is reachable at
`http://bike2watch.local` (mDNS) or via its assigned IP.

The auto-popup relies on the OS's own captive-portal detection (a wildcard
DNS entry that resolves every hostname to the bridge while the hotspot is
up, combined with a redirect to `/settings` for any request it doesn't
otherwise recognize), so it works without any app — but it's inherently a
bit OS- and cache-dependent (a device that already "trusts" a network with
this SSID may not re-show the prompt). Opening `192.168.4.1/settings` by
hand always works as a fallback.

### Configuration

Compile-time defaults live in `include/config.h` (bridge name, wheel
size, AP hotspot credentials, MQTT topic prefix, admin password, ...).
These only seed the *very first* boot — after that, WiFi, MQTT, wheel
size and the admin password are stored in flash and edited from the
`/settings` page, so reflashing the device never wipes your live
configuration.

The wheel circumference (in mm) only affects the simulated CSC
wheel-revolution count sent to your watch — it doesn't need to match any
real wheel (the indoor bike has none). The default (2096mm) works with
most head units out of the box; only change it if the speed shown on your
watch doesn't match the bike, to match whatever "wheel size" your watch's
sensor profile is configured with.

On the Settings page, focusing the SSID field triggers a WiFi scan and
lists nearby networks by signal strength (starts automatically if the
bridge is currently running its own hotspot); clicking a result fills in
the SSID for you — you still type the password yourself, it's never read
back. Networks with no name (hidden SSIDs) aren't listed since there's
nothing to click; enter those manually. The scan runs in the background,
same as the FTMS device scan on the Bike page, so it never blocks the
dashboard.

### Password-protecting the bridge

If you expose the bridge on a shared WiFi network, set an admin password
— either `ADMIN_PASSWORD` in `config.h` before the first boot, or the
"Admin password" field on the Settings page at any time — to protect the
entire web interface (dashboard included) and network (OTA) flashing.
Username is always `admin`. To remove password protection again, tick
"Remove password" next to the field and save.

If you forget the password, see "Resetting to factory defaults" below —
both ways to reset also clear it.

### Resetting to factory defaults

Two ways to wipe WiFi, MQTT, the pinned bike, wheel size and the admin
password back to `config.h`'s compile-time defaults:

- **From the Settings page** — the "Danger Zone" section at the bottom has
  a "Reset to factory settings" button.
- **Physically, on the board itself** — hold the **BOOT** button (GPIO0 on
  every ESP32 dev board, no extra wiring needed) for 15 seconds. The status
  LED switches to a fast blink once you've held it for about a second, so
  you know it's registering the hold, then goes solid and the bridge
  reboots once the reset actually happens. This is the way back in if the
  bridge is unreachable over the network at all — wrong WiFi credentials,
  a forgotten admin password, or anything else that keeps you from getting
  to the Settings page. See `RESET_BUTTON_PIN`/`RESET_BUTTON_HOLD_MS` in
  `include/config.h` if your board wires BOOT to a different pin, or you
  want a different hold duration.

A normal, short press of the same BOOT button (released well before the
15s threshold) doesn't reset anything — instead it publishes a momentary
MQTT event, so the button doubles as a Home Assistant automation trigger
(e.g. "Boot Button pressed" → pause a workout, toggle a light, ...) with
no extra hardware. Requires MQTT to be enabled; shows up automatically via
Home Assistant MQTT Discovery as an **event** entity (HA 2023.8+) once the
bridge has connected to the broker at least once. Doesn't apply while
Lite Mode is active — see below, the button means something different
there.

### Lite Mode

A "Lite Mode" section on the Settings page has an "Activate Lite Mode"
button that turns WiFi off completely — station connection and fallback
hotspot both — leaving the bridge running as a pure BLE bridge (bike ↔
watch) with no dashboard, MQTT or network OTA reachable at all. Activating
it reboots the bridge immediately, so the Settings page (and this
decision) is the last thing you can do over the network until you exit
Lite Mode again.

To exit: hold the **BOOT** button and release it somewhere between 5 and
15 seconds — the bridge reboots back into normal (WiFi-enabled) operation.
The status LED blinks twice at the start of every ~3s cycle while Lite
Mode is active, independent of bike/watch connection status, so it stays
visible at a glance; once you start holding BOOT to exit, it switches to
the same fast countdown blink used for a factory reset. Holding past 15
seconds factory-resets instead of just exiting Lite Mode, same as it
always does — see `LITE_MODE_EXIT_HOLD_MS`/`RESET_BUTTON_HOLD_MS` in
`include/config.h` for both durations.

### Setting a target resistance

Tapping the Resistance card on the dashboard opens a small picker with a
number field and a Send button. This sends a "Set Target Resistance
Level" command to the bike over the FTMS Control Point — the same
mechanism apps like Zwift use to control a trainer's resistance. It only
appears clickable when the connected bike actually exposed a Control
Point characteristic; most modern smart trainers do, but it's optional in
the spec, so a few won't offer it.

If the bike also exposes the optional "Supported Resistance Level Range"
characteristic, the picker is bounded to its real min/max/step; otherwise
it falls back to a generic 0–100 range. There's no confirmation that the
bike actually applied the value — the resistance shown on the dashboard
will simply update (or not) within a second or two, same as any other
live reading.

If an admin password is set, sending a resistance command requires it,
same as every other page — your browser will prompt for it the first
time.

**Over MQTT**, publish a plain number to `<prefix>/resistance_level/set`
(e.g. `mosquitto_pub -t bike2watch/resistance_level/set -m 50`) — same
effect as the dashboard picker, just without opening a browser. With Home
Assistant MQTT Discovery it also shows up as a settable "Target
Resistance" number entity (in addition to the existing read-only
"Resistance" sensor); its min/max shown in the Home Assistant UI are a
generic placeholder, not the connected bike's real range, since that's
only known once a bike is actually connected. As with the dashboard,
there's no confirmation that the bike accepted the value — check the
"Resistance" sensor/dashboard a moment later to see whether it changed.

### Troubleshooting: it keeps opening the hotspot again after saving WiFi

After saving the Settings page, the bridge reboots, tries the configured
network for up to `WIFI_CONNECT_TIMEOUT_MS` (default 15s), retries once if
that fails, and only falls back to its own hotspot if both attempts fail.
If that keeps happening even though the SSID/password look right, check
the serial log (USB + `pio device monitor`) — it prints the actual reason
for the failed attempt:

- **"SSID not found"** — the ESP32 never even saw a beacon for that
  network. Most common cause: a router or mesh system broadcasting the
  same SSID on both 2.4GHz and 5GHz with band-steering. The ESP32 only
  has a 2.4GHz radio — if steering hides the 2.4GHz beacon behind the
  5GHz one, this fails no matter how correct the password is. Giving the
  2.4GHz band its own SSID (even temporarily, to set up the bridge) is
  the usual fix.
- **"Connection rejected"** — the AP was found but rejected the
  association, almost always a wrong password.
- **generic timeout** — usually a weak signal or slow DHCP; moving the
  bridge closer to the router while testing rules this out.

The Settings page's "Test Wi-Fi" button catches most of this before you
even save — see "Configuration" above.

## Releasing new firmware versions

Before tagging, bump `FIRMWARE_VERSION` in `include/version.h` to match —
it's what the web UI's Firmware Update page shows as the running
version, and isn't derived from the git tag automatically.

Pushing a version tag (e.g. `v1.2.0`) builds firmware for every
PlatformIO env via GitHub Actions
(`.github/workflows/release.yml`) and attaches the resulting `.bin` files
to a new GitHub Release:

```bash
git tag v1.2.0
git push origin v1.2.0
```

Download the `.bin` matching your board from the release, then flash it
via the browser's Firmware Update page (`/update`) or network OTA — see
"OTA updates" above. These are application-only images (no bootloader/
partition table), matching what `/update` expects.

One-time exception: versions from this point on build with a
`min_spiffs` partition table (needed to fit HTTPClient/WiFiClientSecure's
mbedTLS for the GitHub update check — this project doesn't use
SPIFFS/LittleFS, so that partition's space goes to the app instead). A
device still running a firmware built *before* this change has an old,
smaller partition table physically in flash, so its first OTA update (via
either `/update` path) to a `min_spiffs`-built image will safely fail
(reported as "update failed", device stays on its current firmware, same
guarantee as any other failed OTA write) because the new image no longer
fits the old OTA partition size. That one device needs a single USB
reflash (`pio run -t upload`) to pick up the new partition table; every
OTA update after that works normally again.

The Firmware Update page can also do this for you: click **"Check for
updates"** to have the bridge query GitHub directly for the latest
release and compare it to the running version, then **"Install"** to
download and flash the matching `.bin` automatically. The manual upload
form below it still works the same way, e.g. for pre-release or
locally-built binaries.

This check talks to GitHub over HTTPS but skips certificate validation
(`WiFiClientSecure::setInsecure()`) — the common, low-effort approach for
ESP32 OTA. That means it has no protection against a man-in-the-middle on
the network path to GitHub; acceptable here given the binary is only ever
flashed after an explicit second "Install" click, but worth knowing if you
run this bridge on a network you don't fully trust.

## Testing beta builds

Changes are first collected on the `beta` branch. Every push there runs the
unit tests, builds firmware for both boards and publishes a GitHub
**pre-release** named `v<version>-beta.<n>` (see
`.github/workflows/beta.yml`), so you can try a change on real hardware
before it is merged to `main` and released.

1. Open the [Releases page](../../releases) and pick the newest pre-release.
2. Download the `.bin` for your board (`esp32dev` or `esp32-s3-devkitc-1`)
   and flash it via the web UI's Firmware Update page, or with
   `pio run -t upload`. The `.sha256` file lets you verify the download.
3. To go back, flash the latest stable (non-pre-release) release the same way.

The firmware's built-in update check only looks at stable releases, so a
beta is never offered automatically. The beta number is appended at build
time; `include/version.h` is not edited. Only the 10 newest betas are kept.

For serial logging use the `esp32dev-debug` / `esp32-s3-devkitc-1-debug`
environments; the regular builds have logging compiled out.

## Project structure

```
include/            Public headers (protocol logic, settings, web/MQTT interfaces)
src/                 Implementation (main loop, web server, MQTT client, OTA, settings store)
test/test_native/    Native unit tests (protocol parsing, CSC/CP encoding) — run with `pio test -e native`
platformio.ini       Build environments: esp32dev (firmware) and native (unit tests)
```

The FTMS parsing and CSC/CP encoding logic (`include/bike2watch.h`) and
the status JSON builder (`include/web_dashboard.h`) are header-only and
Arduino-free by design, so they can be exercised directly by the native
unit tests without any hardware or emulator.

`include/ble_devices.h` declares the (small, deliberately narrow) interface
the web server uses to show/pin FTMS devices; it's implemented in
`src/main.cpp` alongside the rest of the BLE client/server state, since the
scan/connect logic it hooks into already lives there. `include/ftms_control.h`
is the same idea for the dashboard's resistance picker — the actual FTMS
Control Point write happens asynchronously on the BLE client's own task
(`bleClientTask` in `main.cpp`), never on the web server's task, so it can
never block the dashboard even if the write is slow.

## License

MIT — see [LICENSE](LICENSE).

Like this project? [Buy me a coffee ☕](https://buymeacoffee.com/pjanfred).
