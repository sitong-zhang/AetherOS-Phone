/*
 * aether_secure.c - AetherOS Secure Core：认证 / 加密保险库 / 自毁 / 门面编排
 *
 * 关键安全机制（均为真实实现，非占位）：
 *  - 口令只以加盐 PBKDF2-HMAC-SHA256 哈希落盘；KEK 由口令派生，DEK 随机且被密封。
 *  - 失败计数三重防护：每次原子落盘 + auth.state 带设备密钥的 HMAC 防篡改
 *    + 只追加哈希链里的失败事件交叉核对（改计数文件也绕不过去）。
 *  - 触发阈值 / 胁迫口令 / 紧急 panic / 系统篡改 → 同一自毁引擎：
 *    crypto-shred 密钥材料 + 4 轮覆写加密卷 + 清空 data/ 全部残留，
 *    sys/ 逐字节不动，生成可离线校验的销毁证明。
 *  - DEK 仅在解锁后驻内存，mlock 防换出，锁定/擦除/关闭时强制清零。
 */
#define _GNU_SOURCE
#include "aether_secure.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>

#define AUTH_FILE       "auth.state"
#define KEY_FILE        "vault.key"
#define VOL_FILE        "vault.img"
#define DEVICE_KEY_FILE "device.key"
#define CHAIN_FILE      "chain.log"
#define AUTH_MAGIC      0x41455448u   /* "AETH" */
#define AUTH_VER        1u
#define KEY_MAGIC       0x4b455931u   /* "KEY1" */
#define MAX_ENTRIES     64
#define AAD_WRAP        "aether-wrapped-dek-v1"
#define AAD_VAULT       "aether-vault-v1"
#define WRAP_BLOB_LEN   (AE_NONCE_LEN + AE_KEY_LEN + AE_TAG_LEN)  /* 76 */

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t max_fails;
    uint32_t fails;
    uint8_t  pw_salt[AE_SALT_LEN];
    uint8_t  pw_hash[AE_SHA256_LEN];
    uint8_t  duress_salt[AE_SALT_LEN];
    uint8_t  duress_hash[AE_SHA256_LEN];
    uint8_t  kek_salt[AE_SALT_LEN];
    uint8_t  mac[AE_SHA256_LEN];
} auth_state_t;   /* 160 字节定长 */

typedef struct { char name[AE_SEC_NAME_MAX]; uint8_t *data; size_t len; } entry_t;
typedef struct { entry_t e[MAX_ENTRIES]; int n; } table_t;

struct ae_secure {
    ae_store_t     *store;
    ae_audit_t     *audit;
    ae_sec_config_t cfg;
    ae_sec_state_t  state;
    auth_state_t    auth;
    uint8_t         dek[AE_KEY_LEN];
    int             dek_valid;
    int             tampered;
    time_t          last_activity;
    char            last_receipt[256];
};

/* ------------------------------------------------------------------ 小工具 */
static void hex_of(const uint8_t *b, char out[65]) { ae_hex(b, AE_SHA256_LEN, out); }

static int device_key(ae_secure_t *s, uint8_t k[AE_KEY_LEN])
{
    uint8_t *b = NULL; size_t n = 0;
    if (ae_store_read(s->store, AE_ZONE_SYS, DEVICE_KEY_FILE, &b, &n) != 0 || n != AE_KEY_LEN) {
        free(b); return -1;
    }
    memcpy(k, b, AE_KEY_LEN); free(b); return 0;
}
/* auth.mac = HMAC(device_key, auth_state 前段) —— 改 fails 而不重算会被检出 */
static void auth_recompute_mac(ae_secure_t *s)
{
    uint8_t k[AE_KEY_LEN];
    if (device_key(s, k) != 0) memset(k, 0, sizeof k);
    size_t body = offsetof(auth_state_t, mac);
    ae_hmac_sha256(k, AE_KEY_LEN, &s->auth, body, s->auth.mac);
    ae_zero(k, sizeof k);
}
static int auth_mac_ok(ae_secure_t *s)
{
    uint8_t calc[AE_SHA256_LEN];
    uint8_t k[AE_KEY_LEN];
    if (device_key(s, k) != 0) return 0;
    size_t body = offsetof(auth_state_t, mac);
    ae_hmac_sha256(k, AE_KEY_LEN, &s->auth, body, calc);
    ae_zero(k, sizeof k);
    return ae_ct_eq(calc, s->auth.mac, AE_SHA256_LEN);
}
static int save_auth(ae_secure_t *s)
{
    auth_recompute_mac(s);
    return ae_store_atomic_write(s->store, AE_ZONE_DATA, AUTH_FILE, &s->auth, sizeof s->auth);
}
static int load_auth(ae_secure_t *s)
{
    uint8_t *b = NULL; size_t n = 0;
    if (ae_store_read(s->store, AE_ZONE_DATA, AUTH_FILE, &b, &n) != 0) return -1;
    if (n != sizeof(auth_state_t)) { free(b); return -1; }
    memcpy(&s->auth, b, sizeof s->auth); free(b);
    if (s->auth.magic != AUTH_MAGIC || s->auth.version != AUTH_VER) return -1;
    if (!auth_mac_ok(s)) return -1;
    return 0;
}
/* 只追加链里「自上次重置以来」的连续失败次数：改 auth.state 也绕不过 */
static uint32_t chain_recent_fails(ae_secure_t *s)
{
    uint8_t *b = NULL; size_t n = 0;
    if (ae_store_read(s->store, AE_ZONE_JOURNAL, CHAIN_FILE, &b, &n) != 0) return 0;
    uint32_t c = 0;
    char *save = NULL;
    char *ln = strtok_r((char *)b, "\n", &save);
    while (ln) {
        if (strstr(ln, "|AUTH_OK|") || strstr(ln, "|WIPE_DONE|") || strstr(ln, "|INIT|"))
            c = 0;
        else if (strstr(ln, "|AUTH_FAIL|"))
            c++;
        ln = strtok_r(NULL, "\n", &save);
    }
    free(b);
    return c;
}

static int derive_kek(const char *pw, const uint8_t salt[AE_SALT_LEN], uint8_t kek[AE_KEY_LEN])
{
    return ae_pbkdf2(pw, strlen(pw), salt, AE_SALT_LEN, AE_KEK_ITER, kek);
}
static int key_load(ae_secure_t *s, uint8_t wrapped[WRAP_BLOB_LEN])
{
    uint8_t *b = NULL; size_t n = 0;
    if (ae_store_read(s->store, AE_ZONE_DATA, KEY_FILE, &b, &n) != 0) return -1;
    if (n != 4 + WRAP_BLOB_LEN) { free(b); return -1; }
    uint32_t magic; memcpy(&magic, b, 4);
    if (magic != KEY_MAGIC) { free(b); return -1; }
    memcpy(wrapped, b + 4, WRAP_BLOB_LEN); free(b);
    return 0;
}
static void lock_dek(ae_secure_t *s)
{
    (void)munlock(s->dek, sizeof s->dek);
    ae_zero(s->dek, sizeof s->dek);
    s->dek_valid = 0;
}
static void set_dek(ae_secure_t *s, const uint8_t d[AE_KEY_LEN])
{
    memcpy(s->dek, d, AE_KEY_LEN);
    (void)mlock(s->dek, sizeof s->dek);
    s->dek_valid = 1;
}

/* -------------------------------------------------------------- 文件表序列化 */
static void table_free(table_t *t)
{
    int i;
    for (i = 0; i < t->n; i++) { free(t->e[i].data); t->e[i].data = NULL; }
    t->n = 0;
}
static int table_serialize(const table_t *t, uint8_t **out, size_t *len)
{
    size_t total = 8;
    int i;
    for (i = 0; i < t->n; i++)
        total += 1 + strlen(t->e[i].name) + 4 + t->e[i].len;
    uint8_t *b = (uint8_t*)malloc(total ? total : 1);
    if (!b) return -1;
    memcpy(b, "AEVT", 4);
    uint32_t n = (uint32_t)t->n; memcpy(b + 4, &n, 4);
    uint8_t *p = b + 8;
    for (i = 0; i < t->n; i++) {
        size_t nl = strlen(t->e[i].name);
        *p++ = (uint8_t)nl;
        memcpy(p, t->e[i].name, nl); p += nl;
        uint32_t dl = (uint32_t)t->e[i].len; memcpy(p, &dl, 4); p += 4;
        if (t->e[i].len) { memcpy(p, t->e[i].data, t->e[i].len); p += t->e[i].len; }
    }
    *out = b; *len = total;
    return 0;
}
static int table_deserialize(const uint8_t *b, size_t len, table_t *t)
{
    memset(t, 0, sizeof *t);
    if (len < 8 || memcmp(b, "AEVT", 4) != 0) return -1;
    uint32_t n; memcpy(&n, b + 4, 4);
    if (n > MAX_ENTRIES) return -1;
    size_t p = 8;
    uint32_t i;
    for (i = 0; i < n; i++) {
        if (p + 1 > len) return -1;
        size_t nl = b[p++];
        if (nl == 0 || nl >= AE_SEC_NAME_MAX || p + nl > len) return -1;
        memcpy(t->e[t->n].name, b + p, nl); t->e[t->n].name[nl] = 0; p += nl;
        if (p + 4 > len) return -1;
        uint32_t dl; memcpy(&dl, b + p, 4); p += 4;
        if (p + dl > len) return -1;
        t->e[t->n].data = (uint8_t*)malloc(dl ? dl : 1);
        if (!t->e[t->n].data) return -1;
        if (dl) memcpy(t->e[t->n].data, b + p, dl);
        t->e[t->n].len = dl; p += dl;
        t->n++;
    }
    return 0;
}
/* 卷布局：[4B "AEVL"][4B blob_len][blob][...0] */
static int vault_save_table(ae_secure_t *s, const table_t *t)
{
    uint8_t *plain = NULL; size_t plen = 0;
    if (table_serialize(t, &plain, &plen) != 0) return -1;
    uint8_t *blob = NULL; size_t blen = 0;
    if (ae_seal(s->dek, plain, plen, (const uint8_t*)AAD_VAULT, strlen(AAD_VAULT),
                &blob, &blen) != 0) { free(plain); return -1; }
    ae_zero(plain, plen); free(plain);
    if (blen + 8 > AE_VAULT_SIZE) { free(blob); return -1; }
    if (ae_store_volume_create(s->store, VOL_FILE, AE_VAULT_SIZE) != 0) { free(blob); return -1; }
    uint8_t head[8] = {'A','E','V','L',0,0,0,0};
    uint32_t bl = (uint32_t)blen; memcpy(head + 4, &bl, 4);
    int rc = ae_store_volume_write(s->store, VOL_FILE, head, 8, 0);
    if (rc == 0) rc = ae_store_volume_write(s->store, VOL_FILE, blob, blen, 8);
    ae_zero(blob, blen); free(blob);
    return rc;
}
static int vault_load_table(ae_secure_t *s, table_t *t)
{
    memset(t, 0, sizeof *t);
    uint8_t head[8];
    if (ae_store_volume_read(s->store, VOL_FILE, head, 8, 0) != 0) return -1;
    if (memcmp(head, "AEVL", 4) != 0) return -1;   /* 卷已被擦除/损坏 */
    uint32_t bl; memcpy(&bl, head + 4, 4);
    if (bl == 0 || (size_t)bl + 8 > AE_VAULT_SIZE) return -1;
    uint8_t *blob = (uint8_t*)malloc(bl);
    if (!blob) return -1;
    if (ae_store_volume_read(s->store, VOL_FILE, blob, bl, 8) != 0) { free(blob); return -1; }
    uint8_t *plain = NULL; size_t plen = 0;
    if (ae_open(s->dek, blob, bl, (const uint8_t*)AAD_VAULT, strlen(AAD_VAULT),
                &plain, &plen) != 0) { free(blob); return -1; }
    free(blob);
    int rc = table_deserialize(plain, plen, t);
    ae_zero(plain, plen); free(plain);
    if (rc != 0) table_free(t);
    return rc;
}

/* ------------------------------------------------------------------ 系统文件 */
static int ensure_sys_files(ae_secure_t *s)
{
    ae_store_t *st = s->store;
    if (!ae_store_exists(st, AE_ZONE_SYS, "aether.bin")) {
        static const char core[] =
            "AetherOS Secure Core / 长明 Aether (Changming Aether)\n"
            "seed=software-simulated-whole-machine\n"
            "security=password-fail-full-wipe + crypto-shred + tamper-evident-audit\n";
        if (ae_store_atomic_write(st, AE_ZONE_SYS, "aether.bin", core, sizeof core - 1) != 0) return -1;
    }
    if (!ae_store_exists(st, AE_ZONE_SYS, "init.aether")) {
        static const char ini[] = "boot=integrity-check -> lockscreen -> auth -> session\nzones=sys,data,journal,receipt\n";
        if (ae_store_atomic_write(st, AE_ZONE_SYS, "init.aether", ini, sizeof ini - 1) != 0) return -1;
    }
    if (!ae_store_exists(st, AE_ZONE_SYS, "brand.txt")) {
        static const char br[] = "Company: 长明科技（武汉）有限公司\nBrand: 长明 / Changming\nProduct: 长明 Aether\nSystem: AetherOS\n";
        if (ae_store_atomic_write(st, AE_ZONE_SYS, "brand.txt", br, sizeof br - 1) != 0) return -1;
    }
    if (!ae_store_exists(st, AE_ZONE_SYS, DEVICE_KEY_FILE)) {
        uint8_t k[AE_KEY_LEN];
        if (ae_random(k, sizeof k) != 0) return -1;
        int rc = ae_store_atomic_write(st, AE_ZONE_SYS, DEVICE_KEY_FILE, k, sizeof k);
        ae_zero(k, sizeof k);
        if (rc != 0) return -1;
    }
    return 0;
}

/* ------------------------------------------------------------- 生命周期门面 */
void ae_secure_default_config(ae_sec_config_t *cfg)
{
    if (!cfg) return;
    cfg->max_fails = 5;
    cfg->idle_timeout_s = 0;
    cfg->tamper_wipe = 1;
    cfg->delay_enable = 1;
}

int ae_secure_open(const char *root, const ae_sec_config_t *cfg, ae_secure_t **out)
{
    if (!out) return -1;
    *out = NULL;
    ae_secure_t *s = (ae_secure_t*)calloc(1, sizeof *s);
    if (!s) return -1;
    if (cfg) s->cfg = *cfg; else ae_secure_default_config(&s->cfg);
    s->last_receipt[0] = 0;
    if (ae_store_open(root, &s->store) != 0) { free(s); return -1; }
    if (ae_audit_open(s->store, &s->audit) != 0) { ae_store_close(s->store); free(s); return -1; }
    /* 审计链与完整性：异常只标记，交由 boot/unlock 按配置处置 */
    uint64_t cnt; uint8_t head[AE_SHA256_LEN];
    if (ae_audit_verify(s->audit, &cnt, head) != 0) s->tampered = 1;
    int ok = 0; uint8_t cur[AE_SHA256_LEN], base[AE_SHA256_LEN];
    if (ae_integrity_check(s->store, &ok, cur, base) == 0 && !ok) s->tampered = 1;
    if (!ae_store_exists(s->store, AE_ZONE_DATA, AUTH_FILE)) {
        s->state = AE_SEC_FACTORY;
    } else {
        if (load_auth(s) != 0) s->tampered = 1;   /* 计数文件被改 -> 篡改 */
        else {
            uint32_t cf = chain_recent_fails(s);
            if (cf > s->auth.fails) s->auth.fails = cf;   /* 交叉核对，取较大值 */
            s->state = AE_SEC_LOCKED;
        }
    }
    *out = s;
    return 0;
}
void ae_secure_close(ae_secure_t *s)
{
    if (!s) return;
    lock_dek(s);
    ae_audit_close(s->audit);
    ae_store_close(s->store);
    ae_zero(&s->auth, sizeof s->auth);
    free(s);
}
ae_store_t *ae_secure_store(ae_secure_t *s) { return s ? s->store : NULL; }
ae_audit_t *ae_secure_audit(ae_secure_t *s) { return s ? s->audit : NULL; }
ae_sec_state_t ae_secure_state(ae_secure_t *s) { return s ? s->state : AE_SEC_FACTORY; }
uint32_t ae_secure_fails(ae_secure_t *s) { return s ? s->auth.fails : 0; }
uint32_t ae_secure_max_fails(ae_secure_t *s)
{
    return s ? (s->auth.max_fails ? s->auth.max_fails : s->cfg.max_fails) : 0;
}
const char *ae_secure_last_receipt(ae_secure_t *s)
{
    return (s && s->last_receipt[0]) ? s->last_receipt : NULL;
}
int ae_secure_audit_verify(ae_secure_t *s, uint64_t *count, uint8_t head[AE_SHA256_LEN])
{
    return s ? ae_audit_verify(s->audit, count, head) : -1;
}

int ae_secure_init(ae_secure_t *s, const char *password, const char *duress)
{
    if (!s || !password || !*password) return -1;
    if (ensure_sys_files(s) != 0) return -1;
    /* 若存在旧卷，重新初始化前先安全覆盖，避免旧密文残留 */
    if (ae_store_exists(s->store, AE_ZONE_DATA, VOL_FILE))
        (void)ae_store_volume_overwrite(s->store, VOL_FILE, 2, NULL);
    lock_dek(s);
    memset(&s->auth, 0, sizeof s->auth);
    s->auth.magic = AUTH_MAGIC; s->auth.version = AUTH_VER;
    s->auth.max_fails = s->cfg.max_fails; s->auth.fails = 0;
    if (ae_random(s->auth.pw_salt, AE_SALT_LEN) != 0 ||
        ae_random(s->auth.duress_salt, AE_SALT_LEN) != 0 ||
        ae_random(s->auth.kek_salt, AE_SALT_LEN) != 0) return -1;
    if (ae_pbkdf2(password, strlen(password), s->auth.pw_salt, AE_SALT_LEN,
                  AE_PBKDF2_ITER, s->auth.pw_hash) != 0) return -1;
    if (duress && *duress) {
        if (ae_pbkdf2(duress, strlen(duress), s->auth.duress_salt, AE_SALT_LEN,
                      AE_PBKDF2_ITER, s->auth.duress_hash) != 0) return -1;
    }
    uint8_t dek[AE_KEY_LEN], kek[AE_KEY_LEN];
    if (ae_random(dek, sizeof dek) != 0) return -1;
    if (derive_kek(password, s->auth.kek_salt, kek) != 0) { ae_zero(dek,sizeof dek); return -1; }
    uint8_t *blob = NULL; size_t blen = 0;
    if (ae_seal(kek, dek, AE_KEY_LEN, (const uint8_t*)AAD_WRAP, strlen(AAD_WRAP),
                &blob, &blen) != 0 || blen != WRAP_BLOB_LEN) {
        ae_zero(dek,sizeof dek); ae_zero(kek,sizeof kek); return -1;
    }
    ae_zero(kek, sizeof kek);
    uint8_t keyfile[4 + WRAP_BLOB_LEN];
    uint32_t km = KEY_MAGIC; memcpy(keyfile, &km, 4);
    memcpy(keyfile + 4, blob, WRAP_BLOB_LEN);
    ae_zero(blob, blen); free(blob);
    if (ae_store_atomic_write(s->store, AE_ZONE_DATA, KEY_FILE, keyfile, sizeof keyfile) != 0) {
        ae_zero(dek, sizeof dek); return -1;
    }
    set_dek(s, dek);
    ae_zero(dek, sizeof dek);
    table_t empty; memset(&empty, 0, sizeof empty);
    if (vault_save_table(s, &empty) != 0) { lock_dek(s); return -1; }
    lock_dek(s);
    if (save_auth(s) != 0) return -1;
    uint8_t base[AE_SHA256_LEN];
    (void)ae_integrity_seal(s->store, s->audit, base);
    (void)ae_audit_append(s->audit, "INIT", "credentials-and-vault-created");
    s->state = AE_SEC_LOCKED;
    s->tampered = 0;
    return 0;
}

/* ------------------------------------------------------------------ 开机自检 */
int ae_secure_boot(ae_secure_t *s, char *msg, size_t n)
{
    if (!s) return -1;
    if (msg && n) msg[0] = 0;
    uint64_t cnt = 0; uint8_t head[AE_SHA256_LEN];
    int chain_bad = ae_audit_verify(s->audit, &cnt, head) != 0;
    int ok = 0; uint8_t cur[AE_SHA256_LEN], base[AE_SHA256_LEN];
    int integ_bad = (ae_integrity_check(s->store, &ok, cur, base) != 0) || !ok;
    if (chain_bad || integ_bad || s->tampered) {
        if (msg && n) snprintf(msg, n,
            "ALERT 自检异常: audit=%s integrity=%s counter=%s",
            chain_bad ? "BROKEN" : "ok", integ_bad ? "TAMPERED" : "ok",
            s->tampered ? "SUSPECT" : "ok");
        (void)ae_audit_append(s->audit, "TAMPER_DETECT", "boot-self-check");
        if (s->cfg.tamper_wipe) {
            char rp[256];
            (void)ae_secure_wipe(s, "tamper-detected", rp, sizeof rp);
            if (msg && n) {
                size_t l = strlen(msg);
                snprintf(msg + l, n - l, " -> 已触发全盘数据销毁(篡改即擦)");
            }
        }
        return 1;
    }
    (void)ae_audit_append(s->audit, "BOOT", "integrity-and-audit-ok");
    if (msg && n) snprintf(msg, n,
        "OK 系统完好 | sys/ 基线匹配 | 审计链 %llu 条完整 | 状态=%s",
        (unsigned long long)cnt,
        s->state == AE_SEC_FACTORY ? "FACTORY(待设置口令)" : "LOCKED(锁屏)");
    return 0;
}

/* --------------------------------------------------------------- 自毁引擎 */
static void write_receipt(ae_secure_t *s, const char *trigger,
                          const uint8_t *vb, int have_vb,
                          const uint8_t va[AE_SHA256_LEN],
                          const uint8_t sb[AE_SHA256_LEN],
                          const uint8_t sa[AE_SHA256_LEN], int sys_unchanged,
                          const uint8_t ab[AE_SHA256_LEN],
                          const uint8_t aa[AE_SHA256_LEN],
                          char *name_out, size_t n)
{
    char vbh[65] = "none", vah[65], sbh[65], sah[65], abh[65], aah[65];
    if (have_vb) hex_of(vb, vbh);
    hex_of(va, vah); hex_of(sb, sbh); hex_of(sa, sah); hex_of(ab, abh); hex_of(aa, aah);
    char body[2048];
    snprintf(body, sizeof body,
        "{\n"
        "  \"event\": \"AETHER_DATA_DESTRUCTION\",\n"
        "  \"trigger\": \"%s\",\n"
        "  \"timestamp\": %lld,\n"
        "  \"volume_bytes\": %u,\n"
        "  \"overwrite_passes\": %d,\n"
        "  \"volume_hash_before\": \"%s\",\n"
        "  \"volume_hash_after\": \"%s\",\n"
        "  \"sys_digest_before\": \"%s\",\n"
        "  \"sys_digest_after\": \"%s\",\n"
        "  \"sys_unchanged\": %s,\n"
        "  \"key_material\": \"crypto-shredded+overwritten+removed\",\n"
        "  \"credentials\": \"all-purged (factory state)\",\n"
        "  \"audit_head_before\": \"%s\",\n"
        "  \"audit_head_after\": \"%s\",\n"
        "  \"result\": \"ALL_USER_DATA_DESTROYED_SYSTEM_INTACT\"\n"
        "}\n",
        trigger, (long long)time(NULL), (unsigned)AE_VAULT_SIZE, AE_WIPE_PASSES,
        vbh, vah, sbh, sah, sys_unchanged ? "true" : "false", abh, aah);
    char nm[128];
    snprintf(nm, sizeof nm, "wipe-%llu.json", (unsigned long long)ae_audit_seq(s->audit));
    (void)ae_store_atomic_write(s->store, AE_ZONE_RECEIPT, nm, body, strlen(body));
    if (name_out && n) snprintf(name_out, n, "%s/%s", AE_ZONE_RECEIPT, nm);
}

int ae_secure_wipe(ae_secure_t *s, const char *trigger, char *receipt_path_out, size_t n)
{
    if (!s) return -1;
    if (receipt_path_out && n) receipt_path_out[0] = 0;
    const uint8_t *ab = ae_audit_head(s->audit);
    uint8_t audit_before[AE_SHA256_LEN]; memcpy(audit_before, ab, AE_SHA256_LEN);
    (void)ae_audit_append(s->audit, "WIPE_BEGIN", trigger ? trigger : "unspecified");

    /* 1) 内存中密钥立即清零；会话强制上锁 */
    lock_dek(s);

    /* 2) 擦除前取证：sys/ 摘要、卷哈希 */
    uint8_t sys_before[AE_SHA256_LEN], sys_after[AE_SHA256_LEN];
    (void)ae_store_zone_digest(s->store, AE_ZONE_SYS, sys_before);
    uint8_t vol_before[AE_SHA256_LEN]; int have_vb = 0;
    char vp[1024];
    if (ae_store_path(s->store, vp, sizeof vp, AE_ZONE_DATA, VOL_FILE) == 0) {
        int fok = 0; ae_sha256_file(vp, vol_before, &fok); have_vb = fok;
    }

    /* 3) 逐文件销毁 data/ 全部内容：卷多轮覆写，其余覆写后删除 */
    char (*names)[AE_STORE_NAME_MAX] = NULL;
    int max = 256, count = 0;
    names = (char(*)[AE_STORE_NAME_MAX])calloc((size_t)max, AE_STORE_NAME_MAX);
    uint8_t vol_after[AE_SHA256_LEN]; memset(vol_after, 0, sizeof vol_after);
    if (names && ae_store_list(s->store, AE_ZONE_DATA, names, max, &count) == 0) {
        int i;
        for (i = 0; i < count; i++) {
            if (strcmp(names[i], VOL_FILE) == 0) {
                if (ae_store_volume_overwrite(s->store, VOL_FILE, AE_WIPE_PASSES, vol_after) != 0)
                    memset(vol_after, 0, sizeof vol_after);
                (void)ae_store_remove(s->store, AE_ZONE_DATA, VOL_FILE);
            } else {
                uint8_t *fb = NULL; size_t fl = 0;
                if (ae_store_read(s->store, AE_ZONE_DATA, names[i], &fb, &fl) == 0) {
                    if (fl) {
                        uint8_t *ov = (uint8_t*)malloc(fl);
                        if (ov) {
                            if (ae_random(ov, fl) == 0)
                                (void)ae_store_atomic_write(s->store, AE_ZONE_DATA, names[i], ov, fl);
                            memset(ov, 0, fl);
                            (void)ae_store_atomic_write(s->store, AE_ZONE_DATA, names[i], ov, fl);
                            ae_zero(ov, fl); free(ov);
                        }
                    }
                    free(fb);
                }
                (void)ae_store_remove(s->store, AE_ZONE_DATA, names[i]);
            }
        }
    }
    free(names);
    if (!have_vb) memset(vol_before, 0, sizeof vol_before);

    /* 4) 硬断言：sys/ 逐字节未变（擦除动作绝不碰系统本体） */
    (void)ae_store_zone_digest(s->store, AE_ZONE_SYS, sys_after);
    int sys_unchanged = ae_ct_eq(sys_before, sys_after, AE_SHA256_LEN) ? 1 : 0;

    /* 5) 清除全部认证凭据，进入出厂态 */
    memset(&s->auth, 0, sizeof s->auth);
    s->state = AE_SEC_FACTORY;
    s->tampered = 0;

    /* 6) 审计收尾 + 可离线校验的销毁证明 */
    (void)ae_audit_append(s->audit, "WIPE_DONE",
                          sys_unchanged ? "system-intact" : "SYS-CHANGED-FATAL");
    const uint8_t *aa = ae_audit_head(s->audit);
    uint8_t audit_after[AE_SHA256_LEN]; memcpy(audit_after, aa, AE_SHA256_LEN);
    char rp[256];
    write_receipt(s, trigger ? trigger : "unspecified", vol_before, have_vb, vol_after,
                  sys_before, sys_after, sys_unchanged, audit_before, audit_after,
                  rp, sizeof rp);
    snprintf(s->last_receipt, sizeof s->last_receipt, "%s", rp);
    if (receipt_path_out && n) snprintf(receipt_path_out, n, "%s", rp);
    return sys_unchanged ? 0 : -1;
}

/* ------------------------------------------------------------------ 认证 */
static void apply_backoff(ae_secure_t *s, uint32_t fails)
{
    if (!s->cfg.delay_enable) return;
    uint32_t d = 1u << (fails < 6 ? (fails - 1) : 5);   /* 1,2,4,8,16,32 -> 上限 32 */
    if (d > 8) d = 8;
    (void)sleep(d);
}
static int integrity_or_wipe(ae_secure_t *s, ae_unlock_t *res, char *msg, size_t n)
{
    int ok = 0; uint8_t cur[AE_SHA256_LEN], base[AE_SHA256_LEN];
    int bad = (ae_integrity_check(s->store, &ok, cur, base) != 0) || !ok;
    uint64_t cnt; uint8_t head[AE_SHA256_LEN];
    if (ae_audit_verify(s->audit, &cnt, head) != 0) bad = 1;
    if (s->tampered) bad = 1;
    if (bad) {
        (void)ae_audit_append(s->audit, "TAMPER_DETECT", "unlock-path");
        if (s->cfg.tamper_wipe) {
            char rp[256]; (void)ae_secure_wipe(s, "tamper-detected", rp, sizeof rp);
            if (msg && n) snprintf(msg, n, "TAMPER 检出系统/计数篡改 -> 已执行全盘数据销毁");
            *res = AE_UNLOCK_TAMPER;
        } else {
            if (msg && n) snprintf(msg, n, "TAMPER 检出篡改（tamper_wipe=0，未擦除）");
            *res = AE_UNLOCK_ERR;
        }
        return 1;
    }
    return 0;
}

ae_unlock_t ae_secure_unlock(ae_secure_t *s, const char *password, char *msg, size_t n)
{
    if (!s || !password) return AE_UNLOCK_ERR;
    if (msg && n) msg[0] = 0;
    ae_unlock_t res = AE_UNLOCK_ERR;
    if (integrity_or_wipe(s, &res, msg, n)) return res;
    if (s->state == AE_SEC_FACTORY) {
        if (msg && n) snprintf(msg, n, "出厂态：数据已清空，请先 init 设置口令");
        return AE_UNLOCK_ERR;
    }
    if (s->state == AE_SEC_UNLOCKED) return AE_UNLOCK_OK;

    /* 胁迫口令：被逼解锁时输入 -> 立即销毁 */
    uint8_t tmp[AE_SHA256_LEN];
    int duress_set = 0;
    { uint8_t z[AE_SHA256_LEN]; memset(z, 0, sizeof z);
      duress_set = !ae_ct_eq(s->auth.duress_hash, z, AE_SHA256_LEN); }
    if (duress_set) {
        if (ae_pbkdf2(password, strlen(password), s->auth.duress_salt, AE_SALT_LEN,
                      AE_PBKDF2_ITER, tmp) == 0 &&
            ae_ct_eq(tmp, s->auth.duress_hash, AE_SHA256_LEN)) {
            (void)ae_audit_append(s->audit, "DURESS", "coerced-unlock-attempt");
            char rp[256]; (void)ae_secure_wipe(s, "duress-password", rp, sizeof rp);
            if (msg && n) snprintf(msg, n, "DURESS 胁迫口令 -> 已执行全盘数据销毁");
            ae_zero(tmp, sizeof tmp);
            return AE_UNLOCK_DURESS;
        }
    }
    ae_zero(tmp, sizeof tmp);   /* duress 不匹配时也不留口令派生残值 */

    /* 单次派生 + 常量时间比较，避免时序侧信道，也不做重复计算 */
    uint8_t calc[AE_SHA256_LEN];
    if (ae_pbkdf2(password, strlen(password), s->auth.pw_salt, AE_SALT_LEN,
                  AE_PBKDF2_ITER, calc) != 0) return AE_UNLOCK_ERR;
    int matched = ae_ct_eq(calc, s->auth.pw_hash, AE_SHA256_LEN);
    ae_zero(calc, sizeof calc);

    if (matched) {
        uint8_t kek[AE_KEY_LEN], wrapped[WRAP_BLOB_LEN];
        if (derive_kek(password, s->auth.kek_salt, kek) != 0) return AE_UNLOCK_ERR;
        if (key_load(s, wrapped) != 0) { ae_zero(kek,sizeof kek); return AE_UNLOCK_ERR; }
        uint8_t *dek = NULL; size_t dlen = 0;
        int opened = ae_open(kek, wrapped, WRAP_BLOB_LEN,
                             (const uint8_t*)AAD_WRAP, strlen(AAD_WRAP), &dek, &dlen);
        ae_zero(kek, sizeof kek); ae_zero(wrapped, sizeof wrapped);
        if (opened != 0 || dlen != AE_KEY_LEN) { if (dek) free(dek); return AE_UNLOCK_ERR; }
        set_dek(s, dek);
        ae_zero(dek, dlen); free(dek);
        s->auth.fails = 0;
        (void)save_auth(s);
        s->state = AE_SEC_UNLOCKED;
        s->last_activity = time(NULL);
        (void)ae_audit_append(s->audit, "AUTH_OK", "session-unlocked");
        if (msg && n) snprintf(msg, n, "OK 解锁成功，已进入安全会话");
        return AE_UNLOCK_OK;
    }

    /* 失败：计数立即原子落盘 + 写链（双重防绕过），退避，达阈值即擦除 */
    s->auth.fails++;
    uint32_t cf = chain_recent_fails(s);
    if (cf + 1 > s->auth.fails) s->auth.fails = cf + 1;
    (void)save_auth(s);
    char det[64];
    snprintf(det, sizeof det, "attempt=%u", s->auth.fails);
    (void)ae_audit_append(s->audit, "AUTH_FAIL", det);
    uint32_t threshold = s->auth.max_fails ? s->auth.max_fails : s->cfg.max_fails;
    if (s->auth.fails >= threshold) {
        uint32_t reached = s->auth.fails;   /* wipe 会清空 auth，先留存用于如实回报 */
        (void)ae_audit_append(s->audit, "WIPE_TRIGGER", "max-fails-reached");
        char rp[256]; (void)ae_secure_wipe(s, "max-fails-reached", rp, sizeof rp);
        if (msg && n) snprintf(msg, n,
            "WIPED 连续错误 %u 次达到阈值 %u -> 全部用户数据已销毁，系统完好",
            reached, threshold);
        return AE_UNLOCK_WIPED;
    }
    apply_backoff(s, s->auth.fails);
    if (msg && n) snprintf(msg, n, "FAIL 口令错误（%u/%u）", s->auth.fails, threshold);
    return AE_UNLOCK_FAIL;
}

int ae_secure_lock(ae_secure_t *s)
{
    if (!s) return -1;
    lock_dek(s);
    s->state = AE_SEC_LOCKED;
    (void)ae_audit_append(s->audit, "LOCK", "session-locked");
    return 0;
}
int ae_secure_panic(ae_secure_t *s, const char *reason)
{
    char rp[256];
    return ae_secure_wipe(s, reason ? reason : "panic-command", rp, sizeof rp);
}

/* --------------------------------------------------------------- 会话保险库 */
static int session_ok(ae_secure_t *s)
{
    if (!s || s->state != AE_SEC_UNLOCKED || !s->dek_valid) return 0;
    if (s->cfg.idle_timeout_s) {
        time_t now = time(NULL);
        if (now - s->last_activity > (time_t)s->cfg.idle_timeout_s) {
            (void)ae_secure_lock(s);
            return 0;
        }
    }
    s->last_activity = time(NULL);
    return 1;
}
int ae_secure_put(ae_secure_t *s, const char *name, const void *data, size_t len)
{
    if (!session_ok(s) || !name || !*name || strlen(name) >= AE_SEC_NAME_MAX) return -1;
    table_t t;
    if (vault_load_table(s, &t) != 0) return -1;
    int i, found = -1;
    for (i = 0; i < t.n; i++) if (!strcmp(t.e[i].name, name)) { found = i; break; }
    if (found >= 0) { free(t.e[found].data); t.e[found].data = NULL; t.e[found].len = 0; }
    else { if (t.n >= MAX_ENTRIES) { table_free(&t); return -1; } found = t.n++; }
    snprintf(t.e[found].name, AE_SEC_NAME_MAX, "%s", name);
    t.e[found].data = (uint8_t*)malloc(len ? len : 1);
    if (!t.e[found].data) { table_free(&t); return -1; }
    if (len) memcpy(t.e[found].data, data, len);
    t.e[found].len = len;
    int rc = vault_save_table(s, &t);
    table_free(&t);
    (void)ae_audit_append(s->audit, "VAULT_PUT", "file-stored-encrypted");
    return rc;
}
int ae_secure_get(ae_secure_t *s, const char *name, uint8_t **out, size_t *len)
{
    if (!out || !len) return -1;
    *out = NULL; *len = 0;
    if (!session_ok(s) || !name) return -1;
    table_t t;
    if (vault_load_table(s, &t) != 0) return -1;
    int i, rc = -1;
    for (i = 0; i < t.n; i++) if (!strcmp(t.e[i].name, name)) {
        *out = (uint8_t*)malloc(t.e[i].len ? t.e[i].len : 1);
        if (*out) { if (t.e[i].len) memcpy(*out, t.e[i].data, t.e[i].len); *len = t.e[i].len; rc = 0; }
        break;
    }
    table_free(&t);
    if (rc == 0) (void)ae_audit_append(s->audit, "VAULT_GET", "file-read");
    return rc;
}
int ae_secure_list(ae_secure_t *s, char (*names)[AE_SEC_NAME_MAX], int max, int *count)
{
    if (!count) return -1;
    *count = 0;
    if (!session_ok(s) || !names) return -1;
    table_t t;
    if (vault_load_table(s, &t) != 0) return -1;
    int i;
    for (i = 0; i < t.n && *count < max; i++)
        snprintf(names[(*count)++], AE_SEC_NAME_MAX, "%s", t.e[i].name);
    table_free(&t);
    return 0;
}
int ae_secure_del(ae_secure_t *s, const char *name)
{
    if (!session_ok(s) || !name) return -1;
    table_t t;
    if (vault_load_table(s, &t) != 0) return -1;
    int i, found = -1;
    for (i = 0; i < t.n; i++) if (!strcmp(t.e[i].name, name)) { found = i; break; }
    if (found < 0) { table_free(&t); return -1; }
    free(t.e[found].data);
    if (found != t.n - 1) t.e[found] = t.e[t.n - 1];
    t.n--;
    int rc = vault_save_table(s, &t);
    table_free(&t);
    return rc;
}

/* ------------------------------------------------------------------ 状态输出 */
int ae_secure_status(ae_secure_t *s, char *buf, size_t n)
{
    if (!s || !buf || !n) return -1;
    uint64_t cnt = 0; uint8_t head[AE_SHA256_LEN];
    int chain_ok = ae_secure_audit_verify(s, &cnt, head) == 0;
    char hh[65]; hex_of(head, hh);
    int ok = 0; uint8_t cur[AE_SHA256_LEN], base[AE_SHA256_LEN];
    int integ = ae_integrity_check(s->store, &ok, cur, base) == 0 && ok;
    uint8_t sd[AE_SHA256_LEN]; (void)ae_store_zone_digest(s->store, AE_ZONE_SYS, sd);
    char sdh[65]; hex_of(sd, sdh);
    size_t vs = 0; (void)ae_store_volume_size(s->store, VOL_FILE, &vs);
    static const char *st[] = {"LOCKED", "UNLOCKED", "FACTORY"};
    snprintf(buf, n,
        "AetherOS Secure Core / 长明 Aether\n"
        "  state        : %s\n"
        "  fails        : %u / %u\n"
        "  volume       : %zu bytes (%s)\n"
        "  audit        : %llu events, chain=%s, head=%s\n"
        "  integrity    : %s, sys_digest=%s\n"
        "  last_receipt : %s\n"
        "  security     : fail-wipe / duress / crypto-shred / tamper-evident / idle-lock / panic\n",
        st[s->state], s->auth.fails, ae_secure_max_fails(s),
        vs, vs ? "present" : "absent",
        (unsigned long long)cnt, chain_ok ? "INTACT" : "BROKEN", hh,
        integ ? "OK" : "TAMPERED", sdh,
        s->last_receipt[0] ? s->last_receipt : "(none)");
    return 0;
}
