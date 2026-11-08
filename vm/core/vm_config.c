/* Aether VM - 自研虚拟机配置层实现 */
#include "vm_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>

void aether_vm_config_defaults(aether_vm_config_t *c)
{
    memset(c, 0, sizeof(*c));
    strcpy(c->name,     "Aether OS");
    strcpy(c->machine,  "virt");            /* aarch64 无默认 machine，必须显式 virt */
    strcpy(c->cpu,      "cortex-a57");
    strcpy(c->accel,    "tcg,thread=multi,tb-size=4096");
    c->smp       = 1;
    c->memory_mb = 512;
    strcpy(c->disk_if,  "virtio");
    strcpy(c->append,   "root=/dev/vda rw init=/sbin/aether-init "
                        "console=ttyAMA0 loglevel=4 rootwait aether.phonetest=1");
    strcpy(c->gpu,      "virtio-gpu-pci");
    c->width  = 720;
    c->height = 1280;
    strcpy(c->display,  "sdl");
    c->backend    = AETHER_BACKEND_EXEC;
    c->enable_qmp = 1;
}

/* 简易 key=value 解析：忽略空行与 # 注释，值可含空格 */
int aether_vm_config_load(aether_vm_config_t *c, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '#' || *p == '\0') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = p, *val = eq + 1;
        char *ke = key + strlen(key);
        while (ke > key && isspace((unsigned char)ke[-1])) *--ke = '\0';
        while (*val && isspace((unsigned char)*val)) val++;
        char *ve = val + strlen(val);
        while (ve > val && isspace((unsigned char)ve[-1])) *--ve = '\0';
#define S(field) do { if (!strcmp(key, #field)) { snprintf(c->field, sizeof(c->field), "%s", val); continue; } } while (0)
#define I(field) do { if (!strcmp(key, #field)) { c->field = atoi(val); continue; } } while (0)
        S(name); S(machine); S(cpu); S(accel);
        I(smp); I(memory_mb);
        S(kernel); S(disk); S(disk_if); S(append);
        S(gpu); I(width); I(height); S(display);
        S(bios_dir); S(serial_log); S(share_dir); S(qmp_sock); S(pidfile); S(engine);
        if (!strcmp(key, "backend"))
            c->backend = !strcmp(val, "dlopen") ? AETHER_BACKEND_DLOPEN : AETHER_BACKEND_EXEC;
        if (!strcmp(key, "enable_qmp")) c->enable_qmp = atoi(val) ? 1 : 0;
#undef S
#undef I
    }
    fclose(f);
    return 0;
}

/* 用一个线性 scratch 区拼接所有需要格式化的参数 */
typedef struct { char *base; size_t cap; size_t used; } scratch_t;

static char *sc_fmt(scratch_t *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    size_t left = s->cap - s->used;
    char *p = s->base + s->used;
    int written = vsnprintf(p, left, fmt, ap);
    va_end(ap);
    if (written < 0 || (size_t)written >= left) return NULL;
    s->used += (size_t)written + 1;
    return p;
}

int aether_vm_build_argv(const aether_vm_config_t *c,
                         char **argv, int cap,
                         char *scratch, size_t scratch_len)
{
    scratch_t s = { scratch, scratch_len, 0 };
    int n = 0;
#define PUSH(x) do { if (n >= cap - 1) return -1; argv[n++] = (char *)(x); } while (0)
#define EMIT(...) do { char *_t = sc_fmt(&s, __VA_ARGS__); if (!_t) return -1; PUSH(_t); } while (0)

    /* argv[0]：EXEC 后端是可执行名；DLOPEN 后端 QEMU 也要求首个参数为程序名 */
    PUSH("qemu-system-aarch64");
    PUSH("-M");        PUSH(c->machine);
    if (c->cpu[0] && strcmp(c->cpu, "Default")) { PUSH("-cpu"); PUSH(c->cpu); }
    /* 选项与值必须是两个独立 argv 元素（"-smp" "1"），不能拼成一个 "-smp 1" */
    PUSH("-smp"); EMIT("%d", c->smp);
    PUSH("-m");   EMIT("%d", c->memory_mb);
    if (c->accel[0]) { PUSH("-accel"); PUSH(c->accel); }

    /* 系统盘：virtio + qcow2，root=/dev/vda */
    PUSH("-drive");
    EMIT("index=0,media=disk,if=%s,format=qcow2,file=%s", c->disk_if, c->disk);

    /* 内核直引，绕过 bootloader —— 一键开机成立的前提 */
    PUSH("-kernel");   PUSH(c->kernel);
    PUSH("-append");   PUSH(c->append);

    PUSH("-device");
    EMIT("%s,xres=%d,yres=%d", c->gpu, c->width, c->height);
    PUSH("-device");   PUSH("usb-ehci,id=usbctrl");
    PUSH("-device");   PUSH("sdhci-pci,id=sdpci");
    PUSH("-net");      PUSH("none");
    PUSH("-k");        PUSH("en-us");
    PUSH("-monitor");  PUSH("none");
    PUSH("-parallel"); PUSH("none");
    PUSH("-rtc");      PUSH("base=localtime");
    PUSH("-nodefaults");
    if (c->bios_dir[0]) { PUSH("-L"); PUSH(c->bios_dir); }
    PUSH("-display");  PUSH(c->display);
    if (c->serial_log[0]) { PUSH("-serial"); EMIT("file:%s", c->serial_log); }
    if (c->share_dir[0]) {
        PUSH("-virtfs");
        EMIT("local,path=%s,mount_tag=hostshare,security_model=none,id=hs", c->share_dir);
    }
    if (c->enable_qmp && c->qmp_sock[0]) {
        PUSH("-qmp");
        EMIT("unix:%s,server=on,wait=off", c->qmp_sock);
    }
    if (c->pidfile[0]) { PUSH("-pidfile"); PUSH(c->pidfile); }

    argv[n] = NULL;
#undef EMIT
#undef PUSH
    return n;
}

void aether_vm_config_dump(const aether_vm_config_t *c, char *out, size_t n)
{
    snprintf(out, n,
        "Aether VM :: %s | arch=aarch64 | %s/%s x%d | %dMB | %dx%d | disk=%s | kernel=%s | backend=%s",
        c->name, c->machine, c->cpu[0] ? c->cpu : "default", c->smp,
        c->memory_mb, c->width, c->height,
        c->disk_if, c->kernel,
        c->backend == AETHER_BACKEND_DLOPEN ? "dlopen" : "exec");
}
