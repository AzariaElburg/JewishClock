#pragma once
// PC stand-in (simulator only): real HTTP via the curl.exe that ships with
// Windows, with every answer cached on disk (SIM_HTTP_CACHE) so runs are
// fast and repeatable. Hebcal URLs depend only on date + location, so the
// simulator and the clock get identical data.

#include <Arduino.h>
#include "WiFiClientSecure.h"

#include <string>
#include <cstring>
#include <vector>

#define HTTP_CODE_OK 200
// The library's own default (HTTPClient.h of Arduino-ESP32 3.3.x: HTTPCLIENT_DEFAULT_TCP_TIMEOUT 5000):
// how long the clock's downloads wait for a connection - the stand-in waits the same for a service that
// is down. Tests ask it (jcsim --tunables / /api/tunables: HTTPCLIENT_DEFAULT_TCP_TIMEOUT).
#define HTTPCLIENT_DEFAULT_TCP_TIMEOUT (5000)

enum followRedirects_t { HTTPC_DISABLE_FOLLOW_REDIRECTS, HTTPC_STRICT_FOLLOW_REDIRECTS, HTTPC_FORCE_FOLLOW_REDIRECTS };

class HTTPClient {
public:
    bool begin(WiFiClient&, const String& url) { url_ = url; headers_.clear(); return true; }
    bool begin(const String& url) { url_ = url; headers_.clear(); return true; }
    void setTimeout(uint16_t) {}
    void setConnectTimeout(int32_t) {}
    void addHeader(const String& k, const String& v) { headers_.push_back(std::string(k.c_str()) + ": " + v.c_str()); }
    // As the core's: the answer's headers asked for before GET(), read after it (case-insensitive name).
    void collectHeaders(const char* keys[], size_t n) { want_.clear(); for (size_t i = 0; i < n; i++) want_.push_back(keys[i]); }
    String header(const char* name) {
        for (auto& kv : got_) if (_stricmp(kv.first.c_str(), name) == 0) return String(kv.second.c_str());
        return String();
    }
    bool hasHeader(const char* name) {   // as the core's: the answer had it (one asked for)
        for (auto& kv : got_) if (_stricmp(kv.first.c_str(), name) == 0) return true;
        return false;
    }
    void setUserAgent(const String& ua) { ua_ = ua.c_str(); }
    void setFollowRedirects(followRedirects_t f) { follow_ = f != HTTPC_DISABLE_FOLLOW_REDIRECTS; }
    int GET();
    String getString() { return body_; }
    int getSize() { return (int)raw_.size(); }
    // As the core's: the body into a stream, in pieces (an update download) - the bytes as
    // they came, zeros included (raw_, not the String).
    int writeToStream(Stream* s) {
        size_t done = 0;
        while (done < raw_.size()) {
            size_t n = raw_.size() - done < 4096 ? raw_.size() - done : 4096;
            if (s->write((const uint8_t*)raw_.data() + done, n) != n) return -10;   // HTTPC_ERROR_STREAM_WRITE
            done += n;
        }
        return (int)done;
    }
    void end() {}
    static String errorToString(int code) { return String("HTTP error ") + code; }
private:
    String url_, body_;
    std::string raw_, ua_;
    std::vector<std::string> headers_;
    std::vector<std::string> want_;
    std::vector<std::pair<std::string, std::string>> got_;   // the last answer's headers (of want_)
    bool follow_ = false;
};
