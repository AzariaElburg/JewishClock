#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// UpdateSignature: is an update package really from the clock's maker?
// (2026-11-14). Every package is signed on the author's PC (utils/update_key.py,
// make_update_package.py): an ECDSA P-256 signature of SHA-256 over the package.
// The clock checks it with the public key in its firmware (UpdateKey.h) and
// installs nothing whose signature doesn't check out - from GitHub or a file.
//
// On the clock: mbedtls (UpdateSignature.cpp). In the simulator: Windows' own
// crypto (sim/sim_signature.cpp) - the same answers for the same bytes.
// ─────────────────────────────────────────────────────────────────────────────
#include <stdint.h>
#include <stddef.h>

namespace UpdateSignature {

// SHA-256 over bytes that arrive in pieces.
class Sha256 {
public:
    Sha256();
    ~Sha256();
    void reset();
    void add(const uint8_t* p, size_t n);
    void finish(uint8_t out[32]);
private:
    void* _ctx;
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
};

// The signature (DER) of this hash made with the private half of this public key (DER)?
// Which key: UpdateKeyState.h (the setting, or one pasted for one install).
bool verify(const uint8_t hash[32], const uint8_t* sigDer, size_t sigLen, const uint8_t* keyDer, size_t keyLen);

}  // namespace UpdateSignature
