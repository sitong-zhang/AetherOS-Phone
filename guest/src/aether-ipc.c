/*
 * aether-ipc.c - AetherOS 桌面服务 IPC 命令行客户端
 *
 * 用途：系统诊断 / 自检脚本向合成器发送命令（state / click / key / text /
 * screenshot 等）。也是「系统诊断与调试工具集」的一部分。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define SOCK_PATH "/run/aetheros-comp.sock"

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "用法: aether-ipc <命令> [参数...]\n");
        return 2;
    }
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) { perror("socket"); return 1; }
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, SOCK_PATH, sizeof a.sun_path - 1);
    if (connect(s, (struct sockaddr *)&a, sizeof a) < 0) {
        perror("connect " SOCK_PATH);
        close(s);
        return 1;
    }

    char cmd[4096];
    size_t o = 0;
    for (int i = 1; i < argc && o + 2 < sizeof cmd; i++) {
        size_t len = strlen(argv[i]);
        if (o + len + 2 >= sizeof cmd) break;
        if (i > 1) cmd[o++] = ' ';
        memcpy(cmd + o, argv[i], len);
        o += len;
    }
    cmd[o++] = '\n';
    cmd[o] = 0;
    if (write(s, cmd, o) != (ssize_t)o) { perror("write"); close(s); return 1; }

    char buf[8192];
    ssize_t n, total = 0;
    while ((n = read(s, buf + total, sizeof buf - 1 - (size_t)total)) > 0) {
        total += n;
        if (total >= (ssize_t)sizeof buf - 1) break;
    }
    buf[total] = 0;
    fputs(buf, stdout);
    close(s);
    return 0;
}
