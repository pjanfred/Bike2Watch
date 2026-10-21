#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <cstring>
#include "web_server.h"
#include "settings_store.h"
#include "ble_devices.h"
#include "ftms_control.h"
#include "config.h"
#include "version.h"

#if DEBUG_LOG
  #define LOG(fmt, ...) Serial.printf(fmt "\n", ##__VA_ARGS__)
#else
  #define LOG(fmt, ...)
#endif

static WebServer    g_httpServer(80);
static Bike2WatchStatus g_status;
static Bike2WatchSettings g_settings;
static char g_wifiModeBuf[16] = "connecting";
static char g_ipBuf[16]       = "";
static bool g_uploadAuthorized = false;
static bool g_uploadSameOrigin = false;

// ── Captive portal (hotspot mode only) ──────────────────────────────
// Active only while the bridge is running its own fallback hotspot (no
// WiFi configured yet, or joining the configured network failed) — never
// on a real WiFi network, since hijacking arbitrary DNS lookups there
// would break the user's actual network (see the AP-fallback branch of
// connectWifi() below, the only place that sets g_captivePortalActive).
//
// While active, g_dnsServer resolves *every* DNS query to the bridge's
// own IP regardless of hostname, and handleNotFound() redirects any
// unmatched request to the Settings page. Together, that's what makes
// phones/laptops show "Sign in to network" automatically after joining
// the hotspot: their captive-portal probe (Android's /generate_204,
// iOS's captive.apple.com/hotspot-detect.html, etc.) resolves to us via
// the wildcard DNS entry, hits our server, gets redirected — and every
// major OS treats that as "this network needs sign-in", opening a
// browser to the redirect target automatically.
static DNSServer  g_dnsServer;
static bool       g_captivePortalActive = false;
static const byte DNS_PORT = 53;

// CSS custom properties shared by every page — dark by default, with a
// light override under prefers-color-scheme: light. Prepended (via string
// concatenation, see THEME_VARS's use below) into both PAGE_CSS and
// INDEX_HTML's own inline <style>, since those are two separate PROGMEM
// strings rather than one shared stylesheet.
//
// The dark values are GitHub's own dark-theme design tokens; the light
// override pairs them with GitHub's matching light-theme tokens, so both
// palettes are already tuned for contrast/readability together rather
// than invented from scratch.
#define THEME_VARS \
  ":root {" \
  "  color-scheme: light dark;" \
  "  --bg:#0b0f14; --bg-elevated:#161b22; --bg-input:#0d1117; --bg-active:#1c2128; --bg-btn2:#21262d;" \
  "  --text:#e6edf3; --text-muted:#8b949e; --text-dim:#6e7681; --border:#30363d; --accent:#58a6ff;" \
  "  --success:#3fb950; --success-bg:#0d2818; --btn:#238636; --btn-hover:#2ea043;" \
  "  --danger:#f85149; --danger-strong:#da3633;" \
  "}" \
  "@media (prefers-color-scheme: light) { :root {" \
  "  --bg:#ffffff; --bg-elevated:#f6f8fa; --bg-input:#ffffff; --bg-active:#eaeef2; --bg-btn2:#f6f8fa;" \
  "  --text:#1f2328; --text-muted:#59636e; --text-dim:#6e7781; --border:#d0d7de; --accent:#0969da;" \
  "  --success:#1a7f37; --success-bg:#dafbe1; --btn:#1f883d; --btn-hover:#1a7f37;" \
  "  --danger:#cf222e; --danger-strong:#d1242f;" \
  "} }"

// One slow pulse through --danger (red) on page load, gently drawing the
// eye to the sponsor links. Shared the same way as THEME_VARS, since
// PAGE_CSS and INDEX_HTML's <style> are separate PROGMEM strings.
#define SPONSOR_ATTENTION_CSS \
  "@keyframes sponsorAttention {" \
  "  0%, 100% { color: var(--text-muted); border-color: var(--border); }" \
  "  50% { color: var(--danger); border-color: var(--danger); }" \
  "}" \
  ".sponsor-links a { animation: sponsorAttention 1.4s ease-in-out 1; }"

static const char PAGE_CSS[] PROGMEM = THEME_VARS SPONSOR_ATTENTION_CSS R"CSS(
  * { box-sizing: border-box; }
  body { margin:0; padding:24px 16px 48px; background:var(--bg); color:var(--text); font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif; }
  .wrap { max-width: 480px; margin: 0 auto; }
  h1 { font-size:1.1rem; font-weight:600; text-align:center; margin:0 0 4px; color:var(--text-muted); letter-spacing:.04em; text-transform:uppercase; }
  .name { text-align:center; font-size:1.4rem; font-weight:700; margin-bottom:16px; }
  nav { display:flex; justify-content:center; gap:8px; margin-bottom:24px; flex-wrap:wrap; }
  nav a { color:var(--text-muted); text-decoration:none; font-size:.85rem; padding:6px 12px; border-radius:999px; border:1px solid var(--border); }
  nav a.active, nav a:hover { color:var(--text); border-color:var(--accent); }
  fieldset { border:1px solid var(--border); border-radius:14px; padding:14px 16px 6px; margin-bottom:16px; }
  legend { padding:0 6px; color:var(--text-muted); font-size:.8rem; text-transform:uppercase; letter-spacing:.04em; }
  label { display:block; margin-bottom:14px; font-size:.85rem; color:var(--text-muted); }
  label.checkbox { display:flex; align-items:center; gap:8px; }
  input[type=text], input[type=password], input[type=number] {
    width:100%; margin-top:6px; padding:10px 12px; border-radius:8px; border:1px solid var(--border);
    background:var(--bg-input); color:var(--text); font-size:1rem;
  }
  input[type=file] { width:100%; margin-top:6px; color:var(--text); }
  button, input[type=submit] {
    width:100%; padding:12px; border-radius:10px; border:none; background:var(--btn); color:#fff;
    font-size:1rem; font-weight:600; cursor:pointer;
  }
  button:hover, input[type=submit]:hover { background:var(--btn-hover); }
  .hint { color:var(--text-dim); font-size:.8rem; margin-top:16px; line-height:1.5; }
  .sponsor-links { display:flex; justify-content:flex-end; gap:8px; margin-bottom:12px; flex-wrap:wrap; }
  .sponsor-links a { display:inline-flex; align-items:center; gap:4px; color:var(--text-muted); text-decoration:none; font-size:.75rem; padding:5px 10px; border-radius:999px; border:1px solid var(--border); white-space:nowrap; }
  .sponsor-links a:hover { color:var(--text); border-color:var(--accent); }
  fieldset.danger { border-color:var(--danger); }
  fieldset.danger legend { color:var(--danger); }
  button.danger { background:var(--danger-strong); }
  button.danger:hover { background:var(--danger); }
)CSS";

// Single-page live dashboard. Polls /api/status once a second and updates
// values in place — no page reloads, so it works fine on a phone whether
// it's on the same WiFi network or connected to the bridge's own hotspot.
static const char INDEX_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Bike2Watch</title>
<style>
)HTML" THEME_VARS SPONSOR_ATTENTION_CSS R"HTML(
  * { box-sizing: border-box; }
  body {
    margin: 0; padding: 24px 16px 48px;
    background: var(--bg); color: var(--text);
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
  }
  h1 { font-size: 1.1rem; font-weight: 600; text-align: center; margin: 0 0 4px; color: var(--text-muted); letter-spacing: .04em; text-transform: uppercase; }
  .name { text-align: center; font-size: 1.4rem; font-weight: 700; margin-bottom: 16px; }
  nav { display: flex; justify-content: center; gap: 8px; margin-bottom: 24px; flex-wrap: wrap; }
  nav a { color: var(--text-muted); text-decoration: none; font-size: .85rem; padding: 6px 12px; border-radius: 999px; border: 1px solid var(--border); }
  nav a.active, nav a:hover { color: var(--text); border-color: var(--accent); }
  .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(130px, 1fr)); gap: 12px; max-width: 520px; margin: 0 auto 24px; }
  .card { background: var(--bg-elevated); border: 1px solid var(--border); border-radius: 14px; padding: 18px 12px; text-align: center; }
  .card .value { font-size: 2rem; font-weight: 700; font-variant-numeric: tabular-nums; }
  .card .unit { font-size: .8rem; color: var(--text-muted); margin-top: 2px; }
  .card .label { font-size: .75rem; color: var(--text-muted); text-transform: uppercase; letter-spacing: .04em; margin-top: 6px; }
  .card.clickable { cursor: pointer; }
  .card.clickable:active { background: var(--bg-active); }
  .status { max-width: 520px; margin: 0 auto; display: flex; gap: 10px; justify-content: center; flex-wrap: wrap; }
  .pill { display: flex; align-items: center; gap: 6px; background: var(--bg-elevated); border: 1px solid var(--border); border-radius: 999px; padding: 6px 14px; font-size: .85rem; }
  .dot { width: 8px; height: 8px; border-radius: 50%; background: var(--text-dim); }
  .dot.on { background: var(--success); box-shadow: 0 0 6px var(--success); }
  .foot { text-align: center; color: var(--text-dim); font-size: .75rem; margin-top: 28px; }
  .modal-backdrop { position: fixed; inset: 0; background: rgba(0,0,0,.6); display: flex; align-items: center; justify-content: center; padding: 20px; z-index: 10; }
  .modal { background: var(--bg-elevated); border: 1px solid var(--border); border-radius: 14px; padding: 20px; max-width: 300px; width: 100%; }
  .modal h2 { font-size: 1rem; margin: 0 0 16px; text-align: center; }
  .modal input[type=number] { width: 100%; padding: 12px; border-radius: 8px; border: 1px solid var(--border); background: var(--bg-input); color: var(--text); font-size: 1.4rem; text-align: center; margin-bottom: 6px; }
  .modal .range-hint { text-align: center; color: var(--text-muted); font-size: .75rem; margin-bottom: 18px; }
  .modal .modal-actions { display: flex; gap: 10px; }
  .modal button { flex: 1; padding: 12px; border-radius: 10px; border: none; font-size: 1rem; font-weight: 600; cursor: pointer; }
  .modal button.primary { background: var(--btn); color: #fff; }
  .modal button.primary:disabled { opacity: .6; }
  .modal button.secondary { background: var(--bg-btn2); color: var(--text); border: 1px solid var(--border); }
  .sponsor-links { margin: 0 0 12px; display: flex; justify-content: flex-end; gap: 8px; flex-wrap: wrap; }
  .sponsor-links a { display: inline-flex; align-items: center; gap: 4px; color: var(--text-muted); text-decoration: none; font-size: .75rem; padding: 5px 10px; border-radius: 999px; border: 1px solid var(--border); white-space: nowrap; }
  .sponsor-links a:hover { color: var(--text); border-color: var(--accent); }
</style>
</head>
<body>
  <div class="sponsor-links">
    <a href="https://buymeacoffee.com/pjanfred" target="_blank" rel="noopener">☕ Buy me a coffee</a>
  </div>
  <h1>Bike2Watch</h1>
  <div class="name">Dashboard</div>
  <nav>
    <a class="active" href="/">Dashboard</a>
    <a href="/settings">Settings</a>
    <a href="/devices">Bike</a>
    <a href="/update">Firmware Update</a>
  </nav>

  <div class="grid">
    <div class="card"><div class="value" id="speed">--</div><div class="unit">km/h</div><div class="label">Speed</div></div>
    <div class="card"><div class="value" id="cadence">--</div><div class="unit">rpm</div><div class="label">Cadence</div></div>
    <div class="card"><div class="value" id="power">--</div><div class="unit">watt</div><div class="label">Power</div></div>
    <div class="card" id="resistanceCard"><div class="value" id="resistance">--</div><div class="unit">&nbsp;</div><div class="label">Resistance</div></div>
  </div>

  <div class="status">
    <div class="pill"><span class="dot" id="dotBike"></span>Bike</div>
    <div class="pill"><span class="dot" id="dotWatch"></span>Watch</div>
    <div class="pill"><span class="dot on"></span><span id="wifiMode">--</span></div>
    <div class="pill" id="pillMqtt" style="display:none"><span class="dot" id="dotMqtt"></span>MQTT</div>
  </div>

  <div class="status" id="pinWarning" style="display:none; margin-top:10px">
    <div class="pill" style="border-color:var(--danger);color:var(--danger)">
      <span class="dot" style="background:var(--danger);box-shadow:0 0 6px var(--danger)"></span>
      <span id="pinWarningText"></span>
    </div>
  </div>

  <div class="foot" id="foot">&nbsp;</div>

  <div class="modal-backdrop" id="resistanceModal" style="display:none">
    <div class="modal">
      <h2>Set target resistance</h2>
      <input type="number" id="resistanceInput" inputmode="decimal">
      <div class="range-hint" id="resistanceRangeHint">&nbsp;</div>
      <div class="modal-actions">
        <button class="secondary" type="button" id="resistanceCancel">Cancel</button>
        <button class="primary" type="button" id="resistanceSend">Send</button>
      </div>
    </div>
  </div>

<script>
let lastStatus = null;

async function refresh() {
  try {
    const r = await fetch('/api/status', { cache: 'no-store' });
    const s = await r.json();
    lastStatus = s;
    document.getElementById('speed').textContent = s.hasSpeed ? s.speedKmh.toFixed(1) : '--';
    document.getElementById('cadence').textContent = s.hasCadence ? s.cadenceRpm.toFixed(0) : '--';
    document.getElementById('power').textContent = s.hasPower ? s.power : '--';
    document.getElementById('resistance').textContent = s.hasResistance ? s.resistanceLevel : '--';
    document.getElementById('resistanceCard').classList.toggle('clickable', s.canSetResistance);
    document.getElementById('dotBike').classList.toggle('on', s.bikeConnected);
    document.getElementById('dotWatch').classList.toggle('on', s.watchConnected);
    document.getElementById('wifiMode').textContent = s.wifiMode + ' · ' + s.ipAddress;
    document.getElementById('pillMqtt').style.display = s.mqttEnabled ? 'flex' : 'none';
    document.getElementById('dotMqtt').classList.toggle('on', s.mqttConnected);
    document.getElementById('pinWarning').style.display = s.pinnedDeviceMissing ? 'flex' : 'none';
    if (s.pinnedDeviceMissing) {
      document.getElementById('pinWarningText').textContent =
        'Selected device not found' + (s.pinnedDeviceName ? ': ' + s.pinnedDeviceName : '');
    }
    const mins = Math.floor(s.uptimeSec / 60), secs = s.uptimeSec % 60;
    document.getElementById('foot').textContent =
      'up ' + mins + 'm ' + secs + 's';
  } catch (e) { /* keep last values on a missed poll */ }
}
refresh();
setInterval(refresh, 1000);

// Resistance picker — only opens if the currently connected bike actually
// exposed an FTMS Control Point (see canSetResistance in /api/status);
// clicking the card while unavailable does nothing (it also isn't styled
// as clickable in that case, see the 'clickable' class toggle above).
const resistanceModal = document.getElementById('resistanceModal');
document.getElementById('resistanceCard').addEventListener('click', function() {
  if (!lastStatus || !lastStatus.canSetResistance) return;
  const input = document.getElementById('resistanceInput');
  input.min = lastStatus.resistanceMin;
  input.max = lastStatus.resistanceMax;
  input.step = lastStatus.resistanceStep > 0 ? lastStatus.resistanceStep : 1;
  input.value = lastStatus.hasResistance ? lastStatus.resistanceLevel : lastStatus.resistanceMin;
  document.getElementById('resistanceRangeHint').textContent =
    'Range ' + lastStatus.resistanceMin + ' – ' + lastStatus.resistanceMax;
  resistanceModal.style.display = 'flex';
  input.focus();
});
document.getElementById('resistanceCancel').addEventListener('click', function() {
  resistanceModal.style.display = 'none';
});
resistanceModal.addEventListener('click', function(e) {
  if (e.target === resistanceModal) resistanceModal.style.display = 'none';  // click on backdrop
});
document.getElementById('resistanceSend').addEventListener('click', async function() {
  const val = document.getElementById('resistanceInput').value;
  if (val === '') return;
  const btn = this;
  btn.disabled = true;
  try {
    await fetch('/resistance', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: 'level=' + encodeURIComponent(val)
    });
  } catch (e) { /* the live value will simply not change; nothing more to do here */ }
  btn.disabled = false;
  resistanceModal.style.display = 'none';
});
</script>
</body>
</html>
)HTML";

static const char UPDATE_HTML_HEAD[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Bike2Watch – Firmware Update</title>
<style>
)HTML";

static const char UPDATE_HTML_BODY_TOP[] PROGMEM = R"HTML(
</style>
</head>
<body>
<div class="sponsor-links">
  <a href="https://buymeacoffee.com/pjanfred" target="_blank" rel="noopener">☕ Buy me a coffee</a>
</div>
<div class="wrap">
  <h1>Bike2Watch</h1>
  <div class="name">Firmware Update</div>
  <nav>
    <a href="/">Dashboard</a>
    <a href="/settings">Settings</a>
    <a href="/devices">Bike</a>
    <a class="active" href="/update">Firmware Update</a>
  </nav>

  <p class="hint" style="margin-top:-8px">Running version )HTML";

static const char UPDATE_HTML_BODY_BOTTOM[] PROGMEM = R"HTML(</p>

  <form method="POST" action="/update" enctype="multipart/form-data">
    <fieldset>
      <legend>New firmware (.bin)</legend>
      <label>Choose file<input type="file" name="update" accept=".bin" required></label>
      <button type="submit">Upload &amp; flash</button>
    </fieldset>
  </form>
  <p class="hint">
    After the upload, the bridge restarts automatically.
  </p>
</div>
</body>
</html>
)HTML";

// Checks credentials without writing to the response — safe to call at
// any point, including mid-multipart-upload, unlike requireAuth() below.
static bool isAuthorized() {
    if (strlen(g_settings.adminPassword) == 0) return true;
    return g_httpServer.authenticate("admin", g_settings.adminPassword);
}

// Checks credentials and, if missing or wrong, sends the 401 challenge.
// Only safe to call from a normal request handler, never mid-upload —
// see handleUpdateUpload()/handleUpdateFinish() for why those use
// isAuthorized() instead and defer the challenge.
static bool requireAuth() {
    if (isAuthorized()) return true;
    g_httpServer.requestAuthentication();
    return false;
}

// Lightweight CSRF guard for state-changing (POST) endpoints. requireAuth()
// alone doesn't stop a malicious page open in the same browser from
// submitting a cross-site POST here: with no admin password set (the
// default) there's no credential to check at all, and even with one set,
// browsers auto-attach cached HTTP Basic Auth credentials to cross-site
// requests too. What a cross-site request *can't* fake is the Origin/
// Referer header naming this bridge's own host — only a same-origin form
// submission or fetch() call does that, so a mismatch here means the
// request didn't actually come from one of our own pages.
//
// Checks Origin first (sent by fetch()/modern browsers on essentially every
// cross-site POST), falling back to Referer for older/simpler form
// submissions. A request with neither header is let through rather than
// rejected — that's not a browser CSRF vector, and blocking it would risk
// breaking legitimate non-browser clients (curl, scripts) for no security
// benefit, the same open-by-default trade-off requireAuth() already makes
// when no admin password is set.
static bool isSameOriginHeader(const String& headerValue, const String& host) {
    int schemeEnd = headerValue.indexOf("://");
    if (schemeEnd < 0) return false;
    int hostStart = schemeEnd + 3;
    int hostEnd = headerValue.indexOf('/', hostStart);
    String requestHost = (hostEnd < 0) ? headerValue.substring(hostStart)
                                        : headerValue.substring(hostStart, hostEnd);
    return requestHost.equalsIgnoreCase(host);
}

static bool checkSameOrigin() {
    String host = g_httpServer.hostHeader();
    if (g_httpServer.hasHeader("Origin")) {
        return isSameOriginHeader(g_httpServer.header("Origin"), host);
    }
    if (g_httpServer.hasHeader("Referer")) {
        return isSameOriginHeader(g_httpServer.header("Referer"), host);
    }
    return true;
}

// Checks the guard above and, if it fails, sends a 403 — the CSRF
// equivalent of requireAuth()'s 401. Only safe to call from a normal
// request handler, never mid-upload; see handleUpdateUpload()/
// handleUpdateFinish() for why those check checkSameOrigin() directly and
// defer the response, same reasoning as isAuthorized()/requireAuth().
static bool requireSameOrigin() {
    if (checkSameOrigin()) return true;
    LOG("Web: rejected cross-origin POST to %s (Origin/Referer mismatch)",
        g_httpServer.uri().c_str());
    g_httpServer.send(403, "text/plain", "Forbidden (cross-origin request)");
    return false;
}

static String escapeHtml(const char* s) {
    String out;
    for (const char* p = s; *p; ++p) {
        switch (*p) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            default:   out += *p;
        }
    }
    return out;
}

// Same idea as escapeHtml(), but for embedding into a JSON string value —
// needed anywhere a BLE-advertised device name (untrusted input) ends up
// in an /api/* response.
static String jsonEscape(const char* s) {
    String out;
    for (const char* p = s; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') { out += '\\'; out += (char)c; }
        else if (c >= 0x20)        out += (char)c;
    }
    return out;
}

static void handleRoot() {
    if (!requireAuth()) return;
    g_httpServer.send_P(200, "text/html", INDEX_HTML);
}

static void handleStatus() {
    if (!requireAuth()) return;
    // 640, not 512: the resistance-control fields (canSetResistance,
    // resistanceMin/Max/Step) push the worst case (a long
    // pinnedDeviceName plus extreme float values) close to a smaller
    // buffer's limit — see buildStatusJson() in web_dashboard.h.
    char buf[640];
    size_t len = buildStatusJson(g_status, buf, sizeof(buf));
    if (len == 0) {
        g_httpServer.send(500, "application/json", "{\"error\":\"status too large\"}");
        return;
    }
    g_httpServer.send(200, "application/json", buf);
}

// Sets the connected bike's target resistance via the FTMS Control Point
// (see ftms_control.h) — triggered by the Resistance card's picker on the
// dashboard. Form-encoded request, tiny JSON response: the dashboard is a
// live-polling single page and must never reload, unlike /settings or
// /devices. Gated behind requireAuth(), same as every other endpoint here
// once an admin password is set (see handleRoot()/handleStatus() above).
// A browser that hits the 401 challenge shows its native basic-auth
// prompt on its own; nothing special needed for that in the dashboard's
// JS.
static void handleResistancePost() {
    if (!requireAuth()) return;
    if (!requireSameOrigin()) return;

    if (!g_httpServer.hasArg("level")) {
        g_httpServer.send(400, "application/json", "{\"ok\":false,\"error\":\"missing level\"}");
        return;
    }
    float level = g_httpServer.arg("level").toFloat();
    if (!requestSetResistance(level)) {
        g_httpServer.send(503, "application/json", "{\"ok\":false,\"error\":\"no bike connected\"}");
        return;
    }
    // "true" here means the request was queued for the BLE client task,
    // not that the bike actually accepted it — there's no synchronous
    // confirmation available (see ftms_control.h). The dashboard's normal
    // 1s poll will show the new value once/if the bike applies it.
    g_httpServer.send(200, "application/json", "{\"ok\":true}");
}

static void handleNotFound() {
    if (g_captivePortalActive) {
        // Funnel every unmatched request — captive-portal probes, typos,
        // a browser's own startup fetches — to the Settings page. The
        // Location header alone satisfies virtually every client; the
        // tiny HTML body (meta-refresh plus a plain link) only exists as
        // a fallback for the rare client that renders a redirect's body
        // instead of following it outright.
        String url = "http://";
        url += g_ipBuf;
        url += "/settings";

        String body;
        body += F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
                   "<meta http-equiv='refresh' content='0; url=");
        body += url;
        body += F("'></head><body>Redirecting to <a href='");
        body += url;
        body += F("'>");
        body += url;
        body += F("</a> &hellip;</body></html>");

        g_httpServer.sendHeader("Location", url, true);
        g_httpServer.send(302, "text/html", body);
        return;
    }
    g_httpServer.send(404, "text/plain", "Not found");
}

// JSON feed for the Bike page's live device list — see handleDevicesGet().
// Protected the same as that page itself.
static void handleApiDevices() {
    if (!requireAuth()) return;

    DiscoveredBleDevice devices[8];
    size_t count = getDiscoveredDevices(devices, 8);

    String json;
    json.reserve(600);
    json += "{\"scanning\":";
    json += bleScanInProgress() ? "true" : "false";
    json += ",\"bikeConnected\":";
    json += bleBikeConnected() ? "true" : "false";
    json += ",\"pinnedMac\":\"";
    json += jsonEscape(getPinnedDeviceMac());
    json += "\",\"pinnedName\":\"";
    json += jsonEscape(getPinnedDeviceName());
    json += "\",\"devices\":[";
    for (size_t i = 0; i < count; i++) {
        if (i > 0) json += ",";
        json += "{\"mac\":\"";
        json += jsonEscape(devices[i].mac);
        json += "\",\"name\":\"";
        json += jsonEscape(devices[i].name);
        json += "\",\"rssi\":";
        json += String(devices[i].rssi);
        json += "}";
    }
    json += "]}";

    g_httpServer.send(200, "application/json", json);
}

// True while wifiTestTask() (further down) is running — defined there;
// declared here so handleWifiScanPost() below can avoid starting a scan
// that would race the test for the single WiFi radio.
static bool wifiTestInProgress();

// ── WiFi network scan (Settings page, SSID field) ────────────────────
// Async, mirroring the BLE device scan's design (see bleClientTask() /
// ScanCallbacks in main.cpp): scanNetworks(true, ...) returns immediately
// and results are polled via scanComplete(), so a scan can never block
// the web server. Unlike the BLE scan, this never needs a "might disturb
// an active connection" confirmation — it's a one-off, user-triggered
// action rather than a recurring background loop, and scanning from STA
// doesn't drop the existing WiFi connection.
static void handleWifiScanPost() {
    if (!requireAuth()) return;
    if (!requireSameOrigin()) return;

    // Never start a scan while the WiFi connection test is running (see
    // wifiTestTask()) — both fight over the single radio, and a scan
    // starting mid-test was observed to leave the test hanging or
    // failing (e.g. focusing the SSID field, which auto-starts a scan,
    // right before hitting "Test Wi-Fi"). Silently skipped rather than
    // queued — the SSID field's focus handler retries on the next focus.
    if (!wifiTestInProgress() && WiFi.scanComplete() != WIFI_SCAN_RUNNING) {
        if (WiFi.getMode() == WIFI_AP) {
            // Scanning needs the STA radio, which isn't up yet if we're
            // still AP-only (first-boot / hotspot setup) — enable it
            // alongside the AP rather than replacing it.
            WiFi.mode(WIFI_AP_STA);
        }
        WiFi.scanNetworks(true /* async */, true /* show hidden */);
    }

    g_httpServer.sendHeader("Location", "/settings", true);
    g_httpServer.send(303, "text/plain", "");
}

struct WifiScanEntry {
    String  ssid;
    int32_t rssi;
    bool    secure;
};

// JSON feed for the Settings page's live WiFi network list — see
// handleSettingsGet(). Protected the same as that page itself.
static void handleApiWifiNetworks() {
    if (!requireAuth()) return;

    int16_t n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) {
        g_httpServer.send(200, "application/json", "{\"scanning\":true,\"networks\":[]}");
        return;
    }
    if (n == WIFI_SCAN_FAILED) {
        // No scan has run yet (or it errored) — same empty response either
        // way, the Settings page just shows "no networks found".
        g_httpServer.send(200, "application/json", "{\"scanning\":false,\"networks\":[]}");
        return;
    }

    static const int16_t MAX_ENTRIES = 20;
    WifiScanEntry entries[MAX_ENTRIES];
    int16_t count = 0;

    for (int16_t i = 0; i < n && count < MAX_ENTRIES; i++) {
        String ssid = WiFi.SSID(i);
        if (ssid.length() == 0) continue;  // hidden network — nothing to click; use the manual field
        int16_t existing = -1;
        for (int16_t j = 0; j < count; j++) {
            if (entries[j].ssid == ssid) { existing = j; break; }
        }
        if (existing >= 0) {
            // Same SSID on multiple APs/channels (mesh, repeaters) — keep
            // whichever result has the strongest signal.
            if (WiFi.RSSI(i) > entries[existing].rssi) entries[existing].rssi = WiFi.RSSI(i);
            continue;
        }
        entries[count].ssid   = ssid;
        entries[count].rssi   = WiFi.RSSI(i);
        entries[count].secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
        count++;
    }

    // Strongest signal first. count is small (<= MAX_ENTRIES), so a plain
    // insertion sort is more than fast enough.
    for (int16_t i = 1; i < count; i++) {
        WifiScanEntry key = entries[i];
        int16_t j = i - 1;
        while (j >= 0 && entries[j].rssi < key.rssi) {
            entries[j + 1] = entries[j];
            j--;
        }
        entries[j + 1] = key;
    }

    String json;
    json.reserve(768);
    json += "{\"scanning\":false,\"networks\":[";
    for (int16_t i = 0; i < count; i++) {
        if (i > 0) json += ",";
        json += "{\"ssid\":\"";
        json += jsonEscape(entries[i].ssid.c_str());
        json += "\",\"rssi\":";
        json += String(entries[i].rssi);
        json += ",\"secure\":";
        json += entries[i].secure ? "true" : "false";
        json += "}";
    }
    json += "]}";

    g_httpServer.send(200, "application/json", json);
}

// ── Settings page ────────────────────────────────────────────────
static void handleSettingsGet() {
    if (!requireAuth()) return;

    String html;
    html.reserve(7400);
    html += F("<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'>"
               "<meta name='viewport' content='width=device-width, initial-scale=1'>"
               "<title>Bike2Watch – Settings</title><style>");
    html += FPSTR(PAGE_CSS);
    html += F(
        "table{width:100%;border-collapse:collapse;margin-top:4px}"
        "td{padding:10px 6px;border-bottom:1px solid var(--border);font-size:.9rem;vertical-align:middle}"
        "td.rssi{color:var(--text-muted);font-size:.8rem;text-align:right;white-space:nowrap}"
        "tr.wifiRow{cursor:pointer}"
        "tr.wifiRow:hover{background:var(--bg-elevated)}"
        "button.small{width:auto;padding:6px 14px;font-size:.85rem}"
        ".empty{color:var(--text-dim);font-size:.85rem;padding:10px 0}"
        "#wifiScanState{color:var(--text-muted);font-size:.8rem}"
    );
    html += F("</style></head><body>"
               "<div class='sponsor-links'>"
               "<a href='https://buymeacoffee.com/pjanfred' target='_blank' rel='noopener'>☕ Buy me a coffee</a>"
               "</div>"
               "<div class='wrap'>"
               "<h1>Bike2Watch</h1><div class='name'>Settings</div>"
               "<nav><a href='/'>Dashboard</a>"
               "<a class='active' href='/settings'>Settings</a>"
               "<a href='/devices'>Bike</a>"
               "<a href='/update'>Firmware Update</a></nav>");

    html += F("<form method='POST' action='/settings'>");

    html += F("<fieldset><legend>WiFi</legend>"
               "<label>SSID<input type='text' id='wifiSsidField' name='wifiSsid' maxlength='32' "
               "autocomplete='off' value='");
    html += escapeHtml(g_settings.wifiSsid);
    html += F("'></label>"
               "<div id='wifiScanState' style='margin:-6px 0 8px'></div>"
               "<div id='wifiList'></div>"
               "<label>Password<input type='password' id='wifiPasswordField' name='wifiPassword' "
               "maxlength='64' placeholder='leave unchanged'></label>"
               "<div style='display:flex;align-items:center;gap:10px;margin:0 0 14px'>"
               "<button type='button' id='wifiTestBtn' class='small'>Test Wi-Fi</button>"
               "<span id='wifiTestState'></span></div>"
               "</fieldset>");

    html += F("<fieldset><legend>Wheel</legend>"
               "<label>Wheel circumference (mm)<input type='number' name='wheelCircMm' min='100' max='5000' value='");
    html += String(g_settings.wheelCircumferenceMm);
    html += F("'></label>"
               "<p class='hint' style='margin-top:0'>Used to convert the speed reported by the bike "
               "into simulated wheel revolutions for the CSC sensor (which some watches/head units "
               "expect instead of direct FTMS data). Only relevant if the speed shown on your watch "
               "doesn't match the bike — in that case, enter the value your watch's \"wheel sensor\" "
               "is configured with. Doesn't need to match any real wheel size, since the bike itself "
               "has no wheel.</p>"
               "</fieldset>");

    html += F("<fieldset><legend>MQTT</legend>"
               "<label class='checkbox'><input type='checkbox' name='mqttEnabled' value='1'");
    if (g_settings.mqttEnabled) html += F(" checked");
    html += F("> Enable MQTT</label>"
               "<label>Broker host<input type='text' name='mqttHost' maxlength='64' value='");
    html += escapeHtml(g_settings.mqttHost);
    html += F("' placeholder='e.g. 192.168.1.10 or mqtt.local'></label>"
               "<label>Port<input type='number' name='mqttPort' min='1' max='65535' value='");
    html += String(g_settings.mqttPort);
    html += F("'></label>"
               "<label>Username<input type='text' name='mqttUser' maxlength='32' value='");
    html += escapeHtml(g_settings.mqttUser);
    html += F("'></label>"
               "<label>Password<input type='password' name='mqttPassword' maxlength='64' "
               "placeholder='leave unchanged'></label>"
               "<label>Topic prefix<input type='text' name='mqttTopicPrefix' maxlength='32' value='");
    html += escapeHtml(g_settings.mqttTopicPrefix);
    html += F("'></label></fieldset>");

    html += F("<fieldset><legend>Security</legend>"
               "<label>Admin password<input type='password' name='adminPassword' maxlength='64' "
               "placeholder='");
    html += strlen(g_settings.adminPassword) > 0 ? F("leave unchanged") : F("none set — anyone can access this bridge");
    html += F("'></label>");
    if (strlen(g_settings.adminPassword) > 0) {
        html += F("<label class='checkbox'><input type='checkbox' name='removeAdminPassword' value='1'>"
                   " Remove password (bridge becomes fully open again)</label>");
    }
    html += F("<p class='hint' style='margin-top:0'>Once set, this protects the entire web "
               "interface — the dashboard included, not just Settings/Bike/Firmware Update — plus "
               "network (OTA) flashing, from anyone else who can reach the bridge on your network. "
               "Username is always <code>admin</code>. If you forget it, hold the board's BOOT "
               "button for 15s to factory reset (see the Danger Zone below for what that wipes), or "
               "reflash over USB (<code>pio run -t upload</code>), which leaves settings "
               "untouched.</p>"
               "</fieldset>");

    html += F("<button type='submit'>Save &amp; restart</button></form>");
    html += F("<p class='hint'>After saving, the bridge restarts and connects to the new "
               "WiFi network/MQTT broker. If the WiFi connection fails, it opens its own hotspot "
               "again, <b>");
    html += WIFI_AP_SSID;
    html += F("</b> — after connecting to it, this page should open on its own "
               "(\"Sign in to network\"); if not, open http://192.168.4.1/settings manually. "
               "If the connection keeps failing even though the SSID/password are correct: the "
               "ESP32 only supports 2.4GHz WiFi, not 5GHz — on routers/mesh systems that broadcast "
               "both bands under the same name (band steering), giving the 2.4GHz band its own "
               "SSID often helps. The exact reason for a failed connection attempt is shown in "
               "the bridge's serial log (USB + <code>pio device monitor</code>).</p>");

    html += F("<script>var wifiIsApMode=");
    html += (strcmp(g_wifiModeBuf, "Hotspot") == 0) ? F("true") : F("false");
    html += F(";</script>");

    html += F(
        "<script>"
        "function esc(s){return String(s).replace(/[&<>\"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;'}[c]));}"
        "let wifiPoll=null;"
        "let wifiScanning=false;"
        "let wifiAutoScanned=false;"
        // Triggered by focusing the SSID field (see the listener below)
        // instead of a separate button — and while in AP mode (no WiFi
        // configured yet, or the fallback hotspot), on page load too:
        // whoever's looking at this page almost certainly needs to pick a
        // network right away, so there's no reason to make them click
        // into the field first. Only ever fires once per page load
        // (wifiAutoScanned) — the network list appearing/disappearing
        // right below the field shifts the page layout, and re-triggering
        // on every single focus event caused a loop on iOS Safari: it
        // tends to refocus/reopen the keyboard for a field whose
        // surrounding content just shifted, which fired 'focus' again,
        // starting another scan, shifting the layout again, and so on.
        "function startWifiScan(){"
          "if(wifiScanning)return;"
          "wifiScanning=true;"
          "fetch('/wifi/scan',{method:'POST'}).then(function(){"
            "document.getElementById('wifiScanState').textContent='Scanning…';"
            "document.getElementById('wifiList').innerHTML='';"
            "if(!wifiPoll)wifiPoll=setInterval(pollWifi,1000);"
            "pollWifi();"
          "});"
        "}"
        "async function pollWifi(){"
          "try{"
            "const r=await fetch('/api/wifi-networks',{cache:'no-store'});"
            "const s=await r.json();"
            "document.getElementById('wifiScanState').textContent=s.scanning?'Scanning…':'';"
            "if(s.scanning)return;"
            "if(wifiPoll){clearInterval(wifiPoll);wifiPoll=null;}"
            "wifiScanning=false;"
            "const list=document.getElementById('wifiList');"
            "if(s.networks.length===0){"
              "list.innerHTML='<p class=\"empty\">No networks found.</p>';"
              "return;"
            "}"
            "let h='<table>';"
            "for(const n of s.networks){"
              "h+='<tr class=\"wifiRow\" data-ssid=\"'+esc(n.ssid)+'\"><td>'+esc(n.ssid)"
                "+(n.secure?'':' <span style=\"color:var(--danger)\">(open)</span>')+'</td>'"
                "+'<td class=\"rssi\">'+n.rssi+' dBm</td></tr>';"
            "}"
            "h+='</table>';"
            "list.innerHTML=h;"
            // Reading the SSID back via .dataset (rather than building an
            // onclick=\"...\" string with the SSID spliced in) keeps this
            // safe for SSIDs containing quotes or other characters that
            // would otherwise break out of an inline attribute — a nearby
            // device's advertised SSID isn't something we control.
            "list.querySelectorAll('.wifiRow').forEach(function(row){"
              "row.addEventListener('click',function(){"
                "document.getElementById('wifiSsidField').value=row.dataset.ssid;"
                "list.innerHTML='';"
                "document.getElementById('wifiPasswordField').focus();"
              "});"
            "});"
          "}catch(e){}"
        "}"
        "document.getElementById('wifiSsidField').addEventListener('focus',function(){"
          "if(wifiAutoScanned)return;"
          "wifiAutoScanned=true;"
          "startWifiScan();"
        "});"
        "if(wifiIsApMode){wifiAutoScanned=true;startWifiScan();}"
        // The bridge briefly drops its own WiFi to run this test (see
        // wifiTestTask() in web_server.cpp) — a poll failing mid-test is
        // therefore expected, not an error; keep retrying for a while
        // (~60s, comfortably above the worst case of two back-to-back
        // connect-timeout windows) before giving up. The on-screen
        // countdown starts at 15s to match WIFI_CONNECT_TIMEOUT_MS in
        // config.h — purely cosmetic (the poll loop above is what
        // actually decides when the test is done), so if that constant
        // ever changes this will just count down to 0 a bit early/late.
        "let wifiTestFails=0;"
        "let wifiTestTimer=null;"
        "let wifiTestSecondsLeft=0;"
        "function wifiTestTick(){"
          "const state=document.getElementById('wifiTestState');"
          "if(wifiTestSecondsLeft>0){"
            "state.textContent='Testing your Wi-Fi… ('+wifiTestSecondsLeft+'s)';"
            "wifiTestSecondsLeft--;"
          "}else{"
            "state.textContent='Almost done…';"
          "}"
        "}"
        "function wifiTestStopTimer(){"
          "if(wifiTestTimer){clearInterval(wifiTestTimer);wifiTestTimer=null;}"
        "}"
        // Disabling (rather than just blur()ing) the SSID/password fields
        // for the test's duration is what actually stops iOS Safari from
        // re-showing the keyboard for them on every status-text update —
        // a disabled field can't become focused at all, however the OS
        // reacts to the DOM changing every 500ms. Nothing to type in them
        // while a test is running anyway.
        "function wifiTestSetFieldsEnabled(enabled){"
          "document.getElementById('wifiSsidField').disabled=!enabled;"
          "document.getElementById('wifiPasswordField').disabled=!enabled;"
        "}"
        "async function pollWifiTest(btn){"
          "try{"
            "const r=await fetch('/api/wifi-test',{cache:'no-store'});"
            "const s=await r.json();"
            "wifiTestFails=0;"
            "if(s.inProgress){setTimeout(function(){pollWifiTest(btn);},500);return;}"
            "wifiTestStopTimer();"
            "const state=document.getElementById('wifiTestState');"
            "state.title='';"
            "if(!s.hasResult){"
              "state.textContent='';"
            "}else if(s.ok){"
              "state.textContent='✓ It works! (IP: '+s.ip+') Click \"Save & restart\" below to switch to it.';"
              "state.style.color='var(--success)';"
            "}else{"
              "state.textContent=\"✗ Couldn't connect. Please check the Wi-Fi name and password.\";"
              "state.style.color='var(--danger)';"
              "state.title=s.reason||'';"  // technical detail, only visible on hover
            "}"
            "btn.disabled=false;"
            "wifiTestSetFieldsEnabled(true);"
          "}catch(e){"
            "wifiTestFails++;"
            "if(wifiTestFails>120){"
              "wifiTestStopTimer();"
              "document.getElementById('wifiTestState').textContent="
                "'Lost the connection to the bridge — please check it.';"
              "btn.disabled=false;"
              "wifiTestSetFieldsEnabled(true);"
              "return;"
            "}"
            "setTimeout(function(){pollWifiTest(btn);},500);"
          "}"
        "}"
        "document.getElementById('wifiTestBtn').addEventListener('click',async function(){"
          "const btn=this;"
          // If the SSID/password field is still focused from picking a
          // network or typing a password, iOS Safari tends to keep
          // re-showing the keyboard for it on every DOM update — and the
          // test's status text updates every 500ms while it runs. There's
          // nothing left to type once the test has started, so just drop
          // focus entirely up front.
          "if(document.activeElement&&document.activeElement.blur)document.activeElement.blur();"
          "wifiTestSetFieldsEnabled(false);"
          "btn.disabled=true;"
          "wifiTestFails=0;"
          "const state=document.getElementById('wifiTestState');"
          "state.style.color='';"
          "state.title='';"
          "wifiTestSecondsLeft=15;"
          "wifiTestTick();"
          "wifiTestStopTimer();"
          "wifiTestTimer=setInterval(wifiTestTick,1000);"
          "try{"
            "await fetch('/wifi/test',{"
              "method:'POST',"
              "headers:{'Content-Type':'application/x-www-form-urlencoded'},"
              "body:'wifiSsid='+encodeURIComponent(document.getElementById('wifiSsidField').value)"
                "+'&wifiPassword='+encodeURIComponent(document.getElementById('wifiPasswordField').value)"
            "});"
          "}catch(e){}"
          "pollWifiTest(btn);"
        "});"
        "</script>"
    );

    // Deliberately last on the page and visually set apart (red border/
    // legend via the .danger CSS above) — both actions below have a
    // consequence you can't just undo from this page (Lite Mode cuts off
    // the network until you touch the board; a factory reset wipes
    // everything), so neither belongs next to the routine "Save & restart"
    // action.
    html += F("<fieldset class='danger'><legend>Danger Zone</legend>");

    // Own form/endpoint, deliberately separate from the WiFi/MQTT/Security
    // form above — this isn't a field to bundle into "Save & restart", it's
    // a standalone action with its own irreversible-until-you-touch-the-
    // board consequence (see the confirm() text below). Plain (non-.danger)
    // button, unlike factory reset below — Lite Mode doesn't erase
    // anything, it's fully reversible from the board itself.
    html += F("<p class='hint' style='margin-top:0'>Turns off WiFi entirely — the station "
               "connection and the fallback hotspot both — so the bridge runs as a pure BLE "
               "bridge (bike &harr; watch) with no dashboard, MQTT or network OTA reachable at "
               "all. The status LED blinks twice at the start of every LED cycle while active, "
               "so it's visible even without a watch or bike connected.</p>"
               "<p class='hint'>Once active, this page is unreachable — the only way back is "
               "physical: hold the board's BOOT button for ");
    html += String((unsigned long)(LITE_MODE_EXIT_HOLD_MS / 1000));
    html += F("&ndash;");
    html += String((unsigned long)(RESET_BUTTON_HOLD_MS / 1000));
    html += F("s and release (holding past ");
    html += String((unsigned long)(RESET_BUTTON_HOLD_MS / 1000));
    html += F("s factory-resets instead, same as normal).</p>"
               "<form method='POST' action='/settings/lite-mode' "
               "onsubmit=\"return confirm('Turn on Lite Mode? WiFi (and with it this page, MQTT "
               "and network OTA) will be switched off immediately. The bridge keeps bridging "
               "the bike to your watch either way — the only way back is holding the BOOT "
               "button on the board itself. Continue?');\">"
               "<button type='submit'>Activate Lite Mode</button>"
               "</form>");

    html += F("<p class='hint'>Erases all saved settings — WiFi, MQTT, "
               "pinned bike, wheel size — and restarts the bridge as if it were fresh out of "
               "the box. This cannot be undone; afterwards it starts its own hotspot again, "
               "<b>");
    html += WIFI_AP_SSID;
    html += F("</b>, for initial setup.</p>"
               "<form method='POST' action='/settings/factory-reset' "
               "onsubmit=\"return confirm('Erase ALL settings and reset this bridge to factory "
               "defaults? This cannot be undone.');\">"
               "<button type='submit' class='danger'>Reset to factory settings</button>"
               "</form></fieldset>");

    html += F("</div></body></html>");

    g_httpServer.send(200, "text/html", html);
}

// Small confirmation page shown after anything that reboots the bridge
// (settings save, factory reset, firmware update) — auto-redirects to the
// dashboard after 15s via a plain <meta refresh>, no JS needed, so the
// user doesn't have to remember to reload manually. 15s is a reasonable
// margin for a normal reboot + WiFi reconnect (see WIFI_CONNECT_TIMEOUT_MS
// in config.h); on a slow or failed reconnect the redirect may land
// before the bridge is actually reachable again — the browser then just
// shows its own "can't reach this page", same as today's manual-reload
// flow, so this is a pure improvement with no new failure mode.
static void sendRestartingPage(const char* title, const char* heading, const char* message) {
    String html;
    html.reserve(500);
    html += F("<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'>"
               "<meta name='viewport' content='width=device-width, initial-scale=1'>"
               "<meta http-equiv='refresh' content='15;url=/'>"
               "<title>");
    html += title;
    html += F("</title><style>" THEME_VARS "body{background:var(--bg);color:var(--text);"
               "font-family:-apple-system,sans-serif;text-align:center;padding-top:80px}"
               "</style></head><body><h2>");
    html += heading;
    html += F("</h2><p>");
    html += message;
    html += F("</p></body></html>");
    g_httpServer.send(200, "text/html", html);
}

static void handleSettingsPost() {
    if (!requireAuth()) return;
    if (!requireSameOrigin()) return;

    if (g_httpServer.hasArg("wifiSsid")) {
        copyIntoField(g_settings.wifiSsid, sizeof(g_settings.wifiSsid),
                          g_httpServer.arg("wifiSsid").c_str());
    }
    if (g_httpServer.hasArg("wifiPassword") && g_httpServer.arg("wifiPassword").length() > 0) {
        copyIntoField(g_settings.wifiPassword, sizeof(g_settings.wifiPassword),
                          g_httpServer.arg("wifiPassword").c_str());
    }

    if (g_httpServer.hasArg("wheelCircMm")) {
        long circumference = g_httpServer.arg("wheelCircMm").toInt();
        // Reject 0/negative/nonsense — wheelCircumferenceM is a divisor in
        // updateCsc() (see bike2watch.h), so 0 would mean a division by
        // zero on the very next speed update.
        if (circumference >= 100 && circumference <= 5000) {
            g_settings.wheelCircumferenceMm = (uint16_t)circumference;
        }
    }

    g_settings.mqttEnabled = g_httpServer.hasArg("mqttEnabled");
    if (g_httpServer.hasArg("mqttHost")) {
        copyIntoField(g_settings.mqttHost, sizeof(g_settings.mqttHost),
                          g_httpServer.arg("mqttHost").c_str());
    }
    if (g_httpServer.hasArg("mqttPort")) {
        long port = g_httpServer.arg("mqttPort").toInt();
        if (port > 0 && port <= 65535) g_settings.mqttPort = (uint16_t)port;
    }
    if (g_httpServer.hasArg("mqttUser")) {
        copyIntoField(g_settings.mqttUser, sizeof(g_settings.mqttUser),
                          g_httpServer.arg("mqttUser").c_str());
    }
    if (g_httpServer.hasArg("mqttPassword") && g_httpServer.arg("mqttPassword").length() > 0) {
        copyIntoField(g_settings.mqttPassword, sizeof(g_settings.mqttPassword),
                          g_httpServer.arg("mqttPassword").c_str());
    }
    if (g_httpServer.hasArg("mqttTopicPrefix")) {
        copyIntoField(g_settings.mqttTopicPrefix, sizeof(g_settings.mqttTopicPrefix),
                          g_httpServer.arg("mqttTopicPrefix").c_str());
    }

    if (g_httpServer.hasArg("removeAdminPassword")) {
        // Checked before the field below on purpose: an empty
        // adminPassword field alone means "leave unchanged" (same
        // convention as the WiFi/MQTT passwords), so actually clearing it
        // needs this explicit checkbox rather than just submitting the
        // form with the field left blank.
        g_settings.adminPassword[0] = '\0';
    } else if (g_httpServer.hasArg("adminPassword") && g_httpServer.arg("adminPassword").length() > 0) {
        copyIntoField(g_settings.adminPassword, sizeof(g_settings.adminPassword),
                          g_httpServer.arg("adminPassword").c_str());
    }

    saveSettings(g_settings);
    LOG("Settings: saved via web UI, rebooting...");

    sendRestartingPage("Saved", "Saved.",
        "The bridge is restarting now &hellip; you'll be redirected automatically.");

    delay(1500);
    ESP.restart();
}

// Switches on Lite Mode (see the Lite Mode fieldset in
// handleSettingsGet() and Bike2WatchSettings::liteModeEnabled in settings.h)
// and reboots — after this, WiFi/hotspot/dashboard/MQTT/OTA are all off
// until the BOOT button is used to exit again (see checkResetButton() in
// main.cpp), so this is the last response this page ever gets to send.
// The confirm() dialog on the form is only a client-side speed bump;
// requireAuth()/requireSameOrigin() below are the real guard, same as
// every other state-changing endpoint.
static void handleLiteModePost() {
    if (!requireAuth()) return;
    if (!requireSameOrigin()) return;

    g_settings.liteModeEnabled = true;
    saveSettings(g_settings);
    LOG("Settings: Lite Mode activated via web UI, rebooting...");

    // Spells out the hold duration inline rather than pointing back at the
    // Settings page's own Lite Mode text — this page is the last thing
    // reachable over the network from here on, so "go check the Settings
    // page" wouldn't be actionable once it's actually needed.
    String message = "WiFi is turning off now. To bring it back, hold the board's BOOT button "
                      "for ";
    message += String((unsigned long)(LITE_MODE_EXIT_HOLD_MS / 1000));
    message += "s and release (holding longer factory-resets instead).";
    sendRestartingPage("Lite Mode", "Lite Mode activated.", message.c_str());

    delay(1500);
    ESP.restart();
}

// Wipes every saved setting and reboots — see the Danger Zone fieldset in
// handleSettingsGet(). The confirm() dialog there is only a client-side
// speed bump; requireAuth() below is the real guard, same as every other
// state-changing endpoint.
static void handleFactoryResetPost() {
    if (!requireAuth()) return;
    if (!requireSameOrigin()) return;

    factoryResetSettings();
    LOG("Settings: factory reset via web UI, rebooting...");

    sendRestartingPage("Factory reset", "Factory reset.",
        "All settings were erased. The bridge is restarting now &hellip; "
        "you'll be redirected automatically.");

    delay(1500);
    ESP.restart();
}

// ── Bike (device selection) page ────────────────────────────────────
// Purely opt-in: as long as nobody visits this page or pins a device
// here, the bridge keeps connecting to the first FTMS device a scan
// finds, exactly as if the page didn't exist. Opening it never triggers
// a scan by itself — the list below only shows devices already seen by
// the regular background scan (see include/ble_devices.h).
static void handleDevicesGet() {
    if (!requireAuth()) return;

    bool connected = bleBikeConnected();
    const char* pinnedMac  = getPinnedDeviceMac();
    const char* pinnedName = getPinnedDeviceName();
    bool hasPinned = pinnedMac[0] != '\0';

    String html;
    html.reserve(3400);
    html += F("<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'>"
               "<meta name='viewport' content='width=device-width, initial-scale=1'>"
               "<title>Bike2Watch – Select bike</title><style>");
    html += FPSTR(PAGE_CSS);
    html += F(
        "table{width:100%;border-collapse:collapse;margin-top:4px}"
        "td{padding:10px 6px;border-bottom:1px solid var(--border);font-size:.9rem;vertical-align:middle}"
        "td.mac{color:var(--text-muted);font-size:.75rem}"
        "td.rssi{color:var(--text-muted);font-size:.8rem;text-align:right;white-space:nowrap}"
        "button.small{width:auto;padding:6px 14px;font-size:.85rem}"
        ".pinned{background:var(--success-bg);border:1px solid var(--btn);border-radius:10px;"
        "padding:12px 14px;margin-bottom:16px;font-size:.9rem}"
        ".empty{color:var(--text-dim);font-size:.85rem;padding:10px 0}"
        "#scanState{color:var(--text-muted);font-size:.8rem;text-align:center;margin-top:10px}"
    );
    html += F("</style></head><body>"
               "<div class='sponsor-links'>"
               "<a href='https://buymeacoffee.com/pjanfred' target='_blank' rel='noopener'>☕ Buy me a coffee</a>"
               "</div>"
               "<div class='wrap'>"
               "<h1>Bike2Watch</h1><div class='name'>Select bike</div>"
               "<nav><a href='/'>Dashboard</a>"
               "<a href='/settings'>Settings</a>"
               "<a class='active' href='/devices'>Bike</a>"
               "<a href='/update'>Firmware Update</a></nav>");

    html += F("<p class='hint' style='margin-top:0'>Optional — by default the bridge "
               "connects automatically to the first FTMS bike found (plug &amp; play). Here "
               "you can pin it to one specific bike instead.</p>");

    if (hasPinned) {
        html += F("<div class='pinned'>Pinned to: <b>");
        html += escapeHtml(pinnedName[0] ? pinnedName : "(unnamed)");
        html += F("</b><br><span style='color:var(--text-muted)'>");
        html += escapeHtml(pinnedMac);
        html += F("</span></div><form method='POST' action='/devices/unpin'>"
                   "<button type='submit'>Connect automatically (reset)</button></form>");
    } else {
        html += F("<p class='hint' style='margin-top:0'>Currently: automatic connection — "
                   "no bike pinned.</p>");
    }

    html += F("<fieldset><legend>Discovered bikes</legend>"
               "<div id='deviceList'><p class='empty'>Loading…</p></div>"
               "<form id='scanForm' method='POST' action='/devices/scan' style='margin-top:10px'>"
               "<input type='hidden' name='confirmed' value='1'>");
    if (connected) {
        html += F("<button type='submit' onclick=\"return confirm('Scanning while a connection "
                   "is active can briefly cause dropouts or missed data from your bike. "
                   "Scan anyway?');\">Scan again now</button>");
    } else {
        html += F("<button type='submit'>Scan again now</button>");
    }
    html += F("</form><div id='scanState'></div></fieldset>");

    html += F("<p class='hint'>Shows the most recently seen FTMS devices from the normal "
               "background scan. Opening this page alone doesn't trigger an active scan and "
               "therefore doesn't disturb an ongoing ride.</p>");

    html += F(
        "<script>"
        "function esc(s){return String(s).replace(/[&<>\"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;'}[c]));}"
        "let poll=null;"
        "async function refresh(){"
          "try{"
            "const r=await fetch('/api/devices',{cache:'no-store'});"
            "const s=await r.json();"
            "const list=document.getElementById('deviceList');"
            "if(s.devices.length===0){"
              "list.innerHTML='<p class=\"empty\">No devices seen yet.</p>';"
            "}else{"
              "let h='<table>';"
              "for(const d of s.devices){"
                "const isPinned=d.mac===s.pinnedMac;"
                "h+='<tr><td>'+esc(d.name)+(isPinned?' <b>(pinned)</b>':'')+'</td>'"
                  "+'<td class=\"mac\">'+esc(d.mac)+'</td>'"
                  "+'<td class=\"rssi\">'+d.rssi+' dBm</td>'"
                  "+'<td>'+(isPinned?'':"
                     "'<form method=\"POST\" action=\"/devices/pin\">"
                       "<input type=\"hidden\" name=\"mac\" value=\"'+esc(d.mac)+'\">"
                       "<input type=\"hidden\" name=\"name\" value=\"'+esc(d.name)+'\">"
                       "<button class=\"small\" type=\"submit\">Select</button></form>')"
                  "+'</td></tr>';"
              "}"
              "h+='</table>';"
              "list.innerHTML=h;"
            "}"
            "document.getElementById('scanState').textContent=s.scanning?'Scanning…':'';"
            "if(!s.scanning&&poll){clearInterval(poll);poll=null;}"
          "}catch(e){}"
        "}"
        "refresh();"
        "poll=setInterval(refresh,1500);"
        "</script>"
    );

    html += F("</div></body></html>");

    g_httpServer.send(200, "text/html", html);
}

static void handleDevicesPinPost() {
    if (!requireAuth()) return;
    if (!requireSameOrigin()) return;

    String mac  = g_httpServer.hasArg("mac")  ? g_httpServer.arg("mac")  : String("");
    String name = g_httpServer.hasArg("name") ? g_httpServer.arg("name") : String("");
    if (mac.length() > 0) {
        setPinnedDevice(mac.c_str(), name.c_str());
        LOG("Devices: pinned to %s (%s) via web UI", mac.c_str(), name.c_str());
    }

    g_httpServer.sendHeader("Location", "/devices", true);
    g_httpServer.send(303, "text/plain", "");
}

static void handleDevicesUnpinPost() {
    if (!requireAuth()) return;
    if (!requireSameOrigin()) return;
    clearPinnedDevice();
    LOG("Devices: pin cleared via web UI");

    g_httpServer.sendHeader("Location", "/devices", true);
    g_httpServer.send(303, "text/plain", "");
}

static void handleDevicesScanPost() {
    if (!requireAuth()) return;
    if (!requireSameOrigin()) return;

    if (bleBikeConnected()) {
        // Only ever reaches here with confirmed=1 if the user clicked
        // through the confirm() warning in handleDevicesGet() above (or,
        // without JS, not at all — in which case we simply do nothing
        // rather than risk disturbing the active connection).
        if (g_httpServer.hasArg("confirmed") && g_httpServer.arg("confirmed") == "1") {
            requestDiscoveryScanWhileConnected();
        }
    } else {
        requestImmediateRescan();
    }

    g_httpServer.sendHeader("Location", "/devices", true);
    g_httpServer.send(303, "text/plain", "");
}

// ── Firmware Update page (browser upload, no PlatformIO needed) ────
static void handleUpdateGet() {
    if (!requireAuth()) return;

    String html;
    html.reserve(1800);
    html += FPSTR(UPDATE_HTML_HEAD);
    html += FPSTR(PAGE_CSS);
    html += FPSTR(UPDATE_HTML_BODY_TOP);
    html += FIRMWARE_VERSION;
    html += FPSTR(UPDATE_HTML_BODY_BOTTOM);

    g_httpServer.send(200, "text/html", html);
}

static void handleUpdateUpload() {
    HTTPUpload& upload = g_httpServer.upload();

    if (upload.status == UPLOAD_FILE_START) {
        // Only check credentials/origin here — the 401/403 response itself
        // has to wait for handleUpdateFinish(), since we're mid-multipart-
        // body right now and can't safely write response headers.
        g_uploadAuthorized  = isAuthorized();
        g_uploadSameOrigin  = checkSameOrigin();
        if (!g_uploadAuthorized) {
            LOG("OTA(web): upload rejected (not authorized)");
            return;
        }
        if (!g_uploadSameOrigin) {
            LOG("OTA(web): upload rejected (cross-origin request)");
            return;
        }
        LOG("OTA(web): receiving '%s'", upload.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            Update.printError(Serial);
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (!g_uploadAuthorized || !g_uploadSameOrigin) return;
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            Update.printError(Serial);
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (!g_uploadAuthorized || !g_uploadSameOrigin) return;
        if (Update.end(true)) {
            LOG("OTA(web): update complete (%u bytes)", (unsigned)upload.totalSize);
        } else {
            Update.printError(Serial);
        }
    }
}

static void handleUpdateFinish() {
    if (!g_uploadAuthorized) {
        // Upload was rejected in handleUpdateUpload() without touching the
        // response; this is the first safe point to send the 401 challenge.
        g_httpServer.requestAuthentication();
        return;
    }
    if (!g_uploadSameOrigin) {
        g_httpServer.send(403, "text/plain", "Forbidden (cross-origin request)");
        return;
    }
    g_httpServer.sendHeader("Connection", "close");
    // Restarts either way — even a failed write leaves the bridge
    // rebooting back into the still-intact previous firmware (Update's
    // begin()/write() failures never touch the currently running app
    // partition) — so both cases get the same auto-redirect treatment.
    if (Update.hasError()) {
        sendRestartingPage("Update failed", "Update failed.",
            "Please try again once the bridge is back up. It's restarting now &hellip; "
            "you'll be redirected automatically.");
    } else {
        sendRestartingPage("Update complete", "Update complete.",
            "The bridge is restarting now &hellip; you'll be redirected automatically.");
    }
    delay(500);
    ESP.restart();
}

// Turns a WiFi.status() failure code into something a human can act on —
// used both in the Serial log (connectWifi() falling back to the
// hotspot) and in the WiFi test's result (see handleApiWifiTest()). The
// three reasons below cover the vast majority of real-world cases:
//  - WL_NO_SSID_AVAIL: the ESP32 never saw a beacon for that SSID at all.
//    Most common cause in practice: a router broadcasting the same SSID
//    on 2.4GHz *and* 5GHz (e.g. Fritz!Box, many mesh systems) with
//    band-steering — the ESP32 only has a 2.4GHz radio, so if steering
//    hides the 2.4GHz beacon this fails even with a correct SSID/password.
//  - WL_CONNECT_FAILED: the AP was found but rejected the association —
//    almost always a wrong password.
//  - anything else (WL_DISCONNECTED/WL_IDLE_STATUS/...): generic timeout,
//    e.g. weak signal or slow DHCP.
static const char* wifiFailureReason(wl_status_t status) {
    switch (status) {
        case WL_NO_SSID_AVAIL:  return "SSID not found (out of range, typo, "
                                        "or only visible on 5GHz -- the ESP32 only supports 2.4GHz)";
        case WL_CONNECT_FAILED: return "Connection rejected (wrong password?)";
        default:                return "Timeout (weak signal or DHCP too slow?)";
    }
}

// Starts the bridge's own fallback hotspot — used both when the
// configured network can't be joined at boot (see connectWifi() below)
// and if wifiTestTask() further down can't even revert to the
// previously-working network after a test. Always leaves WiFi in a
// known-reachable state (its own AP up) instead of stuck disconnected.
static void startFallbackHotspot() {
    WiFi.mode(WIFI_AP);
    if (strlen(WIFI_AP_PASSWORD) > 0) {
        WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASSWORD);
    } else {
        WiFi.softAP(WIFI_AP_SSID);
    }
    strncpy(g_wifiModeBuf, "Hotspot", sizeof(g_wifiModeBuf) - 1);
    strncpy(g_ipBuf, WiFi.softAPIP().toString().c_str(), sizeof(g_ipBuf) - 1);
    LOG("WiFi: hotspot '%s' up, IP=%s -- open http://%s/settings to configure your WiFi permanently",
        WIFI_AP_SSID, g_ipBuf, g_ipBuf);

    // Wildcard DNS + the catch-all redirect in handleNotFound() turn the
    // line above into something that (usually) just happens on its own —
    // see the comment on g_dnsServer near the top of this file.
    g_dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
    g_captivePortalActive = true;
    LOG("Captive portal: active (wildcard DNS -> %s)", g_ipBuf);
}

// ── Settings-page WiFi connection test ──────────────────────────────
// Unlike the MQTT test (a second TCP connection layered on top of the
// already-working WiFi link), this can't avoid touching the live
// connection: the ESP32 has only one WiFi radio, so trying a different
// network means disconnecting from whatever's active first. wifiTestTask()
// always reverts to that previous state afterwards, regardless of whether
// the test succeeded, so this only ever *previews* a result — actually
// switching to the new network permanently is still Save & Restart's job.
// If the bridge was only running its own hotspot (first boot, no WiFi
// configured yet), the test instead adds STA alongside the AP
// (WIFI_AP_STA, the same trick handleWifiScanPost() uses for scanning),
// so the hotspot — and whatever's connected to it, e.g. the browser
// running this very test — is never dropped. Otherwise (already on a
// real network) there's no way around a brief, self-healing disconnect
// for the whole network, not just this browser tab, while the test runs.
struct WifiTestResult {
    bool        inProgress = false;
    bool        hasResult  = false;
    bool        ok         = false;
    char        ip[16]     = "";
    wl_status_t status     = WL_IDLE_STATUS;
};
static SemaphoreHandle_t g_wifiTestMutex = nullptr;
static WifiTestResult    g_wifiTestResultState;
static char              g_wifiTestSsid[33]     = "";
static char              g_wifiTestPassword[65] = "";

static void wifiTestTask(void* pvParameters) {
    // Wait out any WiFi scan already in progress (e.g. one just started
    // by focusing the SSID field) instead of racing it — disconnecting
    // and reconnecting mid-scan was observed to leave the radio confused
    // and the test never completing. Scans normally finish within a few
    // seconds; give up waiting (and proceed anyway) after 10s rather than
    // risk hanging forever on a scan that never completes.
    unsigned long scanWaitStart = millis();
    while (WiFi.scanComplete() == WIFI_SCAN_RUNNING && millis() - scanWaitStart < 10000) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    bool wasApOnly = (WiFi.getMode() == WIFI_AP);
    char prevSsid[33];
    char prevPassword[65];
    copyIntoField(prevSsid, sizeof(prevSsid), g_settings.wifiSsid);
    copyIntoField(prevPassword, sizeof(prevPassword), g_settings.wifiPassword);

    if (wasApOnly) {
        WiFi.mode(WIFI_AP_STA);
    }
    WiFi.disconnect();
    delay(100);
    WiFi.begin(g_wifiTestSsid, g_wifiTestPassword);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(250));
    }

    bool        ok     = (WiFi.status() == WL_CONNECTED);
    wl_status_t status = WiFi.status();
    char        ip[16] = "";
    if (ok) {
        copyIntoField(ip, sizeof(ip), WiFi.localIP().toString().c_str());
        LOG("WiFi: test connect to '%s' -> ok, IP=%s", g_wifiTestSsid, ip);
    } else {
        LOG("WiFi: test connect to '%s' -> failed (%s)", g_wifiTestSsid, wifiFailureReason(status));
    }

    // Revert regardless of the outcome above, so even a *successful*
    // test never leaves the bridge stuck on the new network — that's
    // still Save & Restart's job.
    WiFi.disconnect();
    delay(100);
    if (wasApOnly) {
        WiFi.mode(WIFI_AP);
    } else if (strlen(prevSsid) > 0) {
        WiFi.begin(prevSsid, prevPassword);
        unsigned long revertStart = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - revertStart < WIFI_CONNECT_TIMEOUT_MS) {
            vTaskDelay(pdMS_TO_TICKS(250));
        }
        if (WiFi.status() != WL_CONNECTED) {
            // Reverting to the previously-working network failed too —
            // rare, e.g. a router hiccup during the test window. Rather
            // than leave the bridge disconnected with no way to reach
            // it, fall back to the hotspot exactly as connectWifi()
            // would have.
            LOG("WiFi: reverting to '%s' after test failed too (%s), starting fallback hotspot",
                prevSsid, wifiFailureReason(WiFi.status()));
            startFallbackHotspot();
        } else {
            strncpy(g_wifiModeBuf, "WiFi", sizeof(g_wifiModeBuf) - 1);
            strncpy(g_ipBuf, WiFi.localIP().toString().c_str(), sizeof(g_ipBuf) - 1);
        }
    }

    if (xSemaphoreTake(g_wifiTestMutex, portMAX_DELAY) == pdTRUE) {
        g_wifiTestResultState.inProgress = false;
        g_wifiTestResultState.hasResult  = true;
        g_wifiTestResultState.ok         = ok;
        g_wifiTestResultState.status     = status;
        copyIntoField(g_wifiTestResultState.ip, sizeof(g_wifiTestResultState.ip), ip);
        xSemaphoreGive(g_wifiTestMutex);
    }
    vTaskDelete(nullptr);
}

static void requestWifiTest(const char* ssid, const char* password) {
    if (!g_wifiTestMutex) g_wifiTestMutex = xSemaphoreCreateMutex();

    bool alreadyRunning = true;
    if (xSemaphoreTake(g_wifiTestMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        alreadyRunning = g_wifiTestResultState.inProgress;
        if (!alreadyRunning) g_wifiTestResultState.inProgress = true;
        xSemaphoreGive(g_wifiTestMutex);
    }
    if (alreadyRunning) return;

    copyIntoField(g_wifiTestSsid, sizeof(g_wifiTestSsid), ssid ? ssid : "");
    copyIntoField(g_wifiTestPassword, sizeof(g_wifiTestPassword), password ? password : "");

    xTaskCreatePinnedToCore(wifiTestTask, "wifiTest", 4096, nullptr, 1, nullptr, 0);
}

static WifiTestResult pollWifiTest() {
    WifiTestResult out;
    if (g_wifiTestMutex && xSemaphoreTake(g_wifiTestMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        out = g_wifiTestResultState;
        xSemaphoreGive(g_wifiTestMutex);
    }
    return out;
}

// See the forward declaration above handleWifiScanPost().
static bool wifiTestInProgress() {
    bool running = false;
    if (g_wifiTestMutex && xSemaphoreTake(g_wifiTestMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        running = g_wifiTestResultState.inProgress;
        xSemaphoreGive(g_wifiTestMutex);
    }
    return running;
}

// Kicks off a bounded WiFi connection test against whatever's currently
// typed into the Settings form's WiFi fields — not necessarily what's
// saved yet. An empty password field means "use the already-saved
// password", same convention as handleSettingsPost() below.
static void handleWifiTestPost() {
    if (!requireAuth()) return;
    if (!requireSameOrigin()) return;

    String ssid = g_httpServer.hasArg("wifiSsid") ? g_httpServer.arg("wifiSsid") : String("");
    if (ssid.length() == 0) {
        g_httpServer.send(400, "application/json", "{\"ok\":false,\"error\":\"missing ssid\"}");
        return;
    }
    String password = g_httpServer.hasArg("wifiPassword") ? g_httpServer.arg("wifiPassword") : String("");
    if (password.length() == 0) password = g_settings.wifiPassword;

    requestWifiTest(ssid.c_str(), password.c_str());
    g_httpServer.send(200, "application/json", "{\"started\":true}");
}

// JSON feed for the Settings page's WiFi-test poll loop — see
// handleWifiTestPost() and pollWifiTest() above. Protected the same as
// every other state-changing/status endpoint here.
static void handleApiWifiTest() {
    if (!requireAuth()) return;

    WifiTestResult r = pollWifiTest();
    String json = "{\"inProgress\":";
    json += r.inProgress ? "true" : "false";
    json += ",\"hasResult\":";
    json += r.hasResult ? "true" : "false";
    json += ",\"ok\":";
    json += r.ok ? "true" : "false";
    json += ",\"ip\":\"";
    json += jsonEscape(r.ip);
    json += "\",\"reason\":\"";
    json += jsonEscape(r.ok ? "" : wifiFailureReason(r.status));
    json += "\"}";
    g_httpServer.send(200, "application/json", json);
}

// ── WiFi ────────────────────────────────────────────────────────
static void connectWifi() {
    g_captivePortalActive = false;  // only ever turned on in the AP branch below

    if (strlen(g_settings.wifiSsid) == 0) {
        LOG("WiFi: no WiFi configured yet, starting hotspot");
    } else {
        WiFi.mode(WIFI_STA);
        // Belt-and-suspenders against a known ESP32-Arduino quirk: right
        // after WiFi.mode() switches the radio on, the very first
        // WiFi.begin() can silently fail (times out, no real error) even
        // with correct credentials, as if the STA interface wasn't fully
        // up yet. A clean disconnect+retry almost always succeeds where
        // the first attempt didn't, so this always gets one retry before
        // giving up and falling back to the hotspot.
        for (int attempt = 1; attempt <= 2; attempt++) {
            LOG("WiFi: connecting to '%s'... (attempt %d/2)", g_settings.wifiSsid, attempt);
            WiFi.disconnect();
            delay(100);
            WiFi.begin(g_settings.wifiSsid, g_settings.wifiPassword);

            unsigned long start = millis();
            while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
                delay(250);
            }
            if (WiFi.status() == WL_CONNECTED) break;
            if (attempt == 1) {
                LOG("WiFi: attempt 1 failed (%s), retrying once...",
                    wifiFailureReason(WiFi.status()));
            }
        }
    }

    if (WiFi.status() == WL_CONNECTED) {
        strncpy(g_wifiModeBuf, "WiFi", sizeof(g_wifiModeBuf) - 1);
        strncpy(g_ipBuf, WiFi.localIP().toString().c_str(), sizeof(g_ipBuf) - 1);
        LOG("WiFi: connected, IP=%s", g_ipBuf);
    } else {
        LOG("WiFi: joining configured network failed after 2 attempts (%s), starting fallback "
            "hotspot '%s'", wifiFailureReason(WiFi.status()), WIFI_AP_SSID);
        startFallbackHotspot();
    }

    if (MDNS.begin(WEB_HOSTNAME)) {
        MDNS.addService("http", "tcp", 80);
        LOG("mDNS: dashboard at http://%s.local", WEB_HOSTNAME);
    } else {
        LOG("mDNS: failed to start (dashboard still reachable by IP)");
    }
}

void setupWebServer(const Bike2WatchSettings& settings) {
    g_settings = settings;
    g_status.deviceName = DEVICE_NAME;
    connectWifi();

    // Origin/Referer aren't collected by default (see requireSameOrigin()
    // above) — opt in so header()/hasHeader() can actually see them.
    const char* csrfHeaders[] = {"Origin", "Referer"};
    g_httpServer.collectHeaders(csrfHeaders, 2);

    g_httpServer.on("/", handleRoot);
    g_httpServer.on("/api/status", handleStatus);
    g_httpServer.on("/resistance", HTTP_POST, handleResistancePost);
    g_httpServer.on("/settings", HTTP_GET, handleSettingsGet);
    g_httpServer.on("/settings", HTTP_POST, handleSettingsPost);
    g_httpServer.on("/settings/lite-mode", HTTP_POST, handleLiteModePost);
    g_httpServer.on("/settings/factory-reset", HTTP_POST, handleFactoryResetPost);
    g_httpServer.on("/wifi/scan", HTTP_POST, handleWifiScanPost);
    g_httpServer.on("/wifi/test", HTTP_POST, handleWifiTestPost);
    g_httpServer.on("/api/wifi-test", HTTP_GET, handleApiWifiTest);
    g_httpServer.on("/api/wifi-networks", HTTP_GET, handleApiWifiNetworks);
    g_httpServer.on("/devices", HTTP_GET, handleDevicesGet);
    g_httpServer.on("/devices/pin", HTTP_POST, handleDevicesPinPost);
    g_httpServer.on("/devices/unpin", HTTP_POST, handleDevicesUnpinPost);
    g_httpServer.on("/devices/scan", HTTP_POST, handleDevicesScanPost);
    g_httpServer.on("/api/devices", HTTP_GET, handleApiDevices);
    g_httpServer.on("/update", HTTP_GET, handleUpdateGet);
    g_httpServer.on("/update", HTTP_POST, handleUpdateFinish, handleUpdateUpload);
    g_httpServer.onNotFound(handleNotFound);
    g_httpServer.begin();
    LOG("Web: dashboard server started on port 80");
}

void handleWebServerClients() {
    if (g_captivePortalActive) {
        g_dnsServer.processNextRequest();
    }
    g_httpServer.handleClient();
}

void updateWebStatus(const Bike2WatchStatus& status) {
    g_status = status;
    g_status.deviceName = DEVICE_NAME;
    g_status.wifiMode   = g_wifiModeBuf;
    g_status.ipAddress  = g_ipBuf;
}
