/*
 * AetherOS · PID 1 用户态 init（ring3）
 *
 * 完整链路证明：本程序是独立 ELF64，由内核 ELF 加载器映射到 U/S=1 用户页，
 * 通过 int 0x80 系统调用访问内核服务；程序自身不能直连任何设备或内核内存。
 *
 * 自动演示：出厂 -> 解锁 -> 加密存 3 个文件 -> 读回 -> 锁定 ->
 *           连续输错 N 次 -> 全盘用户数据销毁（系统完好）-> 重新初始化 -> 旧数据不可恢复。
 * 之后进入交互 shell（status/selftest/list/audit/help/exit），验证真实输入链路。
 */
#include "aether_uapi.h"

static char g_buf[4096];
static char g_line[256];
static unsigned long g_line_len;

static void out(const char *s) { (void)u_write(s); }
static void out_num(const char *tag, long v) {
    char b[32]; uintoa(b, (unsigned long)(v < 0 ? -v : v));
    out(tag); if (v < 0) out("-"); out(b); out("\n");
}
static int expect(const char *s, const char *key) {
    unsigned long i = 0, j = 0;
    while (s[i]) {
        if (s[i] == key[j]) { j++; if (!key[j]) return 1; }
        else j = 0;
        i++;
    }
    return 0;
}

/* ---- 自动演示 ---- */
static int demo(void) {
    long r;
    out("\n=== AetherOS ring3 init :: SECURITY DEMO ===\n");

    out("[1] boot magic via syscall: ");
    long magic = u_boot_magic();
    if (magic == K_BUILD_MAGIC) out("BOOT_MAGIC_OK (served by ring0 kernel)\n");
    else { out("BOOT_MAGIC_FAIL\n"); return 1; }

    out("[2] kernel selftest:\n");
    r = u_selftest(g_buf, sizeof g_buf); out(g_buf);
    if (!expect(g_buf, "crypto_sha256=PASS")) { out("SELFTEST_FAIL\n"); return 2; }
    (void)r;

    out("[3] factory provisioning (password + duress password):\n");
    r = u_login("S3cret!2026", "Duress911");
    if (r != 0) { out("PROVISION_FAIL rc="); out_num("", r); return 3; }
    out("PROVISION_OK\n");

    out("[4] unlock:\n");
    r = u_unlock("S3cret!2026", g_buf, sizeof g_buf); out(g_buf);
    if (r != K_AUTH_OK) { out("UNLOCK_FAIL\n"); return 4; }
    out("UNLOCK_OK\n");

    out("[5] store 3 encrypted files into vault (data volume):\n");
    if (u_put("design.md", "Changming Aether secure design v1", 33) != 0) return 5;
    if (u_put("keys.bin", "A1B2C3D4E5F6", 13) != 0) return 5;
    if (u_put("contacts.txt", "ZhangSan-13800000000", 21) != 0) return 5;
    out("VAULT_PUT_OK x3\n");
    r = u_list(g_buf, sizeof g_buf); out(g_buf); (void)r;

    out("[6] read back design.md:\n");
    r = u_get("design.md", g_buf, sizeof g_buf);
    if (r > 0 && expect(g_buf, "Changming Aether")) out("VAULT_GET_OK\n");
    else { out("VAULT_GET_FAIL\n"); return 6; }

    out("[7] lock session:\n");
    (void)u_lock(); out("LOCK_OK\n");

    out("[8] three wrong passwords (threshold triggers full data destruction):\n");
    r = u_unlock("wrong1", g_buf, sizeof g_buf); out(g_buf);
    r = u_unlock("wrong2", g_buf, sizeof g_buf); out(g_buf);
    r = u_unlock("wrong3", g_buf, sizeof g_buf); out(g_buf);
    if (r != K_AUTH_WIPED) { out("WIPE_TRIGGER_FAIL\n"); return 7; }

    out("[9] wipe receipt + system integrity:\n");
    r = u_wipe_status(g_buf, sizeof g_buf); out(g_buf); (void)r;
    if (!expect(g_buf, "sys_unchanged\": true")) { out("SYS_INTEGRITY_FAIL\n"); return 8; }
    if (!expect(g_buf, "ALL_USER_DATA_DESTROYED_SYSTEM_INTACT")) { out("NO_VERDICT_FAIL\n"); return 8; }
    out("WIPE_RECEIPT_OK sys_unchanged=true\n");

    out("[10] re-provision after wipe (system survives, reusable):\n");
    r = u_login("NewPass!2026", "Duress911");
    if (r != 0) { out("REINIT_FAIL\n"); return 9; }
    r = u_unlock("NewPass!2026", g_buf, sizeof g_buf); out(g_buf);
    if (r != K_AUTH_OK) { out("REUNLOCK_FAIL\n"); return 9; }
    out("REINIT_OK\n");

    out("[11] old data must be unrecoverable:\n");
    r = u_get("design.md", g_buf, sizeof g_buf);
    if (r < 0) out("OLD_DATA_GONE\n");
    else { out("OLD_DATA_LEAK_FAIL\n"); return 10; }

    out("\nDEMO_DONE=1  ALL STAGES PASSED\n");
    return 0;
}

/* ---- 交互 shell ---- */
static void run_line(void) {
    g_line[g_line_len] = 0;
    if (g_line_len == 0) return;
    /* 去掉尾部 CR */
    while (g_line_len && (g_line[g_line_len-1] == '\r' || g_line[g_line_len-1] == '\n'))
        g_line[--g_line_len] = 0;
    out("SHELL CMD: "); out(g_line); out("\n");

    if (ustrcmp(g_line, "help") == 0) {
        out("commands: status selftest list audit time help exit\n");
    } else if (ustrcmp(g_line, "status") == 0) {
        u_sysinfo(g_buf, sizeof g_buf); out(g_buf);
    } else if (ustrcmp(g_line, "selftest") == 0) {
        u_selftest(g_buf, sizeof g_buf); out(g_buf);
    } else if (ustrcmp(g_line, "list") == 0) {
        u_list(g_buf, sizeof g_buf); out(g_buf);
    } else if (ustrcmp(g_line, "audit") == 0) {
        u_audit(g_buf, sizeof g_buf); out(g_buf); out("\n");
    } else if (ustrcmp(g_line, "time") == 0) {
        out_num("TICKS=", u_get_ticks());
    } else if (ustrcmp(g_line, "exit") == 0) {
        out("SHELL_EXIT\n");
        (void)usyscall(K_SYS_EXIT, 0, 0, 0, 0, 0);
    } else {
        out("UNKNOWN CMD\n");
    }
    g_line_len = 0;
}

int umain(void) {
    int rc = demo();
    if (rc != 0) {
        out("DEMO_FAILED stage="); out_num("", rc);
        (void)usyscall(K_SYS_EXIT, rc, 0, 0, 0, 0);
    }

    out("\n--- interactive shell ready (status/selftest/list/audit/help/exit) ---\n");
    out("SHELL READY\n");

    /* 用 PIT tick 做超时：无输入也会自行结束，保证 QEMU 不会挂住。
     * 有输入时立即响应，用于自动化注入命令验证真实输入链路。 */
    long start = u_get_ticks();
    const long TIMEOUT = 120;   /* 1.2 s @100Hz，足够脚本注入 */
    char tmp[64];
    while (u_get_ticks() - start < TIMEOUT) {
        long n = u_read(tmp, sizeof tmp);
        if (n <= 0) continue;
        for (long i = 0; i < n; i++) {
            char c = tmp[i];
            if (c == '\n' || c == '\r') { run_line(); }
            else if (g_line_len + 1 < sizeof g_line) g_line[g_line_len++] = c;
        }
    }
    out("SHELL IDLE TIMEOUT -> exit\n");
    return 0;
}
