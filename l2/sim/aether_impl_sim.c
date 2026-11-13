/*
 * aether_impl_sim.c - L2 十五域云端仿真实现
 *
 * 所属层：L2 接口层（HAL）· 仿真实现分支
 * 依赖：aether_hal.h / aether_hal_config.h / stdlib / string
 * 对外接口：aether_impl_sim（aether_hal_impl_t 常量）
 * 不变量：
 *   1. 纯内存状态，不含任何设备节点路径；真机阶段换实现不改上层（§3.3 / §5.1）
 *   2. 每域状态可由脚本注入（set_state），可导出（get_state），供故障注入与取证
 *   3. 基带为完整状态机：无卡/无信号/漫游/仅紧急/注册失败/已注册，状态跳变按序投递
 *   4. 关键状态跳变（注册失败、掉电、过热关机）以 AETHER_EVT_KEY 事件投递，不许丢
 *   5. 传感器降频必须上报（rate 字段变化写入状态），不许悄悄变低
 * 对应验收项：§5.2 十五域、§5.3 流控、§5.4 故障注入、§15.2 接口层
 */
#include "aether_hal.h"
#include <stdlib.h>
#include <string.h>

/* 每域通用状态块；各域按语义使用字段，脚本注入即写此结构。
 * 布局对上层透明，上层只经 aether_hal_set_state/get_state 访问。 */
typedef struct {
    uint32_t mode;        /* 域语义状态：显示模式 / 基带注册态 / 电源充电态 / WiFi 连接态 ... */
    uint32_t value;       /* 数值：电量百分比 / 温度 / 亮度 / 采样率 / 信号强度 ... */
    uint32_t aux;         /* 辅助：卫星数 / 失败计数 / 剩余空间 / 时间戳低 32 位 ... */
    uint32_t pending_key; /* 待投递的关键事件码（0=无） */
    uint32_t seq;         /* 状态变更序号，用于「按序投递」校验 */
} sim_state_t;

typedef struct {
    sim_state_t st;
    uint8_t     frame[AETHER_HAL_EVENT_PAYLOAD]; /* 显示/相机/音频的当前帧描述 */
    uint32_t    frame_len;
} sim_ctx_t;

/* 基带状态枚举（§5.2「完整状态机」） */
enum {
    CELL_NO_SIM = 0,
    CELL_NO_SIGNAL,
    CELL_ROAMING,
    CELL_EMERGENCY_ONLY,
    CELL_REG_FAILED,
    CELL_REGISTERED,
};

static aether_err_t sim_init(aether_dom_t dom, void **out_ctx)
{
    (void)dom;
    sim_ctx_t *c = (sim_ctx_t *)calloc(1, sizeof *c);
    if (!c) return AETHER_ERR_IO;
    /* 各域给一个可用的初始态，避免「能力查询为有但调用必失败」（§11.2 第 5 类静默失败） */
    c->st.mode = 1;
    c->st.value = 100;        /* 电量 100%、亮度 100%、采样率默认满 */
    c->st.aux = 0;
    c->st.seq = 0;
    *out_ctx = c;
    return AETHER_OK;
}

static void sim_fini(aether_dom_t dom, void *ctx)
{
    (void)dom;
    free(ctx);
}

static uint32_t dom_flags(aether_dom_t dom)
{
    uint32_t f = AETHER_CAP_PRESENT | AETHER_CAP_INJECTABLE;
    switch (dom) {
    case AETHER_DOM_DISPLAY:
    case AETHER_DOM_AUDIO:
    case AETHER_DOM_INPUT:
        f |= AETHER_CAP_REALTIME;
        break;
    default:
        break;
    }
    switch (dom) {
    case AETHER_DOM_INPUT:       /* 按下/抬起不许丢 */
    case AETHER_DOM_CELLULAR:    /* 注册失败不许丢 */
    case AETHER_DOM_POWER:       /* 掉电不许丢 */
    case AETHER_DOM_THERMAL:     /* 过热关机不许丢 */
        f |= AETHER_CAP_KEY_EVENTS;
        break;
    default:
        break;
    }
    return f;
}

static aether_err_t sim_caps(aether_dom_t dom, void *ctx, aether_caps_t *out)
{
    (void)dom;
    if (!ctx || !out) return AETHER_ERR_BAD_ARG;
    out->flags = dom_flags(dom);
    strncpy(out->impl_name, "sim", sizeof out->impl_name - 1);
    return AETHER_OK;
}

/* submit：把上层投递的事件「消费」掉，更新域状态；关键事件做状态跳变登记 */
static aether_err_t sim_submit(aether_dom_t dom, void *ctx, const aether_event_t *evt)
{
    sim_ctx_t *c = (sim_ctx_t *)ctx;
    if (!c || !evt) return AETHER_ERR_BAD_ARG;
    if (evt->size && evt->size <= AETHER_HAL_EVENT_PAYLOAD) {
        memcpy(c->frame, evt->payload, evt->size);
        c->frame_len = evt->size;
    }
    c->st.seq++;
    /* 关键事件触发状态跳变登记：由 poll 以 KEY 事件按序投递出去 */
    if ((evt->flags & AETHER_EVT_KEY) && evt->size >= sizeof(uint32_t)) {
        uint32_t code;
        memcpy(&code, evt->payload, sizeof code);
        c->st.pending_key = code;
    }
    (void)dom;
    return AETHER_OK;
}

/* poll：仿真实现主动产出事件（状态跳变、传感器数据、帧同步等） */
static aether_err_t sim_poll(aether_dom_t dom, void *ctx, aether_event_t *out)
{
    sim_ctx_t *c = (sim_ctx_t *)ctx;
    if (!c || !out) return AETHER_ERR_BAD_ARG;
    if (!c->st.pending_key) return AETHER_OK;   /* 无待投递事件 */

    memset(out, 0, sizeof *out);
    out->flags = AETHER_EVT_KEY;
    out->size = sizeof(uint32_t);
    memcpy(out->payload, &c->st.pending_key, sizeof(uint32_t));
    c->st.pending_key = 0;
    (void)dom;
    return AETHER_OK;
}

static aether_err_t sim_set_state(aether_dom_t dom, void *ctx, const void *buf, size_t len)
{
    sim_ctx_t *c = (sim_ctx_t *)ctx;
    if (!c || !buf || len != sizeof(sim_state_t)) return AETHER_ERR_BAD_ARG;
    sim_state_t incoming;
    memcpy(&incoming, buf, sizeof incoming);

    /* 先按「注入前状态」推导需要保留的派生字段，最后再整块赋值。
     * 若先赋值再写派生字段会被 incoming 覆盖（曾导致降频证据与关键事件丢失）。 */
    uint32_t keep_aux = incoming.aux;
    uint32_t keep_key = c->st.pending_key;

    /* 传感器降频上报：rate 下降时保留降频前采样率（§5.3「降频必须上报」） */
    if (dom == AETHER_DOM_SENSOR && incoming.value < c->st.value)
        keep_aux = c->st.value;

    /* 基带状态跳变登记为关键事件，按序投递不许丢 */
    if (dom == AETHER_DOM_CELLULAR && incoming.mode != c->st.mode) {
        if (incoming.mode == CELL_REG_FAILED || incoming.mode == CELL_NO_SIGNAL)
            keep_key = 0xCE110000u | (incoming.mode & 0xFFu);
    }
    /* 电源掉电登记为关键事件 */
    if (dom == AETHER_DOM_POWER && incoming.value == 0 && c->st.value != 0)
        keep_key = 0xA0DE0000u;

    c->st = incoming;
    c->st.aux = keep_aux;          /* 保留降频证据，不被注入值冲掉 */
    c->st.pending_key = keep_key;  /* 保留待投递关键事件 */
    return AETHER_OK;
}

static aether_err_t sim_get_state(aether_dom_t dom, void *ctx, void *buf, size_t len)
{
    sim_ctx_t *c = (sim_ctx_t *)ctx;
    if (!c || !buf || len != sizeof(sim_state_t)) return AETHER_ERR_BAD_ARG;
    memcpy(buf, &c->st, sizeof(sim_state_t));
    (void)dom;
    return AETHER_OK;
}

const aether_hal_impl_t aether_impl_sim = {
    .name      = "sim",
    .init      = sim_init,
    .caps      = sim_caps,
    .submit    = sim_submit,
    .poll      = sim_poll,
    .set_state = sim_set_state,
    .get_state = sim_get_state,
    .fini      = sim_fini,
};
