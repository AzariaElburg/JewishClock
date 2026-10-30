#pragma once
// PC stand-in (simulator only): real HTTP via the curl.exe that ships with
// Windows, with every answer cached on disk (SIM_HTTP_CACHE) so runs are
// fast and repeatable. Hebcal URLs depend only on date + location, so the
// simulator and the clock get identical data.

#include <Arduino.h>
#include "WiFiClientSecure.h"

#define HTTP_CODE_OK 200

class HTTPClient {
public:
    bool begin(WiFiClient&, const String& url) { url_ = url; return true; }
    bool begin(const String& url) { url_ = url; return true; }
    void setTimeout(uint16_t) {}
    void setConnectTimeout(int32_t) {}
    void addHeader(const String&, const String&) {}
    int GET();
    String getString() { return body_; }
    int getSize() { return (int)body_.length(); }
    void end() {}
    static String errorToString(int code) { return String("HTTP error ") + code; }
private:
    String url_, body_;
};
