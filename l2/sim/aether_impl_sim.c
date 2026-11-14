/*
 * aether_impl_sim.c - L2 十五域云端仿真实现
 *
 * 所属层：L2 接口层（HAL）· 仿真实现分支
 * 依赖：aether_dev_hal.h / aether_hal_config.h / stdlib / string
 * 对外接口：aether_impl_sim（aether_hal_impl_t 常量）
 * 不变量：
 *   1. 纯内存状态，不含任何设备节点路径；真机阶段换实现不改上层（§3.3 / §5.1）
 *   2. 十五域逐一具备独立语义：初始态、能力位、状态标签、关键事件码均按域分化
 *   3. 每域状态可由脚本注入（set_state），可导出（get_state），供故障注入与取证
 *   4. 关键状态跳变（息屏、按下/抬起、欠载、开流错误、断连、注册失败、定位丢失、
 *      生物识别失败达阈值、掉电、过热关机、空间耗尽、链路断开、时间跳变）
 *      以 AETHER_EVT_KEY 事件投递，事件码携带域编号，不许丢
 *   5. 传感器降频必须上报（rate 字段变化写入状态），不许悄悄变低
 * 对应验收项：§5.2 十五域、§5.3 流控、§5.4 故障注入、§15.2 接口层
 */
#include "aether_dev_hal.h"
#include <stdlib.h>
#include <string.h>

/* 每域通用状态块；各域按语义使用字段，脚本注入即写此结构。
 * 布局对上层透明，上层只经 aether_hal_set_state/get_state 访问。 */
typedef struct {
    uint32_t mode;        /* 域语义状态：显示模式 / 基带注册态 / 链路态 / 温度档 ... */
    uint32_t value;       /* 数值：电量 / 温度 / 亮度 / 采样率 / 信号强度 / 剩余空间 ... */
    uint32_t aux;         /* 辅助：域实例标识 / 帧数 / 失败计数 / 卫星数 / 时间戳 ... */
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

/* 十五域语义表：一项不可缺。每项给出初始态、叠加能力位、故障原因码与字段语义标签。
 * 该表是「每个域都有独立实现」的单一真相源，机检第三门按表项数量静态核验。 */
typedef struct {
    const char *mode_label;
    const char *value_label;
    const char *aux_label;
    uint32_t    init_mode;
    uint32_t    init_value;
    uint32_t    caps;       /* 叠加在 PRESENT | INJECTABLE 之上 */
    uint16_t    reason;     /* 域级故障注入的原因码 */
} dom_sem_t;

static const dom_sem_t k_dom_sem[AETHER_DOM_COUNT] = {
    [AETHER_DOM_DISPLAY]    = { "帧同步态",   "亮度%",     "帧提交数",   1,             100, AETHER_CAP_REALTIME,                   0xD101u },
    [AETHER_DOM_INPUT]      = { "触点态",     "触点数",    "事件计数",   0,               0, AETHER_CAP_REALTIME | AETHER_CAP_KEY_EVENTS, 0xD201u },
    [AETHER_DOM_AUDIO]      = { "音频路由",   "音量%",     "欠载计数",   1,              60, AETHER_CAP_REALTIME,                   0xD301u },
    [AETHER_DOM_CAMERA]     = { "开流态",     "曝光值",    "已取帧数",   0,               0, 0,                                   0xD401u },
    [AETHER_DOM_WIFI]       = { "连接态",     "信号%",     "扫描次数",   1,              75, 0,                                   0xD501u },
    [AETHER_DOM_BLUETOOTH]  = { "配对态",     "传输字节",  "设备数",     0,               0, 0,                                   0xD601u },
    [AETHER_DOM_CELLULAR]   = { "注册态",     "信号%",     "跳变序号",   CELL_REGISTERED, 80, AETHER_CAP_KEY_EVENTS,              0xD701u },
    [AETHER_DOM_LOCATION]   = { "定位可用",   "精度m",     "卫星数",     1,               5, 0,                                   0xD801u },
    [AETHER_DOM_SENSOR]     = { "采样中",     "采样率Hz",  "降频前值",   1,             100, 0,                                   0xD901u },
    [AETHER_DOM_BIOMETRIC]  = { "空闲",       "超时ms",    "失败计数",   0,               0, AETHER_CAP_KEY_EVENTS,              0xDA01u },
    [AETHER_DOM_POWER]      = { "充电中",     "电量%",     "电池健康",   1,             100, AETHER_CAP_KEY_EVENTS,              0xDB01u },
    [AETHER_DOM_THERMAL]    = { "正常",       "温度C",     "限频次数",   0,              35, AETHER_CAP_KEY_EVENTS,              0xDC01u },
    [AETHER_DOM_STORAGE]    = { "已挂载",     "剩余MB",    "分区数",     1,            4096, 0,                                   0xDD01u },
    [AETHER_DOM_NETWORK]    = { "链路up",     "带宽Mbps",  "统计计数",   1,             100, 0,                                   0xDE01u },
    [AETHER_DOM_RTC]        = { "已同步",     "时区",      "时间戳低32", 1,               8, AETHER_CAP_KEY_EVENTS,              0xDF01u },
};

/* 关键事件码：0xD0XXYYYY，XX 为域编号，YYYY 为原因码 */
static uint32_t dom_key(aether_dom_t dom, uint16_t reason)
{
    return AETHER_HAL_DOM_KEY_BASE
         | (((uint32_t)dom & 0xFFu) << 16)
         | (uint32_t)reason;
}

static int dom_valid(aether_dom_t dom)
{
    return (int)dom >= 0 && dom < AETHER_DOM_COUNT;
}

static aether_err_t sim_init(aether_dom_t dom, void **out_ctx)
{
    if (!dom_valid(dom) || !out_ctx) return AETHER_ERR_BAD_ARG;
    sim_ctx_t *c = (sim_ctx_t *)calloc(1, sizeof *c);
    if (!c) return AETHER_ERR_IO;
    /* 各域给出可用的初始态，避免「能力查询为有但调用必失败」（§11.2 第 5 类静默失败）。
     * aux 写入域编号作为实例标识，使每域上下文可被 get_state 外部区分。 */
    c->st.mode = k_dom_sem[dom].init_mode;
    c->st.value = k_dom_sem[dom].init_value;
    c->st.aux = (uint32_t)dom;
    c->st.pending_key = 0;
    c->st.seq = 0;
    *out_ctx = c;
    return AETHER_OK;
}

static void sim_fini(aether_dom_t dom, void *ctx)
{
    (void)dom;
    free(ctx);
}

static aether_err_t sim_caps(aether_dom_t dom, void *ctx, aether_caps_t *out)
{
    if (!ctx || !out) return AETHER_ERR_BAD_ARG;
    if (!dom_valid(dom)) return AETHER_ERR_BAD_ARG;
    out->flags = AETHER_CAP_PRESENT | AETHER_CAP_INJECTABLE | k_dom_sem[dom].caps;
    strncpy(out->impl_name, "sim", sizeof out->impl_name - 1);
    out->impl_name[sizeof out->impl_name - 1] = '\0';
    return AETHER_OK;
}

/* submit：把上层投递的事件「消费」掉，按域更新状态；关键事件做状态跳变登记 */
static aether_err_t sim_submit(aether_dom_t dom, void *ctx, const aether_event_t *evt)
{
    sim_ctx_t *c = (sim_ctx_t *)ctx;
    if (!c || !evt) return AETHER_ERR_BAD_ARG;
    if (!dom_valid(dom)) return AETHER_ERR_BAD_ARG;
    if (evt->size && evt->size <= AETHER_HAL_EVENT_PAYLOAD) {
        memcpy(c->frame, evt->payload, evt->size);
        c->frame_len = evt->size;
    }
    c->st.seq++;
    /* 帧/流类域累计域特有计数，可被 get_state 观测，证明该域确有消费语义 */
    switch (dom) {
    case AETHER_DOM_DISPLAY:
    case AETHER_DOM_AUDIO:
    case AETHER_DOM_CAMERA:
        c->st.aux++;
        break;
    default:
        break;
    }
    if ((evt->flags & AETHER_EVT_KEY) && evt->size >= sizeof(uint32_t)) {
        uint32_t code;
        memcpy(&code, evt->payload, sizeof code);
        c->st.pending_key = code;
    }
    return AETHER_OK;
}

/* poll：仿真实现主动产出事件（状态跳变、传感器数据、帧同步等） */
static aether_err_t sim_poll(aether_dom_t dom, void *ctx, aether_event_t *out)
{
    sim_ctx_t *c = (sim_ctx_t *)ctx;
    if (!c || !out) return AETHER_ERR_BAD_ARG;
    (void)dom;
    if (!c->st.pending_key) return AETHER_OK;   /* 无待投递事件 */

    memset(out, 0, sizeof *out);
    out->flags = AETHER_EVT_KEY;
    out->size = sizeof(uint32_t);
    memcpy(out->payload, &c->st.pending_key, sizeof(uint32_t));
    c->st.pending_key = 0;
    return AETHER_OK;
}

static aether_err_t sim_set_state(aether_dom_t dom, void *ctx, const void *buf, size_t len)
{
    sim_ctx_t *c = (sim_ctx_t *)ctx;
    if (!c || !buf || len != sizeof(sim_state_t)) return AETHER_ERR_BAD_ARG;
    if (!dom_valid(dom)) return AETHER_ERR_BAD_ARG;

    sim_state_t incoming;
    memcpy(&incoming, buf, sizeof incoming);

    /* 先按「注入前状态」推导需要保留的派生字段，最后再整块赋值。
     * 若先赋值再写派生字段会被 incoming 覆盖（曾导致降频证据与关键事件丢失）。 */
    uint32_t keep_aux = incoming.aux;
    uint32_t keep_key = c->st.pending_key;
    uint16_t reason = 0;
    int key = 0;

    switch (dom) {
    case AETHER_DOM_DISPLAY:      /* 息屏跳变；帧提交数不被注入值冲掉 */
        if (incoming.mode == 0 && c->st.mode != 0) key = 1;
        if (incoming.aux == 0) keep_aux = c->st.aux;
        break;
    case AETHER_DOM_INPUT:        /* 按下/抬起不许丢 */
        if (incoming.mode != c->st.mode) key = 1;
        break;
    case AETHER_DOM_AUDIO:        /* 欠载必须上报；欠载计数不被注入值冲掉 */
        if (incoming.mode == 3 && c->st.mode != 3) key = 1;
        if (incoming.aux == 0) keep_aux = c->st.aux;
        break;
    case AETHER_DOM_CAMERA:       /* 开流错误回调；已取帧数不被注入值冲掉 */
        if (incoming.mode == 9 && c->st.mode != 9) key = 1;
        if (incoming.aux == 0) keep_aux = c->st.aux;
        break;
    case AETHER_DOM_WIFI:         /* 断开连接 */
        if (incoming.mode == 0 && c->st.mode != 0) key = 1;
        break;
    case AETHER_DOM_BLUETOOTH:    /* 配对断开 */
        if (incoming.mode == 0 && c->st.mode != 0) key = 1;
        break;
    case AETHER_DOM_CELLULAR:     /* 基带状态跳变登记为关键事件，按序投递不许丢 */
        if (incoming.mode != c->st.mode &&
            (incoming.mode == CELL_REG_FAILED || incoming.mode == CELL_NO_SIGNAL)) {
            key = 1;
            keep_aux = incoming.mode;   /* 记录跳变后的注册态 */
        }
        break;
    case AETHER_DOM_LOCATION:     /* 定位丢失或精度显著退化 */
        if ((incoming.mode == 0 && c->st.mode != 0) ||
            incoming.value > c->st.value + 50u) key = 1;
        break;
    case AETHER_DOM_SENSOR:       /* 降频上报：rate 下降时保留降频前采样率（§5.3） */
        if (incoming.value < c->st.value) keep_aux = c->st.value;
        break;
    case AETHER_DOM_BIOMETRIC:    /* 失败计数达阈值 */
        if (incoming.aux >= 3u && c->st.aux < 3u) key = 1;
        break;
    case AETHER_DOM_POWER:        /* 掉电 */
        if (incoming.value == 0 && c->st.value != 0) key = 1;
        break;
    case AETHER_DOM_THERMAL:      /* 过热关机 */
        if (incoming.value >= 95u && c->st.value < 95u) key = 1;
        break;
    case AETHER_DOM_STORAGE:      /* 剩余空间耗尽 */
        if (incoming.value == 0 && c->st.value != 0) key = 1;
        break;
    case AETHER_DOM_NETWORK:      /* 链路断开 */
        if (incoming.mode == 0 && c->st.mode != 0) key = 1;
        break;
    case AETHER_DOM_RTC:          /* 时间跳变 */
        if (incoming.aux != c->st.aux &&
            (incoming.aux > c->st.aux + 5u || c->st.aux > incoming.aux + 5u)) key = 1;
        break;
    default:
        return AETHER_ERR_BAD_ARG;
    }

    /* 域级故障注入：任一域注入该 mode，都必须上报带域标识的关键事件。
     * 故障原因码取语义表基准值；域特化跳变取基准值 +1，二者可区分。 */
    if (incoming.mode == AETHER_HAL_DOM_FAULT_MODE) {
        key = 1;
        keep_aux = (uint32_t)dom;
        reason = k_dom_sem[dom].reason;
    } else if (key) {
        reason = (uint16_t)(k_dom_sem[dom].reason + 1u);
    }

    if (key) keep_key = dom_key(dom, reason);

    c->st = incoming;
    c->st.aux = keep_aux;          /* 保留域语义派生值，不被注入值冲掉 */
    c->st.pending_key = keep_key;  /* 保留待投递关键事件 */
    return AETHER_OK;
}

static aether_err_t sim_get_state(aether_dom_t dom, void *ctx, void *buf, size_t len)
{
    sim_ctx_t *c = (sim_ctx_t *)ctx;
    if (!c || !buf || len != sizeof(sim_state_t)) return AETHER_ERR_BAD_ARG;
    (void)dom;
    memcpy(buf, &c->st, sizeof(sim_state_t));
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
