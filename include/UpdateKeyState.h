#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// UpdateKeyState: which public key update packages are checked with (2026-11-14,
// the user). It is a setting (AppConfig::updateKey, hex of the DER key), shown
// in Settings > Clock software by its short id, and changed only two ways:
//   - a firmware writes its own key (UpdateKey.h) into it the first time it runs, after
//     every install (the same version again too), and whenever the setting holds no
//     usable key (adopt(); AppConfig::updateKeyBy names the version that wrote it);
//   - Settings' "Paste a key" replaces it (2026-11-17: a new signing key, sent to the
//     family) - checked to be a P-256 public key, its id shown before saving;
//   - nothing else: it isn't a typing field (a lost or extra letter can't happen).
// No key = nothing installed, the same as a wrong key (the user, 2026-11-17; Clear and
// "accepted without validation" are gone, and the update page's key for one install).
// Where to get the key: every release page of the update feed shows it, and
// `python utils/update_key.py public` prints it.
// Portable (the clock and the simulator); the signature check itself is in
// UpdateSignature.cpp (mbedtls) / sim/sim_signature.cpp (Windows CNG).
// ─────────────────────────────────────────────────────────────────────────────
#include <Arduino.h>
#include "Config.h"

namespace UpdateKeyState {
    // At start, after the settings are read: a new firmware writes its own key in.
    // True = the settings changed (save them).
    bool adopt(AppConfig& cfg);
    void use(const char* hex);          // the key to check with ("" = none)
    bool has();                         // a key to check with
    const uint8_t* der(size_t& len);    // the key, DER (len 0 = none)
    String id();                        // its short id (sha-256, 16 hex digits), "" = none
    String idOfHex(const char* hex);    // the same for a key being pasted in Settings ("" = not a key)
    String canonical(const char* hex);  // a pasted key as plain lower-case hex ("" = not a key)
    bool parseHex(const char* hex, uint8_t* out, size_t cap, size_t& len);
}
