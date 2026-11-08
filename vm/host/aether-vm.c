/* Aether VM - 宿主侧独立虚拟机程序（在这里真实开机用）
 * 用法: aether-vm <vm.conf> <waittimeout_s>
 * 它本身就是「一个软件、一个虚拟机」：读配置 → 组装 QEMU 参数 → 拉起引擎
 * → 等待系统就绪 → 导出串口状态证据 → 报告闭环结果。
 * 与 Android 端共用 core/vm_config.c + core/vm_runtime.c。 */
#include "vm_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static void ensure_dir(const char *p)
{
    struct stat st;
    if (stat(p, &st) != 0) mkdir(p, 0755);
}

static void print_closure_evidence(const char *serial)
{
    /* 从串口日志里挑出闭环关键证据行 */
    const char *keys[] = {
        "系统就绪", "显示打开", "逻辑画布", "SELFTEST: shot",
        "SELFTEST_STATE_HOME", "SELFTEST_STATE_CLICK",
        "SELFTEST_FALLBACK", "SELFTEST_STATE_FINAL", "SELFTEST_DONE",
        "BOOT_DIAG_DONE", "PHONETEST_DONE", NULL
    };
    char buf[64 * 1024];
    snprintf(buf, sizeof(buf), "%s", serial);
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        for (int i = 0; keys[i]; i++)
            if (strstr(line, keys[i])) { puts(line); break; }
    }
}

int main(int argc, char **argv)
{
    const char *conf = argc > 1 ? argv[1] : "vm.conf";
    int timeout = argc > 2 ? atoi(argv[2]) : 40;

    aether_vm_config_t cfg;
    aether_vm_config_defaults(&cfg);
    if (aether_vm_config_load(&cfg, conf) != 0) {
        fprintf(stderr, "无法读取配置 %s\n", conf);
        return 2;
    }
    /* 宿主后端一律 exec 独立 QEMU 进程；无图形显示；串口/共享/QMP 都用配置里的路径 */
    cfg.backend = AETHER_BACKEND_EXEC;
    snprintf(cfg.display, sizeof(cfg.display), "none");
    ensure_dir(cfg.share_dir[0] ? cfg.share_dir : ".");

    char summary[512];
    aether_vm_config_dump(&cfg, summary, sizeof(summary));
    printf("=== %s ===\n", summary);
    {
        char *av[AETHER_VM_ARGV_MAX];
        char sc[AETHER_VM_SCRATCH];
        int n = aether_vm_build_argv(&cfg, av, AETHER_VM_ARGV_MAX, sc, sizeof(sc));
        printf("启动参数由 Aether VM 自行组装，共 %d 项（core/vm_config.c）\n", n);
    }

    aether_vm_t *vm = aether_vm_create(&cfg);
    if (!vm) { fprintf(stderr, "create 失败\n"); return 2; }
    if (aether_vm_start(vm) != 0) {
        fprintf(stderr, "start 失败: %s\n", aether_vm_last_error(vm));
        return 3;
    }
    printf("引擎已拉起，等待系统就绪（最多 %ds）...\n", timeout);
    int rc = aether_vm_wait_ready(vm, timeout);
    printf("wait_ready => %s\n", rc == 0 ? "系统已就绪" : aether_vm_last_error(vm));

    /* 给自检脚本一点时间把最后一帧和状态写完 */
    usleep(1500 * 1000);

    char serial[64 * 1024];
    aether_vm_serial_snapshot(vm, serial, sizeof(serial));
    printf("\n=== 闭环状态证据（串口）===\n");
    print_closure_evidence(serial);

    /* 闭环判定（对齐完整手机桌面）：桌面态 -> 真实点击磁贴开窗 ->
     * 控制中心 -> 浅色主题 -> 应用抽屉，均需有真实 state 证据，且不得触发兜底。
     * fallback 纳入最终断言，避免"兜底强行开窗"被误报为闭环跑通。 */
    int home_ok   = strstr(serial, "SELFTEST_STATE_HOME") != NULL;
    int click_ok  = strstr(serial, "SELFTEST_STATE_CLICK") != NULL
                 && (strstr(serial, "\"open\":1") != NULL
                  || strstr(serial, "\"open\": 1") != NULL);
    int panel_ok  = strstr(serial, "SELFTEST_STATE_PANEL") != NULL
                 && strstr(serial, "\"panel\":2") != NULL;
    int light_ok  = strstr(serial, "SELFTEST_STATE_LIGHT") != NULL
                 && strstr(serial, "\"theme\":1") != NULL;
    int drawer_ok = strstr(serial, "SELFTEST_STATE_DRAWER") != NULL
                 && strstr(serial, "\"drawer\":1") != NULL;
    int done_ok   = strstr(serial, "SELFTEST_DONE=1") != NULL;
    int fallback  = strstr(serial, "SELFTEST_FALLBACK_OPEN=1") != NULL;
    int all_ok = home_ok && click_ok && panel_ok && light_ok && drawer_ok
                 && done_ok && !fallback;
    printf("\n=== 闭环判定（完整桌面）===\n");
    printf("  桌面态证据        : %s\n", home_ok ? "有" : "缺");
    printf("  真实点击磁贴开窗  : %s\n", click_ok ? "open:1 已开窗" : "未检出");
    printf("  控制中心浮层      : %s\n", panel_ok ? "panel:2 已展开" : "未检出");
    printf("  浅色主题          : %s\n", light_ok ? "theme:1 已切换" : "未检出");
    printf("  应用抽屉          : %s\n", drawer_ok ? "drawer:1 已展开" : "未检出");
    printf("  真实点击非兜底    : %s\n", !fallback ? "是" : "否（触发兜底）");
    printf("  自检完成          : %s\n", done_ok ? "是" : "否");
    printf("  结果              : %s\n", all_ok ? "闭环跑通 ✅" : "闭环未闭环 ❌");

    aether_vm_stop(vm);
    aether_vm_destroy(vm);
    return all_ok ? 0 : 1;
}
