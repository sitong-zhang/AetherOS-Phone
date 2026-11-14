/*
 * aether_store.h - 隔离存储后端（模拟整机磁盘）
 *
 * 运行根下分五区，语义严格区分，这是「抹除全部数据但系统不抹除」的地基：
 *   sys/       系统本体：只读基线，自毁引擎绝不写/删此处任何文件
 *   data/      用户全部数据：口令状态、密钥材料、加密卷、缓存 —— 自毁目标
 *   journal/   只追加防篡改审计链（系统级安全设施，擦除只追加事件不清空）
 *   receipt/   销毁证明（可离线校验）
 *   lockdown/  安全锁定专用：只放「无法倒推的验证锚」verifier 与变砖标记，
 *              绝不放密码明文；常规 wipe 只抹 data/ 不碰它，熔断才连它一并抹除。
 *
 * 所有写走「临时文件 → fsync → rename → 目录 fsync」原子路径，防断电半写。
 * name 一律拒绝含 '/' 或 ".."，杜绝目录穿越把抹除动作打偏到真实工作区。
 */
#ifndef AETHER_STORE_H
#define AETHER_STORE_H

#include <stddef.h>
#include <stdint.h>
#include "aether_crypto.h"

#define AE_ZONE_SYS      "sys"
#define AE_ZONE_DATA     "data"
#define AE_ZONE_JOURNAL  "journal"
#define AE_ZONE_RECEIPT  "receipt"
#define AE_ZONE_LOCKDOWN "lockdown"   /* 安全锁定：验证锚(非密码) + 变砖标记 */
#define AE_STORE_NAME_MAX 64

typedef struct ae_store ae_store_t;

int         ae_store_open(const char *root, ae_store_t **out);
void        ae_store_close(ae_store_t *s);
const char *ae_store_root(ae_store_t *s);
int         ae_store_path(ae_store_t *s, char *buf, size_t n,
                          const char *zone, const char *name);

int  ae_store_atomic_write(ae_store_t *s, const char *zone, const char *name,
                           const void *buf, size_t len);
int  ae_store_read(ae_store_t *s, const char *zone, const char *name,
                   uint8_t **out, size_t *len);            /* malloc，调用方 free */
int  ae_store_exists(ae_store_t *s, const char *zone, const char *name);
int  ae_store_remove(ae_store_t *s, const char *zone, const char *name);
int  ae_store_list(ae_store_t *s, const char *zone,
                   char (*names)[AE_STORE_NAME_MAX], int max, int *count);
/* 区内全部普通文件按名排序后的整体摘要，用于证明 sys/ 擦除前后逐字节未变 */
int  ae_store_zone_digest(ae_store_t *s, const char *zone, uint8_t out[AE_SHA256_LEN]);

/* 定长加密卷 */
int  ae_store_volume_create(ae_store_t *s, const char *name, size_t size);
int  ae_store_volume_size(ae_store_t *s, const char *name, size_t *size);
int  ae_store_volume_write(ae_store_t *s, const char *name,
                           const void *buf, size_t len, size_t offset);
int  ae_store_volume_read(ae_store_t *s, const char *name,
                          void *buf, size_t len, size_t offset);
/* 多轮覆写(0x00 / 0xFF / 随机 / 0x00...)每轮全卷 fsync，返回末轮卷哈希 */
int  ae_store_volume_overwrite(ae_store_t *s, const char *name, int passes,
                               uint8_t out_hash[AE_SHA256_LEN]);

#endif /* AETHER_STORE_H */
