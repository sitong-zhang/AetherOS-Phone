/* Aether VM - 自研虚拟机配置层
 * 独立工程 dev.aether.vm，无任何第三方成品的代码继承。
 * 只描述「跑一台 Aether OS 虚拟机需要什么」，不含任何 Android/Limbo 结构。 */
#ifndef AETHER_VM_CONFIG_H
#define AETHER_VM_CONFIG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AETHER_VM_ARGV_MAX   96
#define AETHER_VM_SCRATCH    16384

typedef enum {
    AETHER_BACKEND_EXEC = 0,    /* fork/exec 独立 QEMU 进程：桌面/服务器宿主 */
    AETHER_BACKEND_DLOPEN = 1   /* dlopen 引擎 .so 并调用 qemu_* API：Android */
} aether_backend_t;

typedef struct {
    char name[64];
    char machine[32];
    char cpu[32];
    char accel[128];
    int  smp;
    int  memory_mb;

    char kernel[256];           /* -kernel 直引，绕过 bootloader */
    char disk[256];             /* qcow2 系统盘 */
    char disk_if[32];           /* virtio */
    char append[512];           /* 内核命令行 */

    char gpu[32];               /* virtio-gpu-pci */
    int  width;
    int  height;
    char display[32];           /* none / sdl */

    char bios_dir[256];         /* 可选 -L，宿主固件目录 */
    char serial_log[256];       /* 串口日志（闭环状态证据） */
    char share_dir[256];        /* 9p 共享，客机帧导出 */
    char qmp_sock[256];         /* QMP 控制通道 */
    char pidfile[256];
    char engine[256];           /* libqemu-system-aarch64.so，仅 DLOPEN 后端 */

    aether_backend_t backend;
    int  enable_qmp;
} aether_vm_config_t;

/* 填一套 Aether OS arm64 手机的默认值 */
void aether_vm_config_defaults(aether_vm_config_t *c);

/* 从自研 key=value 配置文本载入；未给的键保留默认值 */
int  aether_vm_config_load(aether_vm_config_t *c, const char *path);

/* 组装 QEMU argv。argv 需容纳 AETHER_VM_ARGV_MAX，scratch 供拼接参数用。
 * 返回 argc（不含末尾 NULL 时会一并置 NULL）；失败返回 -1。 */
int  aether_vm_build_argv(const aether_vm_config_t *c,
                          char **argv, int cap,
                          char *scratch, size_t scratch_len);

/* 打印这台虚拟机的启动摘要，供日志/界面显示 */
void aether_vm_config_dump(const aether_vm_config_t *c, char *out, size_t n);

#ifdef __cplusplus
}
#endif
#endif /* AETHER_VM_CONFIG_H */
