#include "WiFiManager.h"
#include "WiFiLock.h"
#include "Config.h"
#include "Calendar.h"
#include "StringTools.h"

#include <ESPmDNS.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "WebUI.h" // Include WebUI.h to access WebUI::sendJson
#include "GeolocationService.h"
#include "DiagnosticLog.h"
#include "LogLevel.h"
#include <cstring>
#include <atomic>
#include <algorithm>
#include <vector>
#include <esp_wifi.h>

// === TUNABLES ===  (rule: see Config.h header)
static const unsigned long WIFI_CONNECT_TIMEOUT_MS = 10000; // Per STA attempt, before retrying
static const int           MAX_STA_RETRIES = 2;             // Allow 3 total attempts (0, 1, 2), then AP portal
static const unsigned long WIFI_LINK_TO_IP_MS = 8000;      // after a link comes up, this long for the IP before retrying

// Switching to a stronger access point while connected (mesh / extender) -
// see serviceRoaming(). Connecting always picks the strongest one already
// (connectSTA()); this covers the signal fading later, e.g. a node switched off.
static const int           ROAM_WEAK_RSSI_DBM   = -78;                   // weaker than this counts as weak
static const unsigned long ROAM_WEAK_FOR_MS     = 30UL * 1000UL;          // weak this long before a scan
static const unsigned long ROAM_MIN_INTERVAL_MS = 10UL * 60UL * 1000UL;   // at most one scan per
static const int           ROAM_MIN_GAIN_DB     = 8;                      // switch only for at least this much better
static const unsigned long ROAM_SAMPLE_MS       = 5000;
// Not lower: the core gives an async scan 20 x this in total before calling
// it failed (WiFiScan.cpp), and while connected - with the clock's own AP up
// too - the radio keeps returning to its home channel between channels. At
// 120 ms every scan "failed" (2.4 s deadline); 300 ms = 6 s (2026-10-17).
static const uint32_t      ROAM_SCAN_MS_PER_CH  = 300;
// Several known networks, and the last connected one failed: the quick scan
// that picks the strongest other one (servicePickScan()). 100 ms per channel,
// ~1.3 s for all 13 - a network not heard that fast is too weak to matter.
// After PICK_SCAN_GIVE_UP_MS without a result, join in the saved order.
static const uint32_t      PICK_SCAN_MS_PER_CH  = 100;
static const unsigned long PICK_SCAN_GIVE_UP_MS = 6000;
// In setup mode with known networks (none could be joined): try them again this often.
static const unsigned long PORTAL_RETRY_MS = 2UL * 60UL * 1000UL;
// The settings page's list of networks around (serviceListScan()): per
// channel like ROAM_SCAN_MS_PER_CH (connected, AP up - same reason), a list
// this young is handed out again instead of scanning anew, and a scan with no
// result by LIST_SCAN_GIVE_UP_MS counts as failed (an empty list).
static const uint32_t      LIST_SCAN_MS_PER_CH  = 300;
static const unsigned long LIST_FRESH_MS        = 30UL * 1000UL;
static const unsigned long LIST_SCAN_GIVE_UP_MS = 15000;

// ─── State Machine Globals ──────────────────────────────────────────────────

static WiFiState       g_state = WiFiState::IDLE;
// Written by the Wi-Fi event handler, read by loop(): atomic, so the handler
// needs no lock - see WiFiEventHandler().
static std::atomic<uint8_t> g_lastDisconnectReason{0};
static std::atomic<bool>    g_disconnectEventTriggered{false};
static std::atomic<bool>    g_gotIpEventTriggered{false};
static std::atomic<uint32_t> g_lastLinkUpMs{0};   // millis() of the last "link established" (0 = none)
static unsigned long   g_stateTimer = 0;
static unsigned long   g_lastConnectAttemptMs = 0;
static unsigned long   g_restartAt = 0;
static int             g_retryCount = 0;

static unsigned long   g_roamWeakSinceMs = 0;
static unsigned long   g_roamLastScanMs = 0;
static unsigned long   g_roamLastSampleMs = 0;
static bool            g_roamScanRunning = false;
static unsigned long   g_roamScanStartedMs = 0;
static const unsigned long ROAM_SCAN_GIVE_UP_MS = 15000;   // a scan with no result by then counts as failed

// Which known network (0..WIFI_NETWORKS-1) is being joined / is joined; -1 =
// none yet. Which one to join: see connectSTA().
static int             g_joinSlot = -1;
static int             g_nextSlot = -1;            // join this one next: "Connect" on the page, or roaming's pick
static std::atomic<int> g_connectRequest{-1};      // "Connect" pressed (web server task) - loop() takes it
static bool            g_pickScanRunning = false;
static unsigned long   g_pickScanStartedMs = 0;

// The settings page's list (heardNetworks() / serviceListScan()). The request
// comes from the web server task; the rest is under WiFiLock.
static std::atomic<bool> g_listRequest{false};
static bool            g_listScanRunning = false;
static unsigned long   g_listScanStartedMs = 0;
static std::vector<WiFiManager::HeardNetwork> g_heard;
static unsigned long   g_heardAtMs = 0;            // when g_heard was made (0 = never)

// 2026-10-08: tried deferring the very first WiFi.begin() by 1.5s, on the
// theory that reason=202 (AUTH_FAIL) - a well-documented ESP32 Arduino core
// quirk, not a bug here - might be a radio-settling timing issue. 5-boot
// sample: 3 clean, 2 still hit it (one twice). Inconclusive - didn't
// reliably fix it, so reverted. The existing retry logic (below) already
// recovers in one cycle regardless; this was purely about avoiding the
// extra logged cycle, not a functional problem.
// 2026-10-17: after switching to a full-channel scan that joins the STRONGEST
// access point (connectSTA()), three test boots in a row showed no 202 at
// all - it was most likely the far mesh node refusing the first attempt.

// ── Read-only diagnostic counters (networking-reliability investigation) ────
static std::atomic<uint32_t> g_diagDisconnectCount{0};   // written by the event handler, like the flags above
static uint32_t g_diagReconnectCount      = 0;
static uint32_t g_diagApRebuildCount      = 0;
static uint32_t g_diagLastReconnectChainMs = 0;

// ─── WiFi Event Handler ─────────────────────────────────────────────────────

void WiFiManager::WiFiEventHandler(WiFiEvent_t event, WiFiEventInfo_t info) {
    // No WiFiLock here (2026-10-18, core 3.x): loop() holds it while calling
    // WiFi.mode(), which in core 3.x waits until the Wi-Fi start event has
    // been through every handler - including this one. Taking the lock here
    // deadlocked the first connect: both CPUs idle until the 20 s watchdog
    // rebooted the clock. Everything written here is atomic instead.
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            {
                uint8_t reason = info.wifi_sta_disconnected.reason;
                // Log only the basic reason to keep serial clean
                CLOCK_LOG("WiFi", "[EVENT] Disconnected (Reason: %d)", reason);
                
                g_lastDisconnectReason = reason;
                g_disconnectEventTriggered = true;
                g_diagDisconnectCount++;
                DiagnosticLog::record("WIFI", "disconnected reason=%d", reason);
            }
            break;
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            CLOCK_LOG("WiFi", "[EVENT] Got IP: %s", IPAddress(info.got_ip.ip_info.ip.addr).toString().c_str());
            g_gotIpEventTriggered = true;
            break;
        case ARDUINO_EVENT_WIFI_STA_CONNECTED:
            g_lastLinkUpMs = millis() | 1;   // never 0
            {
                // Link facts for diagnosing slow connects (2026-10-18, core 3.x):
                // power save should be off (setSleep(false)), transmit power max.
                wifi_ps_type_t ps = WIFI_PS_NONE;
                esp_wifi_get_ps(&ps);
                const uint8_t* b = info.wifi_sta_connected.bssid;
                CLOCK_LOG("WiFi", "Event: Physical Link Established (ap=%02X:%02X ch=%d rssi=%d ps=%d tx=%d/4 dBm)",
                          b[4], b[5], (int)info.wifi_sta_connected.channel, (int)WiFi.RSSI(), (int)ps, (int)WiFi.getTxPower());
            }
            break;
        default: break;
    }
}

// ─── Helpers ──────────────────────────────────────────────────────────────────

static const char* getWlStatusName(wl_status_t status) {
    switch (status) {
        case WL_IDLE_STATUS:     return "IDLE (0)";
        case WL_NO_SSID_AVAIL:   return "NO_SSID_AVAIL (1)";
        case WL_SCAN_COMPLETED:  return "SCAN_COMPLETED (2)";
        case WL_CONNECTED:       return "CONNECTED (3)";
        case WL_CONNECT_FAILED:  return "CONNECT_FAILED (4)";
        case WL_CONNECTION_LOST: return "CONNECTION_LOST (5)";
        case WL_DISCONNECTED:    return "DISCONNECTED (6)";
        case WL_NO_SHIELD:       return "NO_SHIELD (255)";
        default:                 return "UNKNOWN (?)";
    }
}

static String makeMdnsHost(const String& projectName) {
    return StringTools::makeMdnsHostname(projectName);
}

static DNSServer       dnsServer;
static AsyncWebServer* server = nullptr;
static bool            g_apActive = false;
// The AP that's up is the synchronized one (startAP()), not the setup one.
// Atomic: the web server's filters read it (ON_STA_OR_SYNCED_AP_FILTER).
static std::atomic<bool> g_syncedApUp{false};
// Last loop() pass that saw a working connection (offlineForMs()).
static uint32_t        g_lastOnlineMs = 0;
static AppConfig*      g_cfgPtr = nullptr;
static String          g_mdnsHost = makeMdnsHost(PROJECT_NAME_DEFAULT);  // Hostname without .local suffix
static const char* portal_html = R"rawliteral(
<!DOCTYPE HTML><html>
<head><title>Jewish Clock Setup</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
  body { font-family: sans-serif; text-align: center; padding: 16px; }
  select, input { width: 90%; padding: 10px; margin: 8px 0; border-radius: 5px; border: 1px solid #ccc; }
  button { padding: 10px 18px; margin: 8px 4px; border-radius: 5px; border: none; background: #007bff; color: white; cursor: pointer; }
  .row { display: flex; justify-content: center; align-items: center; }
  .small { width: 10%; }
</style>
</head>
<body>
  <h2><<PROJECT_NAME>> Setup</h2>
  <p>Select your network from the list; password is prefilled if remembered.</p>
  <label for="ssidList">Available SSIDs</label><br>
  <select id="ssidList" size="6"></select><br>

  <form id="wifiForm" action="/save" method="POST">
    <input id="ssidInput" name="ssid" type="text" placeholder="SSID" required><br>
    <div class="row">
      <input id="passwordInput" name="password" type="password" placeholder="Password" required>
      <button id="togglePassword" type="button" class="small">Show</button>
    </div>
    <button type="submit">Connect</button>
  </form>
  <p style="margin-top:24px"><a href="/restore">Restore from backup</a></p>

  <script>
    const ssidList = document.getElementById('ssidList');
    const ssidInput = document.getElementById('ssidInput');
    const passwordInput = document.getElementById('passwordInput');
    const togglePassword = document.getElementById('togglePassword');

    function savePasswordForSSID(ssid, password) {
      if (!ssid) return;
      localStorage.setItem('wifiPwd_' + ssid, password || '');
    }

    function getSavedPassword(ssid) {
      return localStorage.getItem('wifiPwd_' + ssid) || '';
    }

    function setScanResults(networks) {
      ssidList.innerHTML = '';
      networks.sort((a,b)=>b.rssi-a.rssi);
      networks.forEach(n => {
        const opt = document.createElement('option');
        opt.value = n.ssid;
        opt.textContent = `${n.ssid} (${n.rssi} dBm${n.secure?' locked':''})`;
        ssidList.append(opt);
      });
      if (networks.length > 0 && !ssidInput.value) {
        ssidList.selectedIndex = 0;
        selectSsidFromDropdown();
      }
    }

    function selectSsidFromDropdown() {
      const ssid = ssidList.value;
      if (!ssid) return;
      ssidInput.value = ssid;
      const pwd = getSavedPassword(ssid);
      if (pwd) passwordInput.value = pwd;
    }

    // The first answer is usually "scanning" with no networks (the clock
    // starts a scan then): ask again until the list is there - asked once,
    // the list stayed empty until a reload (found 2026-11-07, sim/ui_tests.py).
    async function scanNetworks() {
      for (let i = 0; i < 20; i++) {
        try {
          const resp = await fetch('/wifi-scan');
          if (!resp.ok) throw new Error('scan failed');
          const data = await resp.json();
          if (data.status !== 'scanning') { setScanResults(data.networks || []); return; }
        } catch (e) {
          console.log('Scan error', e);
        }
        await new Promise(r => setTimeout(r, 1500));
      }
    }

    ssidList.addEventListener('change', selectSsidFromDropdown);

    togglePassword.addEventListener('click', () => {
      passwordInput.type = passwordInput.type === 'password' ? 'text' : 'password';
      togglePassword.textContent = passwordInput.type === 'password' ? 'Show' : 'Hide';
    });

    document.getElementById('wifiForm').addEventListener('submit', () => {
      savePasswordForSSID(ssidInput.value, passwordInput.value);
    });

    scanNetworks();
  </script>
</body></html>)rawliteral";

bool WiFiManager::hasValidCredentials(const AppConfig& cfg) {
    for (int i = 0; i < WIFI_NETWORKS; i++) if (wifiSsidAt(cfg, i)[0]) return true;
    return false;
}

static int knownNetworkCount(const AppConfig& cfg) {
    int n = 0;
    for (int i = 0; i < WIFI_NETWORKS; i++) if (wifiSsidAt(cfg, i)[0]) n++;
    return n;
}

static bool isKnownSsid(const AppConfig& cfg, const String& ssid) {
    for (int i = 0; i < WIFI_NETWORKS; i++) if (wifiSsidAt(cfg, i)[0] && ssid == wifiSsidAt(cfg, i)) return true;
    return false;
}

// Join known network `slot`. The driver scans all channels for that name and
// takes its strongest access point (see connectSTA()).
static void beginSlot(AppConfig& cfg, int slot) {
    g_joinSlot = slot;
    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
    WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
    WiFi.begin(wifiSsidAt(cfg, slot), wifiPasswordAt(cfg, slot));
    g_lastConnectAttemptMs = millis();   // the connect timeout starts now, not at the scan
}

// The known networks in the order to try them: those in the `n` scan results,
// strongest first, then those not heard (out of range, or a hidden name).
// `heard` gets a short summary for the log.
static std::vector<int> rankKnownNetworks(const AppConfig& cfg, int n, String& heard) {
    struct Candidate { int slot; int rssi; };
    std::vector<Candidate> c;
    for (int s = 0; s < WIFI_NETWORKS; s++) {
        if (!wifiSsidAt(cfg, s)[0]) continue;
        int best = -1000;
        for (int k = 0; k < n; k++)
            if (WiFi.SSID(k) == wifiSsidAt(cfg, s) && WiFi.RSSI(k) > best) best = WiFi.RSSI(k);
        c.push_back({s, best});
        heard += String(wifiSsidAt(cfg, s)) + "=" + (best > -1000 ? String(best) : String("not heard")) + " ";
    }
    std::stable_sort(c.begin(), c.end(), [](const Candidate& a, const Candidate& b) { return a.rssi > b.rssi; });
    std::vector<int> order;
    for (const auto& x : c) order.push_back(x.slot);
    return order;
}

// The scan connectSTA() started is done (or taking too long): join the
// strongest known network - but not the one that just failed, if there is
// another (e.g. a network whose password changed must not block the rest).
void WiFiManager::servicePickScan() {
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING && millis() - g_pickScanStartedMs < PICK_SCAN_GIVE_UP_MS) return;
    if (n == WIFI_SCAN_RUNNING) esp_wifi_scan_stop();
    g_pickScanRunning = false;
    String heard;
    std::vector<int> order = rankKnownNetworks(*g_cfgPtr, n > 0 ? n : 0, heard);
    WiFi.scanDelete();
    if (order.empty()) return;
    int slot = order[0];
    if (g_retryCount > 0 && order.size() > 1 && slot == g_joinSlot) slot = order[1];
    DiagnosticLog::record("WIFI", "joining \"%s\" (attempt %d; scan saw %d networks; heard %s)", wifiSsidAt(*g_cfgPtr, slot),
                          g_retryCount + 1, n, heard.c_str());
    beginSlot(*g_cfgPtr, slot);
}

void WiFiManager::requestConnect(int slot) { g_connectRequest = slot; }

int WiFiManager::connectedSlot() {
    WiFiLock::Guard guard;
    return (g_state == WiFiState::STA_CONNECTED && WiFi.status() == WL_CONNECTED) ? g_joinSlot : -1;
}

String WiFiManager::joiningSsid() {
    WiFiLock::Guard guard;
    if (g_pickScanRunning || g_joinSlot < 0 || !g_cfgPtr) return "";
    return wifiSsidAt(*g_cfgPtr, g_joinSlot);
}

/**
 * Central State Controller
 * Handles all side-effects when switching modes
 */
void WiFiManager::setState(WiFiState newState) {
    // setState() deliberately doesn't take WiFiLock itself (see WiFiLock.h) -
    // every caller is expected to already be inside a Guard. Verify that
    // instead of just trusting it, so a future call site added outside a
    // locked block doesn't silently reintroduce the race this lock exists
    // to fix - loudly, not by crashing a device nobody's watching live.
    if (!WiFiLock::heldByCurrentTask()) {
        LOG_AT(LogLevel::ERROR, "WiFi", "setState(%d) called without WiFiLock held - state is unprotected here", (int)newState);
    }

    if (newState == g_state && newState != WiFiState::STA_CONNECTING) return;
    
    WiFiState oldState = g_state;
    g_state = newState;
    CLOCK_LOG("WiFi", "Transitioning state: %d -> %d", (int)oldState, (int)newState);
    g_stateTimer = millis();

    switch (newState) {
        case WiFiState::STA_CONNECTING:
            // NOTE: this used to call stopAP() here whenever coming from
            // STA_CONNECTED ("to ensure a clean, STA-only reconnection
            // attempt"), tearing the synchronized AP down on every single
            // reconnect - including a brief blip, not just a genuine outage -
            // and then paying the full startAP()+WebUI::startServer() cost
            // again once STA came back. ESP32 supports attempting an STA
            // reconnect while staying in WIFI_AP_STA the whole time (the
            // interfaces are independent netifs), so the AP is left running
            // through the reconnect attempt below instead. Bench-tested via
            // the temporary /api/debug/force-disconnect endpoint on
            // 2026-10-01: confirmed the AP survives (apRebuilds does not
            // increment) and STA still reconnects normally. If this is ever
            // reverted, connectSTA()'s WiFi.mode(WIFI_STA) call below is the
            // other half of the old "drop to pure STA to reconnect" pattern.
            g_lastConnectAttemptMs = millis();
            CLOCK_LOG("WiFi", "State: STA_CONNECTING (ms: %lu)", millis());
            connectSTA(*g_cfgPtr);
            break;

        case WiFiState::STA_CONNECTED:
            g_retryCount = 0; // Reset counter on successful connection
            g_diagReconnectCount++;
            // The network to try first next time (connectSTA()). Saved only
            // when it changes - normally never, so no flash writes per reconnect.
            if (g_joinSlot >= 0 && g_cfgPtr->wifiLastNetwork != g_joinSlot) {
                g_cfgPtr->wifiLastNetwork = (int8_t)g_joinSlot;
                Storage::saveConfigSoon();
            }
            CLOCK_LOG("WiFi", "State: STA_CONNECTED (IP: %s)", WiFi.localIP().toString().c_str());
            // Which access point (BSSID) and channel: tells mesh nodes apart.
            DiagnosticLog::record("WIFI", "reconnected #%u rssi=%d ap=%s ch=%d ip=%s", g_diagReconnectCount, WiFi.RSSI(),
                                  WiFi.BSSIDstr().c_str(), (int)WiFi.channel(), WiFi.localIP().toString().c_str());
            g_mdnsHost = makeMdnsHost(g_cfgPtr->projectName);
            {
                // Set the hostname *after* getting an IP address. This is more reliable.
                String validHostname = StringTools::makeMdnsHostname(g_cfgPtr->projectName);
                validHostname.toLowerCase();
                if (validHostname.length() > 32) {
                    validHostname = validHostname.substring(0, 32);
                }
                WiFi.setHostname(validHostname.c_str());
            }
            // Geolocation is intentionally NOT synced here. It used to run on
            // every single STA (re)connection - including a transient signal
            // blip and recovery, not just first boot - which stacked an
            // ungated, up-to-5-second blocking HTTP call (plus a flash write
            // on success) onto the hot reconnect path. Investigation on
            // 2026-09-30 traced repeated, expensive reconnect-triggered work
            // like this to a device becoming progressively slower and
            // eventually fully unresponsive under poor signal (RSSI ~-88dBm),
            // because every blip re-ran the whole chain instead of just the
            // reconnect itself.
            // The place now follows the network (Location.cpp, from the main
            // loop): joining a network with a known place just activates it;
            // only a network without one gets a single lookup, in the
            // background.
            {
                uint32_t __chainStartUs = micros();
                startAP(*g_cfgPtr); // Start the synchronized AP now that we are connected.
                g_diagLastReconnectChainMs = (micros() - __chainStartUs) / 1000;
                CLOCK_LOG("WiFi", "Diag: reconnect #%u chain (AP+server) took %u ms",
                          (unsigned)g_diagReconnectCount, (unsigned)g_diagLastReconnectChainMs);
            }
            // startAP() above is a no-op after the first successful connection
            // (if (g_apActive) return) - so it can't be relied on to retry mDNS
            // on later reconnects. Call this directly, every time, instead; see
            // WebUI::scheduleMdnsRecheck()'s comment for why that retry matters.
            WebUI::scheduleMdnsRecheck(g_mdnsHost);
            break;

        case WiFiState::AP_PORTAL:
            CLOCK_LOG("WiFi", "State: AP_PORTAL (Fallback)");
            startAPPortal(*g_cfgPtr);
            break;

        case WiFiState::RESTART_PENDING:
            CLOCK_LOG("WiFi", "State: RESTART_PENDING");
            g_restartAt = millis() + 2000;
            break;
            
        default: break;
    }
}

void WiFiManager::loop() {
    WiFiLock::Guard guard;
    // 1. Handle planned reboots
    if (g_state == WiFiState::RESTART_PENDING && millis() > g_restartAt) {
        CLOCK_LOG("WiFi", "Performing planned restart...");
        ESP.restart();
        return;
    }

    // "Connect" pressed on the page for a known network: move there now.
    int asked = g_connectRequest.exchange(-1);
    if (asked >= 0 && asked < WIFI_NETWORKS && wifiSsidAt(*g_cfgPtr, asked)[0] && g_state != WiFiState::AP_PORTAL) {
        DiagnosticLog::record("WIFI", "Connect pressed for \"%s\"", wifiSsidAt(*g_cfgPtr, asked));
        g_nextSlot = asked;
        g_retryCount = 0;
        setState(WiFiState::STA_CONNECTING);
        return;
    }

    if (g_gotIpEventTriggered) {
        g_gotIpEventTriggered = false;
        setState(WiFiState::STA_CONNECTED);
        return; // Return immediately to process the new state in the next loop
    }

    // 2. Handle Asynchronous Disconnect Events
    if (g_disconnectEventTriggered) {
        g_disconnectEventTriggered = false;
        
        if (g_state == WiFiState::STA_CONNECTING && g_pickScanRunning) {
            // Still choosing a network - nothing joined yet, so this is a
            // leftover from the previous connection.
        } else if (g_state == WiFiState::STA_CONNECTING) {
            // Fast-Fail Logic: If we hit a fatal error, don't wait for timeout
            bool isFatal = false;
            switch (g_lastDisconnectReason) {
                case 2:   // AUTH_FAIL
                case 6:   // NOT_AUTHED
                case 15:  // 4WAY_HANDSHAKE_TIMEOUT (Often wrong password)
                case 201: // NO_AP_FOUND (SSID not in range or incorrect)
                case 202: // AUTH_FAIL (Another code for auth failure)
                    isFatal = true;
                    CLOCK_LOG("WiFi", "Disconnect reason %d is considered fatal.", (int)g_lastDisconnectReason.load());
                    break;
                default:
                    CLOCK_LOG("WiFi", "Disconnect reason %d is considered recoverable.", (int)g_lastDisconnectReason.load());
                    break;
            }

            if (isFatal) {
                unsigned long age = millis() - g_stateTimer;
                if (age > 2000) { // Ignore stale events from the teardown process
                    if (g_retryCount < MAX_STA_RETRIES) {
                        g_retryCount++; // This will be attempt #2 or #3
                        CLOCK_LOG("WiFi", "Connection attempt %d failed (Reason: %d). Retrying...", g_retryCount, (int)g_lastDisconnectReason.load());
                        setState(WiFiState::STA_CONNECTING); // Immediate retry
                    } else {
                        CLOCK_LOG("WiFi", "Fatal error %d detected after %d retries. Triggering AP.", (int)g_lastDisconnectReason.load(), g_retryCount);
                        g_retryCount = 0;
                        setState(WiFiState::AP_PORTAL);
                    }
                    return;
                }
            }
        } else if (g_state == WiFiState::STA_CONNECTED) {
            // We were connected but lost it. Re-enter connecting state to show status.
            CLOCK_LOG("WiFi", "Connection lost from STA_CONNECTED state. Attempting to reconnect...");
            setState(WiFiState::STA_CONNECTING);
        }
    }

    // 3. State-specific timeouts and health checks
    switch (g_state) {
        case WiFiState::STA_CONNECTING:
            if (g_pickScanRunning) { servicePickScan(); break; }
            // Use a dedicated timer for connection attempts. It restarts when
            // a link comes up: the IP address follows a few seconds later,
            // and restarting the connection right then threw away a link the
            // core had just re-established on its own (2026-10-18, core 3.x:
            // ~4 s longer to get online after a restart).
            if ((int32_t)(millis() - g_lastConnectAttemptMs) > (int32_t)WIFI_CONNECT_TIMEOUT_MS &&
                (g_lastLinkUpMs == 0 || (int32_t)(millis() - g_lastLinkUpMs) > (int32_t)WIFI_LINK_TO_IP_MS)) {
                if (g_retryCount < MAX_STA_RETRIES) {
                    g_retryCount++; // This will be attempt #2 or #3
                    CLOCK_LOG("WiFi", "Connection timed out (Attempt %d). Retrying...", g_retryCount);
                    setState(WiFiState::STA_CONNECTING);
                } else {
                    CLOCK_LOG("WiFi", "Connection attempt timed out. Reverting to AP.");
                    g_retryCount = 0;
                    setState(WiFiState::AP_PORTAL);
                }
            }
            break;

        case WiFiState::AP_PORTAL:
            // Known networks, but none could be joined (e.g. the router still
            // starting after a power cut, or the network last used gone): keep
            // trying them, the usual way, every PORTAL_RETRY_MS - the setup
            // hotspot stays up meanwhile. Before 2026-10-21 only the core's own
            // auto-reconnect (one network) ever got the clock out of here.
            if (hasValidCredentials(*g_cfgPtr) && millis() - g_stateTimer >= PORTAL_RETRY_MS) {
                DiagnosticLog::record("WIFI", "setup mode: trying the known networks again");
                g_retryCount = 0;
                setState(WiFiState::STA_CONNECTING);
            }
            break;

        case WiFiState::STA_CONNECTED:
            // Verify we still have a valid IP (some routers de-auth silently)
            if (WiFi.status() != WL_CONNECTED || WiFi.localIP() == IPAddress(0,0,0,0)) {
                CLOCK_LOG("WiFi", "Detected IP loss while in connected state. Re-initiating connection.");
                setState(WiFiState::STA_CONNECTING);
            } else {
                g_lastOnlineMs = millis();
                serviceRoaming();
            }
            break;

        default: break;
    }
    serviceListScan();

    if (g_apActive) {
        dnsServer.processNextRequest();
    }

    // 4. Log status changes
    static wl_status_t lastStatus = WL_IDLE_STATUS;
    wl_status_t currentStatus = WiFi.status();
    if (currentStatus != lastStatus) {
        CLOCK_LOG("WiFi", "Physical Status: %s", getWlStatusName(currentStatus));
        lastStatus = currentStatus;
    }
}

/**
 * While connected: if the signal stays weak, look for a clearly stronger
 * access point of any known network and move to it. Runs with WiFiLock held
 * (from loop()). The scan runs in the background (async), so loop() never
 * waits for it. Moving = the normal reconnect path (setState(STA_CONNECTING)),
 * which joins the strongest access point anyway - the scan only decides
 * whether a reconnect is worth it. The display never depends on Wi-Fi; a
 * move costs the clock 1-3 s of network, invisible on the screen.
 */
void WiFiManager::serviceRoaming() {
    unsigned long now = millis();
    if (g_roamScanRunning) {
        int n = WiFi.scanComplete();
        if (n == WIFI_SCAN_RUNNING && now - g_roamScanStartedMs < ROAM_SCAN_GIVE_UP_MS) return;
        g_roamScanRunning = false;
        if (n < 0) {
            DiagnosticLog::record("WIFI", "roam scan: %s (code %d)", n == WIFI_SCAN_RUNNING ? "no result within 15 s" : "failed", n);
            WiFi.scanDelete();
            return;
        }
        int curRssi = WiFi.RSSI();
        String curBssid = WiFi.BSSIDstr();
        int bestRssi = -127, shown = 0;
        String bestBssid, bestSsid, seen;
        for (int i = 0; i < n; i++) {
            if (!isKnownSsid(*g_cfgPtr, WiFi.SSID(i))) continue;   // any known network, not only the current one
            String b = WiFi.BSSIDstr(i);
            int r = WiFi.RSSI(i);
            // Short form for the log line: last two bytes of the address.
            if (shown++ < 5) seen += b.substring(b.length() - 5) + "/ch" + String(WiFi.channel(i)) + "/" + String(r) + " ";
            if (b != curBssid && r > bestRssi) { bestRssi = r; bestBssid = b; bestSsid = WiFi.SSID(i); }
        }
        WiFi.scanDelete();
        bool better = bestBssid.length() && bestRssi >= curRssi + ROAM_MIN_GAIN_DB;
        DiagnosticLog::record("WIFI", "roam scan: on %s at %d dBm; seen %s-> %s",
                              curBssid.substring(curBssid.length() - 5).c_str(), curRssi, seen.c_str(),
                              better ? "switching" : "staying");
        if (better) {
            g_roamWeakSinceMs = 0;
            // Exactly that network next (connectSTA() would otherwise go back
            // to the last connected one first); the driver then joins its
            // strongest access point.
            for (int s = 0; s < WIFI_NETWORKS; s++) if (bestSsid == wifiSsidAt(*g_cfgPtr, s)) g_nextSlot = s;
            setState(WiFiState::STA_CONNECTING);
        }
        return;
    }
    if (g_listScanRunning) return;   // one scan at a time: they share the results
    if (now - g_roamLastSampleMs < ROAM_SAMPLE_MS) return;
    g_roamLastSampleMs = now;
    int rssi = WiFi.RSSI();
    if (rssi == 0 || rssi >= ROAM_WEAK_RSSI_DBM) { g_roamWeakSinceMs = 0; return; }
    if (!g_roamWeakSinceMs) { g_roamWeakSinceMs = now ? now : 1; return; }
    if (now - g_roamWeakSinceMs < ROAM_WEAK_FOR_MS) return;
    if (g_roamLastScanMs && now - g_roamLastScanMs < ROAM_MIN_INTERVAL_MS) return;
    g_roamLastScanMs = now ? now : 1;
    // Our own network's name only, unless more are known: then all names
    // (a scan can only filter by one), and the known ones are picked out above.
    const char* only = knownNetworkCount(*g_cfgPtr) > 1 ? nullptr : wifiSsidAt(*g_cfgPtr, g_joinSlot >= 0 ? g_joinSlot : 0);
    int16_t started = WiFi.scanNetworks(true /*async*/, false, false, ROAM_SCAN_MS_PER_CH, 0, only);
    if (started == WIFI_SCAN_FAILED) {
        DiagnosticLog::record("WIFI", "roam scan: could not start (signal %d dBm)", rssi);
        return;
    }
    g_roamScanStartedMs = now;
    g_roamScanRunning = true;
}

bool WiFiManager::heardNetworks(std::vector<HeardNetwork>& out, bool rescan) {
    WiFiLock::Guard guard;
    if (rescan) g_heardAtMs = 0;   // wait for the next result (a running scan counts)
    if (g_heardAtMs && millis() - g_heardAtMs < LIST_FRESH_MS) {
        out = g_heard;
        return true;
    }
    // Not while one runs: the page asks every second, and each ask started
    // the next scan right after the last - it never saw a result (2026-11-03).
    if (!g_listScanRunning) g_listRequest = true;
    return false;
}

// Runs with WiFiLock held (from loop()). A scan the page asked for starts
// only while connected and no scan of the clock's own runs (they share the
// one result list); until then the request waits. While connecting, the
// radio is busy - connectSTA() cancels a running one.
void WiFiManager::serviceListScan() {
    if (g_listScanRunning) {
        int n = WiFi.scanComplete();
        if (n == WIFI_SCAN_RUNNING && millis() - g_listScanStartedMs < LIST_SCAN_GIVE_UP_MS) return;
        if (n == WIFI_SCAN_RUNNING) esp_wifi_scan_stop();
        g_listScanRunning = false;
        g_heard.clear();
        for (int i = 0; i < n; i++) {
            String ssid = WiFi.SSID(i);
            if (!ssid.length()) continue;   // hidden network
            bool secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
            auto same = std::find_if(g_heard.begin(), g_heard.end(), [&](const HeardNetwork& h) { return h.ssid == ssid; });
            if (same == g_heard.end()) g_heard.push_back({ ssid, WiFi.RSSI(i), secure });
            else if (WiFi.RSSI(i) > same->rssi) same->rssi = WiFi.RSSI(i);
        }
        std::sort(g_heard.begin(), g_heard.end(), [](const HeardNetwork& a, const HeardNetwork& b) { return a.rssi > b.rssi; });
        WiFi.scanDelete();
        g_heardAtMs = millis() | 1;
        if (n < 0) DiagnosticLog::record("WIFI", "network list for the page: scan %s (code %d)", n == WIFI_SCAN_RUNNING ? "gave no result" : "failed", n);
        else CLOCK_LOG("WiFi", "network list for the page: %d names (%d access points)", (int)g_heard.size(), n);
        return;
    }
    if (!g_listRequest || g_state != WiFiState::STA_CONNECTED || g_roamScanRunning || g_pickScanRunning) return;
    g_listRequest = false;
    if (WiFi.scanNetworks(true /*async*/, false, false, LIST_SCAN_MS_PER_CH) == WIFI_SCAN_FAILED) {
        DiagnosticLog::record("WIFI", "network list for the page: scan could not start");
        g_heard.clear();
        g_heardAtMs = millis() | 1;
        return;
    }
    g_listScanStartedMs = millis();
    g_listScanRunning = true;
}

String WiFiManager::getStatusMessage() {
    WiFiLock::Guard guard;
    String info = "";
    switch(g_state) {
        // The "Connecting..." message is now handled by MessageAggregator
        // for better context (it can show the SSID name).
        case WiFiState::AP_PORTAL:
            // Only while the setup hotspot is what's up. Once the clock has
            // been online its own synchronized Wi-Fi stays up instead
            // (startAPPortal() doesn't replace it) with the full page - the
            // screen then keeps its slides plus "Internet unreachable"
            // (MessageAggregator, the user 2026-11-06); naming "...Setup"
            // there sent people to a network that didn't exist.
            // Scrolls, and starts over when read (MessageAggregator::loop()).
            if (!g_syncedApUp)
                info = "Setup: join Wi-Fi " + String(AP_SSID_PREFIX) + "Setup - password " + String(AP_PASSWORD_DEFAULT) + " - open 192.168.4.1";
            break;
        // In all other states (eg. connected, connecting), return an empty string
        // to avoid cluttering the display.
        default: info = ""; break;
    }
    return info;
}

bool WiFiManager::isApActive() {
    WiFiLock::Guard guard;
    return g_apActive;
}

bool WiFiManager::syncedApUp() { return g_syncedApUp; }

uint32_t WiFiManager::offlineForMs() {
    WiFiLock::Guard guard;
    if (g_state == WiFiState::STA_CONNECTED && WiFi.status() == WL_CONNECTED) return 0;
    return millis() - g_lastOnlineMs;
}

// ─── Private Actions (Triggered by State Transitions) ───────────────────────

void WiFiManager::begin(AppConfig& cfg) {
    g_cfgPtr = &cfg;
    g_mdnsHost = makeMdnsHost(cfg.projectName);
    g_retryCount = 0; // Reset retries on fresh boot
    WiFi.persistent(false);
    WiFi.onEvent(WiFiEventHandler);

    // The event handler registered just above can already be running on the
    // WiFi event task, so this first transition needs the lock like any other
    // (setState() verifies it, and logged a violation on every boot without it).
    WiFiLock::Guard guard;
    if (hasValidCredentials(cfg)) {
        setState(WiFiState::STA_CONNECTING);
    } else {
        setState(WiFiState::AP_PORTAL);
    }
}

void WiFiManager::connectSTA(AppConfig& cfg) {
    // Off (2026-10-21): this state machine does every reconnect itself -
    // the last network first, then the others (connectSTA()), and from setup
    // mode every PORTAL_RETRY_MS. The core's own auto-reconnect kept retrying
    // just the one network every ~3 s in parallel, which kept the radio busy:
    // scans for the other networks failed or came back empty.
    WiFi.setAutoReconnect(false);

    // Only force pure STA mode when there's no AP running yet (first boot, or
    // falling back from AP_PORTAL). If the synchronized AP is already active,
    // this is a reconnect attempt - leave WiFi in WIFI_AP_STA. WiFi.begin()
    // reconnects the STA side of a combined AP_STA interface on its own;
    // forcing WIFI_STA here would silently drop the AP for the whole
    // reconnect attempt, same as the stopAP() call removed from setState()'s
    // STA_CONNECTING case above (the other half of this fix). Bench-tested
    // via /api/debug/force-disconnect and a real RF dip on 2026-10-01:
    // apRebuilds stays flat, STA still reconnects normally either way.
    if (!g_apActive) {
        WiFi.mode(WIFI_STA);
    }

    // Disable WiFi power save mode. This can improve stability and prevent DHCP issues.
    WiFi.setSleep(false);

    // For Arduino Core 2.x, explicitly set the minimum security to WPA2-PSK.
    // This can help with WPA2/WPA3 mixed-mode routers.
    CLOCK_LOG("WiFi", "Setting minimum security to WPA2-PSK.");
    WiFi.setMinSecurity(WIFI_AUTH_WPA2_PSK);

    // Join the STRONGEST access point with this network name, not the first
    // one found. The default "fast" scan stops at the first match (by channel
    // order), so with a mesh / router + extender the clock could latch onto a
    // far node: seen 2026-10-17, three restarts in a row at -90..-93 dBm
    // (web page unreachable for minutes) until a disconnect made it rejoin
    // at -64..-73. A full scan costs ~1-2 s per connect; sorting by signal is
    // already the default but only matters with a full scan. (beginSlot())
    if (g_roamScanRunning) { WiFi.scanDelete(); g_roamScanRunning = false; }
    if (g_listScanRunning) {   // the page gets "none heard"
        esp_wifi_scan_stop(); WiFi.scanDelete();
        g_listScanRunning = false; g_heard.clear(); g_heardAtMs = millis() | 1;
    }
    g_pickScanRunning = false;
    int known = knownNetworkCount(cfg);
    if (!known) return;
    // Which known network to join (the user, 2026-10-21: stay with the last
    // connected one while it works, even if another is stronger):
    // 1. One asked for - "Connect" on the page, or the clearly stronger
    //    one roaming found (serviceRoaming()).
    int asked = g_nextSlot;
    g_nextSlot = -1;
    if (asked >= 0 && asked < WIFI_NETWORKS && wifiSsidAt(cfg, asked)[0]) {
        DiagnosticLog::record("WIFI", "joining \"%s\" (asked for)", wifiSsidAt(cfg, asked));
        beginSlot(cfg, asked);
        return;
    }
    // 2. First try: the last connected one, straight away - no scan, no delay.
    int last = cfg.wifiLastNetwork;
    if (g_retryCount == 0 && last >= 0 && last < WIFI_NETWORKS && wifiSsidAt(cfg, last)[0]) {
        beginSlot(cfg, last);
        return;
    }
    // 3. Only one known: that one.
    if (known == 1) {
        for (int s = 0; s < WIFI_NETWORKS; s++) {
            if (wifiSsidAt(cfg, s)[0]) { beginSlot(cfg, s); return; }
        }
    }
    // 4. Otherwise (it failed, or none yet): a quick scan in the background,
    //    then the strongest known one - loop() picks it (servicePickScan()).
    // A scan can't start while the radio is still trying to connect (ESP-IDF
    // refuses it: ESP_ERR_WIFI_STATE) - and after a failed network the core
    // keeps retrying it by itself every ~3 s (auto-reconnect). Stop that
    // first: on 2026-10-21 every scan failed this way, and with the hotspot
    // it had been on switched off, the clock never tried the home network.
    WiFi.disconnect(false, false);
    int16_t started = WiFi.scanNetworks(true /*async*/, false, false, PICK_SCAN_MS_PER_CH);
    if (started == WIFI_SCAN_FAILED) {
        String heard;
        std::vector<int> order = rankKnownNetworks(cfg, 0, heard);   // saved order
        int slot = order[g_retryCount % order.size()];
        if (slot == g_joinSlot && order.size() > 1) slot = order[(g_retryCount + 1) % order.size()];   // not the one that just failed
        DiagnosticLog::record("WIFI", "network scan could not start - trying \"%s\"", wifiSsidAt(cfg, slot));
        beginSlot(cfg, slot);
        return;
    }
    g_pickScanRunning = true;
    g_pickScanStartedMs = millis();
}

void WiFiManager::startAP(AppConfig& cfg) {
    if (g_apActive) return;
    g_diagApRebuildCount++;

    // cfg.projectName already defaults to "JewishClock" and is normalized to a
    // prefix-free identifier, so use it directly - prepending AP_SSID_PREFIX here
    // would double up the branding (e.g. "JewishClock-JewishClock").
    String apName = String(cfg.projectName);
    int channel = WiFi.channel(); // Get the channel we are connected on

    CLOCK_LOG("WiFi", "Starting synchronized AP '%s' on channel %d", apName.c_str(), channel);

    WiFi.mode(WIFI_AP_STA);
    // With STA already connected, calling softAP directly will implicitly
    // switch to WIFI_AP_STA mode without causing an interface restart.
    // WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(apName.c_str(), AP_PASSWORD_DEFAULT, channel);

    delay(100); // Allow radio to stabilize
    IPAddress apIP = WiFi.softAPIP();
    CLOCK_LOG("WiFi", "AP active at %s", apIP.toString().c_str());

    dnsServer.start(53, "*", apIP);
    g_apActive = true;
    g_syncedApUp = true;

    // Start the web server now that both interfaces are ready
    WebUI::startServer(cfg);
}

// Not currently called anywhere (the STA_CONNECTING transition no longer
// tears the AP down before reconnecting - see setState()'s comment). Kept,
// like WebUI::stopServer(), as a deliberate explicit teardown for any future
// caller that genuinely needs one, rather than removed outright.
void WiFiManager::stopAP() {
    if (!g_apActive) return;
    dnsServer.stop();
    WiFi.softAPdisconnect(true);
    g_apActive = false;
    g_syncedApUp = false;
    CLOCK_LOG("WiFi", "AP stopped.");
}

void WiFiManager::startAPPortal(AppConfig& cfg) {
    if (g_apActive) return;
    WiFi.mode(WIFI_AP); // Use pure AP mode for stability
    CLOCK_LOG("WiFi", "Starting AP Setup Portal...");

    g_apActive = true;
    g_cfgPtr = &cfg;
    server = WebUI::getAsyncServer(); // Shared instance

    String apName = String(AP_SSID_PREFIX) + "Setup";
    // Configure AP with specific IP
    WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
    WiFi.softAP(apName.c_str(), AP_PASSWORD_DEFAULT);
    
    delay(100); // Give AP time to start
    IPAddress apIP = WiFi.softAPIP();
    CLOCK_LOG("WiFi", "AP '%s' active at %s", apName.c_str(), apIP.toString().c_str());

    dnsServer.start(53, "*", apIP);

    static bool portalRoutesRegistered = false;

    if (!portalRoutesRegistered) {
        // Setup portal routes
        server->on("/", HTTP_GET, [&cfg](AsyncWebServerRequest *request) {
            // Force the browser to use the local IP instead of the captive portal detection domain
            if (request->host() != WiFi.softAPIP().toString()) {
                request->redirect("http://" + WiFi.softAPIP().toString() + "/");
                return;
            }
            
            String html = String(portal_html);
            html.replace("<<PROJECT_NAME>>", StringTools::formatProjectName(cfg.projectName));
            request->send(200, "text/html", html);
        }).setFilter(ON_AP_FILTER);

        server->on("/save", HTTP_POST, [](AsyncWebServerRequest *request) {
            String ssid = request->arg("ssid");
            String pass = request->arg("password");

            // Debugging: Log SSID and encrypted password hash for verification.
            String encrypted = Storage::encryptPassword(pass.c_str());
            CLOCK_LOG("WiFi", "Portal Setup - SSID: %s, Encrypted Password: %s", ssid.c_str(), encrypted.c_str());

            // Into the slot that already has this name (new password), else
            // the first free one, else the last - the others are kept, so
            // setting the clock up somewhere new doesn't forget home.
            int slot = -1;
            for (int i = 0; i < WIFI_NETWORKS && slot < 0; i++) if (ssid == wifiSsidAt(*g_cfgPtr, i)) slot = i;
            for (int i = 0; i < WIFI_NETWORKS && slot < 0; i++) if (!wifiSsidAt(*g_cfgPtr, i)[0]) slot = i;
            if (slot < 0) slot = WIFI_NETWORKS - 1;
            // A different network in that slot: its place is unknown (looked
            // up when joined). Directly - the clock restarts right after this.
            if (ssid != wifiSsidAt(*g_cfgPtr, slot)) memset(&g_cfgPtr->places[slot], 0, sizeof(Place));
            strlcpy(wifiSsidAt(*g_cfgPtr, slot), ssid.c_str(), sizeof(g_cfgPtr->wifiSSID));
            strlcpy(wifiPasswordAt(*g_cfgPtr, slot), pass.c_str(), sizeof(g_cfgPtr->wifiPassword));
            Storage::saveConfig(*g_cfgPtr);

            // The clock's name on the network (its mDNS name follows the clock's
            // name - "jewishclock" was fixed here until 2026-11-07), and the
            // page goes there after the 10 s it says (it reloaded the setup
            // page after 5 s - gone with the setup Wi-Fi).
            String host = makeMdnsHost(g_cfgPtr->projectName) + ".local";
            String html = "<html><head><meta http-equiv='refresh' content='10;url=http://" + host + "/'></head>";
            html += "<body style='font-family:sans-serif; text-align:center; padding-top:50px;'>";
            html += "<h2>Settings Saved</h2>";
            html += "<p>Connecting to <b>" + ssid + "</b>...</p>";
            html += "<p>The setup hotspot will close shortly.</p>";
            html += "<p>If your device supports mDNS, you will be redirected to <a href='http://" + host + "/'>" + host + "</a> in 10 seconds.</p>";
            html += "<p>Otherwise, check your router for the new IP address.</p>";
            html += "</body></html>";

            request->send(200, "text/html", html);

            // The only call into setState() from outside loop()/WiFiEventHandler()
            // (both already-guarded) - this one runs on the AsyncTCP task, so it
            // needs its own explicit guard. setState() itself never takes this
            // lock (see WiFiLock.h) specifically so this doesn't deadlock.
            { WiFiLock::Guard guard; setState(WiFiState::RESTART_PENDING); }
        }).setFilter(ON_AP_FILTER);

        // Any attempt to load index.html on AP should redirect to the portal root
        server->on("/index.html", HTTP_GET, [](AsyncWebServerRequest *request) {
            request->redirect("/");
        }).setFilter(ON_AP_FILTER);

        // API routes would need to be added here too, but for now just redirect to setup
        server->on("/wifi-scan", HTTP_GET, [](AsyncWebServerRequest *request) {
            int n = WiFi.scanComplete();
            if (n == -2) {
                CLOCK_LOG("WiFi", "SSID scan requested by UI; starting new async scan...");
                WiFi.scanNetworks(true, false, false, 100); 
                request->send(200, "application/json", "{\"networks\":[], \"status\":\"scanning\"}");
                return;
            }
            if (n == -1) {
                request->send(200, "application/json", "{\"networks\":[], \"status\":\"scanning\"}");
                return;
            }

            JsonDocument doc;
            JsonArray networks = doc["networks"].to<JsonArray>();
            for (int i = 0; i < n; i++) {
                JsonObject net = networks.add<JsonObject>();
                net["ssid"] = WiFi.SSID(i);
                net["rssi"] = WiFi.RSSI(i);
                net["secure"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
            }
            WebUI::sendJson(request, doc);
            WiFi.scanDelete(); // Clear for next time
        }).setFilter(ON_AP_FILTER);

        // Catch-all for AP mode to handle captive portal detection and other requests
        server->onNotFound([](AsyncWebServerRequest *request) {
            if (ON_AP_FILTER(request)) {
                // Redirect any other request to the portal root IP.
                request->redirect("http://192.168.4.1/");
            }
        });

        portalRoutesRegistered = true;
    }

    // Centralize server startup via WebUI
    WebUI::startServer(cfg);
}

// ── Read-only diagnostic getters ─────────────────────────────────────────────
uint32_t WiFiManager::getDisconnectCount()      { WiFiLock::Guard g; return g_diagDisconnectCount; }
uint32_t WiFiManager::getReconnectCount()       { WiFiLock::Guard g; return g_diagReconnectCount; }
uint32_t WiFiManager::getApRebuildCount()       { WiFiLock::Guard g; return g_diagApRebuildCount; }
uint32_t WiFiManager::getLastReconnectChainMs() { WiFiLock::Guard g; return g_diagLastReconnectChainMs; }
