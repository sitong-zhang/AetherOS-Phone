/*
 * aether-logd.c - AetherOS 序列化日志服务（P2-13a）。
 *
 * 职责（单一）：
 *   1) bind Unix datagram socket /run/aether-log.sock（mode 0660），
 *      接收各服务经 aether-log.h 客户端库发来的单行记录：
 *        <ver> <uptime_s> <LEVEL> <service> <pid> <message...>
 *   2) 追加落盘 /var/log/aether.log，每行带 ISO8601 UTC 墙钟 + monotonic：
 *        2026-10-19T18:04:22Z up=123.45 INFO aetherosd[65] tick 5 ...
 *   3) 同步转发 /dev/kmsg（内核日志环，printk 级别映射）；
 *   4) 超过 LOG_MAX_BYTES 自动轮转为 aether.log.1（只保留一代）；
 *      SIGUSR1 强制轮转（测试钩子——boot-smoke 的 L 段断言用）。
 *
 * 诚实边界：
 *   - 不提供网络远程日志（无 TCP/TLS 上报）；
 *   - 轮转只保留一代（嵌入式系统，不假装 logrotate；
 *     历史归档应由 OTA 备份 /data 或宿主侧 9p hostshare 承担）；
 *   - 解析失败的消息按原文记录（不猜格式），确保任何记录不丢失。
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

#define SOCK_PATH       "/run/aether-log.sock"
#define LOG_FILE        "/var/log/aether.log"
#define LOG_FILE_ROT    "/var/log/aether.log.1"
#define LOG_MAX_BYTES   (256 * 1024)
#define REC_MAX         2048

static volatile sig_atomic_t g_rotate = 0;
static volatile sig_atomic_t g_stop = 0;

static void on_sigusr1(int sig) { (void)sig; g_rotate = 1; }
static void on_sigterm(int sig) { (void)sig; g_stop = 1; }

static int kmsg_level(const char *lvl)
{
    if (!strcmp(lvl, "ERROR") || !strcmp(lvl, "CRITICAL")) return 3;
    if (!strcmp(lvl, "WARN")) return 4;
    return 6;
}

static long file_size(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (long)st.st_size;
}

/* 轮转：aether.log -> aether.log.1（覆盖旧 .1）。 */
static void rotate_log(void)
{
    if (file_size(LOG_FILE) < 0) return;
    if (rename(LOG_FILE, LOG_FILE_ROT) != 0 && errno != ENOENT) {
        fprintf(stderr, "aether-logd: rotate failed (%s)\n", strerror(errno));
    }
}

static void iso_now(char *buf, size_t n)
{
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    if (!strftime(buf, n, "%Y-%m-%dT%H:%M:%SZ", &tm)) {
        snprintf(buf, n, "?");
    }
}

static void handle_record(const char *rec)
{
    char wall[32];
    iso_now(wall, sizeof wall);

    /* 解析: <ver> <uptime> <LEVEL> <svc> <pid> <msg...>；宽松容错，
     * 解析失败按原文记录（不猜格式、不丢记录）。 */
    char ver[8] = "1", up[32] = "-1", level[16] = "INFO";
    char svc[64] = "?", pid[16] = "?";
    int moff = 0;
    int nf = sscanf(rec, "%7s %31s %15s %63s %15s %n",
                    ver, up, level, svc, pid, &moff);
    (void)ver;   /* 版本字段：当前仅 v1，保留用于前向兼容 */
    const char *msg = (nf >= 5 && moff > 0 && moff < (int)strlen(rec))
                        ? rec + moff : rec;

    char line[REC_MAX + 64];
    int n = snprintf(line, sizeof line, "%s up=%s %s %s[%s] %s\n",
                     wall, up, level, svc, pid, msg);
    if (n <= 0) return;
    size_t len = (size_t)n > sizeof line - 1 ? sizeof line - 1 : (size_t)n;

    /* 落盘 */
    FILE *f = fopen(LOG_FILE, "a");
    if (f) {
        fwrite(line, 1, len, f);
        fclose(f);
    }

    /* 转发内核日志环 */
    int kfd = open("/dev/kmsg", O_WRONLY);
    if (kfd >= 0) {
        char kline[REC_MAX + 64];
        int kn = snprintf(kline, sizeof kline, "<%d>aetheros: %s %s[%s]: %s\n",
                          kmsg_level(level), level, svc, pid, msg);
        if (kn > 0) {
            size_t kl = (size_t)kn > sizeof kline - 1 ? sizeof kline - 1 : (size_t)kn;
            ssize_t w = write(kfd, kline, kl);
            (void)w;
        }
        close(kfd);
    }
}

int main(void)
{
    signal(SIGUSR1, on_sigusr1);
    signal(SIGTERM, on_sigterm);
    signal(SIGINT,  on_sigterm);
    signal(SIGPIPE, SIG_IGN);

    unlink(SOCK_PATH);
    int srv = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (srv < 0) {
        fprintf(stderr, "aether-logd: socket failed (%s)\n", strerror(errno));
        return 1;
    }
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, SOCK_PATH, sizeof sa.sun_path - 1);
    if (bind(srv, (struct sockaddr *)&sa, sizeof sa) != 0) {
        fprintf(stderr, "aether-logd: bind %s failed (%s)\n", SOCK_PATH, strerror(errno));
        close(srv);
        return 1;
    }
    chmod(SOCK_PATH, 0660);   /* 与 aetheros.sock 一致的内核准入 */
    printf("aether-logd: online at %s (mode 0660), file=%s max=%dKiB\n",
           SOCK_PATH, LOG_FILE, LOG_MAX_BYTES / 1024);
    fflush(stdout);

    char rec[REC_MAX];
    while (!g_stop) {
        if (g_rotate) {
            g_rotate = 0;
            rotate_log();
        }
        ssize_t got = recvfrom(srv, rec, sizeof rec - 1, MSG_DONTWAIT, NULL, NULL);
        if (got < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* 空闲：检查轮转阈值后让出 CPU */
                if (file_size(LOG_FILE) >= LOG_MAX_BYTES) rotate_log();
                usleep(100000);   /* 100ms */
            }
            continue;             /* EINTR 等：继续循环（信号已置位） */
        }
        rec[got] = '\0';
        handle_record(rec);
    }

    close(srv);
    unlink(SOCK_PATH);
    printf("aether-logd: stopped\n");
    return 0;
}
