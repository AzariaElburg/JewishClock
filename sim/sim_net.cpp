// =============================================================================
// sim_net.cpp - the full simulator's UDP and name lookup (jcclock): what lwIP
// does under the clock's NTPManager.cpp, done by the PC for real. Packets go
// out only while the simulated clock is on a network with internet
// (Sim::wifiInternet); otherwise they are lost, as with no network.
// =============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <cstdlib>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <WiFiUdp.h>
#include <lwip/dns.h>
#include <lwip/tcpip.h>
#include <thread>
#include <string>
#include <map>
#include <mutex>
#include "sim_clock.h"

std::atomic<int> Sim::localNtpPort{0};

static void ensureWsa() { static bool done = false; if (!done) { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); done = true; } }

// Which name each address was looked up for (the world's "ntp:NAME" blocks one server).
static std::mutex g_namesLock;
static std::map<uint32_t, std::string> g_names;
static std::string nameOf(uint32_t ip) {
    std::lock_guard<std::mutex> lk(g_namesLock);
    auto it = g_names.find(ip);
    return it == g_names.end() ? std::string() : it->second;
}

// ── UDP ───────────────────────────────────────────────────────────────────────
uint8_t WiFiUDP::begin(uint16_t port) {
    ensureWsa();
    stop();
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return 0;
    u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);   // parsePacket() never waits, as on the clock
    // Not bound to `port`: binding on all interfaces makes Windows' firewall
    // ask the user to allow the program (once per build - every version's is
    // another program). The first sendto() picks a port by itself, and the
    // server's answer comes back to it - all an NTP client needs.
    (void)port;
    sock_ = (intptr_t)s;
    return 1;
}
void WiFiUDP::stop() { if (sock_ != -1) { closesocket((SOCKET)sock_); sock_ = -1; } }
int WiFiUDP::beginPacket(IPAddress ip, uint16_t port) { toIp_ = ip.addr(); toPort_ = port; out_.clear(); return 1; }
size_t WiFiUDP::write(const uint8_t* buf, size_t n) { out_.append((const char*)buf, n); return n; }
int WiFiUDP::endPacket() {
    if (sock_ == -1 || !Sim::wifiInternet()) { out_.clear(); return 0; }
    // NTP blocked (the world's "blocked"): the packet goes out and never gets
    // an answer - as with a router dropping port 123. Sent "successfully".
    // "ntp:NAME": only the server looked up by that name (the address it got).
    if (toPort_ == 123 && (Sim::serviceBlocked("ntp") || Sim::serviceBlocked("ntp:" + nameOf(toIp_)))) {
        size_t n = out_.size(); out_.clear(); return (int)n > 0;
    }
    uint16_t port = toPort_;
    uint32_t ip = toIp_;
    if (port == 123 && (toIp_ & 0xFF) == 127 && Sim::localNtpPort > 0) port = (uint16_t)Sim::localNtpPort.load();   // the tests' time server
    // A test run's own time server (sim/time_relay.py, 2026-11-21): every other time question goes there,
    // not to pool.ntp.org & co - dozens of test clocks from one home address got it rate-limited.
    static const int relay = getenv("JC_NTP_RELAY") ? atoi(getenv("JC_NTP_RELAY")) : 0;
    // Only for a time server's name (pool.ntp.org, time.cloudflare.com, time.windows.com...): a question
    // to anything else (example.com) goes out and gets the real nothing - the server check must see that.
    const std::string asked = nameOf(toIp_);
    const bool timeServer = asked.find("ntp") != std::string::npos || asked.rfind("time.", 0) == 0;
    relayFor_ = 0;   // per question: a stale one made a local test server's answer look as if from the last relayed name (2026-11-22)
    if (port == 123 && (toIp_ & 0xFF) != 127 && relay > 0 && timeServer) {
        relayFor_ = toIp_;
        port = (uint16_t)relay;
        ip = htonl(INADDR_LOOPBACK);
    }
    sockaddr_in to{}; to.sin_family = AF_INET; to.sin_port = htons(port); to.sin_addr.s_addr = ip;   // lwIP order = network order
    int n = sendto((SOCKET)sock_, out_.data(), (int)out_.size(), 0, (sockaddr*)&to, sizeof(to));
    out_.clear();
    return n > 0 ? 1 : 0;
}
int WiFiUDP::parsePacket() {
    in_.clear(); inPos_ = 0;
    if (sock_ == -1) return 0;
    sockaddr_in me{}; int len = sizeof(me);
    if (getsockname((SOCKET)sock_, (sockaddr*)&me, &len) != 0) return 0;   // nothing sent yet: no port, nothing to receive
    char buf[1500];
    sockaddr_in from{}; int fromLen = sizeof(from);
    int n = recvfrom((SOCKET)sock_, buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen);
    if (n <= 0 || !Sim::wifiInternet()) return 0;
    from_ = from.sin_addr.s_addr;
    if (relayFor_ && from_ == htonl(INADDR_LOOPBACK)) from_ = relayFor_;   // the relay's answer: as from the server asked
    in_.assign(buf, n);
    return n;
}
int WiFiUDP::read(uint8_t* buf, size_t n) {
    size_t k = std::min(n, in_.size() - inPos_);
    memcpy(buf, in_.data() + inPos_, k); inPos_ += k;
    return (int)k;
}
int WiFiUDP::available() { return (int)(in_.size() - inPos_); }

// ── lwIP's task and name lookup ───────────────────────────────────────────────
err_t tcpip_callback(tcpip_callback_fn fn, void* ctx) {
    std::thread([fn, ctx] { fn(ctx); }).detach();
    return ERR_OK;
}
err_t dns_gethostbyname_addrtype(const char* hostname, ip_addr_t*, dns_found_callback found, void* arg, uint8_t) {
    if (!hostname || !found) return ERR_ARG;
    ensureWsa();
    std::string host = hostname;
    std::thread([host, found, arg] {
        if (!Sim::wifiInternet()) { found(host.c_str(), nullptr, arg); return; }
        addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) { found(host.c_str(), nullptr, arg); return; }
        ip_addr_t ip{}; ip.type = 0;
        ip.ip4.addr = ((sockaddr_in*)res->ai_addr)->sin_addr.s_addr;
        freeaddrinfo(res);
        { std::lock_guard<std::mutex> lk(g_namesLock); g_names[ip.ip4.addr] = host; }
        found(host.c_str(), &ip, arg);
    }).detach();
    return ERR_INPROGRESS;
}
err_t dns_gethostbyname(const char* hostname, ip_addr_t* addr, dns_found_callback found, void* arg) {
    return dns_gethostbyname_addrtype(hostname, addr, found, arg, LWIP_DNS_ADDRTYPE_IPV4);
}
