#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
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
    // The known network being joined (or joined); "" while still choosing.
    static String joiningSsid();
    // Join known network `slot` now ("Connect" on the page; any task).
    static void requestConnect(int slot);
    // The known network the clock is connected to now (0..WIFI_NETWORKS-1),
    // -1 while not connected. Any task.
    static int connectedSlot();

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
    static bool hasValidCredentials(const AppConfig& cfg);
    static void setState(WiFiState newState);
};
