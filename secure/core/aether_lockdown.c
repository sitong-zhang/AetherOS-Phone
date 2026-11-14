/*
 * aether_lockdown.c - 安全锁定 / 熔断模式实现
 *
 * 三档语义严格分开：
 *   常规档(ae_secure_wipe)  : 抹 data/ 用户数据，sys/ 系统本体逐字节保留。
 *   胁迫档(duress)          : 诱饵环境（既有 secure 能力）。
 *   熔断档(ae_ld_brick)     : 抹除包括 sys/ 在内的所有区 + 报警 + 软件熔断标记
 *                             + 烧毁硬件指令(硬件熔断)。
 *
 * 熔断触发判据：
 *   **事件只作证据，行为/攻击意图才触发**——不是「插 USB / 开盖 / 碰测试点就删」。
 *   L1 无害   → 记录取证、零删除、保持锁定；
 *   L2 可疑   → 断连+记录取证、进 SUSPECT、零删除；
 *   L3 攻击意图(USB破解强插/短接触及核心区/核心区导出/密钥区触碰) → 立即熔断；
 *   维修白名单 → 豁免物理开盖/短接，不豁免 L3 核心区导出（防伪造凭证）。
 *
 * 软件熔断 vs 硬件熔断（报告 §4.3 建议 2，分开命名/验收）：
 *   软件熔断 = 删全部分区 + BRICKED 标记 + 引导级拦截 → 系统不可恢复，刷机可重装；
 *   硬件熔断 = ae_hal_hw_fuse_burn → 真机 eFuse/OTP/crowbar 物理永久变砖，
 *              云端仅仿真指令锁存（ae_hal_hw_fuse_backend 如实返回 simulated）。
 *
 * 「零存储」精确语义：密码本身永不落盘。落盘于 lockdown/ 区的只有一个
 *   verifier = HMAC(device_key, PBKDF2(pw, salt)) —— 由密码单向派生、无法
 *   倒推回密码，且绑定设备密钥。密码明文只在 begin() 的调用栈内瞬时用于
 *   强度校验 / 弹窗回显 / 派生，调用返回后不驻留内存、不写任何文件。
 */
#define _GNU_SOURCE
#include "aether_lockdown.h"
#include "aether_tee.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>

#define ANCHOR_MAGIC   "AETHLD01"        /* 8B */
#define MODE_FILE      "lockdown.mode"   /* LOCKED / SUSPECT 状态持久标记 */
#define ANCHOR_HDR     (8u + AE_SALT_LEN)              /* magic + salt */
#define ANCHOR_BODY    (ANCHOR_HDR + AE_SHA256_LEN)    /* + verifier */
#define ANCHOR_LEN     (ANCHOR_BODY + AE_SHA256_LEN)   /* + mac */

struct ae_ld {
    ae_store_t    *store;        /* 借用，不负责 close */
    ae_hal_t      *hal;          /* 借用，不负责 close */
    ae_ld_state_t  state;
    /* 组合键手势 */
    int      power_down;
    uint64_t gesture_start_ms;
    /* 首启两步设定 */
    int      provisioning;
    uint64_t cooldown_begin_ms;
    uint8_t  pending_anchor[ANCHOR_LEN];  /* 只含派生锚，绝不含密码明文 */
    int      pending_valid;
};

/* ---------------------------------------------------------------- 小工具 */
static const char *st_name(ae_ld_state_t s)
{
    switch (s) {
        case AE_LD_UNSET:   return "UNSET(未设专用密码·硬门拦截)";
        case AE_LD_NORMAL:  return "NORMAL(正常运行)";
        case AE_LD_GESTURE: return "GESTURE(组合键计时中)";
        case AE_LD_ARMED:   return "ARMED(满10秒·待音量上确认)";
        case AE_LD_LOCKED:  return "LOCKED(安全锁定模式)";
        case AE_LD_SUSPECT: return "SUSPECT(可疑·仅取证记录·零删除)";
        case AE_LD_BRICKED: return "BRICKED(软件熔断·系统不可恢复·刷机可重装)";
        default:            return "?";
    }
}

static int is_upper(char c){ return c>='A'&&c<='Z'; }
static int is_lower(char c){ return c>='a'&&c<='z'; }
static int is_digit(char c){ return c>='0'&&c<='9'; }
static int is_special(char c){ return c>=33 && c<=126 && !is_upper(c) && !is_lower(c) && !is_digit(c); }

/* 递增/递减连续序列（12345 / abcde / 98765 …）长度 > limit 即判弱。 */
static int has_long_run(const char *pw, int limit)
{
    size_t i; int run = 1;
    for (i = 1; pw[i]; i++) {
        int d = (int)(unsigned char)pw[i] - (int)(unsigned char)pw[i-1];
        if (d == 1 || d == -1) run++; else run = 1;
        if (run > limit) return 1;
    }
    return 0;
}

static int device_key(ae_store_t *s, uint8_t k[AE_KEY_LEN])
{
    return ae_tee_device_key(s, AE_TEE_SOFTWARE, k);  /* 当前软件锚点 */
}

static int make_verifier(ae_store_t *s, const char *pw,
                         const uint8_t salt[AE_SALT_LEN], uint8_t out[AE_SHA256_LEN])
{
    uint8_t kdf[AE_KEY_LEN], dk[AE_KEY_LEN];
    if (ae_pbkdf2(pw, strlen(pw), salt, AE_SALT_LEN, AE_PBKDF2_ITER, kdf) != 0) return -1;
    if (device_key(s, dk) != 0) { ae_zero(kdf, sizeof kdf); return -1; }
    ae_hmac_sha256(dk, AE_KEY_LEN, kdf, AE_KEY_LEN, out);
    ae_zero(kdf, sizeof kdf); ae_zero(dk, sizeof dk);
    return 0;
}

/* anchor = magic(8) | salt(16) | verifier(32) | mac(32) */
static void anchor_mac(ae_store_t *s, uint8_t a[ANCHOR_LEN])
{
    uint8_t dk[AE_KEY_LEN];
    if (device_key(s, dk) != 0) { memset(dk, 0, sizeof dk); }
    uint8_t m[AE_SHA256_LEN];
    ae_hmac_sha256(dk, AE_KEY_LEN, a, ANCHOR_BODY, m);
    memcpy(a + ANCHOR_BODY, m, AE_SHA256_LEN);
    ae_zero(dk, sizeof dk); ae_zero(m, sizeof m);
}
static int anchor_ok(ae_store_t *s, const uint8_t a[ANCHOR_LEN])
{
    if (memcmp(a, ANCHOR_MAGIC, 8) != 0) return 0;
    uint8_t dk[AE_KEY_LEN], calc[AE_SHA256_LEN];
    if (device_key(s, dk) != 0) return 0;
    ae_hmac_sha256(dk, AE_KEY_LEN, a, ANCHOR_BODY, calc);
    int ok = ae_ct_eq(calc, a + ANCHOR_BODY, AE_SHA256_LEN);
    ae_zero(dk, sizeof dk); ae_zero(calc, sizeof calc);
    return ok;
}

/* ------------------------------------------------ 证据链（只追加，外部可观测） */
static void evidence_append(ae_ld_t *c, ae_hal_event_t ev, ae_threat_t th)
{
    if (!c) return;
    uint8_t *b = NULL; size_t n = 0;
    char buf[8192]; size_t used = 0;
    if (ae_store_read(c->store, AE_ZONE_LOCKDOWN, AE_LD_EVID_FILE, &b, &n) == 0) {
        if (n < sizeof buf - 256) { memcpy(buf, b, n); used = n; }
        free(b);
    }
    int w = snprintf(buf + used, sizeof buf - used, "%s|%s|%s\n",
                     ae_hal_event_name(ev), ae_hal_threat_name(th),
                     ae_hal_backend_name(c->hal));
    if (w > 0) used += (size_t)w;
    (void)ae_store_atomic_write(c->store, AE_ZONE_LOCKDOWN, AE_LD_EVID_FILE, buf, used);
    ae_hal_record_evidence(c->hal, ev);   /* 进程内即时计数 */
}
static uint32_t evidence_file_lines(ae_ld_t *c)
{
    if (!c) return 0;
    uint8_t *b = NULL; size_t n = 0; uint32_t lines = 0;
    if (ae_store_read(c->store, AE_ZONE_LOCKDOWN, AE_LD_EVID_FILE, &b, &n) == 0) {
        for (size_t i = 0; i < n; i++) if (b[i] == '\n') lines++;
        free(b);
    }
    return lines;
}
static const char *evidence_file_last(ae_ld_t *c)
{
    /* 返回 HAL 进程内的最后事件名；跨进程取证请直接读 evidence.log 末行 */
    return c ? ae_hal_evidence_last(c->hal) : "";
}

/* ------------------------------------------------------------ 生命周期/状态 */
int ae_ld_open(ae_store_t *s, ae_hal_t *hal, ae_ld_t **out)
{
    if (!s || !hal || !out) return -1;
    *out = NULL;
    ae_ld_t *c = (ae_ld_t*)calloc(1, sizeof *c);
    if (!c) return -1;
    c->store = s; c->hal = hal;

    if (ae_store_exists(s, AE_ZONE_LOCKDOWN, AE_LD_BRICK_FILE)) {
        c->state = AE_LD_BRICKED;
    } else {
        uint8_t *b = NULL; size_t n = 0;
        int locked = 0, suspect = 0;
        if (ae_store_read(s, AE_ZONE_LOCKDOWN, MODE_FILE, &b, &n) == 0) {
            locked  = (n >= 6 && memcmp(b, AE_LD_MODE_LOCKED, 6) == 0);
            suspect = (n >= 7 && memcmp(b, AE_LD_MODE_SUSPECT, 7) == 0);
            free(b);
        }
        if (locked)       c->state = AE_LD_LOCKED;
        else if (suspect) c->state = AE_LD_SUSPECT;
        else if (ae_store_exists(s, AE_ZONE_LOCKDOWN, AE_LD_ANCHOR_FILE)) c->state = AE_LD_NORMAL;
        else c->state = AE_LD_UNSET;
    }
    *out = c;
    return 0;
}
void ae_ld_close(ae_ld_t *c)
{
    if (!c) return;
    ae_zero(c->pending_anchor, sizeof c->pending_anchor);
    memset(c, 0, sizeof *c);
    free(c);
}
ae_ld_state_t ae_ld_state(ae_ld_t *c) { return c ? c->state : AE_LD_UNSET; }
const char   *ae_ld_state_name(ae_ld_t *c) { return st_name(ae_ld_state(c)); }

int ae_ld_is_provisioned(ae_ld_t *c)
{
    if (!c) return 0;
    return ae_store_exists(c->store, AE_ZONE_LOCKDOWN, AE_LD_ANCHOR_FILE);
}
int ae_ld_oobe_gate(ae_ld_t *c)
{
    /* 未设定 或 已软件熔断：不放行进入系统 */
    if (!c || c->state == AE_LD_BRICKED || c->state == AE_LD_UNSET) return 0;
    return ae_ld_is_provisioned(c) ? 1 : 0;
}

/* -------------------------------------------------------------- 强度策略 */
int ae_ld_check_strength(const char *pw, char *why, size_t n)
{
    if (why && n) why[0] = 0;
    if (!pw) { if (why&&n) snprintf(why,n,"密码为空"); return 6; }
    size_t len = strlen(pw);
    if (len < AE_LD_PW_MIN || len > AE_LD_PW_MAX) {
        if (why&&n) snprintf(why,n,"长度 %zu 不在 %u–%u 之间", len, AE_LD_PW_MIN, AE_LD_PW_MAX);
        return 1;
    }
    int up=0, lo=0, sp=0;
    for (size_t i=0; i<len; i++) {
        if (is_upper(pw[i])) up=1;
        else if (is_lower(pw[i])) lo=1;
        else if (is_special(pw[i])) sp=1;
    }
    if (!up) { if (why&&n) snprintf(why,n,"缺少大写字母"); return 2; }
    if (!lo) { if (why&&n) snprintf(why,n,"缺少小写字母"); return 3; }
    if (!sp) { if (why&&n) snprintf(why,n,"缺少特殊符号"); return 4; }
    if (has_long_run(pw, (int)AE_LD_RUN_LIMIT)) {
        if (why&&n) snprintf(why,n,"存在超过 %u 位的连续序列(如 123456)", AE_LD_RUN_LIMIT);
        return 5;
    }
    return 0;
}

/* ------------------------------------------------- 首启硬门：两步设定 */
int ae_ld_provision_begin(ae_ld_t *c, const char *pw,
                          char *dialog, size_t dialog_n,
                          char *why, size_t why_n)
{
    if (why && why_n) why[0] = 0;
    if (!c) return -2;
    if (c->state == AE_LD_BRICKED) {
        if (why&&why_n) snprintf(why,why_n,"设备已软件熔断，无法设定");
        return -2;
    }
    if (ae_ld_is_provisioned(c)) {
        if (why&&why_n) snprintf(why,why_n,"专用密码仅首次可设、不可重置/找回");
        return -2;   /* 不可重置 */
    }
    int rc = ae_ld_check_strength(pw, why, why_n);
    if (rc != 0) return -1;

    memset(c->pending_anchor, 0, sizeof c->pending_anchor);
    memcpy(c->pending_anchor, ANCHOR_MAGIC, 8);
    uint8_t salt[AE_SALT_LEN];
    if (ae_random(salt, sizeof salt) != 0) return -1;
    memcpy(c->pending_anchor + 8, salt, AE_SALT_LEN);
    uint8_t verifier[AE_SHA256_LEN];
    if (make_verifier(c->store, pw, salt, verifier) != 0) {
        ae_zero(salt, sizeof salt); return -1;
    }
    memcpy(c->pending_anchor + ANCHOR_HDR, verifier, AE_SHA256_LEN);
    anchor_mac(c->store, c->pending_anchor);
    ae_zero(salt, sizeof salt); ae_zero(verifier, sizeof verifier);
    c->pending_valid = 1;
    c->provisioning = 1;
    c->cooldown_begin_ms = ae_hal_now_ms(c->hal);

    if (dialog && dialog_n) {
        snprintf(dialog, dialog_n,
            "==== 安全锁定密码 · 二次确认 ====\n"
            "此密码不可重置、不可找回，仅首次设定有效。\n"
            "进入安全锁定模式后，除非输入此密码，任何方式都无法解锁；\n"
            "一旦忘记，将无法退出安全锁定模式（无任何恢复途径）。\n"
            "请务必用【物理方式】抄录并离线保存：不要截图、不要存电子版、不要联网传输。\n"
            "本次设定的密码（再显示一遍，仅此一次）：\n"
            "    %s\n"
            "「确定」按钮需等待 %u 秒冷却后方可点击。\n",
            pw, AE_LD_COOLDOWN_MS / 1000u);
    }
    return 0;
}

int ae_ld_provision_confirm(ae_ld_t *c, uint64_t now_ms,
                            char *why, size_t why_n)
{
    if (why && why_n) why[0] = 0;
    if (!c) return -2;
    if (!c->provisioning || !c->pending_valid) {
        if (why&&why_n) snprintf(why,why_n,"无待确认的设定会话");
        return -2;
    }
    if (now_ms < c->cooldown_begin_ms ||
        now_ms - c->cooldown_begin_ms < AE_LD_COOLDOWN_MS) {
        if (why&&why_n) snprintf(why,why_n,"确定按钮冷却未满 %u 秒，请稍候",
                                 AE_LD_COOLDOWN_MS/1000u);
        return -1;   /* 冷却未满 */
    }
    if (ae_store_atomic_write(c->store, AE_ZONE_LOCKDOWN, AE_LD_ANCHOR_FILE,
                              c->pending_anchor, ANCHOR_LEN) != 0) {
        if (why&&why_n) snprintf(why,why_n,"验证锚落盘失败");
        return -2;
    }
    ae_zero(c->pending_anchor, sizeof c->pending_anchor);
    c->pending_valid = 0;
    c->provisioning = 0;
    c->state = AE_LD_NORMAL;
    return 0;
}

/* ---------------------------------------------------------- 验证与解锁 */
static int verify_pw(ae_ld_t *c, const char *pw)
{
    uint8_t *b = NULL; size_t n = 0;
    if (ae_store_read(c->store, AE_ZONE_LOCKDOWN, AE_LD_ANCHOR_FILE, &b, &n) != 0) return 0;
    if (n != ANCHOR_LEN || !anchor_ok(c->store, b)) { free(b); return 0; }
    uint8_t salt[AE_SALT_LEN]; memcpy(salt, b + 8, AE_SALT_LEN);
    uint8_t calc[AE_SHA256_LEN];
    int ok = 0;
    if (make_verifier(c->store, pw, salt, calc) == 0)
        ok = ae_ct_eq(calc, b + ANCHOR_HDR, AE_SHA256_LEN);
    free(b);
    ae_zero(salt, sizeof salt); ae_zero(calc, sizeof calc);
    return ok;
}

int ae_ld_unlock(ae_ld_t *c, const char *pw, char *msg, size_t n)
{
    if (msg && n) msg[0] = 0;
    if (!c) return -3;
    if (c->state == AE_LD_BRICKED) {
        if (msg&&n) snprintf(msg,n,"设备已软件熔断：任何方式都无法退出/解锁（刷机可重装）");
        return -3;
    }
    if (c->state != AE_LD_LOCKED && c->state != AE_LD_SUSPECT) {
        if (msg&&n) snprintf(msg,n,"当前不在安全锁定模式");
        return -2;
    }
    if (!pw || !verify_pw(c, pw)) {
        if (msg&&n) snprintf(msg,n,"安全密码错误：仍锁定（无找回/无重置）");
        return -1;
    }
    (void)ae_store_remove(c->store, AE_ZONE_LOCKDOWN, MODE_FILE);
    ae_hal_set_service_mode(c->hal, 0);
    c->state = AE_LD_NORMAL;
    if (msg&&n) snprintf(msg,n,"安全密码正确：已退出安全锁定模式");
    return 0;
}

/* --------------------------------------------------- 熔断（软件 + 硬件） */
int ae_ld_brick(ae_ld_t *c, const char *trigger, char *msg, size_t n)
{
    if (msg && n) msg[0] = 0;
    if (!c) return -1;
    static const char *zones[] = {AE_ZONE_SYS, AE_ZONE_DATA, AE_ZONE_JOURNAL,
                                  AE_ZONE_RECEIPT, AE_ZONE_LOCKDOWN};
    size_t z; int removed = 0;
    for (z = 0; z < 5; z++) {
        char (*names)[AE_STORE_NAME_MAX] = (char(*)[AE_STORE_NAME_MAX])calloc(256, AE_STORE_NAME_MAX);
        int count = 0, i;
        if (names && ae_store_list(c->store, zones[z], names, 256, &count) == 0) {
            for (i = 0; i < count; i++) {
                uint8_t *fb = NULL; size_t fl = 0;
                if (ae_store_read(c->store, zones[z], names[i], &fb, &fl) == 0 && fl) {
                    uint8_t *ov = (uint8_t*)malloc(fl);
                    if (ov) {
                        if (ae_random(ov, fl) == 0)
                            (void)ae_store_atomic_write(c->store, zones[z], names[i], ov, fl);
                        memset(ov, 0, fl);
                        (void)ae_store_atomic_write(c->store, zones[z], names[i], ov, fl);
                        ae_zero(ov, fl); free(ov);
                    }
                    free(fb);
                }
                if (ae_store_remove(c->store, zones[z], names[i]) == 0) removed++;
            }
        }
        free(names);
    }
    /* 软件熔断墓碑：系统本体不可恢复（但刷机可重装，非硬件永久变砖） */
    char tomb[256];
    snprintf(tomb, sizeof tomb, "AETHER_SOFTWARE_BRICK trigger=%s\n", trigger ? trigger : "unspecified");
    (void)ae_store_atomic_write(c->store, AE_ZONE_LOCKDOWN, AE_LD_BRICK_FILE,
                                tomb, strlen(tomb));
    /* 烧毁硬件（硬件熔断）：真机 eFuse/OTP/crowbar；云端仅仿真指令锁存 */
    (void)ae_hal_hw_fuse_burn(c->hal, trigger ? trigger : "L3-attack");
    /* 触发原因放最后一条报警，便于外部可观测断言 alarm_last */
    (void)ae_hal_alarm(c->hal, trigger ? trigger : "L3-attack");
    c->state = AE_LD_BRICKED;
    if (msg && n) snprintf(msg, n,
        "熔断：软件熔断抹除含系统本体的 %d 个文件 + 报警 + 烧毁硬件指令(%s)；系统不可恢复(刷机可重装)",
        removed, ae_hal_hw_fuse_backend(c->hal));
    return 0;
}

/* -------------------------------------------------- 事件驱动状态机（分级判据） */
int ae_ld_on_event(ae_ld_t *c, ae_hal_event_t ev, uint64_t now_ms,
                   char *msg, size_t n)
{
    if (msg && n) msg[0] = 0;
    if (!c) return 0;

    /* 软件熔断终态：任何事件、任何方式都无效 */
    if (c->state == AE_LD_BRICKED) {
        if (msg&&n) snprintf(msg,n,"设备已软件熔断：任何事件均无效");
        return 0;
    }

    /* ============ LOCKED / SUSPECT：分级判据，事件只作证据 ============ */
    if (c->state == AE_LD_LOCKED || c->state == AE_LD_SUSPECT) {
        ae_threat_t th = ae_hal_event_threat(ev);

        /* 官方维修白名单开启（签名凭证）：豁免物理开盖/短接，不豁免 L3 */
        if (th == AE_THREAT_SERVICE) {
            ae_hal_set_service_mode(c->hal, 1);
            evidence_append(c, ev, th);
            if (msg&&n) snprintf(msg,n,
                "已启用官方维修白名单(%s)：豁免物理开盖/短接类信号，但不豁免 L3 核心区导出",
                ae_hal_event_name(ev));
            return 0;
        }

        int whitelisted = ae_hal_service_mode(c->hal);

        /* L1 无害：记录取证 + 轻报警，零删除，保持锁定 */
        if (th == AE_THREAT_L1) {
            evidence_append(c, ev, th);
            (void)ae_hal_alarm(c->hal, "L1-benign(record-only,zero-delete)");
            if (msg&&n) snprintf(msg,n,
                "L1 无害信号(%s)：仅记录取证、零删除，保持 %s%s",
                ae_hal_event_name(ev), ae_ld_state_name(c),
                whitelisted ? "（维修白名单）" : "");
            return 0;
        }

        /* L2 可疑：维修白名单下只记录；否则断连+记录取证 → SUSPECT，零删除。
         * 证据链只作取证与审计，不设自动升级阈值，不参与熔断触发判定。 */
        if (th == AE_THREAT_L2) {
            evidence_append(c, ev, th);
            if (whitelisted) {
                if (msg&&n) snprintf(msg,n,
                    "维修白名单：可疑行为(%s)仅记录取证、不升级、零删除",
                    ae_hal_event_name(ev));
                return 0;
            }
            (void)ae_hal_alarm(c->hal, "L2-suspect(disconnect+forensics)");
            c->state = AE_LD_SUSPECT;
            (void)ae_store_atomic_write(c->store, AE_ZONE_LOCKDOWN, MODE_FILE,
                                        AE_LD_MODE_SUSPECT, 7);
            if (msg&&n) snprintf(msg,n,
                "L2 可疑行为(%s)：断连+记录取证(累计 %u 条)、进入 SUSPECT、零删除",
                ae_hal_event_name(ev), ae_ld_evidence_count(c));
            return 0;
        }

        /* L3 确认攻击意图：立即摧毁系统 + 烧毁硬件（白名单也不豁免核心区导出） */
        if (th == AE_THREAT_L3) {
            evidence_append(c, ev, th);
            const char *tr = ae_hal_event_name(ev);
            char bm[320];
            (void)ae_ld_brick(c, tr, bm, sizeof bm);
            if (msg&&n) snprintf(msg,n,
                "L3 确认攻击意图(%s) → 摧毁系统(软件熔断) + 烧毁硬件 → %s", tr, bm);
            return 1;   /* 已熔断 */
        }

        /* 中性输入（组合键等）：安全锁定中忽略，仅专用密码可解 */
        if (msg&&n) snprintf(msg,n,"安全锁定中：仅专用密码可解锁，其余路径全部封死");
        return 0;
    }

    /* 未设密码：组合键完全无效（硬门） */
    if (c->state == AE_LD_UNSET) {
        if (msg&&n && (ev==AE_EV_KEY_POWER||ev==AE_EV_KEY_VOL_DOWN||ev==AE_EV_KEY_VOL_UP))
            snprintf(msg,n,"未设定专用密码：组合键无效");
        return 0;
    }

    /* GESTURE：先按时间推进是否满 10 秒 */
    if (c->state == AE_LD_GESTURE) {
        uint64_t el = (now_ms >= c->gesture_start_ms) ? now_ms - c->gesture_start_ms : 0;
        if (ev == AE_EV_KEY_RELEASE) {
            c->state = AE_LD_NORMAL; c->power_down = 0;
            if (msg&&n) snprintf(msg,n,"组合键松开且未满 10 秒：取消，未进入");
            return 0;
        }
        if (ev == AE_EV_KEY_VOL_UP && el < AE_LD_GESTURE_MS) {
            c->state = AE_LD_NORMAL; c->power_down = 0;
            if (msg&&n) snprintf(msg,n,"未满 10 秒：不能进入安全锁定模式");
            return 0;
        }
        if (el >= AE_LD_GESTURE_MS) {
            c->state = AE_LD_ARMED;
            if (msg&&n) snprintf(msg,n,"同按已满 10 秒 → 待「音量上」确认（保持，不自动进入）");
        } else {
            if (msg&&n) snprintf(msg,n,"组合键计时中（未满 10 秒）");
            return 0;
        }
    }

    if (c->state == AE_LD_ARMED) {
        if (ev == AE_EV_KEY_VOL_UP) {
            c->state = AE_LD_LOCKED; c->power_down = 0;
            (void)ae_store_atomic_write(c->store, AE_ZONE_LOCKDOWN, MODE_FILE,
                                        AE_LD_MODE_LOCKED, 6);
            if (msg&&n) snprintf(msg,n,"已进入【安全锁定模式】：仅专用密码可解锁，其余任何方式封死");
            return 0;
        }
        if (msg&&n) snprintf(msg,n,"已满 10 秒，持续按住/超时均保持待确认，按「音量上」进入");
        return 0;
    }

    /* NORMAL：组合键开始 */
    if (c->state == AE_LD_NORMAL) {
        if (ev == AE_EV_KEY_POWER) {
            c->power_down = 1;
            if (msg&&n) snprintf(msg,n,"电源键按下，等待「音量下」组成同按");
            return 0;
        }
        if (ev == AE_EV_KEY_VOL_DOWN && c->power_down) {
            c->state = AE_LD_GESTURE;
            c->gesture_start_ms = now_ms;
            if (msg&&n) snprintf(msg,n,"电源+音量下 同按开始计时（需满 10 秒）");
            return 0;
        }
        if (msg&&n) snprintf(msg,n,"正常态：忽略该事件");
        return 0;
    }
    return 0;
}

/* --------------------------------------------- 外部可观测：零存储取证 / 诊断 */
static int file_contains(const char *path, const char *pw, size_t plen)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t buf[4096]; size_t r, carry = 0;
    uint8_t tail[64]; int found = 0;
    while ((r = fread(buf, 1, sizeof buf, f)) > 0) {
        uint8_t *scan = buf; size_t slen = r;
        uint8_t joined[4096 + 64];
        if (carry) { memcpy(joined, tail, carry); memcpy(joined + carry, buf, r); scan = joined; slen = r + carry; }
        if (slen >= plen && memmem(scan, slen, pw, plen)) { found = 1; break; }
        carry = (plen > 1 && slen >= plen - 1) ? plen - 1 : 0;
        if (carry) memcpy(tail, scan + slen - carry, carry);
    }
    fclose(f);
    return found;
}
static int dir_contains(const char *dir, const char *pw, size_t plen)
{
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *de; int found = 0;
    while ((de = readdir(d)) && !found) {
        if (!strcmp(de->d_name,".")||!strcmp(de->d_name,"..")) continue;
        char p[PATH_MAX];
        if (snprintf(p, sizeof p, "%s/%s", dir, de->d_name) >= (int)sizeof p) continue;
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) found = dir_contains(p, pw, plen);
        else if (S_ISREG(st.st_mode)) found = file_contains(p, pw, plen);
    }
    closedir(d);
    return found;
}
int ae_ld_anchor_exists(ae_ld_t *c)
{
    return c ? ae_store_exists(c->store, AE_ZONE_LOCKDOWN, AE_LD_ANCHOR_FILE) : 0;
}
int ae_ld_scan_plaintext(ae_ld_t *c, const char *pw)
{
    if (!c || !pw || !*pw) return 0;
    return dir_contains(ae_store_root(c->store), pw, strlen(pw));
}
uint32_t ae_ld_evidence_count(ae_ld_t *c) { return evidence_file_lines(c); }
const char *ae_ld_evidence_last(ae_ld_t *c) { return evidence_file_last(c); }
int  ae_ld_hw_fuse_burned(ae_ld_t *c) { return c ? ae_hal_hw_fuse_burned(c->hal) : 0; }
const char *ae_ld_hw_fuse_backend(ae_ld_t *c) { return c ? ae_hal_hw_fuse_backend(c->hal) : "none"; }
int  ae_ld_service_mode(ae_ld_t *c) { return c ? ae_hal_service_mode(c->hal) : 0; }
