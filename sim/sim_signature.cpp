// PC stand-in (simulator only - see sim/README.md): UpdateSignature on Windows' own crypto
// (CNG, bcrypt.dll) - the same SHA-256 and the same ECDSA P-256 check as mbedtls on the
// clock. Replaces src/UpdateSignature.cpp (mbedtls isn't on the PC).
#include "UpdateSignature.h"
#include <windows.h>
#include <bcrypt.h>
#include <cstring>
#include <vector>

namespace UpdateSignature {

struct ShaCtx { BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE h = nullptr; };

Sha256::Sha256() : _ctx(new ShaCtx) { reset(); }
Sha256::~Sha256() {
    auto* c = static_cast<ShaCtx*>(_ctx);
    if (c->h) BCryptDestroyHash(c->h);
    if (c->alg) BCryptCloseAlgorithmProvider(c->alg, 0);
    delete c;
}
void Sha256::reset() {
    auto* c = static_cast<ShaCtx*>(_ctx);
    if (c->h) { BCryptDestroyHash(c->h); c->h = nullptr; }
    if (!c->alg) BCryptOpenAlgorithmProvider(&c->alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    BCryptCreateHash(c->alg, &c->h, nullptr, 0, nullptr, 0, 0);
}
void Sha256::add(const uint8_t* p, size_t n) { BCryptHashData(static_cast<ShaCtx*>(_ctx)->h, (PUCHAR)p, (ULONG)n, 0); }
void Sha256::finish(uint8_t out[32]) { BCryptFinishHash(static_cast<ShaCtx*>(_ctx)->h, out, 32, 0); }

// One DER INTEGER -> 32 bytes, big-endian (a leading 0 dropped, short ones padded).
static bool derInt(const uint8_t*& p, const uint8_t* end, uint8_t out[32]) {
    if (end - p < 2 || p[0] != 0x02) return false;
    size_t len = p[1];
    p += 2;
    if ((size_t)(end - p) < len || len == 0 || len > 33) return false;
    const uint8_t* v = p;
    p += len;
    while (len > 32) { if (*v != 0) return false; v++; len--; }
    memset(out, 0, 32);
    memcpy(out + 32 - len, v, len);
    return true;
}

bool verify(const uint8_t hash[32], const uint8_t* sigDer, size_t sigLen, const uint8_t* keyDer, size_t keyLen) {
    // The signature: SEQUENCE { INTEGER r, INTEGER s } -> r || s
    if (sigLen < 8 || sigDer[0] != 0x30 || sigDer[1] + 2u != sigLen) return false;
    const uint8_t* p = sigDer + 2;
    const uint8_t* end = sigDer + sigLen;
    uint8_t rs[64];
    if (!derInt(p, end, rs) || !derInt(p, end, rs + 32) || p != end) return false;
    // The key: a P-256 SubjectPublicKeyInfo ends in 04 || X || Y
    const size_t n = keyLen;
    if (n < 65 || keyDer[n - 65] != 0x04) return false;
    std::vector<uint8_t> blob(sizeof(BCRYPT_ECCKEY_BLOB) + 64);
    auto* hdr = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
    hdr->dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    hdr->cbKey = 32;
    memcpy(blob.data() + sizeof(BCRYPT_ECCKEY_BLOB), keyDer + n - 64, 64);
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    bool ok = false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key, blob.data(), (ULONG)blob.size(), 0) == 0)
        ok = BCryptVerifySignature(key, nullptr, (PUCHAR)hash, 32, rs, 64, 0) == 0;
    if (key) BCryptDestroyKey(key);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

}  // namespace UpdateSignature
