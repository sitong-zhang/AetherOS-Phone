/* Aether VM - 自研虚拟机运行时
 * 一份实现，两个后端：
 *   EXEC   : fork/exec 独立 QEMU 进程（桌面/服务器宿主，本地验证用）
 *   DLOPEN : dlopen libqemu 引擎 .so，调用 qemu_* API（Android 内置用）
 * 两个后端共用 core/vm_config.c 的同一套参数组装与就绪判定。 */
#ifndef AETHER_VM_RUNTIME_H
#define AETHER_VM_RUNTIME_H

#include "vm_config.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aether_vm aether_vm_t;

/* 构造一台虚拟机（不启动）。config 会被拷贝进 handle。 */
aether_vm_t *aether_vm_create(const aether_vm_config_t *config);

/* 启动。返回 0 成功；后端错误返回负值。 */
int  aether_vm_start(aether_vm_t *vm);

/* 阻塞等待系统就绪：轮询串口日志里的就绪标记。timeout_s<=0 表示不等待。
 * 返回 0=已就绪，-1=超时/失败。 */
int  aether_vm_wait_ready(aether_vm_t *vm, int timeout_s);

/* 读取当前串口日志内容（用于状态取证）。返回写入 out 的字节数。 */
int  aether_vm_serial_snapshot(aether_vm_t *vm, char *out, size_t n);

/* VM 是否仍在运行 */
int  aether_vm_running(aether_vm_t *vm);

/* 通过 QMP 连接并发送一条命令（如 qmp_capabilities / system_powerdown / screendump）。
 * reply 可为 NULL。返回 0 成功。 */
int  aether_vm_qmp(aether_vm_t *vm, const char *json_cmd, char *reply, size_t reply_n);

/* 优雅关机：优先 QMP system_powerdown，失败则信号终止。 */
void aether_vm_stop(aether_vm_t *vm);

/* 释放句柄（不会强杀子进程，需先 stop） */
void aether_vm_destroy(aether_vm_t *vm);

/* 最后一次错误 */
const char *aether_vm_last_error(aether_vm_t *vm);

#ifdef __cplusplus
}
#endif
#endif /* AETHER_VM_RUNTIME_H */
