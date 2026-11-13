/*
 * aether_integrity.h - 系统区完整性基线（开机自检 / 篡改即擦）
 *
 * 基线摘要锚定在 journal/（只追加区，不在用户数据擦除范围内），
 * 并写入审计链，使攻击者无法「改系统文件后顺手改基线」而不留痕。
 */
#ifndef AETHER_INTEGRITY_H
#define AETHER_INTEGRITY_H

#include "aether_store.h"
#include "aether_audit.h"

/* 计算 sys/ 摘要、落基线、写审计。首次初始化调用一次。 */
int ae_integrity_seal(ae_store_t *s, ae_audit_t *a, uint8_t baseline[AE_SHA256_LEN]);
/* 重算 sys/ 摘要与基线比对。ok=1 完好，ok=0 检出篡改。 */
int ae_integrity_check(ae_store_t *s, int *ok,
                       uint8_t current[AE_SHA256_LEN],
                       uint8_t baseline[AE_SHA256_LEN]);

#endif /* AETHER_INTEGRITY_H */
