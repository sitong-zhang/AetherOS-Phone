/* Aether VM - 自研虚拟机运行时实现 */
#include "vm_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <dlfcn.h>
#include <pthread.h>

struct aether_vm {
    aether_vm_config_t cfg;
    char   *argv[AETHER_VM_ARGV_MAX];
    char    scratch[AETHER_VM_SCRATCH];
    int     argc;
    pid_t   child;               /* EXEC 后端子进程 */
    void   *engine;              /* DLOPEN 后端 dlopen 句柄 */
    pthread_t thread;            /* DLOPEN 后端主循环线程 */
    volatile int quit;
    char    err[256];
    /* qemu_* 符号 */
    void (*qemu_init)(int, char **, char **);
    void (*qemu_main_loop)(void);
    void (*qemu_cleanup)(void);
    void (*qemu_shutdown_request)(void);
};

static void set_err(aether_vm_t *vm, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(vm->err, sizeof(vm->err), fmt, ap);
    va_end(ap);
}

aether_vm_t *aether_vm_create(const aether_vm_config_t *config)
{
    aether_vm_t *vm = (aether_vm_t *)calloc(1, sizeof(*vm));
    if (!vm) return NULL;
    vm->cfg = *config;
    vm->child = (pid_t)-1;
    vm->argc = aether_vm_build_argv(&vm->cfg, vm->argv, AETHER_VM_ARGV_MAX,
                                    vm->scratch, sizeof(vm->scratch));
    if (vm->argc < 0) { set_err(vm, "参数组装失败"); return vm; }
    return vm;
}

const char *aether_vm_last_error(aether_vm_t *vm) { return vm ? vm->err : "(null)"; }

/* ---------------- DLOPEN 后端：线程里跑 qemu_init + qemu_main_loop ---------------- */
static void *engine_thread(void *arg)
{
    aether_vm_t *vm = (aether_vm_t *)arg;
    extern char **environ;
    /* QEMU 主函数签名：qemu_init(argc, argv, envp) */
    if (vm->qemu_init)  vm->qemu_init(vm->argc, vm->argv, environ);
    if (vm->qemu_main_loop) vm->qemu_main_loop();
    if (vm->qemu_cleanup)   vm->qemu_cleanup();
    return NULL;
}

static int start_dlopen(aether_vm_t *vm)
{
    const char *so = vm->cfg.engine;
    if (!so || !so[0]) { set_err(vm, "dlopen 后端缺少 engine 路径"); return -1; }
    vm->engine = dlopen(so, RTLD_NOW | RTLD_GLOBAL);
    if (!vm->engine) { set_err(vm, "dlopen 失败: %s", dlerror()); return -2; }
    vm->qemu_init             = (void(*)(int,char**,char**))dlsym(vm->engine, "qemu_init");
    vm->qemu_main_loop        = (void(*)(void))dlsym(vm->engine, "qemu_main_loop");
    vm->qemu_cleanup          = (void(*)(void))dlsym(vm->engine, "qemu_cleanup");
    vm->qemu_shutdown_request = (void(*)(void))dlsym(vm->engine, "qemu_system_shutdown_request");
    if (!vm->qemu_init || !vm->qemu_main_loop) {
        set_err(vm, "引擎缺少 qemu_init/qemu_main_loop 符号"); return -3;
    }
    if (pthread_create(&vm->thread, NULL, engine_thread, vm) != 0) {
        set_err(vm, "创建引擎线程失败"); return -4;
    }
    return 0;
}

/* ---------------- EXEC 后端：fork/exec 独立 QEMU 进程 ---------------- */
static int start_exec(aether_vm_t *vm)
{
    pid_t pid = fork();
    if (pid < 0) { set_err(vm, "fork 失败: %s", strerror(errno)); return -1; }
    if (pid == 0) {
        setsid();
        /* 子进程：stdout 丢弃；stderr 落到 pidfile 旁的 .err，便于诊断启动失败 */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) dup2(devnull, 1);
        char errpath[512];
        if (vm->cfg.pidfile[0])
            snprintf(errpath, sizeof(errpath), "%s.err", vm->cfg.pidfile);
        else
            snprintf(errpath, sizeof(errpath), "/tmp/aether-vm-qemu.err");
        int ef = open(errpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (ef >= 0) dup2(ef, 2);
        execvp("qemu-system-aarch64", vm->argv);
        /* exec 失败也留一行错误 */
        dprintf(2, "execvp qemu-system-aarch64 failed: %s\n", strerror(errno));
        _exit(127);
    }
    vm->child = pid;
    return 0;
}

int aether_vm_start(aether_vm_t *vm)
{
    if (!vm) return -1;
    if (vm->argc < 0) return -1;
    return vm->cfg.backend == AETHER_BACKEND_DLOPEN ? start_dlopen(vm) : start_exec(vm);
}

/* 就绪判定：从串口日志找任一就绪标记 */
static int serial_has_ready(const char *path)
{
    if (!path || !path[0]) return 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[512];
    int ready = 0;
    /* 只认「闭环完成」标记：开机后自检脚本跑完 锁屏→桌面→点设置 才写。
     * 内核就绪(BOOT_DIAG_DONE/系统就绪)在 0.12s 就出现，不能当作闭环证据。 */
    static const char *marks[] = { "SELFTEST_DONE=1", NULL };
    while (fgets(line, sizeof(line), f)) {
        for (int i = 0; marks[i]; i++)
            if (strstr(line, marks[i])) { ready = 1; break; }
        if (ready) break;
    }
    fclose(f);
    return ready;
}

int aether_vm_wait_ready(aether_vm_t *vm, int timeout_s)
{
    if (!vm) return -1;
    if (timeout_s <= 0) return serial_has_ready(vm->cfg.serial_log) ? 0 : -1;
    for (int t = 0; t < timeout_s * 2; t++) {
        if (serial_has_ready(vm->cfg.serial_log)) return 0;
        if (!aether_vm_running(vm)) { set_err(vm, "虚拟机已退出，未就绪"); return -1; }
        struct timespec ts = { 0, 500 * 1000 * 1000L }; /* 0.5s */
        nanosleep(&ts, NULL);
    }
    set_err(vm, "等待就绪超时 %ds", timeout_s);
    return -1;
}

int aether_vm_serial_snapshot(aether_vm_t *vm, char *out, size_t n)
{
    if (!vm || !vm->cfg.serial_log[0]) { if (n) out[0] = '\0'; return 0; }
    FILE *f = fopen(vm->cfg.serial_log, "rb");
    if (!f) { if (n) out[0] = '\0'; return 0; }
    size_t got = fread(out, 1, n - 1, f);
    out[got] = '\0';
    fclose(f);
    return (int)got;
}

int aether_vm_running(aether_vm_t *vm)
{
    if (!vm) return 0;
    if (vm->cfg.backend == AETHER_BACKEND_EXEC) {
        if (vm->child <= 0) return 0;
        int st;
        pid_t r = waitpid(vm->child, &st, WNOHANG);
        if (r == 0) return 1;
        if (r == vm->child) vm->child = (pid_t)-1;
        return 0;
    }
    return vm->engine && !vm->quit;
}

int aether_vm_qmp(aether_vm_t *vm, const char *json_cmd, char *reply, size_t reply_n)
{
    if (!vm || !vm->cfg.qmp_sock[0]) { set_err(vm, "未配置 QMP socket"); return -1; }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", vm->cfg.qmp_sock);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd); return -1;
    }
    /* QMP 要求先协商能力 */
    const char *cap = "{\"execute\":\"qmp_capabilities\"}\n";
    char buf[4096];
    ssize_t r = recv(fd, buf, sizeof(buf) - 1, 0); /* greeting */
    (void)r;
    write(fd, cap, strlen(cap));
    r = recv(fd, buf, sizeof(buf) - 1, 0);          /* capabilities return */
    (void)r;
    write(fd, json_cmd, strlen(json_cmd));
    if (json_cmd[strlen(json_cmd) - 1] != '\n') write(fd, "\n", 1);
    ssize_t got = recv(fd, buf, sizeof(buf) - 1, 0);
    if (got > 0) {
        buf[got] = '\0';
        if (reply && reply_n) { snprintf(reply, reply_n, "%s", buf); }
    }
    close(fd);
    return got > 0 ? 0 : -1;
}

void aether_vm_stop(aether_vm_t *vm)
{
    if (!vm) return;
    /* 优先 QMP 优雅关机 */
    char rep[256];
    if (vm->cfg.enable_qmp && vm->cfg.qmp_sock[0]) {
        if (aether_vm_qmp(vm, "{\"execute\":\"system_powerdown\"}", rep, sizeof(rep)) == 0) {
            for (int i = 0; i < 20 && aether_vm_running(vm); i++) usleep(200 * 1000);
        }
    }
    if (vm->cfg.backend == AETHER_BACKEND_DLOPEN) {
        if (vm->qemu_shutdown_request) vm->qemu_shutdown_request();
        vm->quit = 1;
        if (vm->engine) { pthread_join(vm->thread, NULL); dlclose(vm->engine); vm->engine = NULL; }
    } else {
        if (vm->child > 0) {
            kill(vm->child, SIGTERM);
            for (int i = 0; i < 10; i++) {
                if (!aether_vm_running(vm)) break;
                usleep(200 * 1000);
            }
            if (vm->child > 0) { kill(vm->child, SIGKILL); waitpid(vm->child, NULL, 0); vm->child = (pid_t)-1; }
        }
    }
}

void aether_vm_destroy(aether_vm_t *vm)
{
    if (!vm) return;
    free(vm);
}
