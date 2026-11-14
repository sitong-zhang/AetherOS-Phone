/*
 * aether_intent.h - 攻击意图判定层（USB / 物理观测 → 分级判决）
 *
 * 定位：物理安全事件与熔断判据之间的实现层。物理信号本身只是证据，是否构成
 *       「攻击意图」由本层依据可观测的观测记录判定，判定结果才归一化成
 *       ae_hal_event_t 交给安全锁定状态机消费。
 *
 * 数据流：
 *   检测层（真机：USB 控制器枚举、协议解析、电气/防拆触点；云端：结构化观测注入）
 *     → ae_observation_t（原始观测，不含结论）
 *     → ae_intent_classify()（纯函数，可机检、可复现、带判定理由）
 *     → ae_intent_verdict_t（判据等级 + 归一化事件 + 命中规则 + 理由）
 *     → ae_ld_on_event()（状态机按等级动作）
 *
 * 设计不变量：
 *   1. fail-safe：信息不足一律判 L1（只记录取证），绝不因「插了一根线」升 L3。
 *   2. 仅当观测中同时出现「触及核心资产」与「破解/导出/取核心数据」行为特征时，
 *      才判 L3 确认攻击意图；充电、常规枚举、维修开盖、维修短接不触核心区均不判 L3。
 *   3. 官方维修凭证（签名有效）豁免物理开盖/短接，但不豁免核心区导出/密钥触碰。
 *   4. 判定为纯函数：相同观测恒得相同判决，无隐藏状态、无累计阈值。
 *
 * 边界：本层消费的是「检测层输出的观测记录」，不承担电气检测或协议抓包本身；
 *       真机检测层由 S6 阶段接入硬件后端，云端以结构化观测注入验证判定逻辑。
 */
#ifndef AETHER_INTENT_H
#define AETHER_INTENT_H

#include <stdint.h>
#include <stddef.h>
#include "aether_phys_hal.h"

/* USB 枚举类别：描述「这根线是什么」，不含攻击结论。 */
typedef enum {
    AE_USB_NONE        = 0,
    AE_USB_CHARGE_ONLY = 1,  /* 仅 VBUS 充电，无数据枚举 */
    AE_USB_HID         = 2,  /* 充电配件 / HID 枚举 */
    AE_USB_MTP         = 3,  /* 常规文件传输（已鉴权） */
    AE_USB_MASS        = 4,  /* 大容量存储枚举 */
    AE_USB_ADB         = 5,  /* ADB 调试接口枚举 */
    AE_USB_FASTBOOT    = 6,  /* 引导器 / fastboot 接口枚举 */
    AE_USB_CUSTOM      = 7   /* 未知 VID/PID 探针 */
} ae_usb_kind_t;

/* 观测到的行为动词：描述「这根线在做什么」，是攻击意图的关键判据来源。 */
typedef enum {
    AE_VERB_NONE        = 0,
    AE_VERB_AUTH_OK     = 1,  /* 已鉴权会话 */
    AE_VERB_AUTH_FAIL   = 2,  /* 鉴权失败 */
    AE_VERB_ENUM_QUERY  = 3,  /* 枚举/查询设备信息 */
    AE_VERB_READ_PUBLIC = 4,  /* 读公开区 */
    AE_VERB_READ_CORE   = 5,  /* 读核心区（侦察） */
    AE_VERB_EXPORT_CORE = 6,  /* 导出核心区 / 用户数据 */
    AE_VERB_WRITE_BOOT  = 7,  /* 写引导区 / 解锁 bootloader */
    AE_VERB_KEY_ACCESS  = 8,  /* 触碰密钥区 */
    AE_VERB_BRUTE       = 9,  /* 反复口令 / 爆破特征 */
    AE_VERB_FLASH_DUMP  = 10  /* 全盘镜像抓取 */
} ae_verb_t;

/* 观测所触及的存储区域。 */
typedef enum {
    AE_REGION_NONE      = 0,
    AE_REGION_PUBLIC    = 1,
    AE_REGION_USER_DATA = 2,
    AE_REGION_SYS_CORE  = 3,
    AE_REGION_KEY       = 4
} ae_region_t;

/* 一条原始观测记录。字段均为检测层可直接给出的客观量。 */
typedef struct {
    ae_usb_kind_t usb;
    ae_verb_t     verb;
    ae_region_t   region;
    uint32_t      auth_fails;       /* 连续鉴权失败次数 */
    uint32_t      bulk_read_bytes;  /* 本次会话批量读取字节数 */
    int           auth_ok;          /* 当前会话是否通过鉴权 */
    int           service_credential;/* 有效官方维修凭证 */
    int           chassis_open;     /* 机壳开启 */
    int           testpoint_short;  /* 测试点短接 */
} ae_observation_t;

/* 判定输出：判据等级、归一化事件、命中规则、可审计理由。 */
typedef struct {
    ae_threat_t    threat;
    ae_hal_event_t event;
    const char    *rule;            /* 命中规则编号，静态字符串 */
    char           reason[192];     /* 人类可读判定理由 */
} ae_intent_verdict_t;

/* 观测清零为「无任何信号」的中性态。 */
void ae_observation_reset(ae_observation_t *o);

/* 核心：纯函数判定。返回 0 成功，-1 参数非法。判决写入 out。 */
int  ae_intent_classify(const ae_observation_t *obs, ae_intent_verdict_t *out);

/* 枚举名（供 CLI / 日志 / 验收输出，外部可观测）。 */
const char *ae_intent_usb_name(ae_usb_kind_t k);
const char *ae_intent_verb_name(ae_verb_t v);
const char *ae_intent_region_name(ae_region_t r);

/* 把 key=value 字符串解析进观测记录；未知键返回 -1，便于 CLI 严格报错。 */
int  ae_intent_parse_kv(const char *kv, ae_observation_t *o);

#endif /* AETHER_INTENT_H */
