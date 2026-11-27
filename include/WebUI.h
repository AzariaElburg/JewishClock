#pragma once

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include "Storage.h"

class WebUI {
public:
    // Initializes the server in PSRAM and sets up routes
    static void begin(AppConfig& cfg, EventList& events);
    static void startServer(AppConfig& cfg);

    // Requests an mDNS (re)check on the next loop() tick after `mdnsHost`'s
    // STA connection is confirmed - see the comment on the call site in
    // WiFiManager.cpp's STA_CONNECTED case for why this needs to fire on
    // every reconnect, not just the first one.
    static void scheduleMdnsRecheck(const String& mdnsHost);
    static void sendJson(AsyncWebServerRequest* req, const JsonDocument& doc);
    // The page files changed (an update put new ones in place, Updater.cpp):
    // what the server knew about them (gzip, ETag) is looked up again.
    static void webFilesChanged();

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