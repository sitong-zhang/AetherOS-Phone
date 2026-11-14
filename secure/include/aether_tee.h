/*
 * aether_tee.h - 安全锚点（TEE anchor）抽象：密钥驻留 / 防物理提取
 *
 * 这是 AetherOS「密钥驻留」的诚实架构层：
 *   - 软件后端（当前）：随机设备主密钥持久化于 sys/device.key，是硬件隔离的软件近似。
 *   - 硬件后端（目标，Pixel Titan M / eFuse）：密钥由安全芯片生成并驻留安全区，
 *     永不落盘明文、不可导出；仅量产阶段在真机上启用。
 *
 * 所有需要「绑定到这台设备」的密钥都从设备主密钥按域（label）派生，实现域隔离：
 *   审计链密钥、保险库密封密钥、完整性基线密钥 各用不同 label，互不通用。
 */
#ifndef AETHER_TEE_H
#define AETHER_TEE_H

#include <stdint.h>
#include "aether_store.h"
#include "aether_crypto.h"

typedef enum {
    AE_TEE_SOFTWARE = 0,   /* 当前：软件锚点（随机设备密钥落 sys/device.key） */
    AE_TEE_HARDWARE = 1    /* 目标：Pixel Titan M / eFuse，密钥不出安全区 */
} ae_tee_backend_t;

/* 取设备主密钥：软件后端持久化于 sys/device.key；硬件后端由 TEE 返回且不可导出。
 * 返回 0 成功，硬件后端未实现时返回 -1（如实标注，不在软件层伪造硬件密钥）。 */
int ae_tee_device_key(ae_store_t *s, ae_tee_backend_t backend, uint8_t key[AE_KEY_LEN]);

/* 从设备主密钥派生子密钥（域隔离）：label 区分用途，如审计链 / 密封 / 完整性。 */
int ae_tee_derive(const uint8_t master[AE_KEY_LEN], const char *label,
                  uint8_t out[AE_KEY_LEN]);

/* 审计链专用密钥（V7：用 HMAC 把链绑到设备，无密钥无法重算伪造）。 */
int ae_tee_audit_key(ae_store_t *s, ae_tee_backend_t backend, uint8_t key[AE_KEY_LEN]);

const char *ae_tee_backend_name(ae_tee_backend_t backend);

#endif /* AETHER_TEE_H */
