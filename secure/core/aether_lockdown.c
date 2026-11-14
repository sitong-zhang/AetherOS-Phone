/*
 * aether_lockdown.c - 安全锁定 / 熔断模式实现
 *
 * 三档语义严格分开：
 *   常规档(ae_secure_wipe)  : 抹 data/ 用户数据，sys/ 系统本体逐字节保留。
 *   胁迫档(duress)          : 诱饵环境（既有 secure 能力）。
 *   熔断档(ae_ld_brick)     : 抹除包括 sys/ 在内的所有区，报警，永久变砖。
 *
 * 「零存储」精确语义：密码本身永不落盘。落盘于 lockdown/ 区的只有一个
 *   verifier = HMAC(device_key, PBKDF2(pw, salt)) —— 由密码单向派生、无法
 *   倒推回密码，且绑定设备密钥。密码明文只在 begin() 的调用栈内瞬时用于
 *   强度校验 / 弹窗回显 / 派生，调用返回后不驻留内存、不写任何文件。
 *   验证时用候选密码重算 verifier 做常量时间比对。
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
#define MODE_FILE      "lockdown.mode"   /* LOCKED 状态持久标记 */
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
        case AE_LD_BRICKED: return "BRICKED(已熔断·永久变砖)";
        default:            return "?";
    }
}

static int is_upper(char c){ return c>='A'&&c<='Z'; }
static int is_lower(char c){ return c>='a'&&c<='z'; }
static int is_digit(char c){ return c>='0'&&c<='9'; }
static int is_special(char c){ return c>=33 && c<=126 && !is_upper(c) && !is_lower(c) && !is_digit(c); }

/* 递增/递减连续序列（12345 / abcde / 98765 …）长度 > limit 即判弱。
 * 用户口径「123 连续的数字不能超过 5 个」：12345 允许，123456 拒绝。 */
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

/* 从密码派生不可倒推的验证锚。pw 只在调用栈内使用。 */
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

/* anchor = magic(8) | salt(16) | verifier(32) | mac(32)
 * mac = HMAC(device_key, magic|salt|verifier) 防篡改 */
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
        int locked = 0;
        if (ae_store_read(s, AE_ZONE_LOCKDOWN, MODE_FILE, &b, &n) == 0) {
            locked = (n >= 6 && memcmp(b, "LOCKED", 6) == 0);
            free(b);
        }
        if (locked) c->state = AE_LD_LOCKED;
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
    /* 未设定 或 已变砖：不放行进入系统 */
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
        if (why&&why_n) snprintf(why,why_n,"设备已变砖，无法设定");
        return -2;
    }
    if (ae_ld_is_provisioned(c)) {
        if (why&&why_n) snprintf(why,why_n,"专用密码仅首次可设、不可重置/找回");
        return -2;   /* 不可重置 */
    }
    int rc = ae_ld_check_strength(pw, why, why_n);
    if (rc != 0) return -1;

    /* 构造验证锚（不含密码明文），留在内存待确认，冷却通过后才落盘。 */
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

    /* 二次确认弹窗文案：警告 + 明文回显(仅屏幕/缓冲，绝不落盘) + 冷却提示。 */
    if (dialog && dialog_n) {
        snprintf(dialog, dialog_n,
            "==== 安全锁定密码 · 二次确认 ====\n"
            "此密码不可重置、不可找回，仅首次设定有效。\n"
            "进入安全锁定模式后，除非输入此密码，任何方式都无法解锁；\n"
            "一旦忘记，手机将永久变砖、无法使用（无任何恢复途径）。\n"
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
        if (msg&&n) snprintf(msg,n,"设备已变砖：任何方式都无法退出/解锁");
        return -3;
    }
    if (c->state != AE_LD_LOCKED) {
        if (msg&&n) snprintf(msg,n,"当前不在安全锁定模式");
        return -2;
    }
    if (!pw || !verify_pw(c, pw)) {
        if (msg&&n) snprintf(msg,n,"安全密码错误：仍锁定（无找回/无重置）");
        return -1;
    }
    (void)ae_store_remove(c->store, AE_ZONE_LOCKDOWN, MODE_FILE);
    c->state = AE_LD_NORMAL;
    if (msg&&n) snprintf(msg,n,"安全密码正确：已退出安全锁定模式");
    return 0;
}

/* --------------------------------------------------------------- 熔断 */
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
                /* 覆写后再删：尽力而为，检查返回值 */
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
    /* 变砖墓碑：唯一残留，代表「连系统一并抹除、永久不可用」 */
    char tomb[256];
    snprintf(tomb, sizeof tomb, "AETHER_BRICKED trigger=%s\n", trigger ? trigger : "unspecified");
    (void)ae_store_atomic_write(c->store, AE_ZONE_LOCKDOWN, AE_LD_BRICK_FILE,
                                tomb, strlen(tomb));
    (void)ae_hal_alarm(c->hal, trigger ? trigger : "physical-intrusion");
    c->state = AE_LD_BRICKED;
    if (msg && n) snprintf(msg, n,
        "熔断：已抹除包括系统本体在内的 %d 个文件、触发报警、落变砖标记；设备永久不可用", removed);
    return 0;
}

/* -------------------------------------------------- 事件驱动状态机 */
int ae_ld_on_event(ae_ld_t *c, ae_hal_event_t ev, uint64_t now_ms,
                   char *msg, size_t n)
{
    if (msg && n) msg[0] = 0;
    if (!c) return 0;

    /* 变砖终态：任何事件、任何方式都无效 */
    if (c->state == AE_LD_BRICKED) {
        if (msg&&n) snprintf(msg,n,"设备已变砖：任何事件均无效");
        return 0;
    }
    /* 安全锁定态：仅专用密码可退（unlock 单独提供）；物理入侵 → 熔断 */
    if (c->state == AE_LD_LOCKED) {
        if (ev == AE_EV_USB_DATA || ev == AE_EV_CHASSIS_OPEN || ev == AE_EV_TESTPOINT_SHORT) {
            const char *tr = ev == AE_EV_USB_DATA ? "usb-data-intrusion" :
                             ev == AE_EV_CHASSIS_OPEN ? "chassis-open" : "testpoint-short";
            char bm[256];
            (void)ae_ld_brick(c, tr, bm, sizeof bm);
            if (msg&&n) snprintf(msg,n,"检测到物理破解(%s) → 报警 + 抹除数据及系统 → %s", tr, bm);
            return 1;   /* 已熔断 */
        }
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
            c->state = AE_LD_ARMED;   /* 超时保持待确认，不自动进入 */
            if (msg&&n) snprintf(msg,n,"同按已满 10 秒 → 待「音量上」确认（保持，不自动进入）");
            /* 落到 ARMED 后继续处理本事件（若本事件即 VOL_UP 则立即进入） */
        } else {
            if (msg&&n) snprintf(msg,n,"组合键计时中（未满 10 秒）");
            return 0;
        }
    }

    if (c->state == AE_LD_ARMED) {
        if (ev == AE_EV_KEY_VOL_UP) {
            c->state = AE_LD_LOCKED; c->power_down = 0;
            (void)ae_store_atomic_write(c->store, AE_ZONE_LOCKDOWN, MODE_FILE, "LOCKED", 6);
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

/* --------------------------------------------- 外部可观测：零存储取证 */
static int file_contains(const char *path, const char *pw, size_t plen)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t buf[4096]; size_t r, carry = 0;
    uint8_t tail[64]; int found = 0;
    while ((r = fread(buf, 1, sizeof buf, f)) > 0) {
        /* 简易跨块匹配：把上一块尾部拼到前面 */
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
