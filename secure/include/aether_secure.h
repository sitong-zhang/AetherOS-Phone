/*
 * aether_secure.h - AetherOS Secure Core 统一门面
 *
 * 串起：认证引擎 / 加密保险库 / 自毁引擎 / 审计链 / 完整性，
 * 形成「开机自检 → 锁屏 → 认证 → 会话 → 触发挥发式全盘擦除 → 系统完好可再用」闭环。
 *
 * 核心安全承诺（用户指定 + 自主扩展）：
 *  1. 口令错误达 max_fails 次 → 自动销毁全部用户数据（不是恢复出厂）：
 *     销毁 DEK(crypto-shred) + 多轮覆写加密卷 + 删除全部用户态残留；
 *     sys/ 系统本体逐字节不变，系统仍可开机、可再用。
 *  2. 胁迫口令(duress)：被逼解锁时输入 → 立即触发销毁。
 *  3. 防重启绕过：失败计数每次立即原子落盘，断电/重启不清零。
 *  4. 指数退避防暴力破解；会话空闲超时自动锁定。
 *  5. 全盘静态加密：DEK 随机，由口令派生 KEK 密封；解锁后 DEK 仅驻内存并清零。
 *  6. 只追加防篡改审计链 + 系统区完整性自检（篡改即擦，可配）。
 *  7. 紧急一键销毁(panic) + 可离线校验的销毁证明(receipt)。
 *
 * 抹除范围界定（重要）：抹除的是「全部用户数据」——保险库文件、DEK、
 * 密钥包装材料、口令/胁迫口令哈希、失败计数、缓存，一个不留；
 * 保留的是「系统本体(sys/) + 只追加审计链(journal/) + 销毁证明」。
 * 擦除后进入 FACTORY 出厂态，需重新 init 设置口令；系统本体逐字节未变、
 * 仍可开机自检，这正是「所有数据全部抹除，但系统不要抹除」。
 * 攻击者即便持有原口令，也因 DEK 已毁、卷已多轮覆写而无法恢复任何旧数据。
 */
#ifndef AETHER_SECURE_H
#define AETHER_SECURE_H

#include <stdint.h>
#include "aether_store.h"
#include "aether_audit.h"
#include "aether_integrity.h"

#define AE_VAULT_SIZE   (4u * 1024u * 1024u)   /* 4 MiB 定长加密卷 */
#define AE_WIPE_PASSES  4                       /* 0x00 / 0xFF / 随机 / 0x00 */
#define AE_SEC_NAME_MAX AE_STORE_NAME_MAX

typedef enum {
    AE_SEC_LOCKED = 0,
    AE_SEC_UNLOCKED = 1,
    AE_SEC_FACTORY = 2      /* 未设置口令的出厂态 */
} ae_sec_state_t;

typedef struct {
    uint32_t max_fails;        /* 输错几次触发擦除，默认 5 */
    uint32_t idle_timeout_s;   /* 会话空闲锁定秒数，0=禁用 */
    int      tamper_wipe;      /* 系统区篡改即擦，默认 1 */
    int      delay_enable;     /* 失败指数退避，默认 1；自动化测试置 0 */
} ae_sec_config_t;

typedef enum {
    AE_UNLOCK_OK = 0,       /* 解锁成功 */
    AE_UNLOCK_FAIL = 1,     /* 口令错误（计数已 +1 并落盘） */
    AE_UNLOCK_WIPED = 2,    /* 本次失败达到阈值，已触发全盘擦除 */
    AE_UNLOCK_DURESS = 3,   /* 胁迫口令，已触发全盘擦除 */
    AE_UNLOCK_TAMPER = 4,   /* 开机自检检出系统篡改（按配置可能已擦除） */
    AE_UNLOCK_ERR = 5       /* 内部错误 / 未初始化 / 已锁定等 */
} ae_unlock_t;

typedef struct ae_secure ae_secure_t;

void ae_secure_default_config(ae_sec_config_t *cfg);

/* 打开/关闭运行根。close 时强制清零内存中的 DEK 与派生密钥。 */
int  ae_secure_open(const char *root, const ae_sec_config_t *cfg, ae_secure_t **out);
void ae_secure_close(ae_secure_t *s);

/* 初始化整机：建系统基线与加密卷、设置口令与胁迫口令（duress 可 NULL） */
int  ae_secure_init(ae_secure_t *s, const char *password, const char *duress);

/* 开机自检：审计链校验 + 系统区完整性；返回 0 完好，1 检出异常 */
int  ae_secure_boot(ae_secure_t *s, char *msg, size_t n);

ae_unlock_t ae_secure_unlock(ae_secure_t *s, const char *password, char *msg, size_t n);
int  ae_secure_lock(ae_secure_t *s);
int  ae_secure_panic(ae_secure_t *s, const char *reason);

/* 会话内加密保险库（需已解锁） */
int  ae_secure_put(ae_secure_t *s, const char *name, const void *data, size_t len);
int  ae_secure_get(ae_secure_t *s, const char *name, uint8_t **out, size_t *len);
int  ae_secure_list(ae_secure_t *s, char (*names)[AE_SEC_NAME_MAX], int max, int *count);
int  ae_secure_del(ae_secure_t *s, const char *name);

/* 状态与证据 */
ae_sec_state_t ae_secure_state(ae_secure_t *s);
uint32_t       ae_secure_fails(ae_secure_t *s);
uint32_t       ae_secure_max_fails(ae_secure_t *s);
int            ae_secure_status(ae_secure_t *s, char *buf, size_t n);
int            ae_secure_audit_verify(ae_secure_t *s, uint64_t *count, uint8_t head[AE_SHA256_LEN]);
const char    *ae_secure_last_receipt(ae_secure_t *s);
ae_store_t    *ae_secure_store(ae_secure_t *s);   /* 供 UI / 证据读取，勿跨层直改 */
ae_audit_t    *ae_secure_audit(ae_secure_t *s);

/* 自毁引擎：也可独立调用（紧急路径 / 测试）。返回销毁证明路径。 */
int  ae_secure_wipe(ae_secure_t *s, const char *trigger,
                    char *receipt_path_out, size_t n);

#endif /* AETHER_SECURE_H */
