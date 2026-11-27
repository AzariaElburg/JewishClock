// UpdateSignature on the clock: mbedtls (part of ESP-IDF). See include/UpdateSignature.h.
#include "UpdateSignature.h"
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>

namespace UpdateSignature {

Sha256::Sha256() : _ctx(new mbedtls_sha256_context) { reset(); }
Sha256::~Sha256() {
    mbedtls_sha256_free(static_cast<mbedtls_sha256_context*>(_ctx));
    delete static_cast<mbedtls_sha256_context*>(_ctx);
}
void Sha256::reset() {
    auto* c = static_cast<mbedtls_sha256_context*>(_ctx);
    mbedtls_sha256_init(c);
    mbedtls_sha256_starts(c, 0);   // 0 = SHA-256 (not 224)
}
void Sha256::add(const uint8_t* p, size_t n) { mbedtls_sha256_update(static_cast<mbedtls_sha256_context*>(_ctx), p, n); }
void Sha256::finish(uint8_t out[32]) { mbedtls_sha256_finish(static_cast<mbedtls_sha256_context*>(_ctx), out); }

bool verify(const uint8_t hash[32], const uint8_t* sigDer, size_t sigLen, const uint8_t* keyDer, size_t keyLen) {
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    bool ok = keyLen && mbedtls_pk_parse_public_key(&pk, keyDer, keyLen) == 0
           && mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, 32, sigDer, sigLen) == 0;
    mbedtls_pk_free(&pk);
    return ok;
}

}  // namespace UpdateSignature
