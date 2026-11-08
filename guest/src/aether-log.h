/*
 * aether-log.h - AetherOS PC 共享日志原语
 *
 * 设计目标：极简、无依赖、可在 PID1 早期与所有守护中复用。
 * 日志统一写到 fd=2（由 init 在启动早期重定向到串口/内核日志），
 * 前缀形如 [ident][TAG] 并带相对启动秒数，便于在 QEMU 串口输出里排序。
 */
#ifndef AETHER_LOG_H
#define AETHER_LOG_H

#include <stdarg.h>

/* 设置进程标识（写进每条日志前缀），可重复调用 */
void aether_log_ident(const char *ident);

/* 通用日志：tag 为模块名，fmt 同 printf */
void aether_log(const char *tag, const char *fmt, ...);

/* 致命日志：打印后可选终止 */
void aether_log_fatal(const char *tag, const char *fmt, ...);

#endif /* AETHER_LOG_H */
