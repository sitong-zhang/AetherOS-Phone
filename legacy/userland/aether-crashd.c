/*
 * aether-crashd.c - AetherOS 崩溃管理器（P2-13b）。
 *
 * 职责（单一）：接收 aether-init 在服务异常退出时发出的事件，
 * 生成结构化崩溃报告落 /data/aether/crash/。
 *
 * 事件协议（Unix datagram，/run/aether-crash.sock，单行）：
 *   crash <svc> <pid> <code> <restarts> <uptime> <cmdline...>
 *     code: sig:N（被信号 N 终止）或 code:N（退出码 N）
 *
 * 报告内容：服务名 / pid / 终止原因（信号名映射）/ monotonic uptime /
 * 重启计数 / 墙钟 / cmdline / init 日志尾部 / 序列化日志尾部。
 *
 * 诚实边界（写进每份报告，不冒充取证能力）：
 *   - 未配置 core dump（不碰 kernel.core_pattern；报告不是 core）；
 *   - 进程退出后 /proc/<pid> 消失，无法采集 maps/status 快照；
 *   - 报告是「日志级事后记录」，不是内核级崩溃分析。
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define SOCK_PATH       "/run/aether-crash.sock"
#define CRASH_DIR       "/data/aether/crash"
#define INIT_LOG        "/run/aether-init.log"
#define AETHER_LOG      "/var/log/aether.log"
#define TAIL_LINES      15
#define REC_MAX         1024

static volatile sig_atomic_t g_stop = 0;
static void on_sigterm(int sig) { (void)sig; g_stop = 1; }

static const char *signal_name(int sig)
{
    switch (sig) {
    case 1:  return "SIGHUP";   case 2:  return "SIGINT";
    case 3:  return "SIGQUIT";  case 4:  return "SIGILL";
    case 6:  return "SIGABRT";  case 8:  return "SIGFPE";
    case 9:  return "SIGKILL";  case 11: return "SIGSEGV";
    case 13: return "SIGPIPE";  case 15: return "SIGTERM";
    case 7:  return "SIGBUS";   case 5:  return "SIGTRAP";
    default: return "SIG?";
    }
}

static void iso_now(char *buf, size_t n)
{
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    if (!strftime(buf, n, "%Y-%m-%dT%H:%M:%SZ", &tm)) snprintf(buf, n, "?");
}

/* 复制 src 文件最后 TAIL_LINES 行到 dst（文件不存在则如实说明）。 */
static void tail_into(FILE *dst, const char *src)
{
    FILE *f = fopen(src, "r");
    fprintf(dst, "---- tail of %s ----\n", src);
    if (!f) {
        fprintf(dst, "(unavailable: %s)\n", strerror(errno));
        return;
    }
    char lines[TAIL_LINES + 1][512];
    int idx = 0, count = 0, wrapped = 0;
    char buf[512];
    while (fgets(buf, sizeof buf, f)) {
        /* 超长行截断保形，不丢后续行 */
        size_t l = strlen(buf);
        if (l && buf[l - 1] != '\n') {
            buf[sizeof buf - 2] = '\n';
            buf[sizeof buf - 1] = '\0';
        }
        if (count < TAIL_LINES) count++;
        size_t bl = strlen(buf);
        if (bl > sizeof lines[0] - 1) bl = sizeof lines[0] - 1;
        memcpy(lines[idx], buf, bl);
        lines[idx][bl] = '\0';
        idx = (idx + 1) % (TAIL_LINES + 1);
        if (count == TAIL_LINES) wrapped = 1;
    }
    fclose(f);
    int start = wrapped ? idx : 0;
    for (int i = 0; i < count; i++) {
        fputs(lines[(start + i) % (TAIL_LINES + 1)], dst);
    }
}

static void write_report(const char *svc, const char *pid, const char *code,
                         const char *restarts, const char *up,
                         const char *cmdline)
{
    mkdir(CRASH_DIR, 0755);   /* /data/aether/crash；递归由 init 建 /data */

    char wall[32];
    iso_now(wall, sizeof wall);

    char path[256];
    snprintf(path, sizeof path, "%s/%s-%ld-%s.txt",
             CRASH_DIR, svc, (long)time(NULL), pid);

    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "aether-crashd: cannot write %s (%s)\n",
                path, strerror(errno));
        return;
    }
    fprintf(f, "===== AetherOS crash report =====\n");
    fprintf(f, "service: %s\n", svc);
    fprintf(f, "pid: %s\n", pid);
    if (!strncmp(code, "sig:", 4)) {
        int sig = atoi(code + 4);
        fprintf(f, "cause: signal %d (%s)\n", sig, signal_name(sig));
    } else if (!strncmp(code, "code:", 5)) {
        fprintf(f, "cause: exit code %s\n", code + 5);
    } else {
        fprintf(f, "cause: %s (unrecognized)\n", code);
    }
    fprintf(f, "uptime_s: %s\n", up);
    fprintf(f, "restart_count: %s\n", restarts);
    fprintf(f, "wallclock: %s\n", wall);
    fprintf(f, "cmdline: %s\n", cmdline);
    tail_into(f, INIT_LOG);
    tail_into(f, AETHER_LOG);
    fprintf(f, "---- boundaries (honest) ----\n");
    fprintf(f, "- no core dump configured (core_pattern untouched; this is a log-level report)\n");
    fprintf(f, "- /proc/<pid> gone after exit; no maps/status snapshot taken\n");
    fclose(f);
    printf("aether-crashd: report %s (%s)\n", path, svc);
    fflush(stdout);
}

int main(void)
{
    signal(SIGTERM, on_sigterm);
    signal(SIGINT,  on_sigterm);
    signal(SIGPIPE, SIG_IGN);

    unlink(SOCK_PATH);
    int srv = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (srv < 0) {
        fprintf(stderr, "aether-crashd: socket failed (%s)\n", strerror(errno));
        return 1;
    }
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, SOCK_PATH, sizeof sa.sun_path - 1);
    if (bind(srv, (struct sockaddr *)&sa, sizeof sa) != 0) {
        fprintf(stderr, "aether-crashd: bind %s failed (%s)\n", SOCK_PATH, strerror(errno));
        close(srv);
        return 1;
    }
    chmod(SOCK_PATH, 0660);
    printf("aether-crashd: online at %s, reports in %s\n", SOCK_PATH, CRASH_DIR);
    fflush(stdout);

    char rec[REC_MAX];
    while (!g_stop) {
        ssize_t got = recvfrom(srv, rec, sizeof rec - 1, MSG_DONTWAIT, NULL, NULL);
        if (got < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) usleep(100000);
            continue;
        }
        rec[got] = '\0';

        /* 解析: crash <svc> <pid> <code> <restarts> <up> <cmdline...> */
        char svc[64] = "?", pid[16] = "?", code[32] = "?";
        char restarts[16] = "0", up[32] = "-1", cmdline[512] = "(unknown)";
        int moff = 0;
        int nf = sscanf(rec, "crash %63s %15s %31s %15s %31s %n",
                        svc, pid, code, restarts, up, &moff);
        if (nf >= 5 && moff > 0 && moff < (int)strlen(rec)) {
            strncpy(cmdline, rec + moff, sizeof cmdline - 1);
            cmdline[sizeof cmdline - 1] = '\0';
        } else if (nf < 5) {
            fprintf(stderr, "aether-crashd: malformed event (nf=%d): %s\n", nf, rec);
            continue;
        }
        write_report(svc, pid, code, restarts, up, cmdline);
    }

    close(srv);
    unlink(SOCK_PATH);
    printf("aether-crashd: stopped\n");
    return 0;
}
