/*
 * aether_os.c - 长明 Aether / AetherOS 安全整机主程序（云电脑软件模拟）
 *
 * 这是「系统本体」的入口：开机自检 → 锁屏 → 认证 → 安全会话 → 触发式全盘擦除。
 * 提供独立子命令（便于脚本/取证）与常驻 shell（真实交互会话，密钥只驻内存）。
 *
 * 说明：CLI 用 --pw 传口令仅为自动化测试与无人值守取证；生产形态应由
 *       安全输入通道读取，不在命令行/历史中留痕。
 */
#define _GNU_SOURCE
#include "aether_secure.h"
#include "aether_phys_hal.h"
#include "aether_lockdown.h"
#include "aether_intent.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <limits.h>
#include <sys/stat.h>

static const char *g_pw = NULL;

/* 安全锁定子系统：借用 ae_secure 的 store(通过 ae_secure_store 取得)，
 * 每个命令独立 open/close 一个 ld 视图，避免跨进程状态错乱。 */
static ae_hal_t *g_hal = NULL;
static ae_ld_t  *g_ld  = NULL;
static uint64_t  g_now_ms = 0;   /* --ld-now 注入的确定性时钟(仅云端仿真/取证) */

static int ld_bind(ae_secure_t *s)
{
    if (ae_hal_open(AE_HAL_SIM, &g_hal) != 0) return -1;
    if (g_now_ms) ae_hal_set_now_ms(g_hal, g_now_ms);
    if (ae_ld_open(ae_secure_store(s), g_hal, &g_ld) != 0) { ae_hal_close(g_hal); g_hal=NULL; return -1; }
    return 0;
}
static void ld_release(void)
{
    if (g_ld)  { ae_ld_close(g_ld);  g_ld = NULL; }
    if (g_hal) { ae_hal_close(g_hal); g_hal = NULL; }
}
/* 命令入口统一包装：绑定 ld → 调 fn → 释放 */
typedef int (*ld_fn)(int argc, char **argv);
static int ld_wrap(ae_secure_t *s, int argc, char **argv, ld_fn fn)
{
    if (ld_bind(s) != 0) { printf("ERR 安全锁定子系统不可用\n"); return -1; }
    int rc = fn(argc, argv);
    ld_release();
    return rc;
}
static uint64_t ld_arg_now(int argc, char **argv, uint64_t def)
{
    for (int i = 0; i < argc; i++)
        if (!strcmp(argv[i], "--now") && i+1 < argc) return strtoull(argv[++i], NULL, 10);
    return def;
}
static const char *ld_arg_pw(int argc, char **argv)
{
    for (int i = 0; i < argc; i++)
        if (!strcmp(argv[i], "--pw") && i+1 < argc) return argv[++i];
    /* main 已把全局 --pw 收到 g_pw；ld 子命令回退复用，避免重复解析。 */
    return g_pw;
}

static int ensure_session(ae_secure_t *s)
{
    if (ae_secure_state(s) == AE_SEC_UNLOCKED) return 0;
    if (!g_pw) { printf("ERR 会话已锁定，请先 unlock（或提供 --pw）\n"); return -1; }
    char m[512];
    ae_unlock_t r = ae_secure_unlock(s, g_pw, m, sizeof m);
    printf("%s\n", m);
    return r == AE_UNLOCK_OK ? 0 : -1;
}
static const char *state_str(ae_sec_state_t st)
{
    switch (st) { case AE_SEC_UNLOCKED: return "UNLOCKED";
                   case AE_SEC_FACTORY: return "FACTORY";
                   default: return "LOCKED"; }
}
static void json_escape(const char *in, char *out, size_t n)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 2 < n; i++) {
        char c = in[i];
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = c; }
        else if (c == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else out[o++] = c;
    }
    out[o] = 0;
}

/* 导出当前「真实运行态」为 JSON，供可视化取证脚本消费（非伪造画面） */
static int build_snapshot(ae_secure_t *s, char *buf, size_t n)
{
    uint64_t cnt = 0; uint8_t head[AE_SHA256_LEN];
    int chain_ok = ae_secure_audit_verify(s, &cnt, head) == 0;
    char hh[65]; ae_hex(head, AE_SHA256_LEN, hh);
    int ok = 0; uint8_t cur[AE_SHA256_LEN], base[AE_SHA256_LEN];
    int integ = ae_integrity_check(ae_secure_store(s), &ok, cur, base) == 0 && ok;
    uint8_t sd[AE_SHA256_LEN]; (void)ae_store_zone_digest(ae_secure_store(s), AE_ZONE_SYS, sd);
    char sdh[65]; ae_hex(sd, AE_SHA256_LEN, sdh);
    size_t vs = 0; (void)ae_store_volume_size(ae_secure_store(s), "vault.img", &vs);
    char roote[512], rce[256];
    json_escape(ae_store_root(ae_secure_store(s)), roote, sizeof roote);
    const char *rp = ae_secure_last_receipt(s);
    json_escape(rp ? rp : "", rce, sizeof rce);

    char files[2048]; files[0] = 0;
    int nf = 0;
    if (ae_secure_state(s) == AE_SEC_UNLOCKED) {
        char (*names)[AE_SEC_NAME_MAX] = (char(*)[AE_SEC_NAME_MAX])calloc(64, AE_SEC_NAME_MAX);
        int count = 0;
        if (names && ae_secure_list(s, names, 64, &count) == 0) {
            size_t off = 0;
            for (int i = 0; i < count && off + 128 < sizeof files; i++) {
                char e[128]; json_escape(names[i], e, sizeof e);
                int r = snprintf(files + off, sizeof files - off, "%s\"%s\"", i ? "," : "", e);
                if (r > 0) off += (size_t)r;
                nf++;
            }
        }
        free(names);
    }
    snprintf(buf, n,
        "{\n"
        "  \"system\": \"AetherOS\",\n"
        "  \"product\": \"长明 Aether / Changming Aether\",\n"
        "  \"root\": \"%s\",\n"
        "  \"state\": \"%s\",\n"
        "  \"fails\": %u,\n"
        "  \"max_fails\": %u,\n"
        "  \"volume_present\": %s,\n"
        "  \"volume_bytes\": %zu,\n"
        "  \"audit_events\": %llu,\n"
        "  \"audit_intact\": %s,\n"
        "  \"audit_head\": \"%s\",\n"
        "  \"integrity\": \"%s\",\n"
        "  \"sys_digest\": \"%s\",\n"
        "  \"last_receipt\": \"%s\",\n"
        "  \"vault_file_count\": %d,\n"
        "  \"vault_files\": [%s],\n"
        "  \"features\": [\"密码错误达阈值全盘抹除\",\"胁迫口令\",\"防重启绕过计数\","
        "\"指数退避\",\"全盘静态加密(DEK+KEK)\",\"crypto-shred 密钥销毁\","
        "\"多轮覆写数据卷\",\"只追加防篡改审计链\",\"系统区完整性自检(篡改即擦)\","
        "\"会话空闲锁定\",\"内存密钥清零(mlock+zero)\",\"紧急一键销毁\","
        "\"系统区/数据区/日志区隔离\",\"可离线校验销毁证明\","
        "\"密钥驻留TEE锚点(软件当前/硬件真机)\",\"设备密钥自证attestation\","
        "\"设备密钥绑定审计链HMAC\",\"安全锁定/熔断模式(首启强设专用密码·零存储)\","
        "\"组合键10秒+二次确认进入\",\"熔断按攻击意图分级(L1/L2零删除·L3软件抹除+烧毁硬件)\"]\n"
        "}\n",
        roote, state_str(ae_secure_state(s)),
        ae_secure_fails(s), ae_secure_max_fails(s),
        vs ? "true" : "false", vs,
        (unsigned long long)cnt, chain_ok ? "true" : "false", hh,
        integ ? "OK" : "TAMPERED", sdh, rce, nf, files);
    return 0;
}

/* ------------------------------------------------------------------ 命令实现 */
static int cmd_init(ae_secure_t *s, int argc, char **argv)
{
    const char *pw = NULL, *duress = NULL;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--pw") && i+1 < argc) pw = argv[++i];
        else if (!strcmp(argv[i], "--duress") && i+1 < argc) duress = argv[++i];
    }
    if (!pw) pw = g_pw;
    if (!pw) { printf("ERR init 需要 --pw <口令>\n"); return -1; }
    int rc = ae_secure_init(s, pw, duress);
    printf(rc == 0 ? "OK 整机初始化完成：系统基线/加密卷/凭据已建立，进入锁屏\n"
                   : "ERR 初始化失败\n");
    return rc;
}
static int cmd_boot(ae_secure_t *s, int argc, char **argv)
{
    (void)argc; (void)argv;
    char m[512];
    int rc = ae_secure_boot(s, m, sizeof m);
    printf("%s\n", m);
    return rc;
}
static int cmd_unlock(ae_secure_t *s, int argc, char **argv)
{
    const char *pw = NULL;
    for (int i = 0; i < argc; i++)
        if (!strcmp(argv[i], "--pw") && i+1 < argc) pw = argv[++i];
    if (!pw) pw = g_pw;
    if (!pw) { printf("ERR unlock 需要 --pw <口令>\n"); return -1; }
    char m[512];
    ae_unlock_t r = ae_secure_unlock(s, pw, m, sizeof m);
    printf("%s\n", m);
    return (r == AE_UNLOCK_OK) ? 0 : 1;
}
static int cmd_lock(ae_secure_t *s, int argc, char **argv)
{
    (void)argc; (void)argv;
    ae_secure_lock(s); printf("OK 会话已锁定，内存密钥已清零\n"); return 0;
}
static int cmd_put(ae_secure_t *s, int argc, char **argv)
{
    if (argc < 2) { printf("用法: put <名称> <内容>\n"); return -1; }
    if (ensure_session(s) != 0) return -1;
    char content[1024]; size_t off = 0;
    for (int i = 1; i < argc && off + 2 < sizeof content; i++) {
        int r = snprintf(content + off, sizeof content - off, "%s%s", i > 1 ? " " : "", argv[i]);
        if (r > 0) off += (size_t)r;
    }
    int rc = ae_secure_put(s, argv[0], content, off);
    printf(rc == 0 ? "OK 已加密存入保险库: %s (%zu 字节)\n" : "ERR 存入失败\n", argv[0], off);
    return rc;
}
static int cmd_get(ae_secure_t *s, int argc, char **argv)
{
    if (argc < 1) { printf("用法: get <名称>\n"); return -1; }
    if (ensure_session(s) != 0) return -1;
    uint8_t *d = NULL; size_t n = 0;
    if (ae_secure_get(s, argv[0], &d, &n) != 0) { printf("ERR 未找到: %s\n", argv[0]); return -1; }
    printf("[%s] %zu 字节: ", argv[0], n);
    fwrite(d, 1, n, stdout); printf("\n");
    ae_zero(d, n); free(d);
    return 0;
}
static int cmd_list(ae_secure_t *s, int argc, char **argv)
{
    (void)argc; (void)argv;
    if (ensure_session(s) != 0) return -1;
    char (*names)[AE_SEC_NAME_MAX] = (char(*)[AE_SEC_NAME_MAX])calloc(64, AE_SEC_NAME_MAX);
    int count = 0;
    printf("== 加密保险库文件 ==\n");
    if (names && ae_secure_list(s, names, 64, &count) == 0)
        for (int i = 0; i < count; i++) printf("  - %s\n", names[i]);
    printf("共 %d 项\n", count);
    free(names);
    return 0;
}
static int cmd_del(ae_secure_t *s, int argc, char **argv)
{
    if (argc < 1) { printf("用法: del <名称>\n"); return -1; }
    if (ensure_session(s) != 0) return -1;
    int rc = ae_secure_del(s, argv[0]);
    printf(rc == 0 ? "OK 已删除: %s\n" : "ERR 删除失败\n", argv[0]);
    return rc;
}
static int cmd_panic(ae_secure_t *s, int argc, char **argv)
{
    const char *reason = (argc > 0) ? argv[0] : "panic-command";
    int rc = ae_secure_panic(s, reason);
    printf("PANIC 紧急销毁执行完毕（rc=%d）。证明: %s\n", rc,
           ae_secure_last_receipt(s) ? ae_secure_last_receipt(s) : "(none)");
    return rc;
}
static int cmd_status(ae_secure_t *s, int argc, char **argv)
{
    int json = 0;
    for (int i = 0; i < argc; i++) if (!strcmp(argv[i], "--json")) json = 1;
    char buf[4096];
    if (json) build_snapshot(s, buf, sizeof buf);
    else ae_secure_status(s, buf, sizeof buf);
    printf("%s", buf);
    return 0;
}
static int cmd_snapshot(ae_secure_t *s, int argc, char **argv)
{
    char buf[8192];
    build_snapshot(s, buf, sizeof buf);
    if (argc > 0) {
        FILE *f = fopen(argv[0], "wb");
        if (!f) { printf("ERR 无法写快照: %s\n", argv[0]); return -1; }
        fwrite(buf, 1, strlen(buf), f); fclose(f);
        printf("OK 真实状态快照已导出: %s\n", argv[0]);
    } else printf("%s", buf);
    return 0;
}
static int cmd_audit(ae_secure_t *s, int argc, char **argv)
{
    (void)argc; (void)argv;
    uint64_t cnt = 0; uint8_t head[AE_SHA256_LEN];
    int ok = ae_secure_audit_verify(s, &cnt, head) == 0;
    char hh[65]; ae_hex(head, AE_SHA256_LEN, hh);
    printf("审计链: %s, %llu 条, head=%s\n", ok ? "INTACT" : "BROKEN",
           (unsigned long long)cnt, hh);
    if (ok) {
        char dump[16384];
        ae_audit_dump(ae_secure_audit(s), dump, sizeof dump);
        printf("---- chain.log ----\n%s", dump);
    }
    return ok ? 0 : 1;
}
static int cmd_attest(ae_secure_t *s, int argc, char **argv)
{
    (void)argc; (void)argv;
    char report[2048];
    if (ae_secure_attest(s, report, sizeof report) != 0) {
        printf("ERR 设备自证失败（TEE 锚点不可用？）\n"); return -1;
    }
    printf("%s", report);
    return 0;
}
/* ---------------------------------------------------- 安全锁定 / 熔断命令 */
static int ld_provision(int argc, char **argv)
{
    const char *pw = ld_arg_pw(argc, argv);
    if (!pw) { printf("ERR ld-provision 需要 --pw <专用安全密码>\n"); return -1; }
    char dialog[2048], why[256];
    int rc = ae_ld_provision_begin(g_ld, pw, dialog, sizeof dialog, why, sizeof why);
    if (rc == -1) { printf("强度不合格: %s\n", why); return -1; }
    if (rc == -2) { printf("拒绝: %s\n", why); return -1; }
    printf("%s", dialog);   /* 二次确认弹窗：警告 + 明文回显(仅屏幕，绝不落盘) */
    /* 冷却窗口由状态机强制；此处按 --now 注入时刻走确认(云端取证用真时钟) */
    uint64_t now = ld_arg_now(argc, argv, ae_hal_now_ms(g_hal) + AE_LD_COOLDOWN_MS);
    int cr = ae_ld_provision_confirm(g_ld, now, why, sizeof why);
    if (cr != 0) { printf("ERR 确认失败: %s\n", why); return -1; }
    printf("OK 专用安全密码已设定（系统零存储密码明文，仅保留不可倒推验证锚）\n");
    return 0;
}
static int ld_status(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("安全锁定状态: %s\n", ae_ld_state_name(g_ld));
    printf("  专用密码已设定: %s | 验证锚落盘: %s | 开机硬门: %s\n",
           ae_ld_is_provisioned(g_ld) ? "是" : "否",
           ae_ld_anchor_exists(g_ld) ? "是" : "否",
           ae_ld_oobe_gate(g_ld) ? "放行" : "拦截(未设定/已软件熔断)");
    printf("  物理后端: %s | 安全芯片: %s | 报警计数: %u\n",
           ae_hal_backend_name(g_hal),
           ae_hal_secure_element(g_hal) ? "硬件在线" : "软件锚点(真机接 TEE/eFuse)",
           ae_hal_alarm_count(g_hal));
    printf("  证据链: %u 条 | 维修白名单: %s | 硬件熔断后端: %s\n",
           ae_ld_evidence_count(g_ld),
           ae_ld_service_mode(g_ld) ? "开启" : "关闭",
           ae_ld_hw_fuse_backend(g_ld));
    printf("  注: L1无害/L2可疑 零删除；仅 L3 确认攻击意图才熔断(软件抹除+烧毁硬件指令)\n");
    return 0;
}
static int ld_gesture(int argc, char **argv)
{
    /* 默认注入一套「满 10 秒 + 音量上确认」事件；--now 指定计时起点 */
    uint64_t t0 = ld_arg_now(argc, argv, ae_hal_now_ms(g_hal));
    char m[512];
    ae_hal_event_t seq[] = {AE_EV_KEY_POWER, AE_EV_KEY_VOL_DOWN};
    uint64_t ts[] = {t0, t0};
    for (int i = 0; i < 2; i++) { (void)ae_ld_on_event(g_ld, seq[i], ts[i], m, sizeof m); printf("%s\n", m); }
    (void)ae_ld_on_event(g_ld, AE_EV_KEY_POWER, t0 + AE_LD_GESTURE_MS, m, sizeof m);
    printf("%s\n", m);
    (void)ae_ld_on_event(g_ld, AE_EV_KEY_VOL_UP, t0 + AE_LD_GESTURE_MS + 1, m, sizeof m);
    printf("%s\n", m);
    printf("最终状态: %s\n", ae_ld_state_name(g_ld));
    return ae_ld_state(g_ld) == AE_LD_LOCKED ? 0 : -1;
}
static int ld_unlock(int argc, char **argv)
{
    const char *pw = ld_arg_pw(argc, argv);
    if (!pw) { printf("ERR ld-unlock 需要 --pw <专用安全密码>\n"); return -1; }
    char m[512];
    int rc = ae_ld_unlock(g_ld, pw, m, sizeof m);
    printf("%s\n", m);
    return rc;
}
/* 事件关键字 → 物理事件（分级判据见 aether_phys_hal.h）。 */
static int ld_parse_event(const char *kind, ae_hal_event_t *out)
{
    struct { const char *k; ae_hal_event_t ev; } map[] = {
        /* L1 无害 */
        {"charge",      AE_EV_USB_CHARGE},
        {"chassis",     AE_EV_CHASSIS_OPEN},
        {"short",       AE_EV_TESTPOINT_SHORT},
        {"service",     AE_EV_SERVICE_MODE},
        /* L2 可疑 */
        {"adb",         AE_EV_ADB_UNAUTH},
        {"auth-fail",   AE_EV_AUTH_FAIL},
        {"enum",        AE_EV_MASS_ENUM},
        {"core-read",   AE_EV_CORE_READ},
        /* L3 确认攻击意图（立即熔断） */
        {"usb",         AE_EV_USB_DATA},        /* USB 数据强插破解 */
        {"core-export", AE_EV_CORE_EXPORT},    /* 核心区/用户数据导出 */
        {"key-tamper",  AE_EV_KEY_TAMPER},     /* 密钥区触碰 */
        {"short-core",  AE_EV_SHORT_CORE},     /* 拆机短接触及核心区 */
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (!strcmp(kind, map[i].k)) { *out = map[i].ev; return 0; }
    return -1;
}
static int ld_inject(int argc, char **argv)
{
    /* 支持一串事件（同一进程内依次注入，便于演示维修白名单/证据累计/攻击升级）；
     * --now <毫秒> 指定起始时刻。 */
    int n_ev = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--now") && i + 1 < argc) { i++; continue; }
        ae_hal_event_t probe;
        if (ld_parse_event(argv[i], &probe) == 0) n_ev++;
    }
    if (n_ev == 0) {
        printf("用法: ld-inject <事件...> [--now <毫秒>]\n"
               "  L1无害(零删除): charge | chassis | short | service(维修白名单)\n"
               "  L2可疑(取证·零删除): adb | auth-fail | enum | core-read\n"
               "  L3攻击意图(立即熔断): usb | core-export | key-tamper | short-core\n");
        return -1;
    }
    uint64_t now = ld_arg_now(argc, argv, ae_hal_now_ms(g_hal));
    char m[512];
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--now") && i + 1 < argc) { i++; continue; }
        ae_hal_event_t ev;
        if (ld_parse_event(argv[i], &ev) != 0) continue;
        (void)ae_hal_inject(g_hal, ev);
        int fired = ae_ld_on_event(g_ld, ev, now + (uint64_t)i, m, sizeof m);
        printf("[%s] %s\n  %s\n  状态=%s | 证据=%u | 硬件熔断=%s\n",
               ae_hal_threat_name(ae_hal_event_threat(ev)), ae_hal_event_name(ev), m,
               ae_ld_state_name(g_ld), ae_ld_evidence_count(g_ld),
               ae_ld_hw_fuse_burned(g_ld) ? "已锁存(云端仿真)" : "无");
        if (fired) { printf("  >>> 熔断已触发：软件抹除 + 烧毁硬件指令\n"); break; }
    }
    return 0;
}
/* 观测 → 意图判定 → 归一化事件 → 状态机（完整链路，外部可观测）。
 * 用法: ld-observe usb=<...> verb=<...> region=<...> [auth=0|1] [service=0|1]
 *       [chassis=0|1] [short=0|1] [fails=N] [bulk=N] [key=... ...] [--now <ms>]
 * 未给出任何观测字段时打印用法与内置观测维度。 */
static int ld_observe(int argc, char **argv)
{
    ae_observation_t obs;
    ae_intent_verdict_t v;
    ae_observation_reset(&obs);
    int n_kv = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--now") && i + 1 < argc) { i++; continue; }
        if (ae_intent_parse_kv(argv[i], &obs) != 0) {
            printf("ERR 未知观测字段: %s\n", argv[i]);
            return -1;
        }
        n_kv++;
    }
    if (n_kv == 0) {
        printf("用法: ld-observe <key=value ...> [--now <毫秒>]\n"
               "  usb    : none charge hid mtp mass adb fastboot custom\n"
               "  verb   : none auth-ok auth-fail enum-query read-public read-core\n"
               "           export-core write-boot key-access brute flash-dump\n"
               "  region : none public user-data sys-core key\n"
               "  auth / service / chassis / short : 0|1   fails / bulk : 整数\n"
               "  例: ld-observe usb=fastboot verb=export-core region=sys-core auth=0\n");
        return -1;
    }
    if (ae_intent_classify(&obs, &v) != 0) {
        printf("ERR 意图判定失败\n"); return -1;
    }
    printf("观测: usb=%s verb=%s region=%s auth=%s service=%s chassis=%d "
           "short=%d fails=%u bulk=%u\n",
           ae_intent_usb_name(obs.usb), ae_intent_verb_name(obs.verb),
           ae_intent_region_name(obs.region), obs.auth_ok ? "ok" : "no",
           obs.service_credential ? "yes" : "no", obs.chassis_open,
           obs.testpoint_short, obs.auth_fails, obs.bulk_read_bytes);
    printf("判定: [%s] 规则=%s 归一化事件=%s\n  理由: %s\n",
           ae_hal_threat_name(v.threat), v.rule,
           ae_hal_event_name(v.event), v.reason);
    /* 判定等级与归一化事件必须一致（判定层不产生自相矛盾的判决）。 */
    if (v.threat != AE_THREAT_SERVICE &&
        ae_hal_event_threat(v.event) != v.threat) {
        printf("ERR 判定等级与归一化事件不一致（内部一致性失败）\n");
        return -1;
    }
    uint64_t now = ld_arg_now(argc, argv, ae_hal_now_ms(g_hal));
    char m[512];
    (void)ae_hal_inject(g_hal, v.event);
    int fired = ae_ld_on_event(g_ld, v.event, now, m, sizeof m);
    printf("动作: %s\n  状态=%s | 证据=%u | 硬件熔断=%s\n",
           m, ae_ld_state_name(g_ld), ae_ld_evidence_count(g_ld),
           ae_ld_hw_fuse_burned(g_ld) ? "已锁存(云端仿真)" : "无");
    if (fired) printf("  >>> 确认攻击意图：软件抹除 + 烧毁硬件指令\n");
    return 0;
}

static int cmd_help(ae_secure_t *s, int argc, char **argv)
{
    (void)s; (void)argc; (void)argv;
    printf(
      "长明 Aether / AetherOS 安全整机命令\n"
      "  init    --pw <口令> [--duress <胁迫口令>]   初始化整机\n"
      "  boot                                         开机自检\n"
      "  unlock  --pw <口令>                          认证解锁/累计失败\n"
      "  lock                                         锁定会话（清零内存密钥）\n"
      "  put     <名称> <内容>                        加密存入保险库\n"
      "  get     <名称>                               读取并解密\n"
      "  list                                         列出保险库文件\n"
      "  del     <名称>                               删除\n"
      "  panic   [原因]                               紧急一键销毁全部数据\n"
      "  status  [--json]                             系统状态\n"
      "  snapshot [路径.json]                          导出真实状态快照\n"
      "  audit                                        校验防篡改审计链\n"
      "  attest                                       设备自证（设备密钥签名的可验证完整性报告）\n"
      "  --- 安全锁定 / 熔断模式 ---\n"
      "  ld-provision --pw <专用密码> [--now <毫秒>]    首启强制设定(强度+弹窗回显+10秒冷却)\n"
      "  ld-status                                     查看锁定状态/硬门/物理后端\n"
      "  ld-gesture   [--now <毫秒>]                   组合键：电源+音量下满10秒+音量上确认\n"
      "  ld-unlock    --pw <专用密码>                  用专用密码退出安全锁定模式\n"
      "  ld-inject    <事件> [--now ...]               注入已归一化的物理事件(状态机测试入口)\n"
      "      L1无害零删除: charge chassis short service | L2取证零删除: adb auth-fail enum core-read\n"
      "      L3攻击意图→软件抹除+烧毁硬件: usb core-export key-tamper short-core\n"
      "  ld-observe   <key=value ...> [--now ...]       观测→攻击意图判定→动作(完整链路)\n"
      "      usb= verb= region= auth= service= chassis= short= fails= bulk=（ld-observe 无参看全表）\n"
      "  help / exit                                  帮助 / 退出\n");
    return 0;
}

static int dispatch(ae_secure_t *s, const char *cmd, int argc, char **argv)
{
    if      (!strcmp(cmd,"init"))     return cmd_init(s,argc,argv);
    else if (!strcmp(cmd,"boot"))     return cmd_boot(s,argc,argv);
    else if (!strcmp(cmd,"unlock"))   return cmd_unlock(s,argc,argv);
    else if (!strcmp(cmd,"lock"))     return cmd_lock(s,argc,argv);
    else if (!strcmp(cmd,"put"))      return cmd_put(s,argc,argv);
    else if (!strcmp(cmd,"get"))      return cmd_get(s,argc,argv);
    else if (!strcmp(cmd,"list"))     return cmd_list(s,argc,argv);
    else if (!strcmp(cmd,"del"))      return cmd_del(s,argc,argv);
    else if (!strcmp(cmd,"panic"))    return cmd_panic(s,argc,argv);
    else if (!strcmp(cmd,"status"))   return cmd_status(s,argc,argv);
    else if (!strcmp(cmd,"snapshot")) return cmd_snapshot(s,argc,argv);
    else if (!strcmp(cmd,"audit"))    return cmd_audit(s,argc,argv);
    else if (!strcmp(cmd,"attest"))   return cmd_attest(s,argc,argv);
    else if (!strcmp(cmd,"ld-provision")) return ld_wrap(s,argc,argv,ld_provision);
    else if (!strcmp(cmd,"ld-status"))    return ld_wrap(s,argc,argv,ld_status);
    else if (!strcmp(cmd,"ld-gesture"))   return ld_wrap(s,argc,argv,ld_gesture);
    else if (!strcmp(cmd,"ld-unlock"))    return ld_wrap(s,argc,argv,ld_unlock);
    else if (!strcmp(cmd,"ld-inject"))    return ld_wrap(s,argc,argv,ld_inject);
    else if (!strcmp(cmd,"ld-observe"))   return ld_wrap(s,argc,argv,ld_observe);
    else if (!strcmp(cmd,"help") || !strcmp(cmd,"?")) return cmd_help(s,argc,argv);
    else { printf("未知命令: %s（输入 help）\n", cmd); return -1; }
}

static int cmd_shell(ae_secure_t *s)
{
    char line[2048];
    printf("AetherOS Secure Shell — 输入 help 查看命令，exit 退出\n");
    while (fgets(line, sizeof line, stdin)) {
        char *words[64]; int wn = 0;
        char *sv = NULL;
        char *t = strtok_r(line, " \t\r\n", &sv);
        while (t && wn < 64) { words[wn++] = t; t = strtok_r(NULL, " \t\r\n", &sv); }
        if (!wn) continue;
        if (!strcmp(words[0], "exit") || !strcmp(words[0], "quit")) break;
        int rc = dispatch(s, words[0], wn-1, words+1);
        printf("[rc=%d]\n", rc);
        fflush(stdout);
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *root = "build/secure-root";
    ae_sec_config_t cfg; ae_secure_default_config(&cfg);
    char *words[64]; int wn = 0;
    int i;
    for (i = 1; i < argc && wn < 64; i++) {
        if (!strcmp(argv[i], "--root") && i+1 < argc) root = argv[++i];
        else if (!strcmp(argv[i], "--pw") && i+1 < argc) g_pw = argv[++i];
        else if (!strcmp(argv[i], "--max-fails") && i+1 < argc) cfg.max_fails = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--idle") && i+1 < argc) cfg.idle_timeout_s = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--no-delay")) cfg.delay_enable = 0;
        else if (!strcmp(argv[i], "--no-tamper-wipe")) cfg.tamper_wipe = 0;
        else if (!strcmp(argv[i], "--ld-now") && i+1 < argc) g_now_ms = strtoull(argv[++i], NULL, 10);
        else words[wn++] = argv[i];
    }
    const char *cmd = wn ? words[0] : "status";
    int cargc = wn ? wn-1 : 0;
    char **cargv = wn ? words+1 : NULL;

    /* 变砖是 L0 引导级终态：任何子系统(含 secure 重建系统文件)都不许再启动。
     * 必须在 ae_secure_open 之前拦截，否则会重建 sys/device.key 造成"没删干净"。 */
    char brick[PATH_MAX];
    if (snprintf(brick, sizeof brick, "%s/%s/%s", root, AE_ZONE_LOCKDOWN,
                 AE_LD_BRICK_FILE) > 0) {
        struct stat bst;
        if (stat(brick, &bst) == 0 && S_ISREG(bst.st_mode)) {
            if (!strcmp(cmd, "ld-status")) {
                printf("安全锁定状态: BRICKED(软件熔断·系统本体不可恢复)\n"
                       "  开机硬门: 拦截 | 系统本体已抹除 | 任何操作/任何密码均无法在本系统内恢复\n"
                       "  如实标注: 软件熔断=刷机可重装(非硬件永久变砖)；硬件永久变砖由真机 eFuse(S6) 实现\n"
                       "  唯一出路: 外部重刷镜像(真机由 bootloader/安全芯片管控)\n");
                return 0;
            }
            printf("ERR 设备已软件熔断：系统本体已抹除，任何命令均被拒（刷机可重装）\n");
            return 3;
        }
    }

    ae_secure_t *s = NULL;
    if (ae_secure_open(root, &cfg, &s) != 0) { printf("ERR 无法打开运行根: %s\n", root); return 2; }
    int rc;
    if (!strcmp(cmd, "shell")) rc = cmd_shell(s);
    else if (!strcmp(cmd, "help")) rc = cmd_help(s, cargc, cargv);
    else rc = dispatch(s, cmd, cargc, cargv);
    ae_secure_close(s);
    return rc == 0 ? 0 : 1;
}
