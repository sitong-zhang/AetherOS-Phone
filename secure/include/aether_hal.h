/*
 * aether_hal.h - 物理安全接口层（Hardware Abstraction Layer, 云端仿真）
 *
 * 立场（V2 §3.3 / §5.2）：所有「物理」信号都抽象成可替换接口——
 *   USB 数据接入、机壳防拆触点、测试点短接、组合键(电源/音量)、蜂鸣器报警、
 *   安全芯片在线状态、单调时钟。上层(安全锁定状态机)只认接口，不认 GPIO/寄存器。
 *   云端用「故障注入」仿真；真机 S6 换 GPIO + TEE/eFuse 实现，上层零改动。
 *
 * 诚实边界：当前只有 AE_HAL_BACKEND_SIM 仿真后端；硬件后端为占位（未实现）。
 *           云端「物理检测」是注入信号，不是真的拆机/USB 电气检测。
 */
#ifndef AETHER_HAL_H
#define AETHER_HAL_H

#include <stdint.h>
#include <stddef.h>

typedef enum {
    AE_HAL_SIM = 0,     /* 云端软件仿真（当前唯一实现） */
    AE_HAL_HW  = 1      /* 真机 GPIO/安全芯片（S6 目标，占位未实现） */
} ae_hal_backend_t;

/* 物理安全事件：进入安全锁定模式后，任一事件都应触发熔断 */
typedef enum {
    AE_EV_NONE           = 0,
    AE_EV_USB_DATA       = 1, /* USB 数据通道被强行接入（非仅充电） */
    AE_EV_CHASSIS_OPEN   = 2, /* 机壳防拆触点断开 */
    AE_EV_TESTPOINT_SHORT= 3, /* 主板测试点被短接 */
    AE_EV_KEY_POWER      = 4, /* 电源键（按下并保持） */
    AE_EV_KEY_VOL_DOWN   = 5, /* 音量下（按下并保持） */
    AE_EV_KEY_VOL_UP     = 6, /* 音量上（满 10 秒后的进入键） */
    AE_EV_KEY_RELEASE    = 7  /* 组合键松开（中途放弃） */
} ae_hal_event_t;

typedef struct ae_hal ae_hal_t;

/* 打开/关闭。hw 后端未实现时返回 -1（如实，不伪造硬件）。 */
int  ae_hal_open(ae_hal_backend_t backend, ae_hal_t **out);
void ae_hal_close(ae_hal_t *h);
const char *ae_hal_backend_name(ae_hal_t *h);

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

/* 安全芯片状态：0=软件锚点(当前) 1=硬件 TEE 在线(真机)。 */
int ae_hal_secure_element(ae_hal_t *h);

#endif /* AETHER_HAL_H */
