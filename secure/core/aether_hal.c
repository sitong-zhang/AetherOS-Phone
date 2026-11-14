/*
 * aether_hal.c - 物理安全接口层实现（云端仿真后端）
 *
 * 只实现 AE_HAL_SIM：物理事件由 ae_hal_inject() 故障注入，时钟可注入以便
 * 确定性测试。AE_HAL_HW（真机 GPIO / 安全芯片 / eFuse）为 S6 目标，此处
 * 仅保留后端枚举与占位，open 时如实返回「未实现」，绝不伪造硬件能力。
 */
#define _GNU_SOURCE
#include "aether_hal.h"
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

int ae_hal_secure_element(ae_hal_t *h)
{
    /* 软件锚点阶段：无硬件安全单元；真机接 Titan M/eFuse 后返回 1。 */
    return (h && h->backend == AE_HAL_HW) ? 1 : 0;
}
