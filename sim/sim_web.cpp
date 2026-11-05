// =============================================================================
// sim_web.cpp - the PC side of the full simulator's web server (jcclock):
// sockets, HTTP parsing, choosing the handler as ESPAsyncWebServer does, and
// the replies as it sends them (clock_stubs/ESPAsyncWebServer.h says which of
// its habits are copied). Checked against the real clock's recorded answers -
// sim/compare_web.py.
// =============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <thread>
#include <vector>
#include <string>
#include <map>
#include <algorithm>
#include "sim_clock.h"

int Sim::homePort = 8080, Sim::apPort = 8081;
static AsyncWebServer* g_server = nullptr;   // the clock has one (WebUI::getAsyncServer())

// ── Responses ─────────────────────────────────────────────────────────────────
void AsyncWebServerResponse::addHeader(const char* name, const char* value, bool replace) {
    for (auto& h : headers_) {
        if (strcasecmp(h.first.c_str(), name) == 0) { if (replace) h.second = value; return; }
    }
    headers_.emplace_back(name, value);
}

static const char* reasonPhrase(int code) {
    switch (code) {
        case 200: return "OK"; case 204: return "No Content"; case 301: return "Moved Permanently"; case 302: return "Found";
        case 304: return "Not Modified"; case 400: return "Bad Request"; case 403: return "Forbidden"; case 404: return "Not Found";
        case 409: return "Conflict"; case 413: return "Payload Too Large"; case 500: return "Internal Server Error";
        case 503: return "Service Unavailable"; default: return "";
    }
}

std::string AsyncWebServerResponse::head() const {
    std::string h = "HTTP/1.1 " + std::to_string(code_) + " " + reasonPhrase(code_) + "\r\n";
    bool hasConn = false;
    for (auto& kv : headers_) { h += kv.first + ": " + kv.second + "\r\n"; if (strcasecmp(kv.first.c_str(), "Connection") == 0) hasConn = true; }
    if (!hasConn) h += "Connection: close\r\n";
    h += "Accept-Ranges: none\r\n";
    h += "Content-Length: " + std::to_string(bodyLength()) + "\r\n";
    if (!type_.empty()) h += "Content-Type: " + type_ + "\r\n";
    return h + "\r\n";
}

namespace {
// send(code, type, String) and beginResponse(code, type, String): the
// library's AsyncBasicResponse - "Connection: close" first, content cut at a NUL.
struct BasicResponse : AsyncWebServerResponse {
    std::string body;
    BasicResponse(int code, const char* type, const char* content) {
        code_ = code; type_ = type ? type : ""; body = content ? content : "";
        if (!body.empty() && type_.empty()) type_ = "text/plain";
        headers_.emplace_back("Connection", "close");
    }
    size_t bodyLength() const override { return body.size(); }
    size_t readBody(uint8_t* buf, size_t max, size_t index) override {
        size_t n = index < body.size() ? std::min(max, body.size() - index) : 0;
        memcpy(buf, body.data() + index, n); return n;
    }
};
// beginResponse(code, type, bytes, len) - a buffer of known length.
struct BytesResponse : AsyncWebServerResponse {
    std::string body;
    BytesResponse(int code, const char* type, const uint8_t* p, size_t n) { code_ = code; type_ = type ? type : ""; body.assign((const char*)p, n); }
    size_t bodyLength() const override { return body.size(); }
    size_t readBody(uint8_t* buf, size_t max, size_t index) override {
        size_t n = index < body.size() ? std::min(max, body.size() - index) : 0;
        memcpy(buf, body.data() + index, n); return n;
    }
};
// A file (AsyncFileResponse, WebResponses.cpp): <path>, else <path>.gz with
// Content-Encoding, ETag (the gzip's CRC) and no-cache, else 404.
struct FileResponse : AsyncWebServerResponse {
    std::string body;
    FileResponse(fs::FS& fs, const String& path, const char* type, bool download) {
        File f = fs.open(path, "r");
        if (!f || f.isDirectory() || !f.available()) {
            if (f) f.close();
            f = fs.open(path + ".gz", "r");
            if (!f || f.size() < 8) { code_ = 404; return; }
            uint32_t crc = 0; f.seek(f.size() - 8); f.read((uint8_t*)&crc, 4); f.seek(0);
            char etag[12]; snprintf(etag, sizeof(etag), "\"%08X\"", (unsigned)crc);
            headers_.emplace_back("Content-Encoding", "gzip");
            headers_.emplace_back("ETag", etag);
            headers_.emplace_back("Cache-Control", "no-cache");
        }
        uint8_t buf[8192]; size_t n;
        while ((n = f.read(buf, sizeof(buf))) > 0) body.append((const char*)buf, n);
        f.close();
        type_ = type && *type ? type : "application/octet-stream";
        if (download) {
            std::string p = path.c_str();
            headers_.emplace_back("Content-Disposition", "attachment; filename=\"" + p.substr(p.find_last_of('/') + 1) + "\"");
        } else {
            headers_.emplace_back("Content-Disposition", "inline");
        }
        code_ = 200;
    }
    size_t bodyLength() const override { return body.size(); }
    size_t readBody(uint8_t* buf, size_t max, size_t index) override {
        size_t n = index < body.size() ? std::min(max, body.size() - index) : 0;
        memcpy(buf, body.data() + index, n); return n;
    }
};
// beginResponse(type, len, filler) - the filler is asked chunk by chunk.
struct FillerResponse : AsyncWebServerResponse {
    size_t len; AwsResponseFiller filler;
    FillerResponse(const char* type, size_t n, AwsResponseFiller f) : len(n), filler(f) { type_ = type ? type : ""; }
    size_t bodyLength() const override { return len; }
    size_t readBody(uint8_t* buf, size_t max, size_t index) override { return filler(buf, max, index); }
};
} // namespace

// ── Requests ──────────────────────────────────────────────────────────────────
const AsyncWebParameter* AsyncWebServerRequest::getParam(const char* name, bool post, bool file) const {
    for (auto& p : params_) if (p.name() == name && p.isPost() == post && p.isFile() == file) return &p;
    return nullptr;
}
String AsyncWebServerRequest::arg(const char* name) const {
    for (auto& p : params_) if (p.name() == name && !p.isFile()) return p.value();
    return String();
}
bool AsyncWebServerRequest::hasArg(const char* name) const {
    for (auto& p : params_) if (p.name() == name && !p.isFile()) return true;
    return false;
}
const AsyncWebHeader* AsyncWebServerRequest::getHeader(const char* name) const {
    for (auto& h : headers_) if (strcasecmp(h.name().c_str(), name) == 0) return &h;
    return nullptr;
}
void AsyncWebServerRequest::send(AsyncWebServerResponse* r) { if (response_ != r) { delete response_; response_ = r; } }
void AsyncWebServerRequest::send(int code, const char* type, const char* content) { send(beginResponse(code, type, content)); }
void AsyncWebServerRequest::send(fs::FS& fs, const String& path, const char* type, bool download) {
    // The library answers 404 itself when neither the file nor its .gz exists.
    if (!fs.exists(path) && !fs.exists(path + ".gz")) { send(404); return; }
    send(beginResponse(fs, path, type, download));
}
AsyncWebServerResponse* AsyncWebServerRequest::beginResponse(int code, const char* type, const char* content) { return new BasicResponse(code, type, content); }
AsyncWebServerResponse* AsyncWebServerRequest::beginResponse(int code, const char* type, const uint8_t* p, size_t n) { return new BytesResponse(code, type, p, n); }
AsyncWebServerResponse* AsyncWebServerRequest::beginResponse(fs::FS& fs, const String& path, const char* type, bool download) { return new FileResponse(fs, path, type, download); }
AsyncWebServerResponse* AsyncWebServerRequest::beginResponse(const char* type, size_t len, AwsResponseFiller filler) { return new FillerResponse(type, len, filler); }
void AsyncWebServerRequest::redirect(const char* url, int code) {
    AsyncWebServerResponse* r = beginResponse(code);
    r->addHeader("Location", url);
    send(r);
}

// The library's filters (WebServer.cpp): by the address the request came in on.
bool ON_STA_FILTER(AsyncWebServerRequest* r) { return WiFi.localIP() == r->client()->localIP(); }
bool ON_AP_FILTER(AsyncWebServerRequest* r) { return WiFi.localIP() != r->client()->localIP(); }

// ── Handlers ──────────────────────────────────────────────────────────────────
// AsyncCallbackWebHandler::canHandle (WebHandlers.cpp) with a plain URI: the
// method, then "uri" itself or anything under "uri/", then the filter.
bool AsyncCallbackWebHandler::canHandle(AsyncWebServerRequest* r) const {
    if (!(method_ & r->method())) return false;
    const String& u = r->url();
    if (!(u == uri_ || (uri_.length() && u.startsWith(uri_ + "/")) || (uri_ == "/" && u == "/"))) return false;
    return !filter_ || filter_(r);
}

AsyncCallbackWebHandler& AsyncWebServer::on(const char* uri, WebRequestMethodComposite m, ArRequestHandlerFunction fn) {
    return on(uri, m, fn, nullptr, nullptr);
}
AsyncCallbackWebHandler& AsyncWebServer::on(const char* uri, WebRequestMethodComposite m, ArRequestHandlerFunction fn,
                                            ArUploadHandlerFunction up, ArBodyHandlerFunction body) {
    auto h = std::make_unique<AsyncCallbackWebHandler>();
    String u = uri;
    if (u.length() > 1 && u.endsWith("/")) u = u.substring(0, u.length() - 1);   // the library drops a trailing slash
    h->uri_ = u; h->method_ = m; h->onRequest_ = fn; h->onUpload_ = up; h->onBody_ = body;
    handlers_.push_back(std::move(h));
    return *handlers_.back();
}
void AsyncWebServer::begin() { g_server = this; Sim::startWebListeners(); }
void AsyncWebServer::end() {}

// ── HTTP on the PC ────────────────────────────────────────────────────────────
static std::string urlDecode(const std::string& s, bool plusIsSpace) {
    std::string o; o.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            o += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16); i += 2;
        } else if (s[i] == '+' && plusIsSpace) o += ' ';
        else o += s[i];
    }
    return o;
}
static void parseQuery(const std::string& q, bool post, std::vector<AsyncWebParameter>& out) {
    size_t pos = 0;
    while (pos <= q.size() && !q.empty()) {
        size_t amp = q.find('&', pos); if (amp == std::string::npos) amp = q.size();
        std::string kv = q.substr(pos, amp - pos);
        if (!kv.empty()) {
            size_t eq = kv.find('=');
            std::string k = urlDecode(kv.substr(0, eq), true), v = eq == std::string::npos ? "" : urlDecode(kv.substr(eq + 1), true);
            out.emplace_back(String(k.c_str()), String(v.c_str()), post);
        }
        pos = amp + 1;
        if (amp >= q.size()) break;
    }
}

namespace {
struct UploadPart { std::string name, filename, data; };

bool recvAll(SOCKET s, std::string& buf, size_t want) {
    char tmp[16384];
    while (buf.size() < want) {
        int n = recv(s, tmp, sizeof(tmp), 0);
        if (n <= 0) return false;
        buf.append(tmp, n);
    }
    return true;
}

void sendAll(SOCKET s, const char* p, size_t n) {
    while (n) { int k = ::send(s, p, (int)std::min<size_t>(n, 65536), 0); if (k <= 0) return; p += k; n -= k; }
}

// multipart/form-data: fields become post parameters, files go to the upload handler.
void parseMultipart(const std::string& body, const std::string& boundary, std::vector<AsyncWebParameter>& params, std::vector<UploadPart>& files) {
    std::string delim = "--" + boundary;
    size_t pos = body.find(delim);
    while (pos != std::string::npos) {
        pos += delim.size();
        if (body.compare(pos, 2, "--") == 0) break;
        if (body.compare(pos, 2, "\r\n") == 0) pos += 2;
        size_t hdrEnd = body.find("\r\n\r\n", pos);
        if (hdrEnd == std::string::npos) break;
        std::string hdr = body.substr(pos, hdrEnd - pos);
        size_t dataStart = hdrEnd + 4;
        size_t next = body.find("\r\n" + delim, dataStart);
        if (next == std::string::npos) break;
        std::string data = body.substr(dataStart, next - dataStart);
        auto attr = [&](const char* key) -> std::string {
            std::string k = std::string(key) + "=\"";
            size_t a = hdr.find(k); if (a == std::string::npos) return std::string();
            a += k.size(); size_t b = hdr.find('"', a); return hdr.substr(a, b - a);
        };
        std::string name = attr("name"), filename = attr("filename");
        if (hdr.find("filename=") != std::string::npos) files.push_back({name, filename, data});
        else params.emplace_back(String(name.c_str()), String(data.c_str()), true);
        pos = next + 2;
    }
}

void serveConnection(SOCKET s, bool viaAp) {
    std::string buf;
    size_t hdrEnd;
    char tmp[16384];
    while ((hdrEnd = buf.find("\r\n\r\n")) == std::string::npos) {
        int n = recv(s, tmp, sizeof(tmp), 0);
        if (n <= 0 || buf.size() > 64 * 1024) { closesocket(s); return; }
        buf.append(tmp, n);
    }
    // The interface must be up (on the clock: no address on it = unreachable).
    if (viaAp ? !Sim::apUp() : !Sim::staUp()) { closesocket(s); return; }

    AsyncWebServerRequest req;
    std::string head = buf.substr(0, hdrEnd);
    size_t lineEnd = head.find("\r\n");
    std::string first = head.substr(0, lineEnd);
    size_t sp1 = first.find(' '), sp2 = first.find(' ', sp1 + 1);
    std::string method = first.substr(0, sp1), target = first.substr(sp1 + 1, sp2 - sp1 - 1);
    req.method_ = method == "GET" ? HTTP_GET : method == "POST" ? HTTP_POST : method == "PUT" ? HTTP_PUT
                : method == "DELETE" ? HTTP_DELETE : method == "PATCH" ? HTTP_PATCH : method == "HEAD" ? HTTP_HEAD : HTTP_OPTIONS;
    size_t q = target.find('?');
    req.url_ = String(urlDecode(target.substr(0, q), false).c_str());
    if (q != std::string::npos) parseQuery(target.substr(q + 1), false, req.params_);
    size_t pos = lineEnd + 2;
    while (pos < head.size()) {
        size_t e = head.find("\r\n", pos); if (e == std::string::npos) e = head.size();
        std::string line = head.substr(pos, e - pos);
        size_t c = line.find(':');
        if (c != std::string::npos) {
            std::string k = line.substr(0, c), v = line.substr(c + 1);
            while (!v.empty() && v[0] == ' ') v.erase(0, 1);
            req.headers_.emplace_back(String(k.c_str()), String(v.c_str()));
            if (strcasecmp(k.c_str(), "Host") == 0) req.host_ = String(v.c_str());
            if (strcasecmp(k.c_str(), "Content-Type") == 0) req.contentType_ = v;
            if (strcasecmp(k.c_str(), "Content-Length") == 0) req.contentLength_ = strtoul(v.c_str(), nullptr, 10);
        }
        pos = e + 2;
    }
    std::string body = buf.substr(hdrEnd + 4);
    if (req.contentLength_ && !recvAll(s, body, req.contentLength_)) { closesocket(s); return; }
    body.resize(std::min(body.size(), req.contentLength_));

    IPAddress local = viaAp ? WiFi.softAPIP() : WiFi.localIP();
    req.client_.local_ = local;
    // Asked for by the PC's address (127.0.0.1:port): to the clock it's the
    // address of the side it came in on, as a browser on that network asks
    // for it (the setup page redirects any other host to 192.168.4.1).
    if (req.host_.startsWith("127.0.0.1") || req.host_.startsWith("localhost")) req.host_ = local.toString();
    req.client_.remote_ = IPAddress(local[0], local[1], local[2], viaAp ? 2 : 50);

    std::vector<UploadPart> files;
    std::string ct = req.contentType_;
    if (ct.rfind("application/x-www-form-urlencoded", 0) == 0) parseQuery(body, true, req.params_);
    else if (ct.rfind("multipart/form-data", 0) == 0) {
        size_t b = ct.find("boundary=");
        std::string boundary = b == std::string::npos ? "" : ct.substr(b + 9);
        if (!boundary.empty() && boundary.front() == '"') boundary = boundary.substr(1, boundary.size() - 2);
        parseMultipart(body, boundary, req.params_, files);
    }

    AsyncCallbackWebHandler* handler = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lk(Sim::clockLock());
        for (auto& h : g_server->handlers_) if (h->canHandle(&req)) { handler = h.get(); break; }
    }
    if (handler) {
        // Uploads arrive in pieces, as over TCP: the main loop runs between
        // them (e.g. the "Updating..." screen), each piece under the lock.
        const size_t CHUNK = 1436;
        for (auto& f : files) {
            if (!handler->onUpload_) continue;
            size_t n = f.data.size(), i = 0;
            do {
                size_t k = std::min(CHUNK, n - i);
                std::lock_guard<std::recursive_mutex> lk(Sim::clockLock());
                handler->onUpload_(&req, String(f.filename.c_str()), i, (uint8_t*)f.data.data() + i, k, i + k >= n);
                i += k;
            } while (i < n);
        }
        if (handler->onBody_ && files.empty() && !body.empty() && ct.rfind("application/x-www-form-urlencoded", 0) != 0) {
            std::lock_guard<std::recursive_mutex> lk(Sim::clockLock());
            handler->onBody_(&req, (uint8_t*)body.data(), body.size(), 0, body.size());
        }
        std::lock_guard<std::recursive_mutex> lk(Sim::clockLock());
        if (handler->onRequest_) handler->onRequest_(&req);
    } else {
        std::lock_guard<std::recursive_mutex> lk(Sim::clockLock());
        if (g_server->notFound_) g_server->notFound_(&req);
    }
    std::string out;
    {
        std::lock_guard<std::recursive_mutex> lk(Sim::clockLock());   // fillers read the clock's memory
        if (!req.response_) req.send(500);   // no handler sent anything
        AsyncWebServerResponse* r = req.response_;
        out = r->head();
        if (req.method_ != HTTP_HEAD) {
            size_t len = r->bodyLength(), done = 0;
            std::vector<uint8_t> chunk(8192);
            while (done < len) {
                size_t n = r->readBody(chunk.data(), std::min(chunk.size(), len - done), done);
                if (!n) break;
                out.append((const char*)chunk.data(), n); done += n;
            }
        }
    }
    sendAll(s, out.data(), out.size());
    shutdown(s, SD_SEND);
    closesocket(s);
}

void listenOn(int port, bool viaAp) {
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    BOOL yes = TRUE;
    setsockopt(ls, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&yes, sizeof(yes));
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((u_short)port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int tries = 0;   // the run before this one (a restart) may still hold the port for a moment
    while (bind(ls, (sockaddr*)&a, sizeof(a)) != 0) {
        if (++tries > 75) { fprintf(stderr, "[sim] cannot listen on 127.0.0.1:%d (in use?)\n", port); std::exit(10); }
        Sleep(200);
    }
    if (listen(ls, 32) != 0) { fprintf(stderr, "[sim] listen failed on 127.0.0.1:%d\n", port); std::exit(10); }
    fprintf(stderr, "[sim] %s at http://127.0.0.1:%d\n", viaAp ? "clock's own Wi-Fi (192.168.4.1)" : "home network", port);
    for (;;) {
        SOCKET c = accept(ls, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;
        std::thread(serveConnection, c, viaAp).detach();
    }
}
} // namespace

void Sim::startWebListeners() {
    static bool started = false;
    if (started) return;
    started = true;
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    std::thread(listenOn, homePort, false).detach();
    std::thread(listenOn, apPort, true).detach();
}
