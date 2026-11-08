/*
 * aether-sysuictl.c - aether-systemui 的命令行客户端（P3-14）。
 *
 *   aether-sysuictl -c status          SystemUI 真实状态（JSON）
 *   aether-sysuictl -c "launch 2"      启动第 2 个应用（1-based）
 *   aether-sysuictl -c "launch <id>"   按应用 id 启动
 *   aether-sysuictl -c "refresh off"   冻结定时重绘（确定化测试用）
 *   aether-sysuictl -c "refresh on"    恢复定时重绘
 *
 * 退出码：0 = 收到响应；1 = 连不上 SystemUI（如实报错，不编造状态）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define SYSUI_SOCK "/run/aetheros-sysui.sock"

int main(int argc, char **argv)
{
    if (argc < 3 || strcmp(argv[1], "-c") != 0) {
        fprintf(stderr,
                "usage: %s -c <status|launch N|launch <id>|refresh off|on>\n", argv[0]);
        return 2;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "aether-sysuictl: socket failed\n");
        return 1;
    }
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", SYSUI_SOCK);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        fprintf(stderr, "aether-sysuictl: cannot reach %s\n", SYSUI_SOCK);
        close(fd);
        return 1;
    }
    if (write(fd, argv[2], strlen(argv[2])) < 0) {
        fprintf(stderr, "aether-sysuictl: write failed\n");
        close(fd);
        return 1;
    }
    char buf[8192];
    ssize_t n, total = 0;
    while ((n = read(fd, buf + total, sizeof buf - 1 - (size_t)total)) > 0) {
        total += n;
        if (total > 0 && buf[total - 1] == '\n') break;
    }
    buf[total > 0 ? total : 0] = '\0';
    fputs(buf, stdout);
    close(fd);
    return total > 0 ? 0 : 1;
}
