/*
 * aether_audit.h - 只追加、防篡改、设备密钥绑定的哈希链审计日志
 *
 * 每行：seq|ts|event|detail|prev_hash_hex|hash_hex
 * hash = HMAC(audit_key, prev_hash(32B raw) || 本行 body)。创世 prev = 32 个 0。
 * audit_key 由设备密钥派生（见 aether_tee.h），无设备密钥无法重算或伪造链。
 * 任何一行被改/删/插，后续链断裂或 HMAC 不匹配，ae_audit_verify() 必然检出。
 * detail 只记事件类型与中性描述，绝不写口令、密钥、用户文件明文。
 */
#ifndef AETHER_AUDIT_H
#define AETHER_AUDIT_H

#include <stdint.h>
#include "aether_store.h"
#include "aether_crypto.h"

typedef struct ae_audit ae_audit_t;

int          ae_audit_open(ae_store_t *s, const uint8_t key[AE_KEY_LEN], ae_audit_t **out);
void         ae_audit_close(ae_audit_t *a);
int          ae_audit_append(ae_audit_t *a, const char *event, const char *detail);
int          ae_audit_verify(ae_audit_t *a, uint64_t *count, uint8_t head[AE_SHA256_LEN]);
uint64_t     ae_audit_seq(ae_audit_t *a);
const uint8_t *ae_audit_head(ae_audit_t *a);
int          ae_audit_dump(ae_audit_t *a, char *buf, size_t n);

#endif /* AETHER_AUDIT_H */
