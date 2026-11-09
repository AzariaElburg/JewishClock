#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <vector>
#include "Storage.h"

enum class WiFiState {
    IDLE,
    STA_CONNECTING,
    STA_CONNECTED,
    AP_PORTAL,
    RESTART_PENDING
};

class WiFiManager {
public:
    static void begin(AppConfig& cfg);
    static void loop();
    static String getStatusMessage();
    static bool   isApActive();
    // The clock's own Wi-Fi is up as its synchronized AP (projectName, the
    // full page - startAP()), not the setup one (startAPPortal()). Stays up
    // while the home network is gone. Any task, no lock (web filters ask).
    static bool   syncedApUp();
    // How long no known network has been joined: 0 while connected, else ms
    // since the last connection (since startup if none yet). Main loop only.
    static uint32_t offlineForMs();
    // The known network being joined (or joined); "" while still choosing.
    static String joiningSsid();
    // Join known network `slot` now ("Connect" on the page; any task).
    static void requestConnect(int slot);
    // The known network the clock is connected to now (0..WIFI_NETWORKS-1),
    // -1 while not connected. Any task.
    static int connectedSlot();

    // Networks heard on the air, for the settings page's "Network name" list
    // (any task). True = `out` is a recent list (strongest first, one entry
    // per name, hidden networks left out; may be empty); false = a scan is
    // asked for or running - ask again shortly (rescan: not the recent list,
    // a new scan). loop() runs it when no scan of its own is running
    // (serviceListScan()).
    struct HeardNetwork { String ssid; int rssi; bool secure; };
    static bool heardNetworks(std::vector<HeardNetwork>& out, bool rescan = false);

    // ── Read-only diagnostic counters (added for the networking-reliability
    //    investigation; purely observational, no effect on control flow) ──
    static uint32_t getDisconnectCount();
    static uint32_t getReconnectCount();
    static uint32_t getApRebuildCount();
    static uint32_t getLastReconnectChainMs();

private:
    static void WiFiEventHandler(WiFiEvent_t event, WiFiEventInfo_t info);
    static void startAP(AppConfig& cfg);
    static void startAPPortal(AppConfig& cfg);
    static void stopAP();
    static void connectSTA(AppConfig& cfg);
    static void serviceRoaming();   // move to a clearly stronger access point if the signal stays weak
    static void servicePickScan();  // several known networks: join the strongest once the scan is done
    static void serviceListScan();  // the settings page asked which networks are around
    static bool hasValidCredentials(const AppConfig& cfg);
    static void setState(WiFiState newState);
};
