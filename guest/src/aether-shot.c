/*
 * aether-shot.c - AetherOS 截图客户端
 * 用法：aether-shot <path.ppm>
 * 连接到合成器 IPC，请求把当前上屏帧导出为 PPM。
 */
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define COMP_SOCK "/run/aetheros-comp.sock"

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "用法: aether-shot <path.ppm>\n");
        return 1;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return 1;
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, COMP_SOCK, sizeof a.sun_path - 1);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        fprintf(stderr, "无法连接合成器 (%s)\n", COMP_SOCK);
        return 1;
    }
    char cmd[300];
    snprintf(cmd, sizeof cmd, "screenshot %s\n", argv[1]);
    if (send(fd, cmd, strlen(cmd), 0) < 0) {
        close(fd);
        return 1;
    }
    char buf[256];
    ssize_t n = recv(fd, buf, sizeof buf - 1, 0);
    if (n < 0)
        n = 0;
    buf[n] = 0;
    printf("%s", buf);
    close(fd);
    return 0;
}
