/*
 * aether_tee.c - 安全锚点实现：软件后端（当前）+ 硬件后端占位（真机启用）
 *
 * 设计立场：密钥驻留必须诚实。软件后端只是把随机设备密钥锁在本机存储，
 * 不是真正的硬件隔离，对外明确标注；真机的 Titan M / eFuse 路径在量产阶段替换，
 * 届时密钥由安全芯片生成、驻留、不可导出，本文件只提供接口与降级行为。
 */
#define _GNU_SOURCE
#include "aether_tee.h"
#include <string.h>
#include <stdlib.h>

const char *ae_tee_backend_name(ae_tee_backend_t backend)
{
    return backend == AE_TEE_HARDWARE ? "hardware(Titan-M/eFuse,真机启用)"
                                      : "software(随机设备密钥落 sys/device.key)";
}

int ae_tee_derive(const uint8_t master[AE_KEY_LEN], const char *label,
                  uint8_t out[AE_KEY_LEN])
{
    if (!master || !label || !out) return -1;
    ae_hmac_sha256(master, AE_KEY_LEN, (const uint8_t *)label, strlen(label), out);
    return 0;
}

int ae_tee_device_key(ae_store_t *s, ae_tee_backend_t backend, uint8_t key[AE_KEY_LEN])
{
    if (!s || !key) return -1;
    if (backend == AE_TEE_HARDWARE) {
        /* 真实硬件路径（Pixel Titan M / eFuse）：
         *   密钥由安全芯片生成并驻留安全区，永不落盘明文、不可导出到 sys/。
         *   此处为占位，量产阶段由硬件后端实现替换；当前软件层绝不伪造硬件密钥。
         *   如实返回 -1，让上层在该后端未就绪时明确失败而非静默降级。 */
        return -1;
    }
    /* 软件锚点：sys/device.key 不存在则随机生成并持久化；存在则加载。
     * 这是「密钥驻留」的软件近似：设备密钥绑定到本机存储，但非硬件隔离。 */
    uint8_t *b = NULL; size_t n = 0;
    if (ae_store_read(s, AE_ZONE_SYS, "device.key", &b, &n) == 0 && n == AE_KEY_LEN) {
        memcpy(key, b, AE_KEY_LEN); free(b); return 0;
    }
    free(b);
    if (ae_random(key, AE_KEY_LEN) != 0) return -1;
    int rc = ae_store_atomic_write(s, AE_ZONE_SYS, "device.key", key, AE_KEY_LEN);
    return rc == 0 ? 0 : -1;
}

int ae_tee_audit_key(ae_store_t *s, ae_tee_backend_t backend, uint8_t key[AE_KEY_LEN])
{
    uint8_t dk[AE_KEY_LEN];
    if (ae_tee_device_key(s, backend, dk) != 0) return -1;
    int rc = ae_tee_derive(dk, "aether/audit/v1", key);
    ae_zero(dk, sizeof dk);
    return rc;
}
