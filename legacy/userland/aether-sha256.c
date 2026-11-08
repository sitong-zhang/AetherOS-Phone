/*
 * aether-sha256.c — SHA-256 (FIPS 180-4), written from the specification.
 *
 * FIPS 180-4 §4.1.2 initial hash values, §4.2.2 the 64 round constants,
 * §6.2.2 the SHA-256 functions (Ch, Maj, Σ0, Σ1, σ0, σ1), §5.1 padding.
 */
#include "aether-sha256.h"

#include <stdio.h>
#include <string.h>

static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static uint32_t rotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }

static void sha256_block(uint32_t h[8], const unsigned char *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[4*i] << 24) | ((uint32_t)p[4*i+1] << 16) |
               ((uint32_t)p[4*i+2] << 8) | (uint32_t)p[4*i+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i-15],7) ^ rotr(w[i-15],18) ^ (w[i-15] >> 3);
        uint32_t s1 = rotr(w[i-2],17) ^ rotr(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + S1 + ch + K[i] + w[i];
        uint32_t S0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
}

void aether_sha256_init(aether_sha256_ctx *c)
{
    c->state[0]=0x6a09e667; c->state[1]=0xbb67ae85; c->state[2]=0x3c6ef372;
    c->state[3]=0xa54ff53a; c->state[4]=0x510e527f; c->state[5]=0x9b05688c;
    c->state[6]=0x1f83d9ab; c->state[7]=0x5be0cd19;
    c->bitlen = 0;
    c->buflen = 0;
}

void aether_sha256_update(aether_sha256_ctx *c, const void *data, size_t len)
{
    const unsigned char *p = data;
    c->bitlen += (uint64_t)len * 8;
    if (c->buflen) {
        size_t need = 64 - c->buflen;
        size_t take = len < need ? len : need;
        memcpy(c->buf + c->buflen, p, take);
        c->buflen += take; p += take; len -= take;
        if (c->buflen == 64) { sha256_block(c->state, c->buf); c->buflen = 0; }
    }
    while (len >= 64) { sha256_block(c->state, p); p += 64; len -= 64; }
    if (len) { memcpy(c->buf, p, len); c->buflen = len; }
}

void aether_sha256_final(aether_sha256_ctx *c, unsigned char out[32])
{
    uint64_t bits = c->bitlen;
    unsigned char pad[72];
    size_t padlen;                       /* 0x80 + zeros + 8-byte length */
    size_t rem = c->buflen;              /* bytes already in buf (0..63)   */

    /* Padding per FIPS 180-4 §5.1: append 0x80, zeros, then the 64-bit
     * big-endian bit length, so the TOTAL (message + padding) is a multiple
     * of 64 bytes. The final block is 64 bytes: [msg tail][0x80][zeros]
     * [8-byte length] — so we append (64 - rem) bytes, or (128 - rem) when
     * the tail already spills past byte 56 into a second block. */
    padlen = (rem < 56) ? (64 - rem) : (128 - rem);
    pad[0] = 0x80;
    memset(pad + 1, 0, padlen - 1);
    for (int i = 0; i < 8; i++)
        pad[padlen - 1 - i] = (unsigned char)(bits >> (8 * i));
    aether_sha256_update(c, pad, padlen);
    /* update() only compresses FULL blocks; after padding exactly one
     * partial block (56..63 bytes) remains in buf — compress it now. */
    if (c->buflen) { sha256_block(c->state, c->buf); c->buflen = 0; }
    for (int i = 0; i < 8; i++) {
        out[4*i]   = (unsigned char)(c->state[i] >> 24);
        out[4*i+1] = (unsigned char)(c->state[i] >> 16);
        out[4*i+2] = (unsigned char)(c->state[i] >> 8);
        out[4*i+3] = (unsigned char)(c->state[i]);
    }
}

void aether_sha256(const void *data, size_t len, unsigned char out[32])
{
    aether_sha256_ctx c;
    aether_sha256_init(&c);
    aether_sha256_update(&c, data, len);
    aether_sha256_final(&c, out);
}

void aether_sha256_hex(const void *data, size_t len, char out_hex[65])
{
    unsigned char d[32];
    aether_sha256(data, len, d);
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out_hex[2*i]   = hx[d[i] >> 4];
        out_hex[2*i+1] = hx[d[i] & 15];
    }
    out_hex[64] = '\0';
}

int aether_sha256_file(const char *path, unsigned char out[32])
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    aether_sha256_ctx c;
    aether_sha256_init(&c);
    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        aether_sha256_update(&c, buf, n);
    int err = ferror(f);
    fclose(f);
    if (err) return -1;
    aether_sha256_final(&c, out);
    return 0;
}

int aether_sha256_selftest(void)
{
    struct { const char *msg; const char *hex; } v[] = {
        { "",    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
        { "abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
        { "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
                 "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1" },
    };
    for (size_t i = 0; i < sizeof(v)/sizeof(v[0]); i++) {
        char hex[65];
        aether_sha256_hex(v[i].msg, strlen(v[i].msg), hex);
        if (strcmp(hex, v[i].hex) != 0) {
            fprintf(stderr, "aether-pkg: SHA-256 self-test FAILED on vector %zu "
                    "(got %s want %s)\n", i, hex, v[i].hex);
            return -1;
        }
    }
    /* Streaming path must agree with one-shot (final() re-uses update()). */
    unsigned char a[32], b[32];
    aether_sha256("hello world", 11, a);
    aether_sha256_ctx c;
    aether_sha256_init(&c);
    aether_sha256_update(&c, "hel", 3);
    aether_sha256_update(&c, "lo ", 3);
    aether_sha256_update(&c, "world", 5);
    aether_sha256_final(&c, b);
    if (memcmp(a, b, 32) != 0) {
        fprintf(stderr, "aether-pkg: SHA-256 streaming != one-shot\n");
        return -1;
    }
    return 0;
}
