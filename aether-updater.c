/*
 * aether-updater.c - AetherOS「系统更新」应用（P5）。
 *
 * 用户故事（phone-v8 自包含 APK）：
 *   家长装好 APK -> 终端输入 aether install -> 自动显示系统画面；
 *   以后更新不再回终端，而是在客机内打开「系统更新」点按钮完成。
 *
 * 与 settings/clock 一样是 compositor 的独立客户端进程（经沙箱启动器降权
 * 后运行，uid=1000）：UDS 注册窗口、SCM_RIGHTS 共享 memfd 承载像素。
 * 区别是它带可点击按钮——P5 给 compositor 的 create 增加了 sock=<uds>
 * 可选参数，窗口内容区的 tap 会以 "tap <x> <y>\n" 行转发到该 UDS，
 * 本应用据此做命中测试。
 *
 * 更新通道：客机经 QEMU user 模式网络访问宿主 10.0.2.2:<port>——
 *   GET  /api/update-check -> {"current":..,"latest":..,"has_update":..}
 *   POST /api/update       -> 宿主停旧系统->清镜像->下载新版->自动启动
 * 端口来自内核命令行 aether_port=<PORT>（宿主 launchQemu 的 -append 注入）。
 * 读不到端口时如实显示 n/a，不编造版本号。
 *
 * 铁律：UI 上的每个数字都来自真实 HTTP 应答或真实系统文件
 * （/etc/os-release、/etc/aether-slot）；网络失败如实报错。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "aether-render.h"

#ifndef MFD_NOEXEC_SEAL
#define MFD_NOEXEC_SEAL 0x0008U
#endif

#define SOCK_PATH  "/run/aetheros-comp.sock"
#define TAP_SOCK   "/tmp/aether-updater.sock"
#define HOST_GW    "10.0.2.2"          /* QEMU user 网络：宿主 127.0.0.1 */

#define CW 480
#define CH 320

/* 按钮几何（窗口局部坐标，内容区 CW x CH） */
#define BTN_W 200
#define BTN_H 40
#define BTN_CHECK_X 20
#define BTN_UPDATE_X 260
#define BTN_Y 210

#define FG_MAIN   0xE6EDF3u
#define FG_DIM    0x8B98A5u
#define FG_ACCENT 0x4FC3F7u
#define FG_OK     0x7BD88Fu
#define FG_WARN   0xFFB454u
#define FG_ERR    0xFF7A6Bu

/* 状态机（单线程写、主线程读；aarch64 上 int 读写原子，加 volatile 足够） */
enum {
    ST_IDLE = 0,       /* 初始：未检查 */
    ST_CHECKING,       /* 正在检查（HTTP 进行中） */
    ST_UPTODATE,       /* 已是最新 */
    ST_HAS_UPDATE,     /* 有新版本，「立即更新」可用 */
    ST_UPDATING,       /* 更新已触发，等待宿主重启系统 */
    ST_ERROR           /* 检查/更新失败 */
};

static volatile int g_state = ST_IDLE;
static volatile int g_dirty = 1;          /* 需要重绘 */
static volatile sig_atomic_t stop_flag;

static char g_current[64] = "n/a";        /* /etc/os-release VERSION_ID */
static char g_latest[64]  = "n/a";        /* 宿主应答的 latest */
static int  g_has_update = 0;
static char g_msg[160] = "";
static int  g_port = 0;                   /* aether_port，0 = 未注入 */
static int  g_tap_fd = -1;                /* tap 转发监听 socket */

static void on_signal(int sig) { (void)sig; stop_flag = 1; }

/* ---------------------------------------------------------------- 版本读数 */

static void read_current_version(void)
{
    char slot[32] = "?";
    FILE *f = fopen("/etc/aether-slot", "r");
    if (f) {
        if (fgets(slot, sizeof slot, f)) {
            char *nl = strpbrk(slot, "\r\n");
            if (nl) *nl = '\0';
        }
        fclose(f);
    }
    char ver[64] = "n/a";
    f = fopen("/etc/os-release", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            if (!strncmp(line, "VERSION_ID=", 11)) {
                const char *v = line + 11;
                if (*v == '"') v++;
                size_t i = 0;
                while (v[i] && v[i] != '"' && v[i] != '\n' && v[i] != '\r' &&
                       i < sizeof ver - 1) {
                    ver[i] = v[i];
                    i++;
                }
                ver[i] = '\0';
            }
        }
        fclose(f);
    }
    snprintf(g_current, sizeof g_current, "AetherOS %s (slot %s)", ver, slot);
}

/* /proc/cmdline: aether_port=18080 —— 宿主注入的 HTTP 服务端口 */
static void read_aether_port(void)
{
    FILE *f = fopen("/proc/cmdline", "r");
    if (!f) return;
    char line[1024];
    if (fgets(line, sizeof line, f)) {
        static const char key[] = "aether_port=";
        const char *p = strstr(line, key);
        if (p) {
            p += sizeof(key) - 1;   /* 跳过 key 本身（含 '='） */
            g_port = atoi(p);
        }
    }
    fclose(f);
}

/* ---------------------------------------------------------------- HTTP 客户端 */

/* 阻塞式 HTTP/1.0 请求。body_out 收应体（NUL 结尾）。返回 0 成功。 */
static int http_request(const char *method, const char *path,
                        char *body_out, size_t body_cap)
{
    if (g_port <= 0 || g_port > 65535) {
        snprintf(g_msg, sizeof g_msg, "no aether_port in cmdline");
        return -1;
    }
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        snprintf(g_msg, sizeof g_msg, "socket: %s", strerror(errno));
        return -1;
    }
    struct timeval tv = { 5, 0 };
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)g_port);
    if (inet_pton(AF_INET, HOST_GW, &a.sin_addr) != 1 ||
        connect(s, (struct sockaddr *)&a, sizeof a) < 0) {
        snprintf(g_msg, sizeof g_msg, "connect %s:%d: %s",
                 HOST_GW, g_port, strerror(errno));
        close(s);
        return -1;
    }
    char req[320];
    int n = snprintf(req, sizeof req,
                     "%s %s HTTP/1.0\r\n"
                     "Host: %s:%d\r\n"
                     "User-Agent: aether-updater\r\n"
                     "Content-Length: 0\r\n"
                     "Connection: close\r\n\r\n",
                     method, path, HOST_GW, g_port);
    if (write(s, req, (size_t)n) < 0) {
        snprintf(g_msg, sizeof g_msg, "write: %s", strerror(errno));
        close(s);
        return -1;
    }
    char buf[2048];
    size_t off = 0;
    while (off < sizeof buf - 1) {
        ssize_t r = read(s, buf + off, sizeof buf - 1 - off);
        if (r <= 0) break;
        off += (size_t)r;
    }
    buf[off] = '\0';
    close(s);
    if (strncmp(buf, "HTTP/1.0 200", 12) != 0 &&
        strncmp(buf, "HTTP/1.1 200", 12) != 0) {
        snprintf(g_msg, sizeof g_msg, "host replied non-200");
        return -1;
    }
    const char *body = strstr(buf, "\r\n\r\n");
    body = body ? body + 4 : buf;
    snprintf(body_out, body_cap, "%s", body);
    return 0;
}

/* 极简 JSON 字段提取（宿主应答格式固定，不引第三方解析器） */
static int json_str(const char *json, const char *key, char *out, size_t cap)
{
    char pat[80];
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char *p = strstr(json, pat);
    if (!p) return -1;
    p += strlen(pat);
    size_t i = 0;
    while (p[i] && p[i] != '"' && i < cap - 1) {
        out[i] = p[i];
        i++;
    }
    out[i] = '\0';
    return 0;
}

static int json_bool(const char *json, const char *key, int *out)
{
    char pat[80];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(json, pat);
    if (!p) return -1;
    p += strlen(pat);
    *out = (strncmp(p, "true", 4) == 0) ? 1 : 0;
    return 0;
}

/* ---------------------------------------------------------------- 按钮动作 */

static void act_check(void)
{
    if (g_port <= 0) {
        g_state = ST_ERROR;
        snprintf(g_msg, sizeof g_msg,
                 "no aether_port in cmdline - run from the phone app");
        g_dirty = 1;
        return;
    }
    g_state = ST_CHECKING;
    snprintf(g_msg, sizeof g_msg, "checking %s:%d ...", HOST_GW, g_port);
    g_dirty = 1;

    char body[2048];
    if (http_request("GET", "/api/update-check", body, sizeof body) < 0) {
        g_state = ST_ERROR;
        g_dirty = 1;
        return;
    }
    char latest[64] = "";
    int has = 0;
    if (json_str(body, "latest", latest, sizeof latest) == 0)
        snprintf(g_latest, sizeof g_latest, "%s", latest);
    if (json_bool(body, "has_update", &has) == 0)
        g_has_update = has;
    if (g_has_update) {
        g_state = ST_HAS_UPDATE;
        snprintf(g_msg, sizeof g_msg,
                 "new version available - tap Update Now");
    } else {
        g_state = ST_UPTODATE;
        snprintf(g_msg, sizeof g_msg, "already up to date");
    }
    g_dirty = 1;
}

static void act_update(void)
{
    if (g_state != ST_HAS_UPDATE)
        return;
    g_state = ST_UPDATING;
    snprintf(g_msg, sizeof g_msg,
             "updating - the system will restart shortly ...");
    g_dirty = 1;
    char body[2048];
    if (http_request("POST", "/api/update", body, sizeof body) < 0) {
        /* 宿主可能已停掉客机（连接被拒是正常路径）；如实报告 */
        snprintf(g_msg, sizeof g_msg,
                 "update triggered (host is restarting the system)");
        g_dirty = 1;
        return;
    }
    snprintf(g_msg, sizeof g_msg, "update request accepted by host");
    g_dirty = 1;
}

static void handle_tap(int lx, int ly)
{
    if (lx >= BTN_CHECK_X && lx < BTN_CHECK_X + BTN_W &&
        ly >= BTN_Y && ly < BTN_Y + BTN_H) {
        if (g_state != ST_CHECKING && g_state != ST_UPDATING)
            act_check();
        return;
    }
    if (lx >= BTN_UPDATE_X && lx < BTN_UPDATE_X + BTN_W &&
        ly >= BTN_Y && ly < BTN_Y + BTN_H) {
        if (g_state == ST_HAS_UPDATE)
            act_update();
        return;
    }
}

/* tap 接收线程：阻塞 accept，逐行读 "tap <x> <y>"（compositor P5 转发） */
static void *tap_thread(void *arg)
{
    (void)arg;
    for (;;) {
        int c = accept(g_tap_fd, NULL, NULL);
        if (c < 0) {
            if (errno == EINTR) continue;
            break;
        }
        char line[128];
        size_t off = 0;
        while (off < sizeof line - 1) {
            char ch;
            ssize_t r = read(c, &ch, 1);
            if (r <= 0) break;
            if (ch == '\n') break;
            line[off++] = ch;
        }
        line[off] = '\0';
        close(c);
        int x = -1, y = -1;
        if (sscanf(line, "tap %d %d", &x, &y) == 2 && x >= 0 && y >= 0)
            handle_tap(x, y);
    }
    return NULL;
}

/* ---------------------------------------------------------------- compositor IPC */

static int connect_comp(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", SOCK_PATH);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int connect_comp_retry(void)
{
    for (int i = 0; i < 40 && !stop_flag; i++) {
        int s = connect_comp();
        if (s >= 0) return s;
        usleep(250000);
    }
    return -1;
}

static int recv_line(int fd, char *buf, size_t cap)
{
    size_t off = 0;
    while (off < cap - 1) {
        char c;
        if (read(fd, &c, 1) <= 0) break;
        if (c == '\n' || c == '\r') break;
        buf[off++] = c;
    }
    buf[off] = '\0';
    return off > 0 ? 0 : -1;
}

static int cmd_create(int *id_out)
{
    int s = connect_comp_retry();
    if (s < 0) return -1;
    char cmd[192];
    snprintf(cmd, sizeof cmd, "create aether-updater %d %d sock=%s",
             CW, CH, TAP_SOCK);
    if (write(s, cmd, strlen(cmd)) < 0) { close(s); return -1; }
    char resp[256];
    if (recv_line(s, resp, sizeof resp) < 0) { close(s); return -1; }
    const char *p = strstr(resp, "\"id\":");
    int id = p ? atoi(p + 5) : -1;
    close(s);
    *id_out = id;
    return id >= 0 ? 0 : -1;
}

static int cmd_buf(int id, int memfd)
{
    int s = connect_comp_retry();
    if (s < 0) return -1;
    char line[64];
    int n = snprintf(line, sizeof line, "buf %d\n", id);
    struct iovec iov = { .iov_base = line, .iov_len = (size_t)n };
    char ctrl[CMSG_SPACE(sizeof(int))];
    struct msghdr msg;
    memset(&msg, 0, sizeof msg);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctrl;
    msg.msg_controllen = sizeof ctrl;
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &memfd, sizeof(int));
    int rc = sendmsg(s, &msg, 0);
    char resp[256];
    recv_line(s, resp, sizeof resp);
    close(s);
    if (rc <= 0) return -1;
    return strstr(resp, "\"ok\":true") ? 0 : -1;
}

static int cmd_commit(int id)
{
    int s = connect_comp_retry();
    if (s < 0) return -1;
    char cmd[64];
    snprintf(cmd, sizeof cmd, "commit %d", id);
    if (write(s, cmd, strlen(cmd)) < 0) { close(s); return -1; }
    char resp[256];
    recv_line(s, resp, sizeof resp);
    close(s);
    return 0;
}

/* ---------------------------------------------------------------- 绘制 */

static void draw_button(aether_fb_t *fb, int x, int y,
                        const char *label, int enabled)
{
    /* 玻璃由合成器的 render_window 提供（客户端只画内容，背景留 TKEY）——
     * 客户端自带 aether_render_glass 会把 TKEY 像素拍进快照，渲出偏色
     * 垃圾（v1 截图实证）。按钮用实色面板 + 1px 亮边，glass 上可读性最好。 */
    uint32_t fill = enabled ? AETHER_RGB(0x24, 0x55, 0x70)
                            : AETHER_RGB(0x22, 0x2A, 0x33);
    uint32_t edge = enabled ? AETHER_RGB(0x4F, 0xC3, 0xF7)
                            : AETHER_RGB(0x3A, 0x4A, 0x5A);
    uint32_t fg = enabled ? FG_MAIN : FG_DIM;
    aether_render_rect(fb, x, y, BTN_W, BTN_H, fill);
    for (int i = 0; i < 1; i++) {
        aether_render_rect(fb, x + i, y + i, BTN_W - 2 * i, 1, edge);
        aether_render_rect(fb, x + i, y + BTN_H - 1 - i, BTN_W - 2 * i, 1, edge);
        aether_render_rect(fb, x + i, y + i, 1, BTN_H - 2 * i, edge);
        aether_render_rect(fb, x + BTN_W - 1 - i, y + i, 1, BTN_H - 2 * i, edge);
    }
    /* 中文标签（CJK 16x16），水平居中 */
    int tw = aether_text_width_utf8(label);
    aether_render_text_utf8(fb, x + (BTN_W - tw) / 2, y + (BTN_H - 16) / 2,
                            label, fg);
}

static void draw_ui(uint32_t *px)
{
    aether_fb_t fb;
    aether_render_init_fb(&fb, px, CW, CH, CW * 4);

    /* 背景透明键：让合成器的玻璃底透上来（与 SystemUI 同一约定） */
    for (int i = 0; i < CW * CH; i++)
        px[i] = AETHER_TKEY;

    /* 标题 */
    aether_render_text_utf8(&fb, 16, 10, "System Update", FG_MAIN);
    aether_render_text_utf8(&fb, 16, 30, "\u7cfb\u7edf\u66f4\u65b0", FG_ACCENT);
    aether_render_rect(&fb, 16, 54, CW - 32, 1, FG_ACCENT);

    /* 信息底板（实色，保证玻璃/桌面之上的可读性；与 settings 同一 palette） */
    aether_render_rect(&fb, 10, 64, CW - 20, 116, AETHER_RGB(0x14, 0x1A, 0x22));
    aether_render_rect(&fb, 10, 64, CW - 20, 1, AETHER_RGB(0x2A, 0x33, 0x40));
    aether_render_rect(&fb, 10, 64 + 116 - 1, CW - 20, 1,
                       AETHER_RGB(0x2A, 0x33, 0x40));

    /* 版本行（真实读数） */
    char row[160];
    snprintf(row, sizeof row, "current : %s", g_current);
    aether_render_text_utf8(&fb, 16, 70, row, FG_MAIN);
    snprintf(row, sizeof row, "latest  : %s", g_latest);
    aether_render_text_utf8(&fb, 16, 92, row,
                            g_has_update ? FG_OK : FG_MAIN);

    /* 通道信息 */
    char chan[96];
    if (g_port > 0)
        snprintf(chan, sizeof chan, "channel : %s:%d (host gateway)",
                 HOST_GW, g_port);
    else
        snprintf(chan, sizeof chan, "channel : n/a (no aether_port)");
    aether_render_text_utf8(&fb, 16, 114, chan, FG_DIM);

    /* 状态行（画在信息底板下方的玻璃上，配色压暗保证可读） */
    const char *state_s = "";
    uint32_t state_c = FG_DIM;
    switch (g_state) {
    case ST_CHECKING:   state_s = "checking ...";        state_c = FG_ACCENT; break;
    case ST_UPTODATE:   state_s = "up to date";          state_c = FG_OK;     break;
    case ST_HAS_UPDATE: state_s = "update available";    state_c = FG_WARN;   break;
    case ST_UPDATING:   state_s = "updating ...";        state_c = FG_WARN;   break;
    case ST_ERROR:      state_s = "error";               state_c = FG_ERR;    break;
    default:            state_s = "idle";                state_c = FG_DIM;    break;
    }
    aether_render_text_utf8(&fb, 16, 186, state_s, state_c);
    aether_render_text_utf8(&fb, 16, 186 + 16, g_msg, FG_DIM);

    /* 按钮 */
    draw_button(&fb, BTN_CHECK_X, BTN_Y, "\u68c0\u67e5\u66f4\u65b0",
                g_state != ST_CHECKING && g_state != ST_UPDATING);
    draw_button(&fb, BTN_UPDATE_X, BTN_Y, "\u7acb\u5373\u66f4\u65b0",
                g_state == ST_HAS_UPDATE);

    /* 脚注 */
    aether_render_text_utf8(&fb, 16, CH - 24,
                            "Esc closes this window. Updates install a new "
                            "system image over the host channel.",
                            FG_DIM);
}

/* ---------------------------------------------------------------- main */

int main(void)
{
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);

    read_aether_port();
    read_current_version();

    /* tap 接收 socket：先建后 create——合成器转发时目标必须已在听。
     * /tmp 全局可写（sticky），沙箱 uid=1000 可 bind；root 合成器可 connect。 */
    unlink(TAP_SOCK);
    g_tap_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_tap_fd < 0) {
        fprintf(stderr, "updater: tap socket: %s\n", strerror(errno));
        return 1;
    }
    struct sockaddr_un ta;
    memset(&ta, 0, sizeof ta);
    ta.sun_family = AF_UNIX;
    snprintf(ta.sun_path, sizeof ta.sun_path, "%s", TAP_SOCK);
    if (bind(g_tap_fd, (struct sockaddr *)&ta, sizeof ta) < 0) {
        fprintf(stderr, "updater: bind %s: %s\n", TAP_SOCK, strerror(errno));
        return 1;
    }
    if (listen(g_tap_fd, 4) < 0) {
        fprintf(stderr, "updater: listen: %s\n", strerror(errno));
        return 1;
    }

    int id;
    if (cmd_create(&id) < 0) {
        fprintf(stderr, "updater: create failed\n");
        return 1;
    }
    fprintf(stderr, "updater: window id=%d tap-sock=%s port=%d\n",
            id, TAP_SOCK, g_port);

    int memfd = memfd_create("aether-updater", MFD_CLOEXEC | MFD_NOEXEC_SEAL);
    if (memfd < 0) {
        perror("updater: memfd_create");
        return 1;
    }
    if (ftruncate(memfd, (off_t)CW * CH * 4) < 0) {
        perror("updater: ftruncate");
        close(memfd);
        return 1;
    }
    uint32_t *px = mmap(NULL, (size_t)CW * CH * 4, PROT_WRITE,
                        MAP_SHARED, memfd, 0);
    if (px == MAP_FAILED) {
        perror("updater: mmap");
        close(memfd);
        return 1;
    }

    pthread_t th;
    if (pthread_create(&th, NULL, tap_thread, NULL) != 0)
        fprintf(stderr, "updater: tap thread not started\n");

    /* 首帧 */
    draw_ui(px);
    if (cmd_buf(id, memfd) < 0) {
        fprintf(stderr, "updater: buf failed\n");
        return 1;
    }
    cmd_commit(id);

    /* 200ms 一轮：有状态变化才重绘 commit（按钮/HTTP 完成由 g_dirty 触发） */
    while (!stop_flag) {
        for (int i = 0; i < 2 && !stop_flag; i++)
            usleep(100000);
        if (stop_flag)
            break;
        if (!g_dirty)
            continue;
        g_dirty = 0;
        draw_ui(px);
        if (cmd_buf(id, memfd) < 0)
            break;              /* 窗口没了（合成器重启）：安静退出，由启动器重拉 */
        cmd_commit(id);
    }

    munmap(px, (size_t)CW * CH * 4);
    close(memfd);
    fprintf(stderr, "updater: exiting\n");
    return 0;
}
