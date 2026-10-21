/*
 * aether-sha256.h — SHA-256 (FIPS 180-4) for AetherOS package integrity.
 *
 * Written from the FIPS 180-4 specification (no third-party code). Self-test
 * vectors from FIPS 180-4 / the SHA-256 standard test suite are wired into
 * aether_sha256_selftest() and are run at the start of every `aether-pkg
 * create` and `aether-pkg install` — a broken hash means a broken package
 * format, so it must fail loudly rather than silently accept garbage.
 */
#ifndef AETHER_SHA256_H
#define AETHER_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define AETHER_SHA256_DIGEST_LEN 32
#define AETHER_SHA256_HEX_LEN    64   /* 32 bytes, lowercase hex + NUL */

typedef struct {
    uint32_t      state[8];
    uint64_t      bitlen;
    unsigned char buf[64];
    size_t        buflen;
} aether_sha256_ctx;

void aether_sha256_init(aether_sha256_ctx *c);
void aether_sha256_update(aether_sha256_ctx *c, const void *data, size_t len);
void aether_sha256_final(aether_sha256_ctx *c, unsigned char out[32]);

/* One-shot helpers. */
void aether_sha256(const void *data, size_t len, unsigned char out[32]);
void aether_sha256_hex(const void *data, size_t len, char out_hex[65]);

/* Streaming hash of a whole file; returns 0 on success, -1 on read error. */
int  aether_sha256_file(const char *path, unsigned char out[32]);

/* FIPS 180-4 test vectors; returns 0 when all pass. */
int  aether_sha256_selftest(void);

#endif /* AETHER_SHA256_H */
