/*
 * aether-demo.c - AetherOS sample native application.
 *
 * Proves the compositor is a real multi-process window system: this
 * program is a SEPARATE process from the compositor. It registers a
 * window via the IPC socket, shares a pixel buffer with the compositor
 * over a Unix socket using SCM_RIGHTS (the same shm mechanism Wayland's
 * wl_shm uses), paints into that buffer, and asks for a repaint. No
 * fake data, no in-process shortcut.
 *
 * The compositor is a short-lived-connection, request/response service
 * (one connection per command, like aethercompctl). Window state lives
 * in the compositor's global table and survives across a client's
 * connections, so this app reconnects per command.
 *
 * Build: aarch64-linux-gnu-gcc -static -O2 -o aether-demo aether-demo.c
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef MFD_NOEXEC_SEAL
#define MFD_NOEXEC_SEAL 0x0008U
#endif

#define SOCK_PATH "/run/aetheros-comp.sock"
#define CW 220
#define CH 140
#define ANIM_FRAMES 40   /* animate briefly, then hold the last frame */

static volatile sig_atomic_t stop_flag;

static void on_signal(int sig) { (void)sig; stop_flag = 1; }

static int connect_comp(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", SOCK_PATH);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* connect with backoff: compositor may not have bound its socket yet
 * (depends only orders startup, it does not guarantee readiness). */
static int connect_comp_retry(void)
{
    for (int i = 0; i < 40 && !stop_flag; i++) {
        int s = connect_comp();
        if (s >= 0)
            return s;
        usleep(250000);  /* 250 ms */
    }
    return -1;
}

/* Read one IPC response line (terminated by '\n' or EOF). */
static int recv_line(int fd, char *buf, size_t cap)
{
    size_t off = 0;
    while (off < cap - 1) {
        char c;
        if (read(fd, &c, 1) <= 0)
            break;
        if (c == '\n' || c == '\r')
            break;
        buf[off++] = c;
    }
    buf[off] = '\0';
    return off > 0 ? 0 : -1;
}

static int cmd_create(int *id_out)
{
    int s = connect_comp_retry();
    if (s < 0)
        return -1;
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "create demo %d %d", CW, CH);
    if (write(s, cmd, strlen(cmd)) < 0) {
        close(s);
        return -1;
    }
    char resp[256];
    if (recv_line(s, resp, sizeof(resp)) < 0) {
        close(s);
        return -1;
    }
    const char *p = strstr(resp, "\"id\":");
    int id = p ? atoi(p + 5) : -1;
    close(s);
    *id_out = id;
    return id >= 0 ? 0 : -1;
}

/* Attach memfd pixel buffer to a window id, carrying the fd over the
 * socket via SCM_RIGHTS in the SAME message as the "buf <id>" line. */
static int cmd_buf(int id, int memfd)
{
    int s = connect_comp_retry();
    if (s < 0)
        return -1;
    char line[64];
    int n = snprintf(line, sizeof(line), "buf %d\n", id);
    struct iovec iov = { .iov_base = line, .iov_len = (size_t)n };
    char ctrl[CMSG_SPACE(sizeof(int))];
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctrl;
    msg.msg_controllen = sizeof(ctrl);
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &memfd, sizeof(int));
    int rc = sendmsg(s, &msg, 0);
    char resp[256];
    recv_line(s, resp, sizeof(resp));  /* drain confirmation */
    close(s);
    return rc > 0 ? 0 : -1;
}

static int cmd_commit(int id)
{
    int s = connect_comp_retry();
    if (s < 0)
        return -1;
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "commit %d", id);
    if (write(s, cmd, strlen(cmd)) < 0) {
        close(s);
        return -1;
    }
    char resp[256];
    recv_line(s, resp, sizeof(resp));
    close(s);
    return 0;
}

int main(void)
{
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);  /* robust against transient peer close */

    int id;
    if (cmd_create(&id) < 0) {
        fprintf(stderr, "demo: create failed\n");
        return 1;
    }
    fprintf(stderr, "demo: window id=%d\n", id);

    int memfd = memfd_create("aether-demo", MFD_CLOEXEC | MFD_NOEXEC_SEAL);
    if (memfd < 0) {
        perror("demo: memfd_create");
        return 1;
    }
    if (ftruncate(memfd, (off_t)CW * CH * 4) < 0) {
        perror("demo: ftruncate");
        close(memfd);
        return 1;
    }
    uint32_t *px = mmap(NULL, (size_t)CW * CH * 4, PROT_WRITE,
                        MAP_SHARED, memfd, 0);
    if (px == MAP_FAILED) {
        perror("demo: mmap");
        close(memfd);
        return 1;
    }

    int frame = 0;
    while (!stop_flag) {
        if (frame < ANIM_FRAMES) {
            /* background */
            for (int i = 0; i < CW * CH; i++)
                px[i] = 0xFF2A2A30u;  /* XRGB: dark slate */
            /* moving accent block (bright red) — a clear, scannable
             * signature proving the client drives pixels onto the screen */
            int bx = (frame * 5) % (CW - 50);
            int by = 30 + ((frame / 12) % 3) * 30;
            for (int y = by; y < by + 40; y++)
                for (int x = bx; x < bx + 40; x++)
                    if (x >= 0 && x < CW && y >= 0 && y < CH)
                        px[(size_t)y * CW + x] = 0xFFFF3030u;  /* XRGB red */
            /* progress stripe */
            int py = CH - 6;
            for (int x = 0; x < (frame % CW); x++)
                px[(size_t)py * CW + x] = 0xFF4FC3F7u;  /* cyan */

            if (cmd_buf(id, memfd) < 0)
                break;
            cmd_commit(id);
            frame++;
            usleep(120000);  /* ~8 fps during animation */
        } else {
            /* animation done: hold the last frame so the screen is
             * deterministic (idle hash stable) until we are asked to exit */
            usleep(200000);
        }
    }

    munmap(px, (size_t)CW * CH * 4);
    close(memfd);
    fprintf(stderr, "demo: exiting after %d frames\n", frame);
    return 0;
}
