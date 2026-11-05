#pragma once
// PC stand-in for ESPAsyncWebServer (the full simulator, jcclock - sim/README.md).
// Only what the clock's code uses, behaving like the library version in
// pioarduino.ini (3.12.1) where the clock depends on it:
//  - a route "/a" also matches "/a/anything" (WebServer.cpp, BackwardCompatible)
//  - the first registered handler whose method, path and filter fit wins
//  - send(code, type, String) goes through c_str(): stops at a NUL (CLAUDE.md)
//  - headers in the library's order, "Connection: close" and "Accept-Ranges: none"
//  - a file response serves "<path>.gz" (with Content-Encoding) when <path> is missing
//  - ON_STA_FILTER / ON_AP_FILTER by the address the request arrived on
// The HTTP side (sockets, parsing, uploads) is sim/sim_web.cpp.
#include <Arduino.h>
#include <FS.h>
#include <functional>
#include <memory>
#include <vector>
#include <string>
#include "WiFi.h"

enum WebRequestMethod : uint8_t { HTTP_GET = 1, HTTP_POST = 2, HTTP_DELETE = 4, HTTP_PUT = 8, HTTP_PATCH = 16, HTTP_HEAD = 32, HTTP_OPTIONS = 64, HTTP_ANY = 127 };
typedef uint8_t WebRequestMethodComposite;

class AsyncWebServerRequest;
class AsyncWebServerResponse;
typedef std::function<void(AsyncWebServerRequest*)> ArRequestHandlerFunction;
typedef std::function<void(AsyncWebServerRequest*, const String&, size_t, uint8_t*, size_t, bool)> ArUploadHandlerFunction;
typedef std::function<void(AsyncWebServerRequest*, uint8_t*, size_t, size_t, size_t)> ArBodyHandlerFunction;
typedef std::function<bool(AsyncWebServerRequest*)> ArRequestFilterFunction;
typedef std::function<size_t(uint8_t*, size_t, size_t)> AwsResponseFiller;

bool ON_STA_FILTER(AsyncWebServerRequest* request);
bool ON_AP_FILTER(AsyncWebServerRequest* request);

class AsyncWebParameter {
public:
    AsyncWebParameter(const String& name, const String& value, bool form = false, bool file = false, size_t size = 0)
        : name_(name), value_(value), form_(form), file_(file), size_(size) {}
    const String& name() const { return name_; }
    const String& value() const { return value_; }
    size_t size() const { return size_; }
    bool isPost() const { return form_; }
    bool isFile() const { return file_; }
private:
    String name_, value_; bool form_, file_; size_t size_;
};

class AsyncWebHeader {
public:
    AsyncWebHeader(const String& n, const String& v) : name_(n), value_(v) {}
    const String& name() const { return name_; }
    const String& value() const { return value_; }
private:
    String name_, value_;
};

class AsyncClient {
public:
    IPAddress remoteIP() const { return remote_; }
    IPAddress localIP() const { return local_; }
    IPAddress remote_, local_;
};

class AsyncWebServerResponse {
public:
    virtual ~AsyncWebServerResponse() {}
    void addHeader(const char* name, const char* value, bool replace = true);
    void addHeader(const String& name, const String& value, bool replace = true) { addHeader(name.c_str(), value.c_str(), replace); }
    void setCode(int code) { code_ = code; }
    int code() const { return code_; }
    // The PC side: everything to put on the wire, in the library's order.
    std::string head() const;
    virtual size_t bodyLength() const = 0;
    virtual size_t readBody(uint8_t* buf, size_t max, size_t index) = 0;
protected:
    int code_ = 200;
    std::string type_;
    std::vector<std::pair<std::string, std::string>> headers_;
};

class AsyncWebServerRequest {
public:
    // --- what the clock's handlers call ---
    WebRequestMethod method() const { return method_; }
    const String& url() const { return url_; }
    const String& host() const { return host_; }
    size_t contentLength() const { return contentLength_; }
    AsyncClient* client() { return &client_; }
    bool hasParam(const char* name, bool post = false, bool file = false) const { return getParam(name, post, file) != nullptr; }
    bool hasParam(const String& name, bool post = false, bool file = false) const { return hasParam(name.c_str(), post, file); }
    const AsyncWebParameter* getParam(const char* name, bool post = false, bool file = false) const;
    const AsyncWebParameter* getParam(const String& name, bool post = false, bool file = false) const { return getParam(name.c_str(), post, file); }
    const AsyncWebParameter* getParam(size_t i) const { return i < params_.size() ? &params_[i] : nullptr; }
    size_t params() const { return params_.size(); }
    String arg(const char* name) const;
    String arg(const String& name) const { return arg(name.c_str()); }
    bool hasArg(const char* name) const;
    bool hasHeader(const char* name) const { return getHeader(name) != nullptr; }
    const AsyncWebHeader* getHeader(const char* name) const;
    const AsyncWebHeader* getHeader(const String& name) const { return getHeader(name.c_str()); }

    void send(AsyncWebServerResponse* response);
    void send(int code, const char* contentType = "", const char* content = "");
    void send(int code, const String& contentType, const String& content = String()) { send(code, contentType.c_str(), content.c_str()); }
    void send(int code, const char* contentType, const String& content) { send(code, contentType, content.c_str()); }
    void send(fs::FS& fs, const String& path, const char* contentType = "", bool download = false);
    void send(fs::FS& fs, const String& path, const String& contentType, bool download = false) { send(fs, path, contentType.c_str(), download); }
    AsyncWebServerResponse* beginResponse(int code, const char* contentType = "", const char* content = "");
    AsyncWebServerResponse* beginResponse(int code, const String& contentType, const String& content = String()) { return beginResponse(code, contentType.c_str(), content.c_str()); }
    AsyncWebServerResponse* beginResponse(int code, const char* contentType, const uint8_t* content, size_t len);
    AsyncWebServerResponse* beginResponse(fs::FS& fs, const String& path, const char* contentType = "", bool download = false);
    AsyncWebServerResponse* beginResponse(fs::FS& fs, const String& path, const String& contentType, bool download = false) { return beginResponse(fs, path, contentType.c_str(), download); }
    AsyncWebServerResponse* beginResponse(const char* contentType, size_t len, AwsResponseFiller filler);
    AsyncWebServerResponse* beginResponse(const String& contentType, size_t len, AwsResponseFiller filler) { return beginResponse(contentType.c_str(), len, filler); }
    void redirect(const char* url, int code = 302);
    void redirect(const String& url, int code = 302) { redirect(url.c_str(), code); }

    // --- the PC side (sim_web.cpp) ---
    WebRequestMethod method_ = HTTP_GET;
    String url_, host_;
    size_t contentLength_ = 0;
    AsyncClient client_;
    std::vector<AsyncWebParameter> params_;
    std::vector<AsyncWebHeader> headers_;
    AsyncWebServerResponse* response_ = nullptr;   // owned
    std::string body_;                             // the raw body (urlencoded / multipart)
    std::string contentType_;
    ~AsyncWebServerRequest() { delete response_; }
};

class AsyncCallbackWebHandler {
public:
    AsyncCallbackWebHandler& setFilter(ArRequestFilterFunction f) { filter_ = f; return *this; }
    String uri_;
    WebRequestMethodComposite method_ = HTTP_ANY;
    ArRequestHandlerFunction onRequest_;
    ArUploadHandlerFunction onUpload_;
    ArBodyHandlerFunction onBody_;
    ArRequestFilterFunction filter_;
    bool canHandle(AsyncWebServerRequest* r) const;
};

class AsyncWebServer {
public:
    explicit AsyncWebServer(uint16_t port) : port_(port) {}
    void begin();
    void end();
    AsyncCallbackWebHandler& on(const char* uri, WebRequestMethodComposite method, ArRequestHandlerFunction onRequest);
    AsyncCallbackWebHandler& on(const char* uri, WebRequestMethodComposite method, ArRequestHandlerFunction onRequest,
                                ArUploadHandlerFunction onUpload, ArBodyHandlerFunction onBody = nullptr);
    void onNotFound(ArRequestHandlerFunction fn) { notFound_ = fn; }
    // --- the PC side ---
    uint16_t port_;
    std::vector<std::unique_ptr<AsyncCallbackWebHandler>> handlers_;
    ArRequestHandlerFunction notFound_;
};
