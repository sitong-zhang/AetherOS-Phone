/*
 * aether_crypto.c - SHA-256 / HMAC / PBKDF2 / ChaCha20 / encrypt-then-MAC / CSPRNG
 *
 * 全部为公开标准的忠实实现，便于用 openssl(1) 交叉验证：
 *   SHA-256        : FIPS 180-4
 *   HMAC           : RFC 2104
 *   PBKDF2         : RFC 2898 (HMAC-SHA256)
 *   ChaCha20       : RFC 8439 (IETF 变体：96-bit nonce + 32-bit counter)
 */
#define _GNU_SOURCE
#include "aether_crypto.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <sys/random.h>

/* ------------------------------------------------------------------ SHA-256 */
static const uint32_t K[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

#define ROR32(x,n) (((x) >> (n)) | ((x) << (32u-(n))))
#define LOAD32(p)  ((uint32_t)(p)[0]<<24 | (uint32_t)(p)[1]<<16 | (uint32_t)(p)[2]<<8 | (uint32_t)(p)[3])
#define STORE32(p,v) do { (p)[0]=(uint8_t)((v)>>24); (p)[1]=(uint8_t)((v)>>16); \
                          (p)[2]=(uint8_t)((v)>>8);  (p)[3]=(uint8_t)(v); } while(0)

static void sha256_block(ae_sha256_ctx *c, const uint8_t p[64])
{
    uint32_t w[64], a,b,cc,d,e,f,g,h, t1,t2;
    uint32_t i;
    for (i = 0; i < 16; i++) w[i] = LOAD32(p + 4*i);
    for (i = 16; i < 64; i++) {
        uint32_t s0 = ROR32(w[i-15],7) ^ ROR32(w[i-15],18) ^ (w[i-15] >> 3);
        uint32_t s1 = ROR32(w[i-2],17) ^ ROR32(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a=c->h[0]; b=c->h[1]; cc=c->h[2]; d=c->h[3];
    e=c->h[4]; f=c->h[5]; g=c->h[6]; h=c->h[7];
    for (i = 0; i < 64; i++) {
        uint32_t S1 = ROR32(e,6) ^ ROR32(e,11) ^ ROR32(e,25);
        uint32_t ch = (e & f) ^ (~e & g);
        t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = ROR32(a,2) ^ ROR32(a,13) ^ ROR32(a,22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        t2 = S0 + maj;
        h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
    }
    c->h[0]+=a; c->h[1]+=b; c->h[2]+=cc; c->h[3]+=d;
    c->h[4]+=e; c->h[5]+=f; c->h[6]+=g; c->h[7]+=h;
}

void ae_sha256_init(ae_sha256_ctx *c)
{
    c->h[0]=0x6a09e667u; c->h[1]=0xbb67ae85u; c->h[2]=0x3c6ef372u; c->h[3]=0xa54ff53au;
    c->h[4]=0x510e527fu; c->h[5]=0x9b05688cu; c->h[6]=0x1f83d9abu; c->h[7]=0x5be0cd19u;
    c->total = 0; c->buflen = 0;
}

void ae_sha256_update(ae_sha256_ctx *c, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    c->total += (uint64_t)len;
    while (len) {
        size_t take = 64 - c->buflen;
        if (take > len) take = len;
        memcpy(c->buf + c->buflen, p, take);
        c->buflen += take; p += take; len -= take;
        if (c->buflen == 64) { sha256_block(c, c->buf); c->buflen = 0; }
    }
}

void ae_sha256_final(ae_sha256_ctx *c, uint8_t out[AE_SHA256_LEN])
{
    uint64_t bits = c->total * 8u;
    uint8_t pad = 0x80u;
    ae_sha256_update(c, &pad, 1);
    uint8_t zero = 0;
    while (c->buflen != 56) ae_sha256_update(c, &zero, 1);
    uint8_t tail[8];
    int i;
    for (i = 0; i < 8; i++) tail[i] = (uint8_t)(bits >> (56 - 8*i));
    /* 此时 c->buflen==56：把 8 字节长度追加到 padding 块尾部即构成最后一个 64B 块 */
    memcpy(c->buf + 56, tail, 8);
    sha256_block(c, c->buf);
    for (i = 0; i < 8; i++) STORE32(out + 4*i, c->h[i]);
    ae_zero(c, sizeof(*c));
}

void ae_sha256(const void *data, size_t len, uint8_t out[AE_SHA256_LEN])
{
    ae_sha256_ctx c;
    ae_sha256_init(&c);
    ae_sha256_update(&c, data, len);
    ae_sha256_final(&c, out);
}

void ae_sha256_file(const char *path, uint8_t out[AE_SHA256_LEN], int *ok)
{
    FILE *f = fopen(path, "rb");
    if (!f) { if (ok) *ok = 0; memset(out, 0, AE_SHA256_LEN); return; }
    ae_sha256_ctx c; ae_sha256_init(&c);
    uint8_t buf[8192]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) ae_sha256_update(&c, buf, n);
    if (ferror(f)) { fclose(f); if (ok) *ok = 0; memset(out, 0, AE_SHA256_LEN); return; }
    fclose(f);
    ae_sha256_final(&c, out);
    if (ok) *ok = 1;
}

/* --------------------------------------------------------------------- HMAC */
void ae_hmac_sha256(const uint8_t *key, size_t keylen,
                    const void *data, size_t len,
                    uint8_t out[AE_SHA256_LEN])
{
    uint8_t k[64], inner[32];
    memset(k, 0, sizeof k);
    if (keylen > 64) ae_sha256(key, keylen, k);
    else memcpy(k, key, keylen);
    uint8_t ipad[64], opad[64];
    size_t i;
    for (i = 0; i < 64; i++) { ipad[i]=k[i]^0x36u; opad[i]=k[i]^0x5cu; }
    ae_sha256_ctx c; ae_sha256_init(&c);
    ae_sha256_update(&c, ipad, 64);
    ae_sha256_update(&c, data, len);
    ae_sha256_final(&c, inner);
    ae_sha256_init(&c);
    ae_sha256_update(&c, opad, 64);
    ae_sha256_update(&c, inner, 32);
    ae_sha256_final(&c, out);
    ae_zero(k, sizeof k); ae_zero(ipad, sizeof ipad); ae_zero(opad, sizeof opad);
    ae_zero(inner, sizeof inner);
}

/* ------------------------------------------------------------------- PBKDF2 */
int ae_pbkdf2(const char *pw, size_t pwlen,
              const uint8_t *salt, size_t saltlen,
              uint32_t iter, uint8_t out[AE_KEY_LEN])
{
    if (!pw || !salt || iter == 0) return -1;
    uint8_t u[32], t[32];
    uint8_t block[AE_SALT_LEN + 4];
    if (saltlen > AE_SALT_LEN) saltlen = AE_SALT_LEN; /* 本实现单块 salt，接口约束见文档 */
    memcpy(block, salt, saltlen);
    block[saltlen+0]=0; block[saltlen+1]=0; block[saltlen+2]=0; block[saltlen+3]=1;
    ae_hmac_sha256((const uint8_t*)pw, pwlen, block, saltlen+4, u);
    memcpy(t, u, 32);
    uint32_t j;
    for (j = 1; j < iter; j++) {
        ae_hmac_sha256((const uint8_t*)pw, pwlen, u, 32, u);
        size_t i;
        for (i = 0; i < 32; i++) t[i] ^= u[i];
    }
    memcpy(out, t, 32);
    ae_zero(u, sizeof u); ae_zero(t, sizeof t); ae_zero(block, sizeof block);
    return 0;
}

/* ----------------------------------------------------------------- ChaCha20 */
#define ROL32(x,n) (((x) << (n)) | ((x) >> (32u-(n))))
#define QR(a,b,c,d) do { \
    (a)+=(b); (d)^=(a); (d)=ROL32((d),16); \
    (c)+=(d); (b)^=(c); (b)=ROL32((b),12); \
    (a)+=(b); (d)^=(a); (d)=ROL32((d),8);  \
    (c)+=(d); (b)^=(c); (b)=ROL32((b),7);  } while(0)

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static void st32(uint8_t *p, uint32_t v)
{
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}

static void chacha20_block(const uint8_t key[32], const uint8_t nonce[12],
                           uint32_t counter, uint8_t ks[64])
{
    uint32_t s[16], x[16];
    s[0]=0x61707865u; s[1]=0x3320646eu; s[2]=0x79622d32u; s[3]=0x6b206574u;
    int i;
    for (i = 0; i < 8; i++) s[4+i] = le32(key + 4*i);
    s[12] = counter;
    s[13] = le32(nonce); s[14] = le32(nonce+4); s[15] = le32(nonce+8);
    memcpy(x, s, sizeof s);
    for (i = 0; i < 10; i++) {         /* 20 轮 = 10 个双轮 */
        QR(x[0],x[4],x[8], x[12]); QR(x[1],x[5],x[9], x[13]);
        QR(x[2],x[6],x[10],x[14]); QR(x[3],x[7],x[11],x[15]);
        QR(x[0],x[5],x[10],x[15]); QR(x[1],x[6],x[11],x[12]);
        QR(x[2],x[7],x[8], x[13]); QR(x[3],x[4],x[9], x[14]);
    }
    for (i = 0; i < 16; i++) st32(ks + 4*i, x[i] + s[i]);
    ae_zero(x, sizeof x); ae_zero(s, sizeof s);
}

void ae_chacha20(const uint8_t key[AE_KEY_LEN], const uint8_t nonce[AE_NONCE_LEN],
                 uint32_t counter, const uint8_t *in, uint8_t *out, size_t len)
{
    uint8_t ks[64];
    size_t off = 0;
    while (off < len) {
        chacha20_block(key, nonce, counter, ks);
        size_t n = len - off < 64 ? len - off : 64;
        size_t i;
        for (i = 0; i < n; i++) out[off+i] = (in ? in[off+i] : 0) ^ ks[i];
        off += n; counter++;
    }
    ae_zero(ks, sizeof ks);
}

/* --------------------------------------------------------------------- CSPRNG */
int ae_random(uint8_t *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t r = getrandom(buf + off, len - off, 0);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) return -1;
        off += (size_t)r;
    }
    return 0;
}

/* --------------------------------------------------------------- seal / open */
static void derive_subkeys(const uint8_t key[AE_KEY_LEN],
                           uint8_t enc[AE_KEY_LEN], uint8_t mac[AE_KEY_LEN])
{
    static const char *L1 = "aether/enc/v1", *L2 = "aether/mac/v1";
    ae_hmac_sha256(key, AE_KEY_LEN, L1, strlen(L1), enc);
    ae_hmac_sha256(key, AE_KEY_LEN, L2, strlen(L2), mac);
}

int ae_seal(const uint8_t key[AE_KEY_LEN],
            const uint8_t *plain, size_t len,
            const uint8_t *aad, size_t aadlen,
            uint8_t **out, size_t *outlen)
{
    if (!out || !outlen) return -1;
    *out = NULL; *outlen = 0;
    uint8_t enc[32], mac[32], nonce[12], tag[32];
    derive_subkeys(key, enc, mac);
    if (ae_random(nonce, sizeof nonce) != 0) goto fail;
    size_t total = AE_NONCE_LEN + len + AE_TAG_LEN;
    uint8_t *blob = (uint8_t*)malloc(total ? total : 1);
    if (!blob) goto fail;
    memcpy(blob, nonce, AE_NONCE_LEN);
    uint8_t *ct = blob + AE_NONCE_LEN;
    ae_chacha20(enc, nonce, 0, plain, ct, len);
    /* tag = HMAC(mac, aad || nonce || ciphertext) */
    ae_sha256_ctx c; ae_sha256_init(&c);
    /* 用 HMAC 一次性构造：拼 aad+nonce+ct 到临时缓冲 */
    size_t mlen = aadlen + AE_NONCE_LEN + len;
    uint8_t *m = (uint8_t*)malloc(mlen ? mlen : 1);
    if (!m) { free(blob); goto fail; }
    if (aadlen) memcpy(m, aad, aadlen);
    memcpy(m + aadlen, nonce, AE_NONCE_LEN);
    if (len) memcpy(m + aadlen + AE_NONCE_LEN, ct, len);
    ae_hmac_sha256(mac, 32, m, mlen, tag);
    ae_zero(m, mlen); free(m);
    memcpy(blob + AE_NONCE_LEN + len, tag, AE_TAG_LEN);
    *out = blob; *outlen = total;
    ae_zero(enc, sizeof enc); ae_zero(mac, sizeof mac); ae_zero(tag, sizeof tag);
    return 0;
fail:
    ae_zero(enc, sizeof enc); ae_zero(mac, sizeof mac);
    return -1;
}

int ae_open(const uint8_t key[AE_KEY_LEN],
            const uint8_t *blob, size_t bloblen,
            const uint8_t *aad, size_t aadlen,
            uint8_t **out, size_t *outlen)
{
    if (!out || !outlen || !blob) return -1;
    *out = NULL; *outlen = 0;
    if (bloblen < AE_NONCE_LEN + AE_TAG_LEN) return -1;
    size_t len = bloblen - AE_NONCE_LEN - AE_TAG_LEN;
    const uint8_t *nonce = blob;
    const uint8_t *ct = blob + AE_NONCE_LEN;
    const uint8_t *tag = blob + AE_NONCE_LEN + len;
    uint8_t enc[32], mac[32], calc[32];
    derive_subkeys(key, enc, mac);
    size_t mlen = aadlen + AE_NONCE_LEN + len;
    uint8_t *m = (uint8_t*)malloc(mlen ? mlen : 1);
    if (!m) goto fail;
    if (aadlen) memcpy(m, aad, aadlen);
    memcpy(m + aadlen, nonce, AE_NONCE_LEN);
    if (len) memcpy(m + aadlen + AE_NONCE_LEN, ct, len);
    ae_hmac_sha256(mac, 32, m, mlen, calc);
    ae_zero(m, mlen); free(m);
    if (!ae_ct_eq(calc, tag, AE_TAG_LEN)) goto fail;   /* 先验后解，失败不产出明文 */
    uint8_t *pt = (uint8_t*)malloc(len ? len : 1);
    if (!pt) goto fail;
    ae_chacha20(enc, nonce, 0, ct, pt, len);
    *out = pt; *outlen = len;
    ae_zero(enc, sizeof enc); ae_zero(mac, sizeof mac); ae_zero(calc, sizeof calc);
    return 0;
fail:
    ae_zero(enc, sizeof enc); ae_zero(mac, sizeof mac); ae_zero(calc, sizeof calc);
    return -1;
}

/* ------------------------------------------------------------------- 工具函数 */
int ae_ct_eq(const void *a, const void *b, size_t n)
{
    const volatile uint8_t *x = (const volatile uint8_t*)a;
    const volatile uint8_t *y = (const volatile uint8_t*)b;
    uint8_t d = 0;
    size_t i;
    for (i = 0; i < n; i++) d |= (uint8_t)(x[i] ^ y[i]);
    return d == 0 ? 1 : 0;
}

void ae_zero(void *p, size_t n)
{
    if (!p) return;
    /* volatile 函数指针：编译器无法把这次 memset 优化掉，且不依赖任何 libc 扩展 */
    static void *(*volatile vp)(void *, int, size_t) = memset;
    (void)vp(p, 0, n);
}

void ae_hex(const uint8_t *buf, size_t len, char *out)
{
    static const char H[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < len; i++) {
        out[2*i]   = H[buf[i] >> 4];
        out[2*i+1] = H[buf[i] & 0xf];
    }
    out[2*len] = '\0';
}
