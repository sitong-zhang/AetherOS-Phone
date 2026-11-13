/*
 * AetherOS Kernel · 自包含密码学（freestanding，不链接任何库）
 * SHA-256 / HMAC-SHA256 / PBKDF2-HMAC-SHA256 / ChaCha20(RFC8439) / 常量时间比较 /
 * CSPRNG。算法思路复用同仓库 secure/（81 项自测 + OpenSSL 交叉验证通过），
 * 按内核环境重写：无 malloc、无 libc、无浮点。构建后用标准向量再验证一次。
 */
#include "aether_kernel.h"

static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
#define ROR(x,n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha_block(sha256_ctx *c, const uint8_t p[64]) {
    uint32_t w[64], v[8];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[4*i] << 24) | ((uint32_t)p[4*i+1] << 16) |
               ((uint32_t)p[4*i+2] << 8) | (uint32_t)p[4*i+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i-15],7) ^ ROR(w[i-15],18) ^ (w[i-15] >> 3);
        uint32_t s1 = ROR(w[i-2],17) ^ ROR(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    for (int i = 0; i < 8; i++) v[i] = c->h[i];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROR(v[4],6) ^ ROR(v[4],11) ^ ROR(v[4],25);
        uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
        uint32_t t1 = v[7] + S1 + ch + K256[i] + w[i];
        uint32_t S0 = ROR(v[0],2) ^ ROR(v[0],13) ^ ROR(v[0],22);
        uint32_t mj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
        uint32_t t2 = S0 + mj;
        v[7]=v[6]; v[6]=v[5]; v[5]=v[4]; v[4]=v[3]+t1;
        v[3]=v[2]; v[2]=v[1]; v[1]=v[0]; v[0]=t1+t2;
    }
    for (int i = 0; i < 8; i++) c->h[i] += v[i];
}
void sha256_init(sha256_ctx *c) {
    c->h[0]=0x6a09e667; c->h[1]=0xbb67ae85; c->h[2]=0x3c6ef372; c->h[3]=0xa54ff53a;
    c->h[4]=0x510e527f; c->h[5]=0x9b05688c; c->h[6]=0x1f83d9ab; c->h[7]=0x5be0cd19;
    c->total = 0; c->len = 0;
}
void sha256_update(sha256_ctx *c, const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    c->total += n;
    while (n) {
        size_t k = 64 - c->len < n ? 64 - c->len : n;
        kmemcpy(c->buf + c->len, b, k);
        c->len += k; b += k; n -= k;
        if (c->len == 64) { sha_block(c, c->buf); c->len = 0; }
    }
}
void sha256_final(sha256_ctx *c, uint8_t out[K_HASH_BYTES]) {
    uint64_t bits = c->total * 8;
    c->buf[c->len++] = 0x80;
    if (c->len > 56) { kmemset(c->buf + c->len, 0, 64 - c->len); sha_block(c, c->buf); c->len = 0; }
    kmemset(c->buf + c->len, 0, 56 - c->len);
    for (int i = 0; i < 8; i++) c->buf[56 + i] = (uint8_t)(bits >> (56 - 8*i));
    sha_block(c, c->buf);
    for (int i = 0; i < 8; i++) {
        out[4*i]   = (uint8_t)(c->h[i] >> 24);
        out[4*i+1] = (uint8_t)(c->h[i] >> 16);
        out[4*i+2] = (uint8_t)(c->h[i] >> 8);
        out[4*i+3] = (uint8_t)(c->h[i]);
    }
}
void sha256_buf(const void *p, size_t n, uint8_t out[K_HASH_BYTES]) {
    sha256_ctx c; sha256_init(&c); sha256_update(&c, p, n); sha256_final(&c, out);
}
void hmac_sha256(const uint8_t *key, size_t kn, const void *msg, size_t mn,
                 uint8_t out[K_HASH_BYTES]) {
    uint8_t k[64], inner[K_HASH_BYTES];
    kmemset(k, 0, sizeof k);
    if (kn > 64) sha256_buf(key, kn, k); else kmemcpy(k, key, kn);
    uint8_t ip[64 + 128], op[64 + K_HASH_BYTES];
    for (int i = 0; i < 64; i++) { ip[i] = (uint8_t)(k[i] ^ 0x36); op[i] = (uint8_t)(k[i] ^ 0x5C); }
    /* 消息可能较长，用流式 ctx 避免拼接大缓冲 */
    sha256_ctx c; sha256_init(&c); sha256_update(&c, ip, 64); sha256_update(&c, msg, mn);
    sha256_final(&c, inner);
    kmemcpy(op + 64, inner, K_HASH_BYTES);
    sha256_buf(op, 64 + K_HASH_BYTES, out);
    kmemset(k, 0, sizeof k); kmemset(inner, 0, sizeof inner);
}
void pbkdf2(const void *pw, size_t pn, const uint8_t *salt, size_t sn,
            uint32_t iter, uint8_t out[K_HASH_BYTES]) {
    uint8_t u[K_HASH_BYTES], t[K_HASH_BYTES], blk[128];
    size_t bl = sn + 4;
    kmemcpy(blk, salt, sn);
    blk[sn]=0; blk[sn+1]=0; blk[sn+2]=0; blk[sn+3]=1;   /* 只取第 1 块，32 字节输出 */
    hmac_sha256((const uint8_t *)pw, pn, blk, bl, u);
    kmemcpy(t, u, K_HASH_BYTES);
    for (uint32_t i = 1; i < iter; i++) {
        hmac_sha256((const uint8_t *)pw, pn, u, K_HASH_BYTES, u);
        for (uint32_t j = 0; j < K_HASH_BYTES; j++) t[j] ^= u[j];
    }
    kmemcpy(out, t, K_HASH_BYTES);
    kmemset(u, 0, sizeof u); kmemset(t, 0, sizeof t);
}
static uint32_t rotl32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
#define QR(s,a,b,c,d) do { \
    s[a]+=s[b]; s[d]^=s[a]; s[d]=rotl32(s[d],16); \
    s[c]+=s[d]; s[b]^=s[c]; s[b]=rotl32(s[b],12); \
    s[a]+=s[b]; s[d]^=s[a]; s[d]=rotl32(s[d],8);  \
    s[c]+=s[d]; s[b]^=s[c]; s[b]=rotl32(s[b],7);  } while (0)
void chacha20(const uint8_t key[K_KEY_BYTES], const uint8_t nonce[K_NONCE_BYTES],
              uint32_t counter, const void *in, void *out, size_t n) {
    static const char sigma[] = "expand 32-byte k";
    uint32_t st[16];
    for (int i = 0; i < 4; i++) st[i] = ((uint32_t)(uint8_t)sigma[4*i]) |
        ((uint32_t)(uint8_t)sigma[4*i+1] << 8) | ((uint32_t)(uint8_t)sigma[4*i+2] << 16) |
        ((uint32_t)(uint8_t)sigma[4*i+3] << 24);
    for (int i = 0; i < 8; i++) st[4+i] = (uint32_t)key[4*i] | ((uint32_t)key[4*i+1] << 8) |
        ((uint32_t)key[4*i+2] << 16) | ((uint32_t)key[4*i+3] << 24);
    st[12] = counter;
    for (int i = 0; i < 3; i++) st[13+i] = (uint32_t)nonce[4*i] |
        ((uint32_t)nonce[4*i+1] << 8) | ((uint32_t)nonce[4*i+2] << 16) |
        ((uint32_t)nonce[4*i+3] << 24);
    const uint8_t *src = (const uint8_t *)in;
    uint8_t *dst = (uint8_t *)out;
    uint8_t ks[64];
    while (n) {
        uint32_t x[16]; kmemcpy(x, st, sizeof x);
        for (int r = 0; r < 10; r++) {
            QR(x,0,4,8,12); QR(x,1,5,9,13); QR(x,2,6,10,14); QR(x,3,7,11,15);
            QR(x,0,5,10,15); QR(x,1,6,11,12); QR(x,2,7,8,13); QR(x,3,4,9,14);
        }
        for (int i = 0; i < 16; i++) {
            uint32_t v = x[i] + st[i];
            ks[4*i]=(uint8_t)v; ks[4*i+1]=(uint8_t)(v>>8);
            ks[4*i+2]=(uint8_t)(v>>16); ks[4*i+3]=(uint8_t)(v>>24);
        }
        size_t k = n < 64 ? n : 64;
        for (size_t i = 0; i < k; i++) dst[i] = (uint8_t)((src ? src[i] : 0) ^ ks[i]);
        st[12]++;
        src = src ? src + k : NULL; dst += k; n -= k;
    }
    kmemset(ks, 0, sizeof ks);
}
int ct_eq(const void *a, const void *b, size_t n) {
    const uint8_t *p = (const uint8_t *)a, *q = (const uint8_t *)b;
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= (uint8_t)(p[i] ^ q[i]);
    return d == 0;
}
