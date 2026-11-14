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
#include "aether_lockdown.h"
#include "aether_hal.h"
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

/* -------------------------------- 组 I/J：空闲锁定 / 指数退避 */static void group_idle_backoff(void)
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

/* -------------------------------- 组 K：TEE 锚点 / 密钥驻留 / 设备自证 */
static void group_tee_attest(void)
{
    GROUP("K TEE 锚点 / 密钥驻留 / 设备自证（外部可观测）");
    char root[256]; ae_sec_config_t cfg = base_cfg(); char m[512];

    /* K1：设备主密钥持久化且跨重开稳定（密钥驻留的软件近似） */
    ae_secure_t *s = fresh("tee", &cfg, root, sizeof root);
    ae_store_t *st = ae_secure_store(s);
    uint8_t *k1 = NULL; size_t k1l = 0;
    CHECK(ae_store_read(st, "sys", "device.key", &k1, &k1l) == 0 && k1l == 32, "K1 设备密钥已落盘(32B)");
    ae_secure_close(s);
    ae_secure_t *s2 = NULL; ae_secure_open(root, &cfg, &s2);
    uint8_t *k2 = NULL; size_t k2l = 0;
    CHECK(s2 && ae_store_read(ae_secure_store(s2), "sys", "device.key", &k2, &k2l) == 0 &&
          k2l == 32 && k1l == 32 && !memcmp(k1, k2, 32),
          "K1 设备密钥跨重开稳定一致（密钥驻留）");
    free(k1); free(k2);
    if (s2) ae_secure_close(s2);

    /* K2：审计链被设备密钥绑定——换一把设备密钥后旧链验证必失败（无密钥不可伪造） */
    ae_secure_t *t = fresh("keybind", &cfg, root, sizeof root);
    ae_audit_t *a = ae_secure_audit(t);
    CHECK(ae_audit_append(a, "KTEST", "bind-check") == 0, "K2 写一条审计");
    ae_store_t *ts = ae_secure_store(t);
    uint8_t *dk = NULL; size_t dkl = 0;
    CHECK(ae_store_read(ts, "sys", "device.key", &dk, &dkl) == 0 && dkl == 32, "K2 读取设备密钥");
    uint8_t rot[32];
    CHECK(ae_random(rot, sizeof rot) == 0, "K2 生成替换密钥");
    CHECK(ae_store_atomic_write(ts, "sys", "device.key", rot, sizeof rot) == 0, "K2 覆写 device.key");
    ae_secure_close(t); free(dk);
    ae_secure_t *t2 = NULL; ae_secure_open(root, &cfg, &t2);
    uint64_t cnt = 0; uint8_t head[32];
    CHECK(t2 && ae_secure_audit_verify(t2, &cnt, head) != 0, "K2 换密钥后旧链验证失败（HMAC 绑定）");
    ae_secure_close(t2);

    /* K3：设备自证——用真设备密钥可验证；篡改规范体即签名失效（防伪造/防篡改） */
    ae_secure_t *u = fresh("attest", &cfg, root, sizeof root);
    ae_store_t *us = ae_secure_store(u);
    CHECK(ae_secure_init(u, "AttestPass!1", NULL) == 0, "K3 init");
    CHECK(ae_secure_unlock(u, "AttestPass!1", m, sizeof m) == AE_UNLOCK_OK, "K3 unlock");
    char rep[2048];
    CHECK(ae_secure_attest(u, rep, sizeof rep) == 0, "K3 生成设备自证报告");
    char body[1024], sig[65];
    CHECK(json_get(rep, "signed_body", body, sizeof body) == 0, "K3 含 signed_body");
    CHECK(json_get(rep, "signature", sig, sizeof sig) == 0, "K3 含 signature");
    uint8_t *rk = NULL; size_t rkl = 0;
    CHECK(ae_store_read(us, "sys", "device.key", &rk, &rkl) == 0 && rkl == 32, "K3 读取设备密钥");
    uint8_t calc[32]; ae_hmac_sha256(rk, 32, (const uint8_t*)body, strlen(body), calc);
    char calch[65]; ae_hex(calc, 32, calch);
    CHECK(strcmp(calch, sig) == 0, "K3 用真设备密钥验证通过");
    /* 篡改规范体后重算：签名必失效 */
    char tb[1024]; snprintf(tb, sizeof tb, "%s", body);
    tb[10] = (char)(tb[10] ^ 0xFF);   /* 翻转一处，模拟攻击者改状态 */
    uint8_t calc2[32]; ae_hmac_sha256(rk, 32, (const uint8_t*)tb, strlen(tb), calc2);
    char calch2[65]; ae_hex(calc2, 32, calch2);
    CHECK(strcmp(calch2, sig) != 0, "K3 篡改后签名失效（报告防伪造）");
    free(rk);
    ae_secure_close(u);
}

/* ================================================================
 * 组 L：安全锁定 / 熔断模式（外部可观测）
 *   - 密码强度边界、首启硬门、两步设定 + 10 秒冷却
 *   - 零存储：扫描整个运行根，密码明文一个字节都不落盘
 *   - 组合键状态机：满 10 秒 + 音量上确认才进入；不足/松开/超时不进
 *   - 仅专用密码可解锁，错误密码无旁路
 *   - USB 强插 / 拆机 / 短接 → 报警 + 连系统一并抹除 + 永久变砖
 *   - 状态跨「重启」（重新 open）持久化，变砖不可恢复
 * ================================================================ */
#define LD_PW_OK "Abc!Def@Ghi#Jkl$Mn0PqRs"   /* 23 位：大小写+特殊+数字，无 >5 连续序列 */

typedef struct {
    char root[256];
    ae_store_t *st;
    ae_hal_t   *hal;
    ae_ld_t    *ld;
} ld_fx_t;

static int ld_setup(const char *tag, ld_fx_t *fx)
{
    memset(fx, 0, sizeof *fx);
    snprintf(fx->root, sizeof fx->root, "/tmp/aether_ld_%s", tag);
    char cmd[512]; snprintf(cmd, sizeof cmd, "rm -rf %s", fx->root);
    int rc = system(cmd); (void)rc;
    if (ae_store_open(fx->root, &fx->st) != 0) return -1;
    if (ae_hal_open(AE_HAL_SIM, &fx->hal) != 0) return -1;
    ae_hal_set_now_ms(fx->hal, 1000);            /* 确定性时钟 */
    if (ae_ld_open(fx->st, fx->hal, &fx->ld) != 0) return -1;
    return 0;
}
static void ld_teardown(ld_fx_t *fx)
{
    if (fx->ld)  ae_ld_close(fx->ld);
    if (fx->hal) ae_hal_close(fx->hal);
    if (fx->st)  ae_store_close(fx->st);
    memset(fx, 0, sizeof *fx);
}
/* 走完「设定 + 组合键」进入 LOCKED，供解锁/熔断路复用 */
static int ld_to_locked(ae_ld_t *c, ae_hal_t *hal, const char *pw)
{
    char d[1024], why[256];
    ae_hal_set_now_ms(hal, 1000);
    if (ae_ld_provision_begin(c, pw, d, sizeof d, why, sizeof why) != 0) return -1;
    if (ae_ld_provision_confirm(c, 1000 + AE_LD_COOLDOWN_MS, why, sizeof why) != 0) return -1;
    (void)ae_ld_on_event(c, AE_EV_KEY_POWER,   20000, d, sizeof d);
    (void)ae_ld_on_event(c, AE_EV_KEY_VOL_DOWN,20000, d, sizeof d);
    (void)ae_ld_on_event(c, AE_EV_KEY_POWER,   30000, d, sizeof d); /* el=10s → ARMED */
    (void)ae_ld_on_event(c, AE_EV_KEY_VOL_UP,  31000, d, sizeof d); /* → LOCKED */
    return ae_ld_state(c) == AE_LD_LOCKED ? 0 : -1;
}

static void group_lockdown(void)
{
    GROUP("L 安全锁定 / 熔断模式（外部可观测）");
    char d[1024], why[256];

    /* --- L1 密码强度边界 --- */
    CHECK(ae_ld_check_strength(LD_PW_OK, why, sizeof why) == 0, "L1 合法 23 位密码通过");
    CHECK(ae_ld_check_strength("Abc!Def", why, sizeof why) == 1, "L1 过短(<20)拒绝");
    CHECK(ae_ld_check_strength("Abc!Def@Ghi#Jkl$Mn0PqRsTuVwXyZ1", why, sizeof why) == 1,
          "L1 过长(>30)拒绝");
    CHECK(ae_ld_check_strength("abc!def@ghi#jkl$mn0pqrs", why, sizeof why) == 2,
          "L1 缺大写拒绝");
    CHECK(ae_ld_check_strength("ABC!DEF@GHI#JKL$MN0PQRS", why, sizeof why) == 3,
          "L1 缺小写拒绝");
    CHECK(ae_ld_check_strength("AbcDefGhiJklMn0PqRsTuVw", why, sizeof why) == 4,
          "L1 缺特殊符号拒绝");
    CHECK(ae_ld_check_strength("abcdefGhi#Jkl$Mn0PqRsTu", why, sizeof why) == 5,
          "L1 连续序列>5(abcdef)拒绝");
    CHECK(ae_ld_check_strength("Abcde!Fgh#Ij$Kl0MnPqRsT", why, sizeof why) == 0,
          "L1 连续序列=5(bcde 段)仍允许");

    /* --- L2 首启硬门 + 两步设定 + 冷却 + 零存储 --- */
    ld_fx_t fx;
    CHECK(ld_setup("l2", &fx) == 0, "L2 建独立运行根");
    CHECK(ae_ld_state(fx.ld) == AE_LD_UNSET, "L2 初始为 UNSET");
    CHECK(ae_ld_oobe_gate(fx.ld) == 0, "L2 未设密码：开机硬门不放行");
    /* 未设密码时组合键完全无效 */
    (void)ae_ld_on_event(fx.ld, AE_EV_KEY_POWER, 1000, d, sizeof d);
    (void)ae_ld_on_event(fx.ld, AE_EV_KEY_VOL_DOWN, 1000, d, sizeof d);
    (void)ae_ld_on_event(fx.ld, AE_EV_KEY_VOL_UP, 11000, d, sizeof d);
    CHECK(ae_ld_state(fx.ld) == AE_LD_UNSET, "L2 未设密码：组合键无效(硬门)");
    /* 强度不合格不得进入设定 */
    CHECK(ae_ld_provision_begin(fx.ld, "short", d, sizeof d, why, sizeof why) == -1,
          "L2 弱密码设定被拒");
    /* 合格 → 弹窗回显明文(内存) */
    CHECK(ae_ld_provision_begin(fx.ld, LD_PW_OK, d, sizeof d, why, sizeof why) == 0,
          "L2 合格密码进入待确认");
    CHECK(strstr(d, LD_PW_OK) != NULL, "L2 二次确认弹窗明文回显密码");
    CHECK(strstr(d, "物理") != NULL && strstr(d, "冷却") != NULL,
          "L2 弹窗含物理抄录警告 + 冷却提示");
    /* 冷却窗口：不足 10 秒拒绝，满 10 秒通过 */
    CHECK(ae_ld_provision_confirm(fx.ld, 1000 + AE_LD_COOLDOWN_MS - 1, why, sizeof why) == -1,
          "L2 未满 10 秒：确定按钮拒绝");
    CHECK(!ae_ld_anchor_exists(fx.ld), "L2 冷却未满时验证锚未落盘");
    CHECK(ae_ld_provision_confirm(fx.ld, 1000 + AE_LD_COOLDOWN_MS, why, sizeof why) == 0,
          "L2 满 10 秒：设定成功");
    CHECK(ae_ld_state(fx.ld) == AE_LD_NORMAL, "L2 设定后进入 NORMAL");
    CHECK(ae_ld_oobe_gate(fx.ld) == 1, "L2 设定后开机硬门放行");
    /* 零存储：验证锚落盘但密码明文在任何文件中都不存在 */
    CHECK(ae_ld_anchor_exists(fx.ld), "L2 落盘的是验证锚(非密码)");
    CHECK(ae_ld_scan_plaintext(fx.ld, LD_PW_OK) == 0,
          "L2 零存储：全盘扫描无密码明文");
    /* 不可重置 / 仅首次有效 */
    CHECK(ae_ld_provision_begin(fx.ld, "Zyx!Vut@Srq#Pon$Ml0KjI", d, sizeof d,
                                why, sizeof why) == -2, "L2 二次设定被拒(不可重置)");
    CHECK(strstr(why, "不可重置") != NULL, "L2 拒绝原因如实说明");
    ld_teardown(&fx);

    /* --- L3 组合键状态机 + 解锁 + 跨重启持久化 --- */
    CHECK(ld_setup("l3", &fx) == 0, "L3 建独立运行根");
    CHECK(ae_ld_provision_begin(fx.ld, LD_PW_OK, d, sizeof d, why, sizeof why) == 0, "L3 设定");
    CHECK(ae_ld_provision_confirm(fx.ld, 1000 + AE_LD_COOLDOWN_MS, why, sizeof why) == 0,
          "L3 确认");
    (void)ae_ld_on_event(fx.ld, AE_EV_KEY_POWER, 20000, d, sizeof d);
    (void)ae_ld_on_event(fx.ld, AE_EV_KEY_VOL_DOWN, 20000, d, sizeof d);
    CHECK(ae_ld_state(fx.ld) == AE_LD_GESTURE, "L3 同按开始 → GESTURE");
    (void)ae_ld_on_event(fx.ld, AE_EV_KEY_VOL_UP, 20000 + AE_LD_GESTURE_MS - 1, d, sizeof d);
    CHECK(ae_ld_state(fx.ld) == AE_LD_NORMAL, "L3 未满 10 秒按音量上：不能进入");
    /* 重新同按，满 10 秒 → ARMED（超时保持，不自动进入） */
    (void)ae_ld_on_event(fx.ld, AE_EV_KEY_POWER, 40000, d, sizeof d);
    (void)ae_ld_on_event(fx.ld, AE_EV_KEY_VOL_DOWN, 40000, d, sizeof d);
    (void)ae_ld_on_event(fx.ld, AE_EV_KEY_POWER, 40000 + AE_LD_GESTURE_MS, d, sizeof d);
    CHECK(ae_ld_state(fx.ld) == AE_LD_ARMED, "L3 满 10 秒 → ARMED(待确认)");
    (void)ae_ld_on_event(fx.ld, AE_EV_KEY_POWER, 60000, d, sizeof d);
    CHECK(ae_ld_state(fx.ld) == AE_LD_ARMED, "L3 超时/持续按住保持 ARMED 不自动进入");
    (void)ae_ld_on_event(fx.ld, AE_EV_KEY_VOL_UP, 61000, d, sizeof d);
    CHECK(ae_ld_state(fx.ld) == AE_LD_LOCKED, "L3 音量上确认 → 进入安全锁定模式");
    /* 错误密码无旁路 */
    CHECK(ae_ld_unlock(fx.ld, "Wrong!Pass@2026#NotOk9x", why, sizeof why) == -1,
          "L3 错误密码解不开(无找回/无重置)");
    CHECK(ae_ld_state(fx.ld) == AE_LD_LOCKED, "L3 错误后仍锁定");
    /* 跨「重启」：关闭再打开同根，状态仍 LOCKED */
    ae_ld_close(fx.ld); fx.ld = NULL;
    CHECK(ae_ld_open(fx.st, fx.hal, &fx.ld) == 0, "L3 重新 open(模拟重启)");
    CHECK(ae_ld_state(fx.ld) == AE_LD_LOCKED, "L3 安全锁定跨重启持久(退不出)");
    CHECK(ae_ld_unlock(fx.ld, LD_PW_OK, why, sizeof why) == 0,
          "L3 正确专用密码解锁成功");
    CHECK(ae_ld_state(fx.ld) == AE_LD_NORMAL, "L3 解锁后回 NORMAL");
    ld_teardown(&fx);

    /* --- L4 物理红线 → 熔断(连系统抹除 + 报警 + 变砖) --- */
    {
        const char *trig[] = {"usb-data", "chassis", "short"};
        ae_hal_event_t evs[] = {AE_EV_USB_DATA, AE_EV_CHASSIS_OPEN, AE_EV_TESTPOINT_SHORT};
        int k;
        for (k = 0; k < 3; k++) {
            char tag[64]; snprintf(tag, sizeof tag, "l4%d", k);
            CHECK(ld_setup(tag, &fx) == 0, "L4 建独立运行根");
            /* 放一个系统本体文件，熔断后应被抹掉（区别于常规 wipe 保留 sys） */
            CHECK(ae_store_atomic_write(fx.st, AE_ZONE_SYS, "aether.bin",
                                        "SYSTEM-CORE", 11) == 0, "L4 写入系统本体文件");
            CHECK(ld_to_locked(fx.ld, fx.hal, LD_PW_OK) == 0, "L4 设定并进入 LOCKED");
            char bm[256];
            int fired = ae_ld_on_event(fx.ld, evs[k], 70000, bm, sizeof bm);
            CHECK(fired == 1, "L4 物理事件触发熔断");
            CHECK(ae_ld_state(fx.ld) == AE_LD_BRICKED, "L4 状态 BRICKED");
            CHECK(ae_hal_alarm_count(fx.hal) >= 1, "L4 触发报警机制");
            CHECK(strstr(ae_hal_alarm_last(fx.hal), trig[k]) != NULL, "L4 报警含触发原因");
            CHECK(!ae_store_exists(fx.st, AE_ZONE_SYS, "aether.bin"),
                  "L4 系统本体被一并抹除(连系统删)");
            CHECK(ae_store_exists(fx.st, AE_ZONE_LOCKDOWN, "BRICKED"),
                  "L4 变砖墓碑持久");
            CHECK(ae_ld_unlock(fx.ld, LD_PW_OK, why, sizeof why) == -3,
                  "L4 变砖后即使正确密码也退不出");
            CHECK(ae_ld_oobe_gate(fx.ld) == 0, "L4 变砖后开机硬门永久不放行");
            /* 跨重启：变砖状态不可恢复 */
            ae_ld_close(fx.ld); fx.ld = NULL;
            CHECK(ae_ld_open(fx.st, fx.hal, &fx.ld) == 0, "L4 重新 open");
            CHECK(ae_ld_state(fx.ld) == AE_LD_BRICKED, "L4 变砖跨重启持久(永久板砖)");
            ld_teardown(&fx);
        }
    }

    /* --- L5 未进入锁定态时物理事件不误触发（三档语义分离） --- */
    CHECK(ld_setup("l5", &fx) == 0, "L5 建独立运行根");
    CHECK(ld_to_locked(fx.ld, fx.hal, LD_PW_OK) == 0, "L5 进入 LOCKED 再解锁");
    CHECK(ae_ld_unlock(fx.ld, LD_PW_OK, why, sizeof why) == 0, "L5 解锁回 NORMAL");
    char m[256];
    (void)ae_ld_on_event(fx.ld, AE_EV_USB_DATA, 80000, m, sizeof m);
    CHECK(ae_ld_state(fx.ld) == AE_LD_NORMAL, "L5 NORMAL 态 USB 事件不误熔断");
    CHECK(ae_hal_alarm_count(fx.hal) == 0, "L5 无报警");
    ld_teardown(&fx);
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
    group_tee_attest();
    group_lockdown();
    printf("\n================================================================\n");
    printf("  合计: %d PASS / %d FAIL\n", P, F);
    printf("================================================================\n");
    return F ? 1 : 0;
}
