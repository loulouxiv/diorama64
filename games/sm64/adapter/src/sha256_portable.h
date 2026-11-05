/* SHA-256 (FIPS 180-4) for hosts without CommonCrypto (Linux, Android). Apple builds keep CommonCrypto. */
#ifndef SHA256_PORTABLE_H
#define SHA256_PORTABLE_H
#include <stddef.h>
#include <stdint.h>

#define SHA256_PORTABLE_DIGEST_LENGTH 32

typedef struct {
    uint32_t h[8];
    uint64_t bytes;
    unsigned char block[64];
    size_t fill;
} sha256_portable_ctx;

void sha256_portable_init(sha256_portable_ctx *c);
void sha256_portable_update(sha256_portable_ctx *c, const void *data, size_t len);
void sha256_portable_final(sha256_portable_ctx *c, unsigned char out[SHA256_PORTABLE_DIGEST_LENGTH]);

#endif
