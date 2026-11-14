/*
 * aether_phys_hal.h - 物理安全接口层（Hardware Abstraction Layer, 云端仿真）
 *
 * 立场（V2 §3.3 / §5.2）：所有「物理」信号都抽象成可替换接口——
 *   USB 数据接入、机壳防拆触点、测试点短接、组合键(电源/音量)、蜂鸣器报警、
 *   安全芯片在线状态、单调时钟、硬件熔断。上层(安全锁定状态机)只认接口，不认 GPIO/寄存器。
 *   云端用「故障注入」仿真；真机 S6 换 GPIO + TEE/eFuse 实现，上层零改动。
 *
 * 威胁判据分级：
 *   **物理信号本身只是「证据」，是否熔断取决于「行为 / 攻击意图」是否成立**，
 *   不是「插一次 USB / 开一次盖 / 碰一次测试点就删系统」。HAL 把原始信号翻译为
 *   带判据等级的事件，上层状态机只认等级、不认电气信号——「攻击」因此可机器判定：
 *     L1 无害   : 仅充电(VBUS)、机壳开合(维修/换电池)、维修短接但未触核心区、正常数据连接
 *                 → 只记录取证 + 收紧数据通道，零删除，仍保持锁定
 *     L2 可疑   : 未授权 ADB/fastboot、鉴权反复失败、非白名单存储枚举、核心区批量读(侦察)
 *                 → 断连 + 记录取证，进 SUSPECT，零删除
 *     L3 确认攻击: USB 数据强插且判定破解意图、核心区/用户数据写入导出、密钥区触碰、
 *                 拆机短接触及用户数据或系统核心区
 *                 → 立即摧毁系统(软件熔断) + 烧毁硬件(硬件熔断) + 报警 + 变砖标记
 *     SERVICE   : 官方维修模式(签名凭证) —— 白名单，豁免物理开盖/短接，
 *                 但**不豁免**已确认的核心区导出/破解(L3)，防伪造凭证绕过
 *   物理信号到判据等级的推导由 aether_intent 判定层依据结构化观测完成。
 *
 * 诚实边界：当前只有 AE_HAL_BACKEND_SIM 仿真后端；硬件后端为占位（未实现）。
 *           云端「物理检测」是注入信号，不是真的拆机/USB 电气检测；
 *           「烧毁硬件」在云端只是**仿真熔断指令锁存**，真机才由 eFuse/OTP/crowbar 物理执行(S6)。
 */
#ifndef AETHER_PHYS_HAL_H
#define AETHER_PHYS_HAL_H

#include <stdint.h>
#include <stddef.h>

typedef enum {
    AE_HAL_SIM = 0,     /* 云端软件仿真（当前唯一实现） */
    AE_HAL_HW  = 1      /* 真机 GPIO/安全芯片/eFuse（S6 目标，占位未实现） */
} ae_hal_backend_t;

/* 物理安全事件。数值 1..7 与首版兼容；新分级事件从 10 起。
 * 语义按「威胁判据」而非「物理动作」定义：同名事件在不同判据下等级不同。 */
typedef enum {
    AE_EV_NONE              = 0,

    /* 组合键（非威胁输入） */
    AE_EV_KEY_POWER         = 4,  /* 电源键（按下并保持） */
    AE_EV_KEY_VOL_DOWN      = 5,  /* 音量下（按下并保持） */
    AE_EV_KEY_VOL_UP        = 6,  /* 音量上（满 10 秒后的进入键） */
    AE_EV_KEY_RELEASE       = 7,  /* 组合键松开（中途放弃） */

    /* ===== L1 无害物理信号：只记录/收紧，零删除 ===== */
    AE_EV_USB_CHARGE        = 10, /* USB 仅充电(VBUS)，无数据枚举 */
    AE_EV_CHASSIS_OPEN      = 2,  /* 机壳开合霍尔（正常维修/换电池）—— 首版值保留 */
    AE_EV_TESTPOINT_SHORT   = 3,  /* 测试点短接但未触及核心区（正常维修）—— 首版值保留 */
    AE_EV_SERVICE_MODE      = 11, /* 官方维修模式(签名凭证)：白名单 */

    /* ===== L2 可疑行为：断连+取证+累计，进 SUSPECT，零删除 ===== */
    AE_EV_ADB_UNAUTH        = 12, /* 未授权 ADB / fastboot 命令 */
    AE_EV_AUTH_FAIL         = 13, /* 鉴权反复失败 */
    AE_EV_MASS_ENUM         = 14, /* 非白名单大容量存储枚举 */
    AE_EV_CORE_READ         = 15, /* 对核心区的批量读取（侦察） */

    /* ===== L3 确认攻击意图：立即摧毁系统 + 烧毁硬件 ===== */
    AE_EV_USB_DATA          = 1,  /* USB 数据强插且判定为破解/取证意图 —— 首版值保留、语义收紧 */
    AE_EV_CORE_EXPORT       = 16, /* 核心区 / 用户数据写入或导出 */
    AE_EV_KEY_TAMPER        = 17, /* 密钥区(DEK/device.key)被触碰 */
    AE_EV_SHORT_CORE        = 18  /* 拆机短接且触及用户数据 / 系统核心区 */
} ae_hal_event_t;

/* 威胁判据等级（可机器判定，报告 §4.3 修改 1）。 */
typedef enum {
    AE_THREAT_NONE    = 0, /* 组合键等中性输入 */
    AE_THREAT_L1      = 1, /* 无害：零删除 */
    AE_THREAT_L2      = 2, /* 可疑：取证累计，零删除 */
    AE_THREAT_L3      = 3, /* 确认攻击意图：软件熔断 + 硬件熔断 */
    AE_THREAT_SERVICE = 4  /* 官方维修白名单 */
} ae_threat_t;

typedef struct ae_hal ae_hal_t;

/* 打开/关闭。hw 后端未实现时返回 -1（如实，不伪造硬件）。 */
int  ae_hal_open(ae_hal_backend_t backend, ae_hal_t **out);
void ae_hal_close(ae_hal_t *h);
const char *ae_hal_backend_name(ae_hal_t *h);

/* 事件 → 威胁判据等级（纯函数，可机检）。上层状态机据此分级动作。 */
ae_threat_t  ae_hal_event_threat(ae_hal_event_t ev);
const char  *ae_hal_threat_name(ae_threat_t t);
const char  *ae_hal_event_name(ae_hal_event_t ev);

/* 组合键状态（按住时长由状态机用单调时钟自行判定，HAL 只报「按/放」事件）。
 * 云端：ae_hal_inject 注入；真机：GPIO 中断上报。 */
int ae_hal_inject(ae_hal_t *h, ae_hal_event_t ev);  /* 注入一个物理事件 */
int ae_hal_poll(ae_hal_t *h, ae_hal_event_t *out);  /* 取出一个事件，空返回 -1 */

/* 单调时钟(毫秒)：状态机判定「同按满 10 秒」「冷却 10 秒」用。
 * 云端可注入时钟(便于确定性测试)，真机读 CLOCK_MONOTONIC。 */
uint64_t ae_hal_now_ms(ae_hal_t *h);
void     ae_hal_set_now_ms(ae_hal_t *h, uint64_t ms);  /* 仅仿真：注入当前时刻 */
void     ae_hal_tick_ms(ae_hal_t *h, uint64_t delta);  /* 仅仿真：推进时钟 */

/* 报警：真机为独立供电蜂鸣器/安全芯片；云端记录一条可观测报警日志。 */
int      ae_hal_alarm(ae_hal_t *h, const char *reason);
uint32_t ae_hal_alarm_count(ae_hal_t *h);
const char *ae_hal_alarm_last(ae_hal_t *h);

/* 证据链（外部可观测）：L1/L2 事件只作取证与审计，累计次数与最后一条供验收查询；
 * 不设自动升级阈值，不参与熔断触发判定。 */
void        ae_hal_record_evidence(ae_hal_t *h, ae_hal_event_t ev);
uint32_t    ae_hal_evidence_count(ae_hal_t *h);
const char *ae_hal_evidence_last(ae_hal_t *h);

/* 维修白名单（签名凭证）：开启后豁免物理开盖/短接类信号，但不豁免 L3 核心区导出。 */
void ae_hal_set_service_mode(ae_hal_t *h, int on);
int  ae_hal_service_mode(ae_hal_t *h);

/* 硬件熔断（烧毁硬件）：真机 eFuse/OTP 一次性烧写，或 crowbar 烧毁存储供电，
 * 物理永久变砖；云端 SIM 仅「仿真熔断指令锁存」，如实不伪称真的烧了硬件。
 * 仅 L3 确认攻击意图时由状态机调用。三级判据 + 维修白名单 + 证据链只作取证、
 * 不设自动升级阈值，构成误触防护。 */
int  ae_hal_hw_fuse_burn(ae_hal_t *h, const char *reason); /* 0=指令已下发/锁存 */
int  ae_hal_hw_fuse_burned(ae_hal_t *h);                    /* 外部可观测：是否已锁存 */
const char *ae_hal_hw_fuse_backend(ae_hal_t *h);            /* simulated / efuse(S6-placeholder) */

/* 安全芯片状态：0=软件锚点(当前) 1=硬件 TEE 在线(真机)。 */
int ae_hal_secure_element(ae_hal_t *h);

#endif /* AETHER_PHYS_HAL_H */
