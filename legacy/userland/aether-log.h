/*
 * aether-log.h - AetherOS 序列化日志客户端库（P2-13a）。
 *
 * 所有系统服务（aether-init / aether-compositor / aether-watchdog ...）
 * 通过本库把日志记录发往 aether-logd（Unix datagram socket，
 * /run/aether-log.sock）。记录为单行文本协议：
 *
 *   <ver> <uptime_s> <LEVEL> <service> <pid> <message...>
 *
 *   - ver      : 协议版本，当前 1
 *   - uptime_s: 发送方 /proc/uptime 读数（-1 表示不可读，绝不编造）
 *   - LEVEL   : DEBUG / INFO / WARN / ERROR / CRITICAL
 *   - service : 服务名（aether_log_init 设定）
 *   - message : 其余全部字段（可含空格）
 *
 * 降级路径（诚实原则）：aether-logd 缺席或 socket 不可达时，记录直写
 * /dev/kmsg（printk），永不静默丢弃。发送失败不会阻塞调用方——datagram
 * 语义，失败即刻返回；节流策略：连续失败期间每 64 条重试一次 socket
 * （logd 可能晚于调用方启动），其余走 kmsg 降级。
 */
#ifndef AETHER_LOG_H
#define AETHER_LOG_H

#define AETHER_LOG_DEBUG    0
#define AETHER_LOG_INFO     1
#define AETHER_LOG_WARN     2
#define AETHER_LOG_ERROR    3
#define AETHER_LOG_CRITICAL 4

/* 记录本进程的服务名（进程启动时调用一次；不调用则记为 "unknown"）。 */
void aether_log_init(const char *svcname);

/* 发一条日志记录。fmt 为 printf 格式。绝不阻塞、绝不 abort。 */
void aether_log(int level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* 级别号 -> 名字（"INFO" 等）；越界返回 "?"。 */
const char *aether_log_level_name(int level);

/* 当前是否仍在尝试 socket 主路径（false = 已处于 kmsg 降级重试间隔）。 */
int aether_log_using_daemon(void);

#endif /* AETHER_LOG_H */
