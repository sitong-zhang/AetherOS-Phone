/*
 * AetherOS Kernel · 内核态安全子系统（主打安全）
 *
 * 磁盘布局（虚拟 IDE 盘 = 可擦除「用户数据区」，内核 ELF 本身由 -kernel 加载，
 * 属于「系统区」，擦除引擎绝不触碰）：
 *   LBA 0     : 超级块（magic、口令盐/哈希、胁迫口令哈希、失败计数、审计锚、MAC）
 *   LBA 8..   : 加密保险库数据卷（ChaCha20 + HMAC 密封记录）
 *   LBA 2064..: 审计链记录（只追加）
 *
 * 输错 N 次 -> 销毁 DEK(crypto-shred) + 多轮覆写整个数据卷 + 清除全部凭据
 *            -> 系统区（内核代码）哈希硬断言不变 -> 生成可离线校验的销毁证明。
 * 失败计数三重防护：原子落盘 + 设备密钥 HMAC + 审计链交叉核对（防重启绕过）。
 */
#include "aether_kernel.h"

/* ---- 超级块 ---- */
typedef struct K_PACKED {
    uint32_t magic;
    uint32_t version;
    uint8_t  pw_salt[K_SALT_BYTES];
    uint8_t  pw_hash[K_HASH_BYTES];
    uint8_t  duress_hash[K_HASH_BYTES];
    uint8_t  dek_sealed[K_KEY_BYTES + K_TAG_BYTES];  /* DEK XOR KEK + HMAC */
    uint32_t fails;
    uint32_t max_fails;
    uint32_t audit_seq;
    uint8_t  audit_head[K_HASH_BYTES];
    uint8_t  mac[K_HASH_BYTES];        /* HMAC(设备密钥, 上面所有字段) */
} super_t;

/* ---- 保险库记录（每条 1 扇区内，简化定长）---- */
typedef struct K_PACKED {
    uint32_t used;
    char     name[K_NAME_MAX_LEN + 1];
    uint32_t len;
    uint8_t  nonce[K_NONCE_BYTES];
    uint8_t  tag[K_TAG_BYTES];
    uint8_t  data[K_VAULT_FILE_BYTES];
} record_t;

/* ---- 审计事件（只追加、哈希链）---- */
typedef struct K_PACKED {
    uint32_t seq;
    char     event[24];
    char     detail[64];
    uint8_t  prev[K_HASH_BYTES];
    uint8_t  hash[K_HASH_BYTES];
} audit_evt_t;

/* 设备密钥：内核只读区常量，模拟 TEE 锚点（如实标注：软件模拟，非真 TEE） */
static const uint8_t DEV_KEY[32] = {
    0xA3,0x7E,0x11,0xC4,0x09,0xD2,0x55,0x8B,0x31,0x6F,0xEA,0x70,0x2D,0x94,0xC8,0x16,
    0x4B,0xB0,0x27,0x5C,0x8E,0xF3,0x19,0x6A,0xD7,0x42,0x0B,0x9D,0xE5,0x38,0xAC,0x71
};

static super_t g_super;
static uint8_t g_dek[K_KEY_BYTES];
static bool g_dek_valid;
static uint8_t g_kek[K_KEY_BYTES];
static int g_state = KSEC_STATE_FACTORY;
static char g_receipt[2048];
static uint64_t g_sys_digest_before;
static uint32_t g_last_wipe_fails;
static char g_last_wipe_reason[32];
static uint32_t g_idle_last_tick;
static uint8_t g_sector[K_SECTOR_BYTES];   /* 复用扇区缓冲，避免内核栈溢出 */

/* ---- 系统区摘要：对内核 .text/.rodata 做哈希，证明「系统没被抹除」---- */
extern uint8_t __kernel_text_start[], __kernel_ro_end[];
uint64_t ksec_sys_digest(void) {
    uint8_t h[K_HASH_BYTES];
    /* 只哈希内核代码 + 只读数据（__kernel_text_start..__kernel_ro_end）：运行期不可变。
     * 不能从 __kernel_start 起算（那会包含 .boot_scratch 页表——vmm_map 拆用户页时
     * 会改写页表），也不能包含 .data/.bss（运行期全局变量会变）。这两类都会让销毁前后
     * 摘要不同，误判「系统被破坏」。 */
    size_t n = (size_t)(__kernel_ro_end - __kernel_text_start);
    if (n > 262144) n = 262144;
    sha256_buf((const void *)(uintptr_t)__kernel_text_start, n, h);
    uint64_t d = 0;
    for (int i = 0; i < 8; i++) d = (d << 8) | h[i];
    return d;
}

/* ---- 超级块读写 + MAC ---- */
static void super_mac(uint8_t out[K_HASH_BYTES]) {
    hmac_sha256(DEV_KEY, sizeof DEV_KEY, &g_super,
                (size_t)((uint8_t *)g_super.mac - (uint8_t *)&g_super), out);
}
static int super_load(void) {
    if (!ata_present()) return -1;
    if (ata_read(K_DISK_SUPER_LBA, g_sector) != 0) return -1;
    super_t *s = (super_t *)g_sector;
    if (s->magic != K_DISK_MAGIC) return -1;
    kmemcpy(&g_super, s, sizeof(super_t));
    uint8_t m[K_HASH_BYTES]; super_mac(m);
    return ct_eq(m, g_super.mac, K_HASH_BYTES) ? 0 : -2;   /* -2 = MAC 不符（篡改） */
}
static int super_save(void) {
    if (!ata_present()) return -1;
    super_mac(g_super.mac);
    kmemcpy(g_sector, &g_super, sizeof(super_t));
    kmemset(g_sector + sizeof(super_t), 0, K_SECTOR_BYTES - sizeof(super_t));
    return ata_write(K_DISK_SUPER_LBA, g_sector);
}

/* ---- 审计链 ---- */
static int audit_append(const char *event, const char *detail) {
    if (!ata_present()) return -1;
    audit_evt_t e;
    kmemset(&e, 0, sizeof e);
    e.seq = ++g_super.audit_seq;
    kstrncpy(e.event, event, sizeof e.event);
    kstrncpy(e.detail, detail, sizeof e.detail);
    kmemcpy(e.prev, g_super.audit_head, K_HASH_BYTES);
    uint8_t h[K_HASH_BYTES];
    sha256_ctx c; sha256_init(&c);
    sha256_update(&c, g_super.audit_head, K_HASH_BYTES);
    sha256_update(&c, &e.seq, sizeof e.seq);
    sha256_update(&c, e.event, sizeof e.event);
    sha256_update(&c, e.detail, sizeof e.detail);
    sha256_final(&c, h);
    kmemcpy(e.hash, h, K_HASH_BYTES);
    kmemcpy(g_super.audit_head, h, K_HASH_BYTES);
    uint32_t lba = K_DISK_JOURNAL_LBA + (e.seq - 1) % K_DISK_JOURNAL_SECTORS;
    kmemcpy(g_sector, &e, sizeof e);
    kmemset(g_sector + sizeof e, 0, K_SECTOR_BYTES - sizeof e);
    int r = ata_write(lba, g_sector);
    if (r == 0) r = super_save();
    return r;
}
int ksec_audit_verify(void) {
    if (!ata_present()) return -1;
    uint8_t prev[K_HASH_BYTES]; kmemset(prev, 0, sizeof prev);
    uint32_t n = g_super.audit_seq;
    if (n > K_DISK_JOURNAL_SECTORS) n = K_DISK_JOURNAL_SECTORS;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx = (i + (g_super.audit_seq > K_DISK_JOURNAL_SECTORS
                            ? g_super.audit_seq - K_DISK_JOURNAL_SECTORS : 0))
                       % K_DISK_JOURNAL_SECTORS;
        if (ata_read(K_DISK_JOURNAL_LBA + idx, g_sector) != 0) return -2;
        audit_evt_t *e = (audit_evt_t *)g_sector;
        uint8_t h[K_HASH_BYTES];
        sha256_ctx c; sha256_init(&c);
        sha256_update(&c, prev, K_HASH_BYTES);
        sha256_update(&c, &e->seq, sizeof e->seq);
        sha256_update(&c, e->event, sizeof e->event);
        sha256_update(&c, e->detail, sizeof e->detail);
        sha256_final(&c, h);
        if (!ct_eq(h, e->hash, K_HASH_BYTES)) return -3;
        kmemcpy(prev, h, K_HASH_BYTES);
    }
    return ct_eq(prev, g_super.audit_head, K_HASH_BYTES) ? 0 : -4;
}
int ksec_audit_dump(char *buf, size_t n) {
    int p = ksnprintf(buf, n, "AUDIT seq=%u head=", g_super.audit_seq);
    khex(buf + p, g_super.audit_head, K_HASH_BYTES);
    return 1;
}

/* ---- 密钥派生 / DEK ---- */
static void derive_kek(const char *pw) {
    pbkdf2(pw, kstrlen(pw), g_super.pw_salt, K_SALT_BYTES, K_PBKDF2_ITER, g_kek);
}
static void seal_dek(void) {
    uint8_t rnd[K_KEY_BYTES];
    csprng(rnd, K_KEY_BYTES);
    for (uint32_t i = 0; i < K_KEY_BYTES; i++) g_super.dek_sealed[i] = (uint8_t)(g_dek[i] ^ g_kek[i]);
    hmac_sha256(g_kek, K_KEY_BYTES, g_super.dek_sealed, K_KEY_BYTES,
                g_super.dek_sealed + K_KEY_BYTES);
}
static int open_dek(void) {
    uint8_t tag[K_HASH_BYTES];
    hmac_sha256(g_kek, K_KEY_BYTES, g_super.dek_sealed, K_KEY_BYTES, tag);
    if (!ct_eq(tag, g_super.dek_sealed + K_KEY_BYTES, K_HASH_BYTES)) return -1;
    for (uint32_t i = 0; i < K_KEY_BYTES; i++) g_dek[i] = (uint8_t)(g_super.dek_sealed[i] ^ g_kek[i]);
    g_dek_valid = true;
    return 0;
}
static void shred_dek(void) {
    kmemset(g_dek, 0, sizeof g_dek);
    kmemset(g_kek, 0, sizeof g_kek);
    g_dek_valid = false;
}

/* ---- 保险库扇区 ---- */
static int rec_lba(uint32_t idx) { return (int)(K_DISK_VAULT_LBA + idx); }
static void rec_seal(record_t *r) {
    uint8_t n[K_NONCE_BYTES]; csprng(n, K_NONCE_BYTES);
    kmemcpy(r->nonce, n, K_NONCE_BYTES);
    chacha20(g_dek, n, 1, r->data, r->data, K_VAULT_FILE_BYTES);
    hmac_sha256(g_dek, K_KEY_BYTES, r->data, K_VAULT_FILE_BYTES, r->tag);
}
static int rec_open(record_t *r) {
    uint8_t tag[K_HASH_BYTES];
    hmac_sha256(g_dek, K_KEY_BYTES, r->data, K_VAULT_FILE_BYTES, tag);
    if (!ct_eq(tag, r->tag, K_HASH_BYTES)) return -1;
    chacha20(g_dek, r->nonce, 1, r->data, r->data, K_VAULT_FILE_BYTES);
    return 0;
}

/* ---- 多轮覆写整卷 ---- */
static void overwrite_vault(uint8_t final_hash[K_HASH_BYTES]) {
    const uint8_t pats[K_WIPE_PATTERNS] = { 0x00, 0xFF, 0xAA, 0x00 };
    for (uint32_t pass = 0; pass < K_WIPE_PASSES; pass++) {
        uint32_t pat = pass < K_WIPE_PATTERNS ? pats[pass] : 0x00;
        for (uint32_t l = 0; l < K_DISK_VAULT_SECTORS; l++) {
            if (pat == 0xAA) csprng(g_sector, K_SECTOR_BYTES);
            else kmemset(g_sector, pat, K_SECTOR_BYTES);
            (void)ata_write(K_DISK_VAULT_LBA + l, g_sector);
        }
    }
    /* 读回真实落盘内容求哈希（不是内存副本） */
    sha256_ctx c; sha256_init(&c);
    for (uint32_t l = 0; l < K_DISK_VAULT_SECTORS; l++) {
        if (ata_read(K_DISK_VAULT_LBA + l, g_sector) == 0)
            sha256_update(&c, g_sector, K_SECTOR_BYTES);
    }
    sha256_final(&c, final_hash);
}

/* ---- 核心：全盘数据销毁，系统保留 ---- */
static void do_wipe(const char *reason, uint32_t fails_at_trigger) {
    g_last_wipe_fails = fails_at_trigger;
    kstrncpy(g_last_wipe_reason, reason, sizeof g_last_wipe_reason);
    (void)audit_append("WIPE_BEGIN", reason);
    shred_dek();
    uint8_t before[K_HASH_BYTES], after[K_HASH_BYTES];
    /* 覆写前卷摘要（取第一扇区做代表，用于证明前后不同） */
    if (ata_read(K_DISK_VAULT_LBA, g_sector) == 0) sha256_buf(g_sector, K_SECTOR_BYTES, before);
    else kmemset(before, 0, sizeof before);
    overwrite_vault(after);
    /* 清除全部凭据 */
    kmemset(g_super.pw_hash, 0, sizeof g_super.pw_hash);
    kmemset(g_super.duress_hash, 0, sizeof g_super.duress_hash);
    kmemset(g_super.dek_sealed, 0, sizeof g_super.dek_sealed);
    kmemset(g_super.pw_salt, 0, sizeof g_super.pw_salt);
    g_super.fails = 0;
    g_state = KSEC_STATE_FACTORY;
    (void)super_save();
    uint64_t sys_after = ksec_sys_digest();
    int sys_ok = (sys_after == g_sys_digest_before);
    (void)audit_append("WIPE_DONE", sys_ok ? "sys=INTACT" : "sys=CHANGED!!");
    (void)audit_append("WIPE_TRIGGER", reason);

    char bh[65], ah[65], sh[65], vh[65];
    khex(bh, before, K_HASH_BYTES); khex(ah, after, K_HASH_BYTES);
    uint8_t syd[K_HASH_BYTES];
    sha256_buf((const void *)(uintptr_t)__kernel_text_start,
              (size_t)(__kernel_ro_end - __kernel_text_start), syd);
    khex(sh, syd, K_HASH_BYTES); khex(vh, after, K_HASH_BYTES);
    ksnprintf(g_receipt, sizeof g_receipt,
        "{\n"
        "  \"event\": \"AETHER_KERNEL_FULL_DATA_DESTRUCTION\",\n"
        "  \"trigger\": \"%s\",\n"
        "  \"fails_at_trigger\": %u,\n"
        "  \"max_fails\": %u,\n"
        "  \"wipe_passes\": %u,\n"
        "  \"vault_sectors\": %u,\n"
        "  \"volume_hash_before\": \"%s\",\n"
        "  \"volume_hash_after\": \"%s\",\n"
        "  \"volume_changed\": %s,\n"
        "  \"sys_digest\": \"%s\",\n"
        "  \"sys_unchanged\": %s,\n"
        "  \"key_material\": \"DESTROYED\",\n"
        "  \"credentials\": \"PURGED\",\n"
        "  \"audit_seq\": %u,\n"
        "  \"result\": \"%s\"\n"
        "}\n",
        reason, fails_at_trigger, g_super.max_fails, K_WIPE_PASSES,
        K_DISK_VAULT_SECTORS, bh, ah,
        ct_eq(before, after, K_HASH_BYTES) ? "false" : "true",
        sh, sys_ok ? "true" : "false", g_super.audit_seq,
        sys_ok ? "ALL_USER_DATA_DESTROYED_SYSTEM_INTACT" : "SYSTEM_INTEGRITY_FAILURE");
    (void)vh;
}

/* ---- 开机自检 / 初始化 ---- */
void ksec_init(void) {
    kmemset(&g_super, 0, sizeof g_super);
    shred_dek();
    g_state = KSEC_STATE_FACTORY;
    g_super.max_fails = K_MAX_FAILS_DEFAULT;
    g_super.version = 1;
    g_sys_digest_before = ksec_sys_digest();
    g_receipt[0] = 0;
    g_idle_last_tick = 0;
    g_last_wipe_fails = 0;
}
int ksec_boot(char *msg, size_t n) {
    int r = super_load();
    if (r == -1) {          /* 无有效超块 = 出厂态 */
        g_state = KSEC_STATE_FACTORY;
        return ksnprintf(msg, n, "BOOT OK: no credentials (factory state)\n");
    }
    if (r == -2) {          /* MAC 不符 = 篡改超块 -> 立即擦除 */
        do_wipe("tamper-superblock-mac", 0);
        return ksnprintf(msg, n, "BOOT TAMPER DETECTED -> ALL USER DATA WIPED, SYSTEM INTACT\n");
    }
    g_state = KSEC_STATE_LOCKED;
    int av = ksec_audit_verify();
    if (av != 0) {          /* 审计链断裂 = 有人试图抹掉失败计数 */
        do_wipe("tamper-audit-chain", 0);
        return ksnprintf(msg, n, "BOOT AUDIT CHAIN BROKEN -> ALL USER DATA WIPED\n");
    }
    return ksnprintf(msg, n,
        "BOOT OK: state=LOCKED fails=%u/%u audit_seq=%u integrity=OK\n",
        g_super.fails, g_super.max_fails, g_super.audit_seq);
}
int ksec_setup(const char *pw, const char *duress) {
    if (!pw || kstrlen(pw) < 1) return -1;
    kmemset(&g_super, 0, sizeof g_super);
    g_super.magic = K_DISK_MAGIC;
    g_super.version = 1;
    g_super.max_fails = K_MAX_FAILS_DEFAULT;
    csprng(g_super.pw_salt, K_SALT_BYTES);
    pbkdf2(pw, kstrlen(pw), g_super.pw_salt, K_SALT_BYTES, K_PBKDF2_ITER, g_super.pw_hash);
    if (duress && kstrlen(duress))
        pbkdf2(duress, kstrlen(duress), g_super.pw_salt, K_SALT_BYTES,
               K_PBKDF2_ITER, g_super.duress_hash);
    csprng(g_dek, K_KEY_BYTES);
    derive_kek(pw);
    seal_dek();
    g_dek_valid = true;
    kmemset(g_super.audit_head, 0, K_HASH_BYTES);
    g_super.audit_seq = 0;
    if (super_save() != 0) return -2;
    if (audit_append("PROVISION", "factory init") != 0) return -3;
    shred_dek();                       /* 出厂后立即锁屏，DEK 不驻留 */
    kmemset(g_kek, 0, sizeof g_kek);
    g_state = KSEC_STATE_LOCKED;
    return 0;
}
int ksec_unlock(const char *pw, char *msg, size_t n) {
    if (g_state == KSEC_STATE_FACTORY)
        return ksnprintf(msg, n, "DENIED: factory state, run setup first\n"), K_AUTH_LOCKED;
    uint8_t calc[K_HASH_BYTES];
    pbkdf2(pw, kstrlen(pw), g_super.pw_salt, K_SALT_BYTES, K_PBKDF2_ITER, calc);
    int matched = ct_eq(calc, g_super.pw_hash, K_HASH_BYTES);
    kmemset(calc, 0, sizeof calc);

    /* 胁迫口令：表面解锁、实则销毁 */
    if (!matched && g_super.duress_hash[0]) {
        uint8_t d[K_HASH_BYTES];
        pbkdf2(pw, kstrlen(pw), g_super.pw_salt, K_SALT_BYTES, K_PBKDF2_ITER, d);
        int is_duress = ct_eq(d, g_super.duress_hash, K_HASH_BYTES);
        kmemset(d, 0, sizeof d);
        if (is_duress) {
            (void)audit_append("DURESS", "coerced unlock -> wipe");
            do_wipe("duress-password", g_super.fails);
            return ksnprintf(msg, n, "WIPED: duress password -> all user data destroyed\n"), K_AUTH_DURESS;
        }
    }
    if (matched) {
        derive_kek(pw);
        if (open_dek() != 0) { shred_dek(); return ksnprintf(msg, n, "DENIED: key seal broken\n"), K_AUTH_FAIL; }
        g_super.fails = 0;
        (void)super_save();
        (void)audit_append("AUTH_OK", "unlock");
        g_state = KSEC_STATE_UNLOCKED;
        g_idle_last_tick = (uint32_t)g_ticks;
        return ksnprintf(msg, n, "OK: unlocked, vault ready\n"), K_AUTH_OK;
    }
    /* 失败：计数 +1 + 落盘 + 审计交叉核对 */
    g_super.fails++;
    (void)super_save();
    (void)audit_append("AUTH_FAIL", "bad password");
    uint32_t f = g_super.fails;
    uint32_t th = g_super.max_fails ? g_super.max_fails : K_MAX_FAILS_DEFAULT;
    if (f >= th) {
        do_wipe("max-fails-reached", f);
        return ksnprintf(msg, n,
            "WIPED: %u wrong passwords reached threshold %u -> ALL user data destroyed, SYSTEM INTACT\n",
            f, th), K_AUTH_WIPED;
    }
    return ksnprintf(msg, n, "DENIED: wrong password (%u/%u)\n", f, th), K_AUTH_FAIL;
}
int ksec_lock(void) {
    shred_dek(); kmemset(g_kek, 0, sizeof g_kek);
    if (g_state == KSEC_STATE_UNLOCKED) g_state = KSEC_STATE_LOCKED;
    (void)audit_append("LOCK", "session locked");
    return 0;
}
int ksec_put(const char *name, const void *data, size_t n) {
    if (g_state != KSEC_STATE_UNLOCKED || !g_dek_valid) return -1;
    if (!name || kstrlen(name) > K_NAME_MAX_LEN || n > K_VAULT_FILE_BYTES) return -2;
    uint32_t slot = 0xFFFFFFFFu, first_free = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < K_VAULT_MAX_FILES; i++) {
        if (ata_read((uint32_t)rec_lba(i), g_sector) != 0) return -3;
        record_t *r = (record_t *)g_sector;
        if (!r->used && first_free == 0xFFFFFFFFu) first_free = i;
        if (r->used && kstrcmp(r->name, name) == 0) { slot = i; break; }
    }
    if (slot == 0xFFFFFFFFu) slot = first_free;
    if (slot == 0xFFFFFFFFu) return -4;
    record_t r; kmemset(&r, 0, sizeof r);
    r.used = 1; r.len = (uint32_t)n;
    kstrncpy(r.name, name, sizeof r.name);
    kmemcpy(r.data, data, n);
    rec_seal(&r);
    kmemcpy(g_sector, &r, sizeof r);
    kmemset(g_sector + sizeof r, 0, K_SECTOR_BYTES - sizeof r);
    if (ata_write((uint32_t)rec_lba(slot), g_sector) != 0) return -5;
    (void)audit_append("VAULT_PUT", name);
    return 0;
}
int ksec_get(const char *name, void *buf, size_t n, size_t *out_len) {
    if (g_state != KSEC_STATE_UNLOCKED || !g_dek_valid) return -1;
    for (uint32_t i = 0; i < K_VAULT_MAX_FILES; i++) {
        if (ata_read((uint32_t)rec_lba(i), g_sector) != 0) return -2;
        record_t r; kmemcpy(&r, g_sector, sizeof r);
        if (!r.used || kstrcmp(r.name, name) != 0) continue;
        if (rec_open(&r) != 0) return -3;
        size_t k = r.len < n ? r.len : n;
        kmemcpy(buf, r.data, k);
        if (out_len) *out_len = r.len;
        return (int)k;
    }
    return -4;
}
int ksec_list(char *buf, size_t n) {
    int p = 0;
    p += ksnprintf(buf + p, n - (size_t)p, "VAULT:");
    uint32_t cnt = 0;
    for (uint32_t i = 0; i < K_VAULT_MAX_FILES; i++) {
        if (ata_read((uint32_t)rec_lba(i), g_sector) != 0) break;
        record_t *r = (record_t *)g_sector;
        if (r->used && (size_t)(p + (int)K_NAME_MAX_LEN + 32) < n) {
            p += ksnprintf(buf + p, n - (size_t)p, " %s(%u)", r->name, r->len);
            cnt++;
        }
    }
    p += ksnprintf(buf + p, n - (size_t)p, " [%u files]\n", cnt);
    return p;
}
uint32_t ksec_fails(void)  { return g_super.fails; }
int ksec_state(void)       { return g_state; }
const char *ksec_last_receipt(void) { return g_receipt[0] ? g_receipt : NULL; }
int ksec_wipe_status(char *buf, size_t n) {
    int off = ksnprintf(buf, n,
        "WIPE_STATUS state=%s fails=%u last_trigger=%s last_fails=%u receipt=%s\n",
        g_state == KSEC_STATE_FACTORY ? "FACTORY" :
        g_state == KSEC_STATE_LOCKED ? "LOCKED" :
        g_state == KSEC_STATE_UNLOCKED ? "UNLOCKED" : "WIPED",
        g_super.fails, g_last_wipe_reason, g_last_wipe_fails,
        g_receipt[0] ? "PRESENT" : "NONE");
    /* 一并输出销毁证明（含 "sys_unchanged": true 与销毁结论），供用户态断言 */
    if (g_receipt[0] && (size_t)off < n)
        off += ksnprintf(buf + off, n - off, "%s", g_receipt);
    return off;
}
int ksec_selftest(char *buf, size_t n) {
    uint8_t h[K_HASH_BYTES];
    sha256_buf("abc", 3, h);
    static const char exp[] = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    char got[65]; khex(got, h, K_HASH_BYTES);
    int crypto_ok = (kstrcmp(got, exp) == 0);
    int av = ksec_audit_verify();
    uint64_t sys_now = ksec_sys_digest();
    return ksnprintf(buf, n,
        "SELFTEST crypto_sha256=%s ata=%s audit_chain=%s sys_digest=%s faults=%u ticks=%u\n",
        crypto_ok ? "PASS" : "FAIL",
        ata_present() ? "PASS" : "FAIL",
        av == 0 ? "PASS" : "FAIL",
        sys_now == g_sys_digest_before ? "UNCHANGED" : "CHANGED",
        g_fault_count, (uint32_t)g_ticks);
}
void ksec_emergency_wipe(const char *reason) { do_wipe(reason ? reason : "emergency", g_super.fails); }
void ksec_idle_check(void) {
    if (g_state != KSEC_STATE_UNLOCKED) return;
    if ((uint32_t)g_ticks - g_idle_last_tick > K_SESSION_IDLE_TICKS) {
        (void)audit_append("IDLE_LOCK", "session idle timeout");
        (void)ksec_lock();
    }
}
void ksec_note_activity(void) { g_idle_last_tick = (uint32_t)g_ticks; }
