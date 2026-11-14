/*
 * aether_intent.c - 攻击意图判定层实现（纯函数，可机检、可复现）
 *
 * 判定顺序即安全策略：先查「确认攻击意图」特征，再查维修白名单，再查可疑行为，
 * 最后兜底为无害。信息不足一律落到最低等级，绝不因单一 USB 枚举升到 L3。
 */
#include "aether_intent.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define RULE_L3_KEY     "R2-L3-key-tamper"
#define RULE_L3_EXPORT  "R1-L3-core-export"
#define RULE_L3_SHORT   "R3-L3-short-to-core"
#define RULE_L3_USB     "R4-L3-usb-intrusion"
#define RULE_SERVICE    "R5-service-whitelist"
#define RULE_L2_RECON   "R6-L2-core-recon"
#define RULE_L2_ADB     "R7-L2-unauth-debug"
#define RULE_L2_AUTH    "R8-L2-auth-failure"
#define RULE_L2_MASS    "R9-L2-mass-enum"
#define RULE_L1_SHORT   "RA-L1-service-short"
#define RULE_L1_CHASSIS "RB-L1-chassis-open"
#define RULE_L1_CHARGE  "RC-L1-charge-or-normal"
#define RULE_L1_DEFAULT "RD-L1-default-failsafe"

void ae_observation_reset(ae_observation_t *o)
{
    if (!o) return;
    memset(o, 0, sizeof(*o));
}

const char *ae_intent_usb_name(ae_usb_kind_t k)
{
    static const char *n[] = {
        "none", "charge", "hid", "mtp", "mass", "adb", "fastboot", "custom"
    };
    if ((int)k < 0 || (size_t)k >= sizeof(n) / sizeof(n[0])) return "unknown";
    return n[k];
}
const char *ae_intent_verb_name(ae_verb_t v)
{
    static const char *n[] = {
        "none", "auth-ok", "auth-fail", "enum-query", "read-public", "read-core",
        "export-core", "write-boot", "key-access", "brute", "flash-dump"
    };
    if ((int)v < 0 || (size_t)v >= sizeof(n) / sizeof(n[0])) return "unknown";
    return n[v];
}
const char *ae_intent_region_name(ae_region_t r)
{
    static const char *n[] = {
        "none", "public", "user-data", "sys-core", "key"
    };
    if ((int)r < 0 || (size_t)r >= sizeof(n) / sizeof(n[0])) return "unknown";
    return n[r];
}

static int core_region(ae_region_t r)
{
    return r == AE_REGION_USER_DATA || r == AE_REGION_SYS_CORE || r == AE_REGION_KEY;
}
static int attack_verb(ae_verb_t v)
{
    return v == AE_VERB_EXPORT_CORE || v == AE_VERB_FLASH_DUMP ||
           v == AE_VERB_WRITE_BOOT  || v == AE_VERB_KEY_ACCESS  ||
           v == AE_VERB_BRUTE;
}
static int debug_usb(ae_usb_kind_t u)
{
    return u == AE_USB_ADB || u == AE_USB_FASTBOOT || u == AE_USB_CUSTOM;
}
/* 密钥区触碰谓词：动词命中，或目标区域就是密钥区且伴随导出/读操作。 */
static int o_key_access(const ae_observation_t *o)
{
    if (o->verb == AE_VERB_KEY_ACCESS) return 1;
    if (o->region == AE_REGION_KEY &&
        (o->verb == AE_VERB_EXPORT_CORE || o->verb == AE_VERB_FLASH_DUMP ||
         o->verb == AE_VERB_READ_CORE)) return 1;
    return 0;
}

static void set_verdict(ae_intent_verdict_t *out, ae_threat_t th,
                        ae_hal_event_t ev, const char *rule,
                        const ae_observation_t *o, const char *detail)
{
    out->threat = th;
    out->event  = ev;
    out->rule   = rule;
    snprintf(out->reason, sizeof(out->reason),
             "usb=%s verb=%s region=%s auth=%s short=%d :: %s",
             ae_intent_usb_name(o->usb), ae_intent_verb_name(o->verb),
             ae_intent_region_name(o->region), o->auth_ok ? "ok" : "no",
             o->testpoint_short, detail);
}

int ae_intent_classify(const ae_observation_t *obs, ae_intent_verdict_t *out)
{
    if (!obs || !out) return -1;

    /* ---- L3 确认攻击意图：必须同时具备「核心资产」与「破解/导出行为」 ---- */

    /* 密钥区触碰：直接确认攻击意图（白名单也不豁免）。 */
    if (o_key_access(obs)) {
        set_verdict(out, AE_THREAT_L3, AE_EV_KEY_TAMPER, RULE_L3_KEY, obs,
                    "密钥区被触碰，确认攻击意图");
        return 0;
    }
    /* 导出 / 全盘抓取 / 写引导 / 爆破，且目标落在核心资产区。 */
    if (attack_verb(obs->verb) && core_region(obs->region)) {
        ae_hal_event_t ev = (obs->verb == AE_VERB_WRITE_BOOT || obs->verb == AE_VERB_BRUTE)
                            ? AE_EV_USB_DATA : AE_EV_CORE_EXPORT;
        set_verdict(out, AE_THREAT_L3, ev, RULE_L3_EXPORT, obs,
                    "核心资产区出现导出/写引导/爆破行为，确认攻击意图");
        return 0;
    }
    /* 拆机短接且触及用户数据 / 系统核心区。 */
    if (obs->testpoint_short && core_region(obs->region)) {
        set_verdict(out, AE_THREAT_L3, AE_EV_SHORT_CORE, RULE_L3_SHORT, obs,
                    "拆机短接触及核心资产区，确认攻击意图");
        return 0;
    }
    /* 未鉴权的调试/引导器接口 + 导出类动词：USB 破解强插。 */
    if (!obs->auth_ok && debug_usb(obs->usb) &&
        (obs->verb == AE_VERB_EXPORT_CORE || obs->verb == AE_VERB_FLASH_DUMP ||
         obs->verb == AE_VERB_WRITE_BOOT  || obs->verb == AE_VERB_KEY_ACCESS)) {
        set_verdict(out, AE_THREAT_L3, AE_EV_USB_DATA, RULE_L3_USB, obs,
                    "未鉴权调试接口执行导出/刷写，判定为 USB 破解强插");
        return 0;
    }

    /* ---- SERVICE 官方维修白名单（签名凭证）：豁免物理开盖/短接，不豁免上面的 L3 ---- */
    if (obs->service_credential) {
        set_verdict(out, AE_THREAT_SERVICE, AE_EV_SERVICE_MODE, RULE_SERVICE, obs,
                    "有效官方维修凭证，按白名单处理");
        return 0;
    }

    /* ---- L2 可疑行为：断连取证，零删除 ---- */
    if (obs->verb == AE_VERB_READ_CORE || (obs->bulk_read_bytes >= 4096 &&
        obs->region == AE_REGION_SYS_CORE)) {
        set_verdict(out, AE_THREAT_L2, AE_EV_CORE_READ, RULE_L2_RECON, obs,
                    "核心区侦察读，可疑但未构成导出");
        return 0;
    }
    if (!obs->auth_ok && debug_usb(obs->usb)) {
        set_verdict(out, AE_THREAT_L2, AE_EV_ADB_UNAUTH, RULE_L2_ADB, obs,
                    "未授权调试/引导器接口枚举，可疑但无导出行为");
        return 0;
    }
    if (obs->verb == AE_VERB_AUTH_FAIL || obs->auth_fails > 0) {
        set_verdict(out, AE_THREAT_L2, AE_EV_AUTH_FAIL, RULE_L2_AUTH, obs,
                    "鉴权失败，取证累计、零删除");
        return 0;
    }
    if (obs->usb == AE_USB_MASS || obs->verb == AE_VERB_ENUM_QUERY ||
        obs->bulk_read_bytes >= 65536) {
        set_verdict(out, AE_THREAT_L2, AE_EV_MASS_ENUM, RULE_L2_MASS, obs,
                    "非白名单存储枚举/批量查询，可疑但未触核心资产");
        return 0;
    }

    /* ---- L1 无害：只记录取证、零删除 ---- */
    if (obs->testpoint_short) {
        set_verdict(out, AE_THREAT_L1, AE_EV_TESTPOINT_SHORT, RULE_L1_SHORT, obs,
                    "维修短接未触核心区，无害");
        return 0;
    }
    if (obs->chassis_open) {
        set_verdict(out, AE_THREAT_L1, AE_EV_CHASSIS_OPEN, RULE_L1_CHASSIS, obs,
                    "机壳开合（维修/换电池），无害");
        return 0;
    }
    if (obs->usb == AE_USB_CHARGE_ONLY || obs->usb == AE_USB_HID ||
        (obs->usb == AE_USB_MTP && obs->auth_ok)) {
        set_verdict(out, AE_THREAT_L1, AE_EV_USB_CHARGE, RULE_L1_CHARGE, obs,
                    "仅充电/常规已鉴权传输，无害");
        return 0;
    }

    /* ---- 兜底 fail-safe：信息不足一律判最低等级，绝不升 L3 ---- */
    set_verdict(out, AE_THREAT_L1, AE_EV_USB_CHARGE, RULE_L1_DEFAULT, obs,
                "观测不足以判定攻击意图，按无害处理（fail-safe）");
    return 0;
}

/* ---------------- key=value 解析（供 CLI / 验收注入结构化观测） ---------------- */
static ae_usb_kind_t parse_usb(const char *s)
{
    static const char *n[] = {"none","charge","hid","mtp","mass","adb","fastboot","custom"};
    size_t i;
    for (i = 0; i < sizeof(n)/sizeof(n[0]); i++)
        if (!strcmp(s, n[i])) return (ae_usb_kind_t)i;
    return (ae_usb_kind_t)-1;
}
static ae_verb_t parse_verb(const char *s)
{
    static const char *n[] = {"none","auth-ok","auth-fail","enum-query","read-public",
        "read-core","export-core","write-boot","key-access","brute","flash-dump"};
    size_t i;
    for (i = 0; i < sizeof(n)/sizeof(n[0]); i++)
        if (!strcmp(s, n[i])) return (ae_verb_t)i;
    return (ae_verb_t)-1;
}
static ae_region_t parse_region(const char *s)
{
    static const char *n[] = {"none","public","user-data","sys-core","key"};
    size_t i;
    for (i = 0; i < sizeof(n)/sizeof(n[0]); i++)
        if (!strcmp(s, n[i])) return (ae_region_t)i;
    return (ae_region_t)-1;
}

int ae_intent_parse_kv(const char *kv, ae_observation_t *o)
{
    if (!kv || !o) return -1;
    const char *eq = strchr(kv, '=');
    if (!eq) return -1;
    size_t klen = (size_t)(eq - kv);
    const char *v = eq + 1;
    char key[24];
    if (klen >= sizeof(key)) return -1;
    memcpy(key, kv, klen); key[klen] = '\0';

    if (!strcmp(key, "usb")) {
        ae_usb_kind_t u = parse_usb(v);
        if ((int)u < 0) return -1;
        o->usb = u; return 0;
    }
    if (!strcmp(key, "verb")) {
        ae_verb_t x = parse_verb(v);
        if ((int)x < 0) return -1;
        o->verb = x; return 0;
    }
    if (!strcmp(key, "region")) {
        ae_region_t r = parse_region(v);
        if ((int)r < 0) return -1;
        o->region = r; return 0;
    }
    if (!strcmp(key, "auth"))    { o->auth_ok = atoi(v) ? 1 : 0; return 0; }
    if (!strcmp(key, "service")) { o->service_credential = atoi(v) ? 1 : 0; return 0; }
    if (!strcmp(key, "chassis")) { o->chassis_open = atoi(v) ? 1 : 0; return 0; }
    if (!strcmp(key, "short"))   { o->testpoint_short = atoi(v) ? 1 : 0; return 0; }
    if (!strcmp(key, "fails"))   { o->auth_fails = (uint32_t)strtoul(v, NULL, 10); return 0; }
    if (!strcmp(key, "bulk"))    { o->bulk_read_bytes = (uint32_t)strtoul(v, NULL, 10); return 0; }
    return -1;
}
