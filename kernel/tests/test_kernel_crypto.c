/*
 * AetherOS 内核密码学 · host 侧标准向量单测
 * 把内核 freestanding 的 crypto.c / klib.c 原样在 host 编译运行（不改动内核代码），
 * 用 kmemcmp 而非 strcmp 避免 -fno-builtin 下的隐式声明歧义。
 * 由 tools/gate_kernel.py 调用，并与 OpenSSL 命令行交叉验证。
 */
#include "aether_kernel.h"

static int P = 0, F = 0;
#define CHECK(c, m) do { if (c) P++; else { con_placeholder_fail(m); F++; } } while (0)

/* host 侧最小输出（不引入系统头，避免 size_t 冲突） */
extern int printf(const char *, ...);
static void con_placeholder_fail(const char *m) { printf("  FAIL: %s\n", m); }
static int heq(const uint8_t *d, const char *s) {
    char h[65]; khex(h, d, K_HASH_BYTES);
    return kstrcmp(h, s) == 0;
}

int main(void) {
    uint8_t o[K_HASH_BYTES];

    sha256_buf("abc", 3, o);
    CHECK(heq(o, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
          "SHA-256(abc) 标准向量");
    sha256_buf("", 0, o);
    CHECK(heq(o, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
          "SHA-256(empty) 标准向量");
    {
        static const char msg[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
        sha256_buf(msg, sizeof(msg) - 1, o);
        CHECK(heq(o, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"),
              "SHA-256(448-bit 单块边界以外) 标准向量");
    }
    hmac_sha256((const uint8_t *)"key", 3,
                "The quick brown fox jumps over the lazy dog", 43, o);
    CHECK(heq(o, "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8"),
          "HMAC-SHA256 向量");
    pbkdf2("password", 8, (const uint8_t *)"salt", 4, 1, o);
    CHECK(heq(o, "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b"),
          "PBKDF2-HMAC-SHA256 iter=1 (RFC 7914)");
    pbkdf2("password", 8, (const uint8_t *)"salt", 4, 2, o);
    CHECK(heq(o, "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43"),
          "PBKDF2 iter=2 (RFC 7914)");
    pbkdf2("password", 8, (const uint8_t *)"salt", 4, 4096, o);
    CHECK(heq(o, "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a"),
          "PBKDF2 iter=4096 (RFC 7914)");
    {
        uint8_t key[K_KEY_BYTES], nonce[K_NONCE_BYTES], ks[64];
        for (uint32_t i = 0; i < K_KEY_BYTES; i++) key[i] = (uint8_t)i;
        uint8_t nn[K_NONCE_BYTES] = { 0,0,0,9, 0,0,0,0x4a, 0,0,0,0 };
        kmemcpy(nonce, nn, sizeof nn);
        chacha20(key, nonce, 1, NULL, ks, 64);
        char h[65]; khex(h, ks, 16);
        CHECK(kstrcmp(h, "10f1e7e4d13b5915500fdd1fa32071c4") == 0,
              "ChaCha20 keystream (RFC 8439 2.3.2, counter=1)");
        uint8_t pt[64], ct[64], back[64];
        kmemset(pt, 0x5A, 64);
        chacha20(key, nonce, 1, pt, ct, 64);
        chacha20(key, nonce, 1, ct, back, 64);
        CHECK(kmemcmp(pt, back, 64) == 0, "ChaCha20 加解密往返");
    }
    CHECK(ct_eq("same", "same", 4) == 1, "常量时间比较：相等");
    CHECK(ct_eq("same", "sane", 4) == 0, "常量时间比较：不等");
    CHECK(kstrlen("AetherOS") == 8 && kstrcmp("a", "b") < 0, "freestanding 字符串函数");

    printf("\n== 内核密码学单测: %d PASS / %d FAIL ==\n", P, F);
    return F ? 1 : 0;
}
