// UpdateKeyState - see include/UpdateKeyState.h.
#include "UpdateKeyState.h"
#include "UpdateKey.h"
#include "UpdateSignature.h"
#include "Version.h"
#include "Updater.h"
#include "StringTools.h"
#include "DiagnosticLog.h"

namespace {
uint8_t s_der[128];
size_t  s_len = 0;

String hexOf(const uint8_t* p, size_t n) {
    static const char H[] = "0123456789abcdef";
    String s;
    for (size_t i = 0; i < n; i++) { s += H[p[i] >> 4]; s += H[p[i] & 15]; }
    return s;
}
String idOf(const uint8_t* p, size_t n) {
    UpdateSignature::Sha256 sha;
    sha.add(p, n);
    uint8_t h[32];
    sha.finish(h);
    return hexOf(h, 8);   // as utils/update_key.py key_id()
}
}  // namespace

namespace UpdateKeyState {

bool parseHex(const char* hex, uint8_t* out, size_t cap, size_t& len) {
    len = 0;
    if (!hex) return false;
    int hi = -1;
    for (const char* c = hex; *c; c++) {
        int v = (*c >= '0' && *c <= '9') ? *c - '0' : (*c >= 'a' && *c <= 'f') ? *c - 'a' + 10 : (*c >= 'A' && *c <= 'F') ? *c - 'A' + 10 : -1;
        if (v < 0) { if (*c == ' ' || *c == '\n' || *c == '\r' || *c == ',' || *c == ':') continue; return false; }
        if (hi < 0) { hi = v; continue; }
        if (len >= cap) return false;
        out[len++] = (uint8_t)(hi << 4 | v);
        hi = -1;
    }
    // A P-256 public key in DER: SEQUENCE ... ending in 04 || X || Y
    return hi < 0 && len >= 65 && out[0] == 0x30 && out[len - 65] == 0x04;
}

bool adopt(AppConfig& cfg) {
    uint8_t tmp[128];
    size_t n = 0;
    bool usable = parseHex(cfg.updateKey, tmp, sizeof(tmp), n) && n > 0;   // none, or not a key (a hand-edited file): the firmware's own
    if (usable && strcmp(cfg.updateKeyBy, FIRMWARE_VERSION) == 0 && !Updater::justInstalled()) { use(cfg.updateKey); return false; }
    String hex = hexOf(UPDATE_PUBLIC_KEY_DER, sizeof(UPDATE_PUBLIC_KEY_DER));
    StringTools::utf8Copy(cfg.updateKey, hex.c_str());
    StringTools::utf8Copy(cfg.updateKeyBy, FIRMWARE_VERSION);
    use(cfg.updateKey);
    DiagnosticLog::record("OTA", "update key %s set by version %s", id().c_str(), FIRMWARE_VERSION);
    return true;
}

void use(const char* hex) {
    if (!parseHex(hex, s_der, sizeof(s_der), s_len)) s_len = 0;
}

bool has() { return s_len > 0; }

const uint8_t* der(size_t& len) { len = s_len; return s_der; }

String id() { return s_len ? idOf(s_der, s_len) : String(); }

String canonical(const char* hex) {
    uint8_t d[128];
    size_t n;
    return parseHex(hex, d, sizeof(d), n) ? hexOf(d, n) : String();
}

String idOfHex(const char* hex) {
    uint8_t d[128];
    size_t n;
    return parseHex(hex, d, sizeof(d), n) ? idOf(d, n) : String();
}

}  // namespace UpdateKeyState
