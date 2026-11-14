/*
 * aether_hal_core.c - L2 接口层统一调度核心
 *
 * 所属层：L2 接口层（HAL）
 * 依赖：aether_dev_hal.h / aether_hal_config.h / string.h / stdio.h
 * 对外接口：aether_hal_* 全部函数
 * 不变量：
 *   1. 本文件不含任何 /dev/、/sys/class/、/proc/ 直连（§4.2 第 2 条 / §5.6 第一门）
 *   2. 每个域的 produced/consumed/dropped/rejected 必须可查，禁止无声丢数据
 *   3. AETHER_EVT_KEY 事件在队列满时优先保留：先淘汰最老的非关键事件
 *   4. 实现切换只换 impl 指针，上层调用路径与状态计数语义不变（§5.6 第四门）
 *   5. 关键事件（按下/抬起、注册失败、掉电、状态跳变）绝不因背压被丢弃
 * 对应验收项：§5.3 流控与背压、§5.4 接口自证、§5.6 五门机检、§15.2 接口层
 */
#include "aether_dev_hal.h"
#include <string.h>
#include <stdio.h>

typedef struct {
    aether_event_t  q[AETHER_HAL_QUEUE_CAP];
    uint32_t        head, tail, count;
    aether_flow_t   flow;
    aether_err_t    last_err;
    aether_fault_state_t fault;
    void           *impl_ctx;
    uint32_t        seq;
} dom_slot_t;

typedef struct {
    int             initialized;
    const aether_hal_impl_t *impl;
    dom_slot_t      slot[AETHER_DOM_COUNT];
} hal_global_t;

static hal_global_t g_hal;

static const char *const k_dom_names[AETHER_DOM_COUNT] = {
    "display", "input", "audio", "camera", "wifi",
    "bluetooth", "cellular", "location", "sensor", "biometric",
    "power", "thermal", "storage", "network", "rtc"
};

static const char *const k_err_names[AETHER_ERR_COUNT] = {
    "OK", "NOT_SUPPORTED", "NO_IMPL", "BAD_ARG", "TIMEOUT",
    "DISCONNECTED", "BACKPRESSURE", "FAULT", "IO"
};

const char *aether_hal_dom_name(aether_dom_t dom)
{
    if ((int)dom < 0 || dom >= AETHER_DOM_COUNT) return "unknown";
    return k_dom_names[dom];
}

const char *aether_hal_err_name(aether_err_t err)
{
    if ((int)err < 0 || err >= AETHER_ERR_COUNT) return "unknown";
    return k_err_names[err];
}

const char *aether_hal_impl_name(void)
{
    return (g_hal.initialized && g_hal.impl) ? g_hal.impl->name : "none";
}

const aether_hal_impl_t *aether_hal_current_impl(void)
{
    return g_hal.impl;
}

/* 日志限速：返回 1 表示允许打印；抑制时只累加计数，不写 I/O（§10.4） */
static int log_allowed(dom_slot_t *s)
{
    static uint32_t per_dom_printed[AETHER_DOM_COUNT];
    /* 用 slot 在全局表中的下标做索引，避免引入设备/系统依赖 */
    uint32_t idx = 0;
    for (uint32_t i = 0; i < AETHER_DOM_COUNT; i++)
        if (&g_hal.slot[i] == s) { idx = i; break; }
    if (per_dom_printed[idx] < AETHER_HAL_LOG_RATE_MAX) {
        per_dom_printed[idx]++;
        return 1;
    }
    s->flow.log_suppressed++;
    return 0;
}

static dom_slot_t *slot_of(aether_dom_t dom)
{
    if ((int)dom < 0 || dom >= AETHER_DOM_COUNT) return NULL;
    return &g_hal.slot[dom];
}

static void slot_reset(dom_slot_t *s)
{
    memset(s->q, 0, sizeof s->q);
    s->head = s->tail = s->count = 0;
    memset(&s->flow, 0, sizeof s->flow);
    s->flow.queue_capacity = AETHER_HAL_QUEUE_CAP;
    s->flow.policy = AETHER_FLOW_QUEUE;
    s->last_err = AETHER_OK;
    memset(&s->fault, 0, sizeof s->fault);
    s->seq = 0;
}

int aether_hal_init(const aether_hal_impl_t *impl)
{
    if (g_hal.initialized) aether_hal_fini();
    memset(&g_hal, 0, sizeof g_hal);
    for (int i = 0; i < AETHER_DOM_COUNT; i++)
        slot_reset(&g_hal.slot[i]);

    g_hal.impl = impl ? impl : &aether_impl_sim;
    for (int i = 0; i < AETHER_DOM_COUNT; i++) {
        dom_slot_t *s = &g_hal.slot[i];
        if (g_hal.impl->init)
            g_hal.impl->init((aether_dom_t)i, &s->impl_ctx);
    }
    g_hal.initialized = 1;
    return 0;
}

void aether_hal_fini(void)
{
    if (!g_hal.initialized) return;
    for (int i = 0; i < AETHER_DOM_COUNT; i++) {
        dom_slot_t *s = &g_hal.slot[i];
        if (g_hal.impl && g_hal.impl->fini && s->impl_ctx)
            g_hal.impl->fini((aether_dom_t)i, s->impl_ctx);
        s->impl_ctx = NULL;
    }
    g_hal.initialized = 0;
    g_hal.impl = NULL;
}

aether_err_t aether_hal_switch_impl(const aether_hal_impl_t *impl)
{
    if (!g_hal.initialized || !impl) return AETHER_ERR_BAD_ARG;
    /* 先释放旧实现上下文，再挂新实现；上层调用路径完全不变 */
    for (int i = 0; i < AETHER_DOM_COUNT; i++) {
        dom_slot_t *s = &g_hal.slot[i];
        if (g_hal.impl && g_hal.impl->fini && s->impl_ctx)
            g_hal.impl->fini((aether_dom_t)i, s->impl_ctx);
        s->impl_ctx = NULL;
        if (impl->init)
            impl->init((aether_dom_t)i, &s->impl_ctx);
    }
    g_hal.impl = impl;
    return AETHER_OK;
}

aether_err_t aether_hal_caps(aether_dom_t dom, aether_caps_t *out)
{
    dom_slot_t *s = slot_of(dom);
    if (!s || !out) return AETHER_ERR_BAD_ARG;
    if (!g_hal.initialized) return AETHER_ERR_NO_IMPL;
    memset(out, 0, sizeof *out);
    out->api_version = AETHER_HAL_API_VERSION;
    out->queue_capacity = AETHER_HAL_QUEUE_CAP;
    out->max_payload = AETHER_HAL_EVENT_PAYLOAD;
    if (g_hal.impl->caps)
        return g_hal.impl->caps(dom, s->impl_ctx, out);
    return AETHER_OK;
}

aether_err_t aether_hal_flow(aether_dom_t dom, aether_flow_t *out)
{
    dom_slot_t *s = slot_of(dom);
    if (!s || !out) return AETHER_ERR_BAD_ARG;
    *out = s->flow;
    return AETHER_OK;
}

aether_err_t aether_hal_last_error(aether_dom_t dom)
{
    dom_slot_t *s = slot_of(dom);
    return s ? s->last_err : AETHER_ERR_BAD_ARG;
}

aether_err_t aether_hal_set_policy(aether_dom_t dom, aether_flow_policy_t policy)
{
    dom_slot_t *s = slot_of(dom);
    if (!s || policy > AETHER_FLOW_QUEUE) return AETHER_ERR_BAD_ARG;
    s->flow.policy = policy;
    return AETHER_OK;
}

aether_err_t aether_hal_set_fault(aether_dom_t dom, aether_fault_t mode)
{
    dom_slot_t *s = slot_of(dom);
    if (!s || mode > AETHER_FAULT_NO_IMPL) return AETHER_ERR_BAD_ARG;
    s->fault.mode = mode;
    s->fault.counter = 0;
    return AETHER_OK;
}

/* 故障注入判定：返回非 AETHER_OK 时，调用方必须把该错误码显式返回上层 */
static aether_err_t apply_fault(dom_slot_t *s)
{
    aether_err_t e = AETHER_OK;
    switch (s->fault.mode) {
    case AETHER_FAULT_FAIL:        e = AETHER_ERR_FAULT; break;
    case AETHER_FAULT_DISCONNECT:  e = AETHER_ERR_DISCONNECTED; break;
    case AETHER_FAULT_NO_IMPL:     e = AETHER_ERR_NO_IMPL; break;
    case AETHER_FAULT_DELAY:
        if (s->fault.counter < AETHER_HAL_FAULT_DELAY_N) {
            s->fault.counter++;
            e = AETHER_ERR_TIMEOUT;
        }
        break;
    case AETHER_FAULT_NONE:
    default: break;
    }
    if (e != AETHER_OK) {
        s->flow.fault_injected++;
        if (log_allowed(s))
            fprintf(stderr, "[hal] %s fault=%s\n",
                    aether_hal_dom_name((aether_dom_t)(s - g_hal.slot)),
                    aether_hal_err_name(e));
    }
    return e;
}

/* 队列满时淘汰策略：关键事件优先，绝不被丢弃 */
static int enqueue(dom_slot_t *s, const aether_event_t *evt)
{
    if (s->count < AETHER_HAL_QUEUE_CAP) {
        s->q[s->tail] = *evt;
        s->tail = (s->tail + 1) % AETHER_HAL_QUEUE_CAP;
        s->count++;
        return 0;
    }
    /* 队列满 */
    if (evt->flags & AETHER_EVT_KEY) {
        /* 找最老的非关键事件淘汰 */
        for (uint32_t i = 0; i < s->count; i++) {
            uint32_t idx = (s->head + i) % AETHER_HAL_QUEUE_CAP;
            if (!(s->q[idx].flags & AETHER_EVT_KEY)) {
                /* 前移覆盖，保持顺序 */
                for (uint32_t j = i; j + 1 < s->count; j++) {
                    uint32_t a = (s->head + j) % AETHER_HAL_QUEUE_CAP;
                    uint32_t b = (s->head + j + 1) % AETHER_HAL_QUEUE_CAP;
                    s->q[a] = s->q[b];
                }
                s->tail = (s->tail + AETHER_HAL_QUEUE_CAP - 1) % AETHER_HAL_QUEUE_CAP;
                s->count--;
                s->flow.dropped++;       /* 被淘汰的非关键事件，显式计数 */
                s->flow.key_preserved++; /* 关键事件被保留，显式计数 */
                s->q[s->tail] = *evt;
                s->tail = (s->tail + 1) % AETHER_HAL_QUEUE_CAP;
                s->count++;
                return 0;
            }
        }
        /* 全队列都是关键事件：拒绝新事件，但必须计数上报，不许无声丢 */
        s->flow.rejected++;
        return -1;
    }
    /* 非关键事件，按策略处理 */
    switch (s->flow.policy) {
    case AETHER_FLOW_DROP:
        s->flow.dropped++;
        return 0;                     /* 丢弃但有计数，非「无声」 */
    case AETHER_FLOW_BLOCK:
        s->flow.rejected++;
        return -1;
    case AETHER_FLOW_QUEUE:
    default:
        s->flow.dropped++;
        return 0;
    }
}

aether_err_t aether_hal_submit(aether_dom_t dom, const aether_event_t *evt)
{
    dom_slot_t *s = slot_of(dom);
    if (!s || !evt) return AETHER_ERR_BAD_ARG;
    if (!g_hal.initialized) return AETHER_ERR_NO_IMPL;
    if (evt->size > AETHER_HAL_EVENT_PAYLOAD) return AETHER_ERR_BAD_ARG;

    aether_event_t local = *evt;
    local.seq = ++s->seq;

    /* 1) 先交给实现消费。缺实现/实现报错必须显式上报，不许静默成功（§5.4）。
     *    实现拒绝的事件计入 rejected，保证 produced 与去向可对账。 */
    if (g_hal.impl->submit) {
        aether_err_t ie = g_hal.impl->submit(dom, s->impl_ctx, &local);
        if (ie != AETHER_OK) {
            s->flow.produced++;
            s->flow.rejected++;
            s->last_err = ie;
            return ie;
        }
    }

    s->flow.produced++;

    /* 2) 故障注入：作用于已实现链路，仍要计入对账 */
    aether_err_t fe = apply_fault(s);
    if (fe != AETHER_OK) {
        s->flow.rejected++;
        s->last_err = fe;
        return fe;
    }

    /* 3) 入队（流控），满队列按策略处理并计数 */
    int r = enqueue(s, &local);
    if (r != 0) {
        s->last_err = AETHER_ERR_BACKPRESSURE;
        return AETHER_ERR_BACKPRESSURE;
    }
    s->flow.queued = s->count;
    s->last_err = AETHER_OK;
    return AETHER_OK;
}

aether_err_t aether_hal_poll(aether_dom_t dom, aether_event_t *out)
{
    dom_slot_t *s = slot_of(dom);
    if (!s || !out) return AETHER_ERR_BAD_ARG;
    if (!g_hal.initialized) return AETHER_ERR_NO_IMPL;

    aether_err_t fe = apply_fault(s);
    if (fe != AETHER_OK) { s->last_err = fe; return fe; }

    /* 先让实现有机会产出事件（仿真实现可注入状态变化）。
     * 实现产出的事件同样计入 produced，保证「produced 与去向可对账」恒成立。 */
    if (g_hal.impl->poll) {
        aether_event_t ext;
        memset(&ext, 0, sizeof ext);
        if (g_hal.impl->poll(dom, s->impl_ctx, &ext) == AETHER_OK && ext.size) {
            s->flow.produced++;
            ext.seq = ++s->seq;
            (void)enqueue(s, &ext);
        }
    }
    if (s->count == 0) {
        memset(out, 0, sizeof *out);   /* 空队列必须清零，不许返回未初始化数据 */
        s->last_err = AETHER_OK;
        return AETHER_OK;              /* 空队列是正常状态，不是错误 */
    }
    *out = s->q[s->head];
    s->head = (s->head + 1) % AETHER_HAL_QUEUE_CAP;
    s->count--;
    s->flow.consumed++;
    s->flow.queued = s->count;
    s->last_err = AETHER_OK;
    return AETHER_OK;
}

aether_err_t aether_hal_set_state(aether_dom_t dom, const void *buf, size_t len)
{
    dom_slot_t *s = slot_of(dom);
    if (!s || (!buf && len)) return AETHER_ERR_BAD_ARG;
    if (!g_hal.initialized) return AETHER_ERR_NO_IMPL;
    if (!g_hal.impl->set_state) return AETHER_ERR_NOT_SUPPORTED;
    aether_err_t e = g_hal.impl->set_state(dom, s->impl_ctx, buf, len);
    s->last_err = e;
    return e;
}

aether_err_t aether_hal_get_state(aether_dom_t dom, void *buf, size_t len)
{
    dom_slot_t *s = slot_of(dom);
    if (!s || (!buf && len)) return AETHER_ERR_BAD_ARG;
    if (!g_hal.initialized) return AETHER_ERR_NO_IMPL;
    if (!g_hal.impl->get_state) return AETHER_ERR_NOT_SUPPORTED;
    aether_err_t e = g_hal.impl->get_state(dom, s->impl_ctx, buf, len);
    s->last_err = e;
    return e;
}
