/*
 * aether_audit.h - 只追加、防篡改的哈希链审计日志
 *
 * 每行：seq|ts|event|detail|prev_hash_hex|hash_hex
 * hash = SHA256(prev_hash(32B raw) || 本行 body)。创世 prev = 32 个 0。
 * 任何一行被改/删/插，后续链断裂，ae_audit_verify() 必然检出。
 * detail 只记事件类型与中性描述，绝不写口令、密钥、用户文件明文。
 */
#ifndef AETHER_AUDIT_H
#define AETHER_AUDIT_H

#include <stdint.h>
#include "aether_store.h"

typedef struct ae_audit ae_audit_t;

int          ae_audit_open(ae_store_t *s, ae_audit_t **out);
void         ae_audit_close(ae_audit_t *a);
int          ae_audit_append(ae_audit_t *a, const char *event, const char *detail);
int          ae_audit_verify(ae_audit_t *a, uint64_t *count, uint8_t head[AE_SHA256_LEN]);
uint64_t     ae_audit_seq(ae_audit_t *a);
const uint8_t *ae_audit_head(ae_audit_t *a);
int          ae_audit_dump(ae_audit_t *a, char *buf, size_t n);

#endif /* AETHER_AUDIT_H */
