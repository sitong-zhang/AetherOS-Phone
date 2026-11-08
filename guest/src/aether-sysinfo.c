/*
 * aether-sysinfo.c - AetherBus 客户端：查询系统守护
 * 用法：aether-sysinfo [system|services|ping]
 */
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define SOCK_PATH "/run/aetheros.sock"

static int query(const char *cmd, char *out, size_t sz)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, SOCK_PATH, sizeof a.sun_path - 1);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        close(fd);
        return -1;
    }
    if (send(fd, cmd, strlen(cmd), 0) < 0) {
        close(fd);
        return -1;
    }
    ssize_t n = recv(fd, out, (int)sz - 1, 0);
    if (n < 0)
        n = 0;
    out[n] = 0;
    close(fd);
    return 0;
}

int main(int argc, char **argv)
{
    const char *cmd = (argc > 1) ? argv[1] : "system";
    char buf[8192];
    if (query(cmd, buf, sizeof buf) < 0) {
        fprintf(stderr, "无法连接 AetherBus (%s)\n", SOCK_PATH);
        return 1;
    }
    printf("%s", buf);
    return 0;
}
