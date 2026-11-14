/*
 * aether_dev_hal.h - AetherOS L2 硬件接口层：统一接口定义（不含实现）
 *
 * 所属层：L2 接口层（HAL）
 * 依赖：aether_hal_config.h、C 标准库（stdint/stddef）
 * 对外接口：本文件声明的全部 aether_hal_* 函数与类型
 * 不变量：
 *   1. 上层只经本头文件调用，不得包含任何实现文件、不得出现设备节点路径（§4.2 第 2 条）
 *   2. 每个域必须提供「能力查询 + 错误码 + 流控计数」三件套（§5.2 / §5.6 第三门）
 *   3. 生产快/消费慢时必须有确定策略、有上限、有计数，禁止无声丢数据（§5.3）
 *   4. 实现可在编译期或运行期替换，上层目标文件不因替换而改变（§5.6 第四门）
 *   5. 故障注入只改变返回值与状态，不改变接口签名
 * 对应验收项：§5.2 十五域、§5.3 流控、§5.4 接口自证、§5.6 五门机检、§15.2 接口层
 */
#ifndef AETHER_DEV_HAL_H
#define AETHER_DEV_HAL_H

#include <stddef.h>
#include <stdint.h>
#include "aether_hal_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== 十五个域（§5.2，一项不可缺） ==================== */
typedef enum {
    AETHER_DOM_DISPLAY = 0,   /* 显示：能力/模式/帧提交/同步/亮度/息屏/色彩 */
    AETHER_DOM_INPUT,         /* 输入：触点/压力/多指/原始流/注入回放 */
    AETHER_DOM_AUDIO,         /* 音频：通道/播放/采集/音量/路由/欠载 */
    AETHER_DOM_CAMERA,        /* 相机：枚举/开流/取帧/曝光/错误回调 */
    AETHER_DOM_WIFI,          /* Wi-Fi：扫描/连接/断开/信号/热点/省电 */
    AETHER_DOM_BLUETOOTH,     /* 蓝牙：开关/扫描/配对/传输/断开 */
    AETHER_DOM_CELLULAR,      /* 蜂窝/基带：完整状态机、可注入异常 */
    AETHER_DOM_LOCATION,      /* 定位：坐标/精度/卫星数/轨迹 */
    AETHER_DOM_SENSOR,        /* 传感器：数值曲线注入、降频上报 */
    AETHER_DOM_BIOMETRIC,     /* 生物识别：录入/匹配/失败计数/超时 */
    AETHER_DOM_POWER,         /* 电源与电池：电量/充电/温度/健康/节能 */
    AETHER_DOM_THERMAL,       /* 热管理：温度采样/限频/过热关机 */
    AETHER_DOM_STORAGE,       /* 存储：分区/读写/剩余空间/挂载/健康 */
    AETHER_DOM_NETWORK,       /* 网络通用：链路/地址/统计/限速 */
    AETHER_DOM_RTC,           /* 时钟/RTC：时间/时区/时间跳变注入 */
    AETHER_DOM_COUNT          /* 必须等于 15 */
} aether_dom_t;

/* ==================== 错误码（每个域统一） ==================== */
typedef enum {
    AETHER_OK = 0,
    AETHER_ERR_NOT_SUPPORTED, /* 能力不存在：必须查询能力，不许假设硬件存在 */
    AETHER_ERR_NO_IMPL,       /* 缺实现：系统须明确提示并降级，不许白屏/静默 */
    AETHER_ERR_BAD_ARG,
    AETHER_ERR_TIMEOUT,       /* 接口超时 */
    AETHER_ERR_DISCONNECTED,  /* 断连（故障注入 / 设备移除） */
    AETHER_ERR_BACKPRESSURE,  /* 生产快于消费，按策略拒绝，且已计数 */
    AETHER_ERR_FAULT,         /* 主动注入的失败 */
    AETHER_ERR_IO,
    AETHER_ERR_COUNT
} aether_err_t;

/* ==================== 能力查询（三件套之一） ==================== */
typedef struct {
    uint32_t api_version;     /* AETHER_HAL_API_VERSION */
    uint32_t flags;           /* 位域：见 AETHER_CAP_* */
    uint32_t queue_capacity;  /* AETHER_HAL_QUEUE_CAP */
    uint32_t max_payload;     /* AETHER_HAL_EVENT_PAYLOAD */
    char     impl_name[32];   /* 当前实现名：sim / stub / dev */
} aether_caps_t;

#define AETHER_CAP_PRESENT     (1u << 0)  /* 域有可用实现 */
#define AETHER_CAP_INJECTABLE  (1u << 1)  /* 支持状态/故障注入（云端实现） */
#define AETHER_CAP_REALTIME    (1u << 2)  /* 有实时性约束（显示/音频/输入） */
#define AETHER_CAP_KEY_EVENTS  (1u << 3)  /* 携带不许丢的关键边界事件 */

/* ==================== 流控计数（三件套之二，§5.3） ==================== */
typedef enum {
    AETHER_FLOW_DROP = 0,     /* 超限丢弃新事件（计 dropped），显式上报 */
    AETHER_FLOW_BLOCK,        /* 超限阻塞式返回 AETHER_ERR_BACKPRESSURE */
    AETHER_FLOW_QUEUE         /* 超限排队到上限，仍满则丢弃 + 计数 */
} aether_flow_policy_t;

typedef struct {
    uint64_t produced;        /* 上层提交总数 */
    uint64_t consumed;        /* 实现消费总数 */
    uint64_t dropped;         /* 因背压丢弃总数（禁止无声丢） */
    uint64_t rejected;        /* 因 BACKPRESSURE 拒绝总数 */
    uint64_t key_preserved;   /* 关键事件被优先保留的次数 */
    uint64_t fault_injected;  /* 故障注入命中次数 */
    uint64_t log_suppressed;  /* 日志限速抑制条数 */
    uint32_t queued;          /* 当前队列深度 */
    uint32_t queue_capacity;  /* 上限，来自配置单一源 */
    aether_flow_policy_t policy;
} aether_flow_t;

/* ==================== 事件（统一投递格式） ==================== */
#define AETHER_EVT_KEY  (1u << 0)  /* 关键边界事件：按下/抬起/注册失败/掉电/状态跳变 */

typedef struct {
    uint32_t seq;
    uint32_t flags;           /* AETHER_EVT_KEY 等 */
    uint32_t size;            /* 实际载荷字节数 */
    uint8_t  payload[AETHER_HAL_EVENT_PAYLOAD];
} aether_event_t;

/* ==================== 故障注入（§5.4） ==================== */
typedef enum {
    AETHER_FAULT_NONE = 0,
    AETHER_FAULT_FAIL,        /* 每次调用返回 AETHER_ERR_FAULT */
    AETHER_FAULT_DELAY,       /* 模拟超时：连续 N 次返回 TIMEOUT */
    AETHER_FAULT_DISCONNECT,  /* 返回 DISCONNECTED */
    AETHER_FAULT_NO_IMPL      /* 返回 NO_IMPL，验证上层降级 */
} aether_fault_t;

typedef struct {
    aether_fault_t mode;
    uint32_t counter;         /* 内部计数，供 DELAY 模式判定 */
} aether_fault_state_t;

/* ==================== 实现接口（由 sim / 真机驱动提供） ==================== */
typedef struct aether_hal_impl {
    const char *name;         /* "sim" / "stub" / "dev" */
    aether_err_t (*init)(aether_dom_t dom, void **out_ctx);
    aether_err_t (*caps)(aether_dom_t dom, void *ctx, aether_caps_t *out);
    aether_err_t (*submit)(aether_dom_t dom, void *ctx, const aether_event_t *evt);
    aether_err_t (*poll)(aether_dom_t dom, void *ctx, aether_event_t *out);
    aether_err_t (*set_state)(aether_dom_t dom, void *ctx, const void *buf, size_t len);
    aether_err_t (*get_state)(aether_dom_t dom, void *ctx, void *buf, size_t len);
    void (*fini)(aether_dom_t dom, void *ctx);
} aether_hal_impl_t;

/* ==================== 统一对外 API ==================== */
int  aether_hal_init(const aether_hal_impl_t *impl);  /* 传 NULL 用默认仿真实现 */
void aether_hal_fini(void);

/* 三件套：每个域都必须可查 */
aether_err_t aether_hal_caps(aether_dom_t dom, aether_caps_t *out);
aether_err_t aether_hal_flow(aether_dom_t dom, aether_flow_t *out);
aether_err_t aether_hal_last_error(aether_dom_t dom);

/* 数据面 */
aether_err_t aether_hal_submit(aether_dom_t dom, const aether_event_t *evt);
aether_err_t aether_hal_poll(aether_dom_t dom, aether_event_t *out);
aether_err_t aether_hal_set_state(aether_dom_t dom, const void *buf, size_t len);
aether_err_t aether_hal_get_state(aether_dom_t dom, void *buf, size_t len);

/* 治理面 */
aether_err_t aether_hal_set_policy(aether_dom_t dom, aether_flow_policy_t policy);
aether_err_t aether_hal_set_fault(aether_dom_t dom, aether_fault_t mode);
aether_err_t aether_hal_switch_impl(const aether_hal_impl_t *impl);

const char *aether_hal_dom_name(aether_dom_t dom);
const char *aether_hal_err_name(aether_err_t err);
const char *aether_hal_impl_name(void);

/* 供机检第四门使用：返回当前实现指针，上层不依赖其内部结构 */
const aether_hal_impl_t *aether_hal_current_impl(void);

/* 两套可替换实现（编译进同一库，运行期可切换） */
extern const aether_hal_impl_t aether_impl_sim;
extern const aether_hal_impl_t aether_impl_stub;

#ifdef __cplusplus
}
#endif

#endif /* AETHER_DEV_HAL_H */
