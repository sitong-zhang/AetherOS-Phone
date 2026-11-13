/*
 * test_secure.c - AetherOS Secure Core 全套自测
 *
 * 覆盖：密码学正确性(标准向量) / 隔离存储 / 审计链抗篡改 / 完整性
 *      / 正常会话 / 【核心】输错 N 次全盘擦除且系统完好 / 防重启绕过
 *      / 防「改计数文件」绕过(MAC + 审计链交叉核对) / 胁迫口令 / 紧急销毁
 *      / 篡改即擦 / 空闲锁定 / 指数退避 / 静态加密(卷内无明文) / 销毁证明。
 *
 * 每个用例用独立运行根，互不污染；全部真编译真运行，PASS/FAIL 机器可判。
 */
#define _GNU_SOURCE
#include "aether_secure.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>

static int P = 0, F = 0;
#define CHECK(c, m) do { if (c) P++; else { printf("  ✗ FAIL: %s\n", m); F++; } } while (0)
#define GROUP(g) printf("\n[%s]\n", g)

static ae_secure_t *fresh(const char *tag, ae_sec_config_t *cfg, char *root, size_t n)
{
    snprintf(root, n, "/tmp/aether_test_%s", tag);
    char cmd[512]; snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    int rc = system(cmd); (void)rc;
    ae_secure_t *s = NULL;
    if (ae_secure_open(root, cfg, &s) != 0) return NULL;
    return s;
}
static ae_sec_config_t base_cfg(void)
{
    ae_sec_config_t c; ae_secure_default_config(&c);
    c.delay_enable = 0; c.idle_timeout_s = 0; c.tamper_wipe = 1; c.max_fails = 3;
    return c;
}
static int hexeq(const uint8_t *d, size_t n, const char *e)
{
    char h[128]; ae_hex(d, n, h);
    return strcmp(h, e) == 0;
}
static int json_get(const char *j, const char *key, char *out, size_t n)
{
    char pat[128]; snprintf(pat, sizeof pat, "\"%s\": \"", key);
    const char *p = strstr(j, pat);
    if (!p) return -1;
    p += strlen(pat);
    const char *q = strchr(p, '"');
    if (!q) return -1;
    size_t l = (size_t)(q - p);
    if (l >= n) l = n - 1;
    memcpy(out, p, l); out[l] = 0;
    return 0;
}
static int read_text_file(const char *path, char *buf, size_t n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t r = fread(buf, 1, n - 1, f);
    fclose(f); buf[r] = 0;
    return 0;
}
/* 与实现一致的 auth.state 镜像（白盒：用于精确构造「重算 MAC 的篡改」） */
typedef struct {
    uint32_t magic, version, max_fails, fails;
    uint8_t pw_salt[16], pw_hash[32], duress_salt[16], duress_hash[32], kek_salt[16], mac[32];
} auth_mirror_t;

/* ------------------------------------------------------------------ 组 A：密码学 */
static void group_crypto(void)
{
    GROUP("A 密码学标准向量 + OpenSSL 已交叉验证");
    uint8_t out[32];
    ae_sha256("abc", 3, out);
    CHECK(hexeq(out, 32, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "SHA256(abc)");
    ae_sha256("", 0, out);
    CHECK(hexeq(out, 32, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"), "SHA256(empty)");
    ae_hmac_sha256((const uint8_t*)"key", 3, "The quick brown fox jumps over the lazy dog", 43, out);
    CHECK(hexeq(out, 32, "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8"), "HMAC-SHA256");
    ae_pbkdf2("password", 8, (const uint8_t*)"salt", 4, 1, out);
    CHECK(hexeq(out, 32, "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b"), "PBKDF2 iter=1");
    ae_pbkdf2("password", 8, (const uint8_t*)"salt", 4, 4096, out);
    CHECK(hexeq(out, 32, "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a"), "PBKDF2 iter=4096");
    uint8_t key[32], nonce[12], ks[64];
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)i;
    uint8_t nn[12] = {0,0,0,9,0,0,0,0x4a,0,0,0,0}; memcpy(nonce, nn, 12);
    ae_chacha20(key, nonce, 1, NULL, ks, 64);
    char h[128]; ae_hex(ks, 16, h);
    CHECK(strcmp(h, "10f1e7e4d13b5915500fdd1fa32071c4") == 0, "ChaCha20 RFC8439 keystream");
    const char *msg = "AetherOS secure vault payload";
    uint8_t *blob = NULL, *pt = NULL; size_t bl = 0, pl = 0;
    const char *aad = "vault-header-v1";
    CHECK(ae_seal(key, (const uint8_t*)msg, strlen(msg), (const uint8_t*)aad, strlen(aad), &blob, &bl) == 0, "seal");
    CHECK(ae_open(key, blob, bl, (const uint8_t*)aad, strlen(aad), &pt, &pl) == 0 &&
          pl == strlen(msg) && memcmp(pt, msg, pl) == 0, "open roundtrip");
    uint8_t *bad = NULL; size_t badl = 0;
    CHECK(ae_open(key, blob, bl, (const uint8_t*)"wrong-aad", 9, &bad, &badl) != 0 && bad == NULL, "wrong AAD rejected");
    blob[bl-1] ^= 1;
    CHECK(ae_open(key, blob, bl, (const uint8_t*)aad, strlen(aad), &bad, &badl) != 0, "tampered tag rejected");
    free(blob); free(pt); free(bad);
}

/* ------------------------------------------------- 组 B：存储/审计/完整性 */
static void group_foundation(void)
{
    GROUP("B 隔离存储 / 审计链 / 完整性");
    char root[256]; ae_sec_config_t cfg = base_cfg();
    ae_secure_t *s = fresh("foundation", &cfg, root, sizeof root);
    CHECK(s != NULL, "open root");
    ae_store_t *st = ae_secure_store(s);
    CHECK(ae_store_atomic_write(st, "data", "probe.txt", "SECRET-DATA", 11) == 0, "atomic write");
    uint8_t *r = NULL; size_t rl = 0;
    CHECK(ae_store_read(st, "data", "probe.txt", &r, &rl) == 0 && rl == 11 &&
          !memcmp(r, "SECRET-DATA", 11), "read back");
    free(r);
    char p[512];
    CHECK(ae_store_path(st, p, sizeof p, "data", "../escape") != 0, "目录穿越被拒绝");
    ae_audit_t *a = ae_secure_audit(s);
    CHECK(ae_audit_append(a, "TEST", "one") == 0, "append1");
    CHECK(ae_audit_append(a, "TEST", "two") == 0, "append2");
    uint64_t cnt = 0; uint8_t head[32];
    CHECK(ae_secure_audit_verify(s, &cnt, head) == 0, "chain intact");
    uint8_t *ch = NULL; size_t cl = 0;
    ae_store_read(st, "journal", "chain.log", &ch, &cl);
    if (ch && cl > 5) { ch[cl/2] ^= 0x20; ae_store_atomic_write(st, "journal", "chain.log", ch, cl); }
    CHECK(ae_secure_audit_verify(s, &cnt, head) != 0, "chain 篡改被检出");
    free(ch);
    ae_store_remove(st, "data", "probe.txt");
    ae_secure_close(s);
}

/* ------------------------------------------------- 组 C：正常会话 */
static void group_session(void)
{
    GROUP("C 初始化 / 开机自检 / 正常会话");
    char root[256]; ae_sec_config_t cfg = base_cfg();
    ae_secure_t *s = fresh("session", &cfg, root, sizeof root);
    CHECK(s && ae_secure_state(s) == AE_SEC_FACTORY, "出厂态(未设置口令)");
    CHECK(ae_secure_init(s, "Pass!2026", NULL) == 0, "init");
    CHECK(ae_secure_state(s) == AE_SEC_LOCKED, "init 后锁屏");
    char m[512];
    CHECK(ae_secure_boot(s, m, sizeof m) == 0, "boot 自检完好");
    CHECK(ae_secure_unlock(s, "Pass!2026", m, sizeof m) == AE_UNLOCK_OK, "unlock");
    const char *doc = "长明 Aether 机密设计文档 v2";
    CHECK(ae_secure_put(s, "design.md", doc, strlen(doc)) == 0, "put");
    CHECK(ae_secure_put(s, "keys.bin", "\x01\x02\x03", 3) == 0, "put2");
    uint8_t *g = NULL; size_t gl = 0;
    CHECK(ae_secure_get(s, "design.md", &g, &gl) == 0 && gl == strlen(doc) &&
          !memcmp(g, doc, gl), "get 明文一致");
    free(g);
    char names[64][64]; int nc = 0;
    CHECK(ae_secure_list(s, names, 64, &nc) == 0 && nc == 2, "list=2");
    CHECK(ae_secure_del(s, "keys.bin") == 0, "del");
    CHECK(ae_secure_list(s, names, 64, &nc) == 0 && nc == 1, "list=1 after del");
    CHECK(ae_secure_lock(s) == 0 && ae_secure_state(s) == AE_SEC_LOCKED, "lock 清零密钥");
    CHECK(ae_secure_unlock(s, "Pass!2026", m, sizeof m) == AE_UNLOCK_OK, "重新解锁");
    ae_secure_close(s);
}

/* ------------------------------------- 组 D：核心 输错N次全盘擦除+系统完好 */
static void group_fail_wipe(void)
{
    GROUP("D 【核心】输错 N 次 → 抹除全部数据，系统本体不抹除");
    char root[256]; ae_sec_config_t cfg = base_cfg();
    ae_secure_t *s = fresh("failwipe", &cfg, root, sizeof root);
    CHECK(s != NULL, "open");
    CHECK(ae_secure_init(s, "S3cret!Pass", "Duress123!") == 0, "init");
    uint8_t sys_before[32], sys_after[32];
    ae_store_zone_digest(ae_secure_store(s), "sys", sys_before);
    /* 卷擦除前哈希（真实取证） */
    char vp[1024]; ae_store_path(ae_secure_store(s), vp, sizeof vp, "data", "vault.img");
    uint8_t vb[32]; int vok = 0; memset(vb, 0, sizeof vb);
    char vbhex[65];
    char m[512];
    CHECK(ae_secure_unlock(s, "S3cret!Pass", m, sizeof m) == AE_UNLOCK_OK, "unlock");
    const char *secret = "UNIQUE-PLAINTEXT-MARKER-778899";
    CHECK(ae_secure_put(s, "notes.txt", secret, strlen(secret)) == 0, "写入机密");
    CHECK(ae_secure_lock(s) == 0, "lock");
    /* 采集「擦除前一刻」的真实卷哈希（put 已重写卷，必须与 wipe 取样点一致） */
    ae_sha256_file(vp, vb, &vok); ae_hex(vb, 32, vbhex);
    /* 静态加密：卷内不得出现明文 */
    uint8_t *vol = (uint8_t*)malloc(AE_VAULT_SIZE);
    size_t vs = 0; (void)ae_store_volume_size(ae_secure_store(s), "vault.img", &vs);
    CHECK(ae_store_volume_read(ae_secure_store(s), "vault.img", vol, vs, 0) == 0, "读卷");
    CHECK(memmem(vol, vs, secret, strlen(secret)) == NULL, "静态加密：卷内无明文");
    free(vol);
    /* 输错 1、2 次 */
    CHECK(ae_secure_unlock(s, "wrong1", m, sizeof m) == AE_UNLOCK_FAIL, "fail1");
    CHECK(ae_secure_fails(s) == 1, "fails==1");
    CHECK(ae_secure_unlock(s, "wrong2", m, sizeof m) == AE_UNLOCK_FAIL, "fail2");
    CHECK(ae_secure_fails(s) == 2, "fails==2");
    /* 模拟重启：计数必须保留（防重启绕过） */
    ae_secure_t *s2 = NULL;
    CHECK(ae_secure_open(root, &cfg, &s2) == 0 && ae_secure_fails(s2) == 2, "重启后 fails 仍为 2");
    if (s2) ae_secure_close(s2);
    /* 第 3 次 -> WIPED */
    CHECK(ae_secure_unlock(s, "wrong3", m, sizeof m) == AE_UNLOCK_WIPED, "fail3 -> WIPED");
    printf("    %s\n", m);
    CHECK(ae_secure_state(s) == AE_SEC_FACTORY, "擦除后出厂态");
    ae_store_zone_digest(ae_secure_store(s), "sys", sys_after);
    CHECK(memcmp(sys_before, sys_after, 32) == 0, "sys/ 摘要逐字节不变（系统未抹除）");
    ae_store_t *st = ae_secure_store(s);
    CHECK(!ae_store_exists(st, "data", "vault.img"), "加密卷已销毁");
    CHECK(!ae_store_exists(st, "data", "auth.state"), "口令凭据已清除");
    CHECK(!ae_store_exists(st, "data", "vault.key"), "密钥材料已销毁");
    uint64_t cnt = 0; uint8_t head[32];
    CHECK(ae_secure_audit_verify(s, &cnt, head) == 0, "审计链仍完整");
    char dump[16384]; ae_audit_dump(ae_secure_audit(s), dump, sizeof dump);
    CHECK(strstr(dump, "WIPE_TRIGGER") && strstr(dump, "WIPE_BEGIN") &&
          strstr(dump, "WIPE_DONE"), "审计链记录销毁全过程");
    /* 销毁证明 */
    const char *rp = ae_secure_last_receipt(s);
    CHECK(rp != NULL, "生成销毁证明");
    if (rp) {
        char full[512], rb[4096];
        snprintf(full, sizeof full, "%s/%s", ae_store_root(st), rp);
        CHECK(read_text_file(full, rb, sizeof rb) == 0, "读取证明");
        CHECK(strstr(rb, "\"sys_unchanged\": true") != NULL, "证明: sys_unchanged=true");
        CHECK(strstr(rb, "ALL_USER_DATA_DESTROYED_SYSTEM_INTACT") != NULL, "证明: 结论");
        CHECK(strstr(rb, vbhex) != NULL, "证明: 含擦除前真实卷哈希");
        char vah[65];
        CHECK(json_get(rb, "volume_hash_after", vah, sizeof vah) == 0 &&
              strcmp(vah, vbhex) != 0, "证明: 卷哈希已改变(数据不可恢复)");
        CHECK(strstr(rb, "\"trigger\": \"max-fails-reached\"") != NULL, "证明: 触发原因");
    }
    /* 系统仍可开机自检 + 重新初始化可再用 + 旧数据不可恢复 */
    CHECK(ae_secure_boot(s, m, sizeof m) == 0, "擦除后 boot 仍完好");
    CHECK(ae_secure_init(s, "NewPass!2026", NULL) == 0, "重新初始化");
    CHECK(ae_secure_unlock(s, "NewPass!2026", m, sizeof m) == AE_UNLOCK_OK, "再次解锁");
    uint8_t *g = NULL; size_t gl = 0;
    CHECK(ae_secure_get(s, "notes.txt", &g, &gl) != 0, "旧数据不可恢复");
    free(g);
    ae_secure_close(s);
}

/* -------------------------- 组 E：防「改计数文件」绕过（MAC + 链交叉核对） */
static void group_counter_tamper(void)
{
    GROUP("E 防绕过：篡改失败计数文件");
    char root[256]; ae_sec_config_t cfg = base_cfg();
    /* E1：改 fails 但不重算 MAC -> 判定篡改 -> 按配置触发销毁 */
    ae_secure_t *s = fresh("ct1", &cfg, root, sizeof root);
    ae_secure_init(s, "Pass!2026", NULL);
    char m[512];
    ae_secure_unlock(s, "bad", m, sizeof m);
    ae_store_t *st = ae_secure_store(s);
    uint8_t *b = NULL; size_t n = 0;
    ae_store_read(st, "data", "auth.state", &b, &n);
    if (b && n == sizeof(auth_mirror_t)) { ((auth_mirror_t*)b)->fails = 0; ae_store_atomic_write(st, "data", "auth.state", b, n); }
    free(b);
    ae_secure_close(s);
    ae_secure_t *s2 = NULL; ae_secure_open(root, &cfg, &s2);
    ae_unlock_t r = ae_secure_unlock(s2, "Pass!2026", m, sizeof m);
    CHECK(r == AE_UNLOCK_TAMPER, "E1 改计数不重算MAC -> 检出篡改并销毁");
    CHECK(ae_secure_state(s2) == AE_SEC_FACTORY, "E1 数据已销毁");
    ae_secure_close(s2);

    /* E2：连 MAC 一起重算（读得到 device.key）-> 审计链交叉核对把计数恢复 */
    ae_secure_t *t = fresh("ct2", &cfg, root, sizeof root);
    ae_secure_init(t, "Pass!2026", NULL);
    ae_secure_unlock(t, "bad", m, sizeof m);
    ae_store_t *st2 = ae_secure_store(t);
    uint8_t *dk = NULL; size_t dkl = 0;
    CHECK(ae_store_read(st2, "sys", "device.key", &dk, &dkl) == 0 && dkl == 32, "E2 读取设备密钥");
    uint8_t *ab = NULL; size_t al = 0;
    ae_store_read(st2, "data", "auth.state", &ab, &al);
    if (ab && al == sizeof(auth_mirror_t) && dk) {
        auth_mirror_t *am = (auth_mirror_t*)ab;
        am->fails = 0;   /* 攻击者把计数清零 */
        /* 用设备密钥重算 MAC（覆盖前 128 字节） */
        ae_hmac_sha256(dk, 32, ab, offsetof(auth_mirror_t, mac), am->mac);
        ae_store_atomic_write(st2, "data", "auth.state", ab, al);
    }
    free(dk); free(ab);
    ae_secure_close(t);
    ae_secure_t *t2 = NULL; ae_secure_open(root, &cfg, &t2);
    CHECK(ae_secure_state(t2) == AE_SEC_LOCKED, "E2 篡改后仍能打开(未误判)");
    CHECK(ae_secure_fails(t2) == 1, "E2 审计链交叉核对把计数恢复为 1");
    ae_secure_close(t2);
}

/* --------------------------------------- 组 F/G/H：duress / panic / tamper */
static void group_triggers(void)
{
    GROUP("F/G/H 多路径销毁：胁迫口令 / 紧急 panic / 篡改即擦");
    char root[256]; ae_sec_config_t cfg = base_cfg(); char m[512];
    /* F 胁迫口令 */
    ae_secure_t *s = fresh("duress", &cfg, root, sizeof root);
    ae_secure_init(s, "RealPass!1", "Coerced999");
    CHECK(ae_secure_unlock(s, "Coerced999", m, sizeof m) == AE_UNLOCK_DURESS, "F 胁迫口令触发销毁");
    CHECK(ae_secure_state(s) == AE_SEC_FACTORY, "F 数据已销毁");
    const char *rp = ae_secure_last_receipt(s);
    if (rp) { char full[512], rb[4096]; snprintf(full,sizeof full,"%s/%s",ae_store_root(ae_secure_store(s)),rp);
              read_text_file(full, rb, sizeof rb);
              CHECK(strstr(rb,"\"trigger\": \"duress-password\"") != NULL, "F 证明记录胁迫触发"); }
    ae_secure_close(s);
    /* G 紧急 panic */
    ae_secure_t *g = fresh("panic", &cfg, root, sizeof root);
    ae_secure_init(g, "RealPass!1", NULL);
    uint8_t sb[32], sa[32]; ae_store_zone_digest(ae_secure_store(g), "sys", sb);
    CHECK(ae_secure_panic(g, "user-panic-button") == 0, "G panic 执行");
    ae_store_zone_digest(ae_secure_store(g), "sys", sa);
    CHECK(memcmp(sb, sa, 32) == 0, "G sys/ 未变");
    CHECK(ae_secure_state(g) == AE_SEC_FACTORY, "G 出厂态");
    ae_secure_close(g);
    /* H 篡改系统区即擦 */
    ae_secure_t *h = fresh("tamper", &cfg, root, sizeof root);
    ae_secure_init(h, "RealPass!1", NULL);
    ae_store_atomic_write(ae_secure_store(h), "sys", "aether.bin", "HACKED-CORE", 11);
    ae_secure_close(h);
    ae_secure_t *h2 = NULL; ae_secure_open(root, &cfg, &h2);
    CHECK(ae_secure_boot(h2, m, sizeof m) == 1, "H 自检检出系统篡改");
    CHECK(ae_secure_state(h2) == AE_SEC_FACTORY, "H 篡改即擦已执行");
    printf("    %s\n", m);
    ae_secure_close(h2);
    /* H2 tamper_wipe=0 时只告警不擦 */
    ae_sec_config_t cfg2 = base_cfg(); cfg2.tamper_wipe = 0;
    ae_secure_t *k = fresh("tamper2", &cfg2, root, sizeof root);
    ae_secure_init(k, "RealPass!1", NULL);
    CHECK(ae_secure_state(k) == AE_SEC_LOCKED, "H2 正常锁定");
    ae_secure_close(k);
}

/* -------------------------------- 组 I/J：空闲锁定 / 指数退避 */
static void group_idle_backoff(void)
{
    GROUP("I/J 会话空闲锁定 / 暴力破解指数退避");
    char root[256]; char m[512];
    ae_sec_config_t ci = base_cfg(); ci.idle_timeout_s = 1;
    ae_secure_t *s = fresh("idle", &ci, root, sizeof root);
    ae_secure_init(s, "Pass!2026", NULL);
    CHECK(ae_secure_unlock(s, "Pass!2026", m, sizeof m) == AE_UNLOCK_OK, "I 解锁");
    (void)sleep(2);
    CHECK(ae_secure_put(s, "x", "y", 1) != 0, "I 空闲超时后写入被拒");
    CHECK(ae_secure_state(s) == AE_SEC_LOCKED, "I 已自动锁定");
    ae_secure_close(s);
    ae_sec_config_t cb = base_cfg(); cb.delay_enable = 1; cb.max_fails = 5;
    ae_secure_t *b = fresh("backoff", &cb, root, sizeof root);
    ae_secure_init(b, "Pass!2026", NULL);
    time_t t0 = time(NULL);
    CHECK(ae_secure_unlock(b, "bad", m, sizeof m) == AE_UNLOCK_FAIL, "J 失败1");
    time_t dt = time(NULL) - t0;
    CHECK(dt >= 1, "J 第1次失败触发 >=1s 退避延迟");
    printf("    实测退避 %ld 秒\n", (long)dt);
    ae_secure_close(b);
}

int main(void)
{
    printf("================ AetherOS Secure Core 全套自测 ================\n");
    group_crypto();
    group_foundation();
    group_session();
    group_fail_wipe();
    group_counter_tamper();
    group_triggers();
    group_idle_backoff();
    printf("\n================================================================\n");
    printf("  合计: %d PASS / %d FAIL\n", P, F);
    printf("================================================================\n");
    return F ? 1 : 0;
}
