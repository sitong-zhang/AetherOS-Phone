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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

static const char *g_pw = NULL;

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
        "\"密钥驻留TEE锚点(软件当前/硬件真机)\",\"设备密钥自证attestation\"]\n"
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
        else words[wn++] = argv[i];
    }
    const char *cmd = wn ? words[0] : "status";
    int cargc = wn ? wn-1 : 0;
    char **cargv = wn ? words+1 : NULL;

    ae_secure_t *s = NULL;
    if (ae_secure_open(root, &cfg, &s) != 0) { printf("ERR 无法打开运行根: %s\n", root); return 2; }
    int rc;
    if (!strcmp(cmd, "shell")) rc = cmd_shell(s);
    else if (!strcmp(cmd, "help")) rc = cmd_help(s, cargc, cargv);
    else rc = dispatch(s, cmd, cargc, cargv);
    ae_secure_close(s);
    return rc == 0 ? 0 : 1;
}
