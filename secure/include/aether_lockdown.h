/*
 * aether_lockdown.h - 安全锁定 / 熔断模式（AetherOS 招牌安全能力）
 *
 * 用户指定的语义（原话归纳）：
 *  1) 首启 OOBE 前置步骤必须设定「专用安全密码」，不设定无法进入系统（硬门）。
 *  2) 设定交互：确定后弹二次确认窗——警告「密码极重要、务必物理抄录」、
 *     明文再回显一遍密码、确定按钮有 10 秒冷却窗口（防手快点过）。
 *  3) 密码强度：长度 20–30、含大写+小写+特殊符号、连续序列(123…/abc…)≤5 位。
 *  4) 零存储：系统在任何地方、任何时间都不储存这个密码本身。
 *     —— 精确工程含义：密码即 wrapping key；落盘的只有「由密码经 KDF 派生、
 *        无法倒推回密码」的验证锚 verifier（+ salt），密码明文永不落盘/日志/
 *        快照，用完即从内存清零。若连验证锚都不留，系统便无法判断对错，
 *        连机主自己也永远解不开——故留「不可倒推的锚」而非「密码」。
 *  5) 不可重置 / 不可找回 / 无修改入口，仅首次设定有效；忘记 = 永久锁死。
 *  6) 触发：电源+音量下 同按满 10 秒(不足无效、超时保持待确认) → 再按一下
 *     音量上二次确认 → 进入安全锁定模式；未设密码则组合键完全无效。
 *  7) 进入后：仅该专用密码可解锁；其余路径(锁屏/生物/ADB/recovery/刷机/
 *     USB/调试口)全部封死——「其他任何方式都破解不了」。
 *  8) 熔断红线：安全锁定态下 USB 数据强插 / 拆机 / 短接 → 触发报警 +
 *     抹除「用户数据 + 系统本体」(连系统一起删)，永久变砖、需重刷。
 *     区别于常规档(输错口令只抹用户数据、系统保留) 与 胁迫档(诱饵环境)。
 *
 * 诚实边界：物理事件检测走 aether_hal 云端仿真注入；真机 S6 接 GPIO/TEE。
 *           「任何方式破解不了」在真机依赖安全芯片/eFuse，云端为软件仿真。
 */
#ifndef AETHER_LOCKDOWN_H
#define AETHER_LOCKDOWN_H

#include <stdint.h>
#include <stddef.h>
#include "aether_store.h"
#include "aether_hal.h"

#define AE_LD_PW_MIN        20u
#define AE_LD_PW_MAX        30u
#define AE_LD_RUN_LIMIT     5u      /* 连续序列最长 5 位（12345 可、123456 拒） */
#define AE_LD_COOLDOWN_MS   10000u  /* 二次确认窗确定按钮冷却 */
#define AE_LD_GESTURE_MS    10000u  /* 组合键同按满 10 秒 */
#define AE_LD_ANCHOR_FILE   "lockdown.anchor"  /* 存 lockdown/ 区：verifier 锚，非密码 */
#define AE_LD_BRICK_FILE    "BRICKED"          /* 变砖标记（落 root，跨区持久） */

typedef enum {
    AE_LD_UNSET     = 0,  /* 首启：尚未设定专用密码（硬门：不放行进系统） */
    AE_LD_NORMAL    = 1,  /* 已设定，正常运行 */
    AE_LD_GESTURE   = 2,  /* 组合键计时中（未满 10 秒） */
    AE_LD_ARMED     = 3,  /* 已满 10 秒，等待「音量上」二次确认 */
    AE_LD_LOCKED    = 4,  /* 安全锁定模式：仅专用密码可退出 */
    AE_LD_BRICKED   = 5   /* 已熔断：连系统抹除、永久变砖 */
} ae_ld_state_t;

typedef struct ae_ld ae_ld_t;

/* 绑定运行根 + 物理接口层打开/关闭 */
int  ae_ld_open(ae_store_t *s, ae_hal_t *hal, ae_ld_t **out);
void ae_ld_close(ae_ld_t *c);

/* 状态机 */
ae_ld_state_t ae_ld_state(ae_ld_t *c);
const char   *ae_ld_state_name(ae_ld_t *c);

/* 密码强度校验（纯函数，无副作用）。返回 0=通过；否则返回错误码并填 why。
 * err: 1 长度  2 缺大写  3 缺小写  4 缺特殊符号  5 连续序列超限  6 空指针 */
int  ae_ld_check_strength(const char *pw, char *why, size_t n);

/* 首启硬门第一步：校验强度 + 生成二次确认弹窗文案(含明文回显) + 进入冷却。
 * 返回 0 进入待确认，-1 强度不合格(why 说明)，-2 已设定(不可重置)。
 * 注意：pw 只在此调用栈内使用，绝不写入磁盘；回显只出现在 dialog 缓冲(内存/屏幕)。 */
int  ae_ld_provision_begin(ae_ld_t *c, const char *pw,
                           char *dialog, size_t dialog_n,
                           char *why, size_t why_n);

/* 首启硬门第二步：确认按钮必须过 10 秒冷却。now_ms 由 HAL 单调时钟给出。
 * 返回 0 成功落锚(系统进入 NORMAL)，-1 冷却未满，-2 无待确认会话。 */
int  ae_ld_provision_confirm(ae_ld_t *c, uint64_t now_ms,
                             char *why, size_t why_n);

/* 是否已设定（供开机链判断硬门：未设定则停留在 OOBE，不放行进系统）。 */
int  ae_ld_is_provisioned(ae_ld_t *c);

/* 开机硬门：0=未设密码/已变砖（不放行进系统），1=允许进入系统。
 * 由 OOBE 开机链在 init 服务编排前调用（真机 S3 接入；云端 CLI 提供 status 可见）。 */
int  ae_ld_oobe_gate(ae_ld_t *c);

/* 喂一个物理事件给状态机。返回 0 已消费；>0 表示触发了熔断(见 ae_ld_brick)。
 *   NORMAL/GESTURE/ARMED：组合键推进；LOCKED：USB/拆机/短接 → 熔断。 */
int  ae_ld_on_event(ae_ld_t *c, ae_hal_event_t ev, uint64_t now_ms,
                    char *msg, size_t n);

/* 安全锁定模式解锁：仅专用密码。返回 0 成功退出到 NORMAL，-1 密码错，
 * -2 当前不在 LOCKED，-3 已变砖(任何方式都退不出)。 */
int  ae_ld_unlock(ae_ld_t *c, const char *pw, char *msg, size_t n);

/* 熔断：抹除全部数据 + 系统本体(sys/ 也删)，报警，落变砖标记。
 * 与常规 wipe(系统保留)严格区分。返回 0 成功。 */
int  ae_ld_brick(ae_ld_t *c, const char *trigger, char *msg, size_t n);

/* 诊断/取证：验证锚是否落盘、密码明文是否出现在任何文件中（外部可观测用）。
 * scan_plaintext：扫描 root 下全部文件，若发现 pw 明文字节串返回 1，0 未发现。 */
int  ae_ld_anchor_exists(ae_ld_t *c);
int  ae_ld_scan_plaintext(ae_ld_t *c, const char *pw);

#endif /* AETHER_LOCKDOWN_H */
