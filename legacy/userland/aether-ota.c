/*
 * aether-ota.c - AetherOS OTA slot manager (A/B dual-slot) + recovery hook.
 *
 * Persists the *active slot* marker (A or B) on a dedicated persist
 * partition (/dev/vdc, mounted at /persist) and flips it to stage an OTA.
 * The one-time boot selector (aether-initramfs /init) reads /persist/slot
 * and switch_roots into the chosen slot's rootfs, so a slot flip plus a
 * reboot actually boots the other system image. A recovery flag on the
 * same partition makes the selector drop into recovery instead.
 *
 * Commands:
 *   aether-ota --current         print the active slot (A/B)
 *   aether-ota --switch          flip A<->B, then (if PID1 is aether-init)
 *                              ask init for a graceful reboot
 *   aether-ota --mark-recovery   write /persist/recovery then reboot
 *   aether-ota --apply           recovery action: validate active slot,
 *                              clear recovery flag (caller reboots)
 *
 * Build: aarch64-linux-gnu-gcc -static -O2 -Wall -o aether-ota aether-ota.c
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#define PERSIST_MNT   "/persist"
#define PERSIST_DEV   "/dev/vdc"
#define SLOT_A_DEV    "/dev/vda"
#define SLOT_B_DEV    "/dev/vdb"
#define SLOT_FILE     PERSIST_MNT "/slot"
#define RECOVERY_FILE PERSIST_MNT "/recovery"

static const char *slot_dev(const char *slot)
{
    return slot[0] == 'B' ? SLOT_B_DEV : SLOT_A_DEV;
}

/* Is the persist partition already mounted at /persist?
 * The initramfs /init selector mounts it before switch_root, and busybox
 * switch_root moves that mount into the new root — so in the running
 * system /dev/vdc is already mounted at /persist. Re-mounting the same
 * block device at another mount point fails with EBUSY (the kernel
 * prevents superblock races), so we must reuse the live mount. */
static int persist_already_mounted(void)
{
    FILE *f = fopen("/proc/mounts", "r");
    if (!f)
        return 0;
    char line[256];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        char dev[128], mnt[128];
        if (sscanf(line, "%127s %127s", dev, mnt) == 2
            && strcmp(dev, PERSIST_DEV) == 0 && strcmp(mnt, PERSIST_MNT) == 0) {
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

/* Mount the persist partition rw. Returns 0 on success. */
static int mount_persist(void)
{
    if (persist_already_mounted())
        return 0;
    mkdir(PERSIST_MNT, 0755);
    if (mount(PERSIST_DEV, PERSIST_MNT, "ext4", 0, NULL) == 0)
        return 0;
    if (mount(PERSIST_DEV, PERSIST_MNT, "vfat", 0, NULL) == 0)
        return 0;
    fprintf(stderr, "aether-ota: cannot mount %s (%s)\n",
            PERSIST_DEV, strerror(errno));
    return -1;
}

static int read_slot(char *out, size_t n)
{
    int fd = open(SLOT_FILE, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t r = read(fd, out, n - 1);
    close(fd);
    if (r <= 0)
        return -1;
    out[r] = '\0';
    /* keep first non-space char; default A if empty/garbage */
    for (char *p = out; *p; p++) {
        if (*p == 'A' || *p == 'B') { out[0] = *p; out[1] = '\0'; return 0; }
    }
    return -1;
}

static int write_slot(const char *slot)
{
    int fd = open(SLOT_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    if (write(fd, slot, 1) < 0 || write(fd, "\n", 1) < 0) { close(fd); return -1; }
    fsync(fd);
    close(fd);
    sync();
    return 0;
}

/* Find an existing mountpoint of dev in /proc/mounts. Returns 0 if found. */
static int find_mount_of(const char *dev, char *out, size_t n)
{
    FILE *f = fopen("/proc/mounts", "r");
    if (!f)
        return -1;
    char line[256];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        char d[128], m[128];
        if (sscanf(line, "%127s %127s", d, m) == 2
            && strcmp(d, dev) == 0) {
            snprintf(out, n, "%s", m);
            found = 1;
            break;
        }
    }
    fclose(f);
    return found ? 0 : -1;
}

/* Validate a slot rootfs: it must be mountable and contain a real init.
 * If the device is already mounted (initramfs recovery mode pre-mounts the
 * active slot rw; the kernel rejects a second mount of the same source
 * with EBUSY), reuse the live mountpoint instead of mounting again. */
static int validate_slot(const char *slot)
{
    const char *dev = slot_dev(slot);
    char mnt[128];
    int self_mounted = 0;
    if (find_mount_of(dev, mnt, sizeof(mnt)) < 0) {
        snprintf(mnt, sizeof(mnt), "%s", "/mnt/otacheck");
        mkdir(mnt, 0755);
        if (mount(dev, mnt, "ext4", MS_RDONLY, NULL) < 0) {
            fprintf(stderr, "aether-ota: slot %s (%s) failed to mount (%s)\n",
                    slot, dev, strerror(errno));
            return -1;
        }
        self_mounted = 1;
    }
    /* mnt 最长 128 + "/sbin/init" 10 字节，buffer 必须放得下，否则
     * -Wformat-truncation 报警且长挂载点路径会被静默截断 */
    char initpath[192];
    snprintf(initpath, sizeof(initpath), "%s/sbin/init", mnt);
    int ok = access(initpath, F_OK) == 0;
    if (!ok) {
        snprintf(initpath, sizeof(initpath), "%s/init", mnt);
        ok = access(initpath, F_OK) == 0;
    }
    if (self_mounted)
        umount2(mnt, MNT_DETACH);
    fprintf(stderr, "aether-ota: slot %s validation %s (via %s)\n",
            slot, ok ? "OK" : "FAILED (no init)", mnt);
    return ok ? 0 : -1;
}

/* Ask PID 1 for a graceful reboot IF it is aether-init; else report. */
static void request_reboot(void)
{
    char comm[32] = {0};
    int fd = open("/proc/1/comm", O_RDONLY);
    if (fd >= 0) {
        ssize_t r = read(fd, comm, sizeof(comm) - 1);
        close(fd);
        if (r > 0) comm[r] = '\0';
    }
    if (strncmp(comm, "aether-init", 11) == 0) {
        printf("aether-ota: signaling aether-init (PID1) for graceful reboot\n");
        kill(1, SIGUSR1);
    } else {
        printf("aether-ota: PID1 is '%s' (not aether-init) — reboot to apply\n", comm);
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: aether-ota --current|--switch|--mark-recovery|--apply\n");
        return 2;
    }

    if (mount_persist() < 0)
        return 1;

    char slot[8] = "A";
    int have = read_slot(slot, sizeof(slot)) == 0;

    if (strcmp(argv[1], "--current") == 0) {
        if (!have) { fprintf(stderr, "aether-ota: no slot marker, assuming A\n"); }
        printf("active_slot=%s\n", slot);
        return 0;
    }

    if (strcmp(argv[1], "--switch") == 0) {
        if (!have) slot[0] = 'A';
        const char target = slot[0] == 'B' ? 'A' : 'B';
        char ns[2] = {target, '\0'};
        /* validate the target BEFORE committing the flip (no brick on bad OTA) */
        if (validate_slot(ns) < 0) {
            fprintf(stderr, "aether-ota: refusing to switch to unvalidated slot %s\n", ns);
            return 1;
        }
        if (write_slot(ns) < 0) {
            fprintf(stderr, "aether-ota: cannot write slot (%s)\n", strerror(errno));
            return 1;
        }
        printf("switched_slot=%s (was %s)\n", ns, slot);
        request_reboot();
        return 0;
    }

    if (strcmp(argv[1], "--mark-recovery") == 0) {
        int fd = open(RECOVERY_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) { fprintf(stderr, "aether-ota: cannot write recovery flag\n"); return 1; }
        if (write(fd, "recovery\n", 9) < 0) {
            close(fd);
            fprintf(stderr, "aether-ota: recovery flag write failed\n");
            return 1;
        }
        fsync(fd);
        close(fd);
        sync();
        printf("recovery_marked=1\n");
        request_reboot();
        return 0;
    }

    if (strcmp(argv[1], "--apply") == 0) {
        /* recovery action: make sure the active slot is sane, then clear the
         * recovery flag so the next boot returns to normal. */
        if (!have) slot[0] = 'A';
        int v = validate_slot(slot);
        unlink(RECOVERY_FILE);
        sync();
        printf("apply: active_slot=%s validated=%d recovery_cleared=1\n",
               slot, v == 0);
        return v == 0 ? 0 : 1;
    }

    fprintf(stderr, "aether-ota: unknown command '%s'\n", argv[1]);
    return 2;
}
