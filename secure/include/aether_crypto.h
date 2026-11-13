/*
 * aether_crypto.h - AetherOS Secure Core 密码学基座（纯自包含、无外部链接依赖）
 *
 * 提供：SHA-256 / HMAC-SHA256 / PBKDF2-HMAC-SHA256 / ChaCha20(RFC 8439 IETF)
 *       / encrypt-then-MAC 密封 / CSPRNG / 常量时间比较 / 安全清零 / 十六进制。
 *
 * 设计立场：这是自研系统本体的密码学原语，实现严格遵循公开标准，
 *          并用 openssl(1) 命令行与标准测试向量做交叉验证（见 test_secure.c），
 *          不接受「玩具算法」，也不把口令/密钥以明文落盘。
 */
#ifndef AETHER_CRYPTO_H
#define AETHER_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#define AE_SHA256_LEN   32u
#define AE_KEY_LEN      32u
#define AE_SALT_LEN     16u
#define AE_NONCE_LEN    12u
#define AE_TAG_LEN      32u
#define AE_PBKDF2_ITER  100000u   /* 口令验证哈希迭代 */
#define AE_KEK_ITER     200000u   /* 密钥包装迭代 */

typedef struct {
    uint32_t h[8];
    uint64_t total;
    uint8_t  buf[64];
    size_t   buflen;
} ae_sha256_ctx;

void ae_sha256_init(ae_sha256_ctx *c);
void ae_sha256_update(ae_sha256_ctx *c, const void *data, size_t len);
void ae_sha256_final(ae_sha256_ctx *c, uint8_t out[AE_SHA256_LEN]);
void ae_sha256(const void *data, size_t len, uint8_t out[AE_SHA256_LEN]);
/* 流式计算文件哈希；ok=0 表示无法读取（区分「空文件哈希」与「读失败」） */
void ae_sha256_file(const char *path, uint8_t out[AE_SHA256_LEN], int *ok);

void ae_hmac_sha256(const uint8_t *key, size_t keylen,
                    const void *data, size_t len,
                    uint8_t out[AE_SHA256_LEN]);

/* 单块派生（dklen=32）；salt 任意长度；返回 0 成功 */
int  ae_pbkdf2(const char *pw, size_t pwlen,
               const uint8_t *salt, size_t saltlen,
               uint32_t iter, uint8_t out[AE_KEY_LEN]);

/* ChaCha20 IETF：key 32B / nonce 12B / 初始 counter；in==NULL 时仅产出密钥流到 out */
void ae_chacha20(const uint8_t key[AE_KEY_LEN], const uint8_t nonce[AE_NONCE_LEN],
                 uint32_t counter, const uint8_t *in, uint8_t *out, size_t len);

/* CSPRNG：getrandom(2)，失败返回 -1 */
int  ae_random(uint8_t *buf, size_t len);

/*
 * encrypt-then-MAC 密封。blob = nonce(12) || ciphertext(len) || tag(32)
 * tag = HMAC(mac_key, aad || nonce || ciphertext)，mac_key/enc_key 由主密钥派生分离。
 * 成功返回 0 并 malloc 出 *out（调用方 free）；失败返回 -1。
 */
int  ae_seal(const uint8_t key[AE_KEY_LEN],
             const uint8_t *plain, size_t len,
             const uint8_t *aad, size_t aadlen,
             uint8_t **out, size_t *outlen);
/* 解封：先验 tag 再解密，tag 不符返回 -1 且 *out 保持 NULL */
int  ae_open(const uint8_t key[AE_KEY_LEN],
             const uint8_t *blob, size_t bloblen,
             const uint8_t *aad, size_t aadlen,
             uint8_t **out, size_t *outlen);

int  ae_ct_eq(const void *a, const void *b, size_t n);  /* 常量时间，相等返回 1 */
void ae_zero(void *p, size_t n);                        /* 不被编译器优化掉的清零 */
void ae_hex(const uint8_t *buf, size_t len, char *out); /* out 需 >= 2*len+1 */

#endif /* AETHER_CRYPTO_H */
