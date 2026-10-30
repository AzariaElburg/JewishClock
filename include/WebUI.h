#pragma once

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include "Storage.h"

class WebUI {
public:
    // Initializes the server in PSRAM and sets up routes
    static void begin(AppConfig& cfg, EventList& events);
    static void startServer(AppConfig& cfg);
    static void stopServer();

    // Requests an mDNS (re)check on the next loop() tick after `mdnsHost`'s
    // STA connection is confirmed - see the comment on the call site in
    // WiFiManager.cpp's STA_CONNECTED case for why this needs to fire on
    // every reconnect, not just the first one.
    static void scheduleMdnsRecheck(const String& mdnsHost);
    static void sendJson(AsyncWebServerRequest* req, const JsonDocument& doc);

    // Count of JSON API responses sent since boot - a safe proxy for request
    // volume (see sendJson()'s comment for why this isn't a raw socket count).
    static uint32_t getJsonResponsesSent();
    
    // Provides access to the shared server instance
    static AsyncWebServer* getAsyncServer();

    // Handles any background server tasks
    static void loop();

private:
    // We use a pointer so we can allocate the actual server object 
    // into the 8MB PSRAM during the begin() call.
    static AsyncWebServer* g_server;
};