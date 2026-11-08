/*
 * aether-wayland-connect-probe
 *
 * 极简 connect() 取证程序: 在调用者的真实运行环境(UID / mount namespace /
 * 文件系统视图)下, 对给定 AF_UNIX 路径执行 socket()+connect(), 并打印:
 *   - socket() 返回值与 errno
 *   - connect() 返回值与 errno
 *   - strerror(errno)
 *
 * 编译(宿主机交叉, 静态链接, 可在 Android/bionic 环境直接执行):
 *   aarch64-linux-gnu-gcc -static -O2 -o aether-wayland-connect-probe connect.c
 *
 * 注意: 本程序必须"在 Composer 的进程中"被调用(由 aether-composer-probe.sh
 * exec/call), 才能继承 Composer 的 UID、GID、Groups 与 mount namespace。
 * 单独运行只能代表运行者自身的环境。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "/run/user/1000/wayland-0";

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "CONNECT_PROBE socket() rc=%d errno=%d (%s)\n",
                fd, errno, strerror(errno));
        return 2;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t plen = strlen(path);
    if (plen >= sizeof(addr.sun_path)) {
        fprintf(stderr, "CONNECT_PROBE path_too_long len=%zu max=%zu\n",
                plen, sizeof(addr.sun_path));
        close(fd);
        return 3;
    }
    strcpy(addr.sun_path, path);

    int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    fprintf(stderr, "CONNECT_PROBE connect(\"%s\") rc=%d errno=%d (%s)\n",
            path, rc, errno, strerror(errno));

    if (rc == 0) {
        fprintf(stderr, "CONNECT_PROBE RESULT=CONNECT_OK\n");
        close(fd);
        return 0;
    }
    fprintf(stderr, "CONNECT_PROBE RESULT=FAIL\n");
    close(fd);
    return 1;
}
