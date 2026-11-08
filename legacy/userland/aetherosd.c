/*
 * aetherosd — AetherOS 系统守护进程（Linux 用户空间原生实现）
 *
 * 这是 AetherOS 服务层在 **真 Linux 内核** 上的原生落地：
 *   对应 AetherOS 侧的 AetherSystem + AetherHal(Linux 后端) + AetherBus(IPC)。
 *
 * 全部数据通过内核自身的用户态接口读取（procfs / sysfs），与我们在 QEMU 里
 * 验证过的 Linux 6.6.70 内核一致；读不到的一律标 "unknown"，绝不编造值
 * （AetherOS.md 原则 7）。
 *
 * 重要设计约束（踩过坑后定下的）：
 *   采集**只使用对常规小文件的 fopen / fgets**，不使用 statvfs() 也不递归遍历
 *   procfs 目录树 —— statvfs 在遇到 fuse / 不可达挂载时会永久阻塞，
 *   readdir("/proc") 在进程很多时开销不可控。因此这里是"最小且必然返回"的采集。
 *
 * 提供：
 *   1) 周期性采集真实内核状态，写入 /run/aetheros.state
 *   2) Unix domain socket IPC（/run/aetheros.sock）——AetherBus 在 Linux 上的形态
 *   3) -1 单次采集后立即退出（供 init 早期取证）
 *   4) -c 客户端模式，向 IPC 总线取一次状态
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <time.h>

#define SOCK_PATH  "/run/aetheros.sock"
#define STATE_PATH "/run/aetheros.state"
#define UNKNOWN    "unknown"

static volatile sig_atomic_t running = 1;
static void on_term(int sig) { (void)sig; running = 0; }

/*
 * 安装 SIGTERM/SIGINT 处理器。
 * 关键：必须用 sigaction 且**不置 SA_RESTART** —— glibc 的 signal() 是 BSD
 * 语义（带 SA_RESTART），accept()/read() 等慢速系统调用被信号打断后会自动
 * 重启，服务永远退不出（aether-init 优雅停机时会等 10s 后 SIGKILL）。
 * 这条坑由 aether-init 的监管取证暴露，2026-10-18 修复。
 */
static void install_term_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_term;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;                /* 无 SA_RESTART：慢速系统调用返回 EINTR */
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
}

/* ---------- 只针对常规小文件的读取原语（不会阻塞） ---------- */

/** 读 procfs “Key: value kB” 字段，返回 kB；读不到返回 -1。 */
static long long meminfo_kb(const char *key) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        size_t klen = strlen(key);
        if (strncmp(line, key, klen) == 0 && line[klen] == ':') {
            fclose(f);
            return strtoll(line + klen + 1, NULL, 10);
        }
    }
    fclose(f);
    return -1;
}

/** 读文件首行（去掉换行）。 */
static int first_line(const char *path, char *buf, size_t n) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    if (!fgets(buf, (int)n, f)) { fclose(f); return 0; }
    fclose(f);
    buf[strcspn(buf, "\n")] = '\0';
    return 1;
}

/** 统计文件里以某前缀开头的行数。 */
static int count_lines(const char *path, const char *prefix) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[512];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        if (!prefix || strncmp(line, prefix, strlen(prefix)) == 0) n++;
    }
    fclose(f);
    return n;
}

/** 在 /proc/mounts 里找某个挂载点，返回其 fs 类型（不调用 statvfs，避免阻塞）。 */
static int find_mount(const char *target, char *fstype, size_t n) {
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return 0;
    char dev[256], mnt[256], fs[64];
    while (fscanf(f, "%255s %255s %63s %*s %*d %*d", dev, mnt, fs) == 3) {
        if (strcmp(mnt, target) == 0) {
            snprintf(fstype, n, "%s", fs);
            fclose(f);
            return 1;
        }
    }
    fclose(f);
    return 0;
}

/* ---------- 状态采集 ---------- */

/**
 * 组装一次系统状态快照 —— 对应 AetherOS 的 DeviceSnapshot / MemoryInfo / VolumeInfo。
 * 全程只对 procfs 常规文件做 fopen + fgets，必然返回，不会因异常挂载点卡死。
 */
static void collect(char *out, size_t n) {
    char kernelver[256] = UNKNOWN, hostname[128] = UNKNOWN, osrel[128] = UNKNOWN;
    double uptime = -1;
    char rootfs[64] = UNKNOWN;

    first_line("/proc/version", kernelver, sizeof kernelver);
    first_line("/etc/hostname", hostname, sizeof hostname);
    first_line("/etc/os-release", osrel, sizeof osrel);

    FILE *u = fopen("/proc/uptime", "r");
    if (u) { if (fscanf(u, "%lf", &uptime) != 1) uptime = -1; fclose(u); }

    find_mount("/", rootfs, sizeof rootfs);

    int cpus = count_lines("/proc/cpuinfo", "processor");
    int mounts = count_lines("/proc/mounts", NULL);
    int netdevLines = count_lines("/proc/net/dev", NULL);
    int netIfaces = netdevLines > 2 ? netdevLines - 2 : 0;

    long long memtotal = meminfo_kb("MemTotal");
    long long memavail = meminfo_kb("MemAvailable");
    long long cached = meminfo_kb("Cached");

    char sharefs[64];
    int shareMounted = find_mount("/mnt/share", sharefs, sizeof sharefs);

    snprintf(out, n,
        "AetherOS state snapshot (Linux native userland)\n"
        "  kernel        : %s\n"
        "  hostname      : %s\n"
        "  os-release    : %s\n"
        "  uptime        : %.0f s\n"
        "  memory        : total %lld kB / available %lld kB / cached %lld kB\n"
        "  cpus          : %d\n"
        "  mounts        : %d\n"
        "  root volume   : / (%s)\n"
        "  net ifaces    : %d\n"
        "  virtio disk   : %s\n"
        "  9p share      : %s\n",
        kernelver, hostname, osrel, uptime,
        memtotal, memavail, cached,
        cpus, mounts, rootfs, netIfaces,
        access("/dev/vda", F_OK) == 0 ? "present (/dev/vda)" : "absent",
        shareMounted ? "mounted at /mnt/share (9p)" : "not mounted"
    );
}

/* ---------- JSON 快照（IPC 契约 / 状态文件规范格式） ---------- */

#ifndef AETHEROS_BUILD
#define AETHEROS_BUILD "dev"
#endif

/** JSON 字符串转义：只转义 " \ 与不可打印控制符，其余原样；越界即截断。 */
static void json_escape(const char *src, char *dst, size_t n) {
    size_t j = 0;
    for (size_t i = 0; src && src[i] && j + 2 < n; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') {
            if (j + 3 >= n) break;
            dst[j++] = '\\'; dst[j++] = (char)c;
        } else if (c == '\n') { dst[j++] = '\\'; dst[j++] = 'n'; }
        else if (c == '\r') { dst[j++] = '\\'; dst[j++] = 'r'; }
        else if (c == '\t') { dst[j++] = '\\'; dst[j++] = 't'; }
        else if (c < 0x20) { /* 丢弃其它控制符 */ }
        else dst[j++] = (char)c;
    }
    dst[j] = '\0';
}

/**
 * 组装一次结构化系统状态快照（AetherBus 在 Linux 上的规范载荷）。
 * 契约（schema: "aetherosd.state/1"）对所有消费方（native SystemUI / 外壳系统自检）稳定。
 * 同样只对 procfs 常规文件做 fopen+fgets，必然返回；读不到的字段保持 UNKNOWN / -1 / false。
 */
static void collect_json(char *out, size_t n) {
    char kernelver[256], hostname[128], osrel[128], rootfs[64];
    strncpy(kernelver, UNKNOWN, sizeof kernelver - 1);
    strncpy(hostname, UNKNOWN, sizeof hostname - 1);
    strncpy(osrel, UNKNOWN, sizeof osrel - 1);
    strncpy(rootfs, UNKNOWN, sizeof rootfs - 1);

    first_line("/proc/version", kernelver, sizeof kernelver);
    first_line("/etc/hostname", hostname, sizeof hostname);
    first_line("/etc/os-release", osrel, sizeof osrel);

    double uptime = -1;
    FILE *u = fopen("/proc/uptime", "r");
    if (u) { if (fscanf(u, "%lf", &uptime) != 1) uptime = -1; fclose(u); }

    find_mount("/", rootfs, sizeof rootfs);

    int cpus = count_lines("/proc/cpuinfo", "processor");
    int mounts = count_lines("/proc/mounts", NULL);
    int netdevLines = count_lines("/proc/net/dev", NULL);
    int netIfaces = netdevLines > 2 ? netdevLines - 2 : 0;

    long long memtotal = meminfo_kb("MemTotal");
    long long memavail = meminfo_kb("MemAvailable");
    long long cached = meminfo_kb("Cached");

    char sharefs[64];
    int shareMounted = find_mount("/mnt/share", sharefs, sizeof sharefs);

    char e_kernel[300], e_host[160], e_os[160], e_root[80];
    json_escape(kernelver, e_kernel, sizeof e_kernel);
    json_escape(hostname, e_host, sizeof e_host);
    json_escape(osrel, e_os, sizeof e_os);
    json_escape(rootfs, e_root, sizeof e_root);

    snprintf(out, n,
        "{\n"
        "  \"schema\": \"aetherosd.state/1\",\n"
        "  \"backend\": \"linux-native\",\n"
        "  \"collected_at\": %ld,\n"
        "  \"build\": \"%s\",\n"
        "  \"kernel\": \"%s\",\n"
        "  \"hostname\": \"%s\",\n"
        "  \"os_release\": \"%s\",\n"
        "  \"uptime_s\": %.0f,\n"
        "  \"memory\": { \"total_kb\": %lld, \"available_kb\": %lld, \"cached_kb\": %lld },\n"
        "  \"cpus\": %d,\n"
        "  \"mounts\": %d,\n"
        "  \"root_volume\": { \"mount\": \"/\", \"fstype\": \"%s\" },\n"
        "  \"net_ifaces\": %d,\n"
        "  \"virtio_disk\": %s,\n"
        "  \"share_mounted\": %s\n"
        "}\n",
        (long)time(NULL), AETHEROS_BUILD,
        e_kernel, e_host, e_os, uptime,
        memtotal, memavail, cached,
        cpus, mounts, e_root, netIfaces,
        access("/dev/vda", F_OK) == 0 ? "true" : "false",
        shareMounted ? "true" : "false"
    );
}

int main(int argc, char **argv) {
    install_term_handlers();

    /* 客户端模式：向 IPC 总线取一次真实系统状态（AetherBus 订阅） */
    if (argc > 1 && strcmp(argv[1], "-c") == 0) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un ca;
        memset(&ca, 0, sizeof ca);
        ca.sun_family = AF_UNIX;
        strncpy(ca.sun_path, SOCK_PATH, sizeof ca.sun_path - 1);
        if (fd < 0 || connect(fd, (struct sockaddr *)&ca, sizeof ca) != 0) {
            fprintf(stderr, "aetherosd -c: cannot reach %s (%s)\n", SOCK_PATH, strerror(errno));
            if (fd >= 0) close(fd);
            return 1;
        }
        char rb[4096];
        ssize_t got = read(fd, rb, sizeof rb - 1);
        if (got > 0) { rb[got] = '\0'; printf("%s", rb); }
        close(fd);
        return 0;
    }

    char state[4096];
    collect(state, sizeof state);
    char state_json[4096];
    collect_json(state_json, sizeof state_json);

    /* 单次采集模式：结果写文件后立即退出（init 早期取证用） */
    if (argc > 1 && strcmp(argv[1], "-1") == 0) {
        FILE *of = fopen(STATE_PATH, "w");
        if (of) { fputs(state_json, of); fclose(of); }
        printf("%s", state);
        fflush(stdout);
        return 0;
    }

    printf("%s", state);
    fflush(stdout);

    FILE *f = fopen(STATE_PATH, "w");
    if (f) { fputs(state_json, f); fclose(f); }

    /* AetherBus 在 Linux 上的形态：Unix domain socket 暴露状态 */
    unlink(SOCK_PATH);
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof addr.sun_path - 1);
    if (srv >= 0 && bind(srv, (struct sockaddr *)&addr, sizeof addr) == 0 && listen(srv, 4) == 0) {
        /*
         * 访问控制（2026-10-18，P0-3）：把 socket 文件设为 0660。
         * Linux UDS 的标准准入机制——无权用户在 connect() 时由内核直接
         * EACCES，不依赖应用层代码。同一进程（root）内的消费方不受影响。
         * 按对端 uid 的精细准入（SO_PEERCRED）需要 JNI，尚未实现，不冒充。
         */
        if (chmod(SOCK_PATH, 0660) == 0) {
            printf("aetherosd: IPC socket online at %s (mode 0660)\n", SOCK_PATH);
        } else {
            printf("aetherosd: IPC socket online at %s (chmod 0660 failed: %s)\n",
                   SOCK_PATH, strerror(errno));
        }
        fflush(stdout);
    } else {
        fprintf(stderr, "aetherosd: IPC socket unavailable (%s)\n", strerror(errno));
    }

    int round = 0;
    while (running) {
        int fd = accept(srv, NULL, NULL);
        if (!running) break;          /* SIGTERM 打断 accept：立即退出，不再 sleep */
        if (fd >= 0) {
            char buf[4096];
            collect_json(buf, sizeof buf);
            ssize_t ignored = write(fd, buf, strlen(buf));
            (void)ignored;
            close(fd);
        }
        sleep(2);
        if (++round % 5 == 0) {
            char now[4096];
            collect_json(now, sizeof now);
            FILE *sf = fopen(STATE_PATH, "w");
            if (sf) { fputs(now, sf); fclose(sf); }
            printf("aetherosd: tick %d - uptime %.0f s, available %lld kB\n",
                   round, 0.0, meminfo_kb("MemAvailable"));
            fflush(stdout);
        }
    }
    if (srv >= 0) close(srv);
    unlink(SOCK_PATH);
    return 0;
}
