/*
 * aether-hal-android —— AetherOS 的「Android 宿主」HAL 后端（客户机侧）。
 *
 * 定位（计划书原则 2：所有 Android 能力必须经过 Backend/HAL，绝不直接
 * 暴露给 AetherOS Core）：
 *   AetherOS 在 Android 手机上以 QEMU 客户机身份运行（Termux 宿主）。宿主
 *   Android 拥有真实硬件与服务（电量、Wi-Fi、蜂窝、通知…）。宿主桥接脚本
 *   aether-android-bridge.sh 每几秒把一份 JSON 快照写到共享目录（QEMU 9p
 *   hostshare，客户机挂载于 /mnt/share）。本服务轮询解析该文件，落盘为
 *   /run/aether-hal-android.state，供 SystemUI / aether-sysuictl 等以普通
 *   文件方式消费——Core 只看到「硬件事实快照」，不知道它来自 Android。
 *
 * 诚实标记（计划书原则 7）：桥接未运行 / 文件不存在时写 backend=NONE，
 *   绝不伪造数值；接入后每条都标 REAL（来自宿主）。
 *
 * 通信通道：9p 文件落盘（零额外依赖，宿主无需 socat/串口）。
 *   宿主文件：/mnt/share/android-host.json
 *   示例：{"battery":87,"charging":1,"wifi":"WLAN","cellular":"LTE","notifications":2}
 */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>

#define STATE_PATH  "/run/aether-hal-android.state"
#define HOST_JSON   "/mnt/share/android-host.json"
#define POLL_MS     1500          /* 轮询宿主快照间隔 */
#define LINE_SZ     2048

static volatile sig_atomic_t g_stop = 0;
static void on_term(int s) { (void)s; g_stop = 1; }

/* 原子写状态快照（临时文件 + rename，读者不会看到半截）。 */
static void write_state(const char *fmt, ...)
{
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    char tmp[sizeof STATE_PATH + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", STATE_PATH);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return;
    size_t n = strlen(buf);
    size_t w = 0;
    while (w < n) {
        ssize_t r = write(fd, buf + w, n - w);
        if (r < 0) { if (errno == EINTR) continue; break; }
        w += (size_t)r;
    }
    close(fd);
    rename(tmp, STATE_PATH);
}

/* 简易 JSON 提取：找 "key"，其后取字符串或整数。 */
static int json_str(const char *src, const char *key, char *out, size_t outsz)
{
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(src, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p && *p != ':') p++;
    if (*p != ':') { out[0] = '\0'; return 0; }
    p++;
    while (*p == ' ' || *p == '"') p++;
    size_t i = 0;
    while (*p && *p != '"' && *p != ',' && *p != '}' &&
           *p != '\n' && i + 1 < outsz) {
        if (*p == '\\' && *(p + 1)) p++;
        out[i++] = *p++;
    }
    out[i] = '\0';
    return 1;
}

static long json_int(const char *src, const char *key, long def)
{
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(src, pat);
    if (!p) return def;
    p += strlen(pat);
    while (*p && *p != ':') p++;
    if (*p != ':') return def;
    return strtol(p + 1, NULL, 10);
}

/* 读整个宿主快照文件；返回长度，失败返回 -1。 */
static ssize_t read_host(char *buf, size_t bufsz)
{
    int fd = open(HOST_JSON, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, bufsz - 1);
    close(fd);
    if (n < 0)
        return -1;
    buf[n] = '\0';
    return n;
}

int main(void)
{
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    mkdir("/run", 0755);

    char buf[LINE_SZ];
    long last_bat = -2;           /* 哨兵：强制首轮写一次 */
    int last_src = -1;            /* 0=NONE 1=REAL */

    /* 首轮先写 NONE，让 SystemUI 立刻有可读快照 */
    write_state("backend=NONE\nbattery_pct=-1\nbattery_src=NONE\n"
                "charging=0\nwifi=\ncellular=\nnotifications=0\n");

    while (!g_stop) {
        ssize_t n = read_host(buf, sizeof buf);
        if (n < 0) {
            if (last_src != 0) {
                write_state("backend=NONE\nbattery_pct=-1\nbattery_src=NONE\n"
                            "charging=0\nwifi=\ncellular=\nnotifications=0\n");
                last_src = 0;
                last_bat = -2;
            }
            usleep((useconds_t)POLL_MS * 1000);
            continue;
        }

        long bat = json_int(buf, "battery", -1);
        long charging = json_int(buf, "charging", 0);
        long notif = json_int(buf, "notifications", 0);
        char wifi[128] = "", cellular[128] = "";
        json_str(buf, "wifi", wifi, sizeof wifi);
        json_str(buf, "cellular", cellular, sizeof cellular);

        if (last_src != 1 || bat != last_bat) {
            fprintf(stderr, "aether-hal-android: 宿主快照 bat=%ld%% chg=%ld "
                            "wifi='%s' cell='%s' notif=%ld\n",
                    bat, charging, wifi, cellular, notif);
            last_bat = bat;
        }
        last_src = 1;

        write_state("backend=REAL\nbattery_pct=%ld\nbattery_src=REAL\n"
                    "charging=%ld\nwifi=%s\ncellular=%s\nnotifications=%ld\n",
                    bat, charging, wifi, cellular, notif);

        usleep((useconds_t)POLL_MS * 1000);
    }

    unlink(STATE_PATH);
    return 0;
}
