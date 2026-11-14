/*
 * aether_hal.c - 物理安全接口层实现（云端仿真后端）
 *
 * 只实现 AE_HAL_SIM：物理事件由 ae_hal_inject() 故障注入，时钟可注入以便
 * 确定性测试。AE_HAL_HW（真机 GPIO / 安全芯片 / eFuse）为 S6 目标，此处
 * 仅保留后端枚举与占位，open 时如实返回「未实现」，绝不伪造硬件能力。
 *
 * 「烧毁硬件」在云端是**仿真熔断指令锁存**（ae_hal_hw_fuse_burn），不是真的
 * 烧 eFuse；ae_hal_hw_fuse_backend() 明确返回 "simulated(cloud-no-physical-fuse)"。
 */
#define _GNU_SOURCE
#include "aether_phys_hal.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#define EVQ_CAP 32

struct ae_hal {
    ae_hal_backend_t backend;
    ae_hal_event_t   evq[EVQ_CAP];
    int              head, tail, count;
    uint64_t         now_ms;
    uint32_t         alarm_count;
    char             alarm_last[128];
    uint32_t         evidence_count;      /* L1/L2 事件记录数（仅取证，不参与判定） */
    char             evidence_last[64];
    int              service_mode;        /* 官方维修白名单 */
    int              hw_fuse_burned;      /* 硬件熔断指令锁存（仿真） */
    char             hw_fuse_reason[128];
};

static uint64_t real_monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

int ae_hal_open(ae_hal_backend_t backend, ae_hal_t **out)
{
    if (!out) return -1;
    *out = NULL;
    if (backend == AE_HAL_HW) return -1;   /* 真机后端未实现，如实标注 */
    if (backend != AE_HAL_SIM) return -1;
    ae_hal_t *h = (ae_hal_t*)calloc(1, sizeof *h);
    if (!h) return -1;
    h->backend = AE_HAL_SIM;
    h->now_ms = real_monotonic_ms();       /* 真实运行用真时钟；测试可注入覆盖 */
    h->alarm_last[0] = 0;
    h->evidence_last[0] = 0;
    h->hw_fuse_reason[0] = 0;
    *out = h;
    return 0;
}

void ae_hal_close(ae_hal_t *h)
{
    if (!h) return;
    memset(h, 0, sizeof *h);
    free(h);
}

const char *ae_hal_backend_name(ae_hal_t *h)
{
    if (!h) return "none";
    return h->backend == AE_HAL_SIM ? "simulated(cloud-inject)" : "hardware(S6-placeholder)";
}

/* --------------------------------------------------- 事件 → 判据等级（纯函数） */
ae_threat_t ae_hal_event_threat(ae_hal_event_t ev)
{
    switch (ev) {
        /* L1 无害：只作证据，零删除 */
        case AE_EV_USB_CHARGE:
        case AE_EV_CHASSIS_OPEN:
        case AE_EV_TESTPOINT_SHORT:
            return AE_THREAT_L1;
        /* 官方维修白名单 */
        case AE_EV_SERVICE_MODE:
            return AE_THREAT_SERVICE;
        /* L2 可疑：记录取证，零删除 */
        case AE_EV_ADB_UNAUTH:
        case AE_EV_AUTH_FAIL:
        case AE_EV_MASS_ENUM:
        case AE_EV_CORE_READ:
            return AE_THREAT_L2;
        /* L3 确认攻击意图：软件熔断 + 硬件熔断 */
        case AE_EV_USB_DATA:
        case AE_EV_CORE_EXPORT:
        case AE_EV_KEY_TAMPER:
        case AE_EV_SHORT_CORE:
            return AE_THREAT_L3;
        /* 组合键 / NONE：中性 */
        default:
            return AE_THREAT_NONE;
    }
}

const char *ae_hal_threat_name(ae_threat_t t)
{
    switch (t) {
        case AE_THREAT_L1:      return "L1-benign";
        case AE_THREAT_L2:      return "L2-suspect";
        case AE_THREAT_L3:      return "L3-attack";
        case AE_THREAT_SERVICE: return "SERVICE-whitelist";
        default:                return "NONE";
    }
}

const char *ae_hal_event_name(ae_hal_event_t ev)
{
    switch (ev) {
        case AE_EV_KEY_POWER:        return "key-power";
        case AE_EV_KEY_VOL_DOWN:     return "key-vol-down";
        case AE_EV_KEY_VOL_UP:       return "key-vol-up";
        case AE_EV_KEY_RELEASE:      return "key-release";
        case AE_EV_USB_CHARGE:       return "usb-charge";
        case AE_EV_CHASSIS_OPEN:     return "chassis-open";
        case AE_EV_TESTPOINT_SHORT:  return "testpoint-short(service)";
        case AE_EV_SERVICE_MODE:     return "service-mode";
        case AE_EV_ADB_UNAUTH:       return "adb-unauthorized";
        case AE_EV_AUTH_FAIL:        return "auth-fail";
        case AE_EV_MASS_ENUM:        return "mass-storage-enum";
        case AE_EV_CORE_READ:        return "core-read(scan)";
        case AE_EV_USB_DATA:         return "usb-data-intrusion";
        case AE_EV_CORE_EXPORT:      return "core-export";
        case AE_EV_KEY_TAMPER:       return "key-tamper";
        case AE_EV_SHORT_CORE:       return "short-to-core";
        default:                     return "none";
    }
}

int ae_hal_inject(ae_hal_t *h, ae_hal_event_t ev)
{
    if (!h || h->count >= EVQ_CAP) return -1;
    h->evq[h->tail] = ev;
    h->tail = (h->tail + 1) % EVQ_CAP;
    h->count++;
    return 0;
}

int ae_hal_poll(ae_hal_t *h, ae_hal_event_t *out)
{
    if (!h || !out || h->count == 0) return -1;
    *out = h->evq[h->head];
    h->head = (h->head + 1) % EVQ_CAP;
    h->count--;
    return 0;
}

uint64_t ae_hal_now_ms(ae_hal_t *h) { return h ? h->now_ms : 0; }
void     ae_hal_set_now_ms(ae_hal_t *h, uint64_t ms) { if (h) h->now_ms = ms; }
void     ae_hal_tick_ms(ae_hal_t *h, uint64_t delta) { if (h) h->now_ms += delta; }

int ae_hal_alarm(ae_hal_t *h, const char *reason)
{
    if (!h) return -1;
    h->alarm_count++;
    snprintf(h->alarm_last, sizeof h->alarm_last, "%s", reason ? reason : "unspecified");
    return 0;
}
uint32_t ae_hal_alarm_count(ae_hal_t *h) { return h ? h->alarm_count : 0; }
const char *ae_hal_alarm_last(ae_hal_t *h) { return h ? h->alarm_last : ""; }

void ae_hal_record_evidence(ae_hal_t *h, ae_hal_event_t ev)
{
    if (!h) return;
    h->evidence_count++;
    snprintf(h->evidence_last, sizeof h->evidence_last, "%s", ae_hal_event_name(ev));
}
uint32_t ae_hal_evidence_count(ae_hal_t *h) { return h ? h->evidence_count : 0; }
const char *ae_hal_evidence_last(ae_hal_t *h) { return h ? h->evidence_last : ""; }

void ae_hal_set_service_mode(ae_hal_t *h, int on) { if (h) h->service_mode = on ? 1 : 0; }
int  ae_hal_service_mode(ae_hal_t *h) { return h ? h->service_mode : 0; }

int ae_hal_hw_fuse_burn(ae_hal_t *h, const char *reason)
{
    if (!h) return -1;
    /* 云端 SIM：锁存「熔断指令已下发」，并留一条可观测报警；不伪称物理烧毁。
     * 真机 HW 后端(S6)：此处换为 eFuse/OTP 烧写或 crowbar 烧毁存储供电。 */
    h->hw_fuse_burned = 1;
    snprintf(h->hw_fuse_reason, sizeof h->hw_fuse_reason, "%s", reason ? reason : "L3-attack");
    (void)ae_hal_alarm(h, "HW-FUSE(simulated) burn-command latched");
    return 0;
}
int ae_hal_hw_fuse_burned(ae_hal_t *h) { return h ? h->hw_fuse_burned : 0; }
const char *ae_hal_hw_fuse_backend(ae_hal_t *h)
{
    if (!h) return "none";
    return h->backend == AE_HAL_HW ? "efuse-otp(S6-placeholder)" : "simulated(cloud-no-physical-fuse)";
}

int ae_hal_secure_element(ae_hal_t *h)
{
    /* 软件锚点阶段：无硬件安全单元；真机接 Titan M/eFuse 后返回 1。 */
    return (h && h->backend == AE_HAL_HW) ? 1 : 0;
}
