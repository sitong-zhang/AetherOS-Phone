/*
 * aether-logctl.c - aether-logd 的手工/测试客户端（P2-13a）。
 *
 * 用法:
 *   aether-logctl <service> <level> <message...>
 *     level: debug|info|warn|error|critical
 *
 * 经 aether-log.h 客户端库发送——logd 缺席时自动降级 kmsg，
 * 退出码恒 0（日志路径失败不冒充业务失败，stderr 会说明）。
 */
#include "aether-log.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

static int parse_level(const char *s)
{
    if (!strcasecmp(s, "debug"))    return AETHER_LOG_DEBUG;
    if (!strcasecmp(s, "info"))     return AETHER_LOG_INFO;
    if (!strcasecmp(s, "warn"))     return AETHER_LOG_WARN;
    if (!strcasecmp(s, "error"))    return AETHER_LOG_ERROR;
    if (!strcasecmp(s, "critical")) return AETHER_LOG_CRITICAL;
    return -1;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr,
                "usage: %s <service> <debug|info|warn|error|critical> <message...>\n",
                argv[0]);
        return 2;
    }
    int level = parse_level(argv[2]);
    if (level < 0) {
        fprintf(stderr, "aether-logctl: unknown level '%s'\n", argv[2]);
        return 2;
    }
    aether_log_init(argv[1]);

    /* 消息可能是多段 argv——原样拼接（保留用户空格分隔语义）。 */
    char msg[1024];
    size_t off = 0;
    for (int i = 3; i < argc && off + 1 < sizeof msg; i++) {
        int n = snprintf(msg + off, sizeof msg - off, "%s%s",
                         i > 3 ? " " : "", argv[i]);
        if (n < 0 || (size_t)n >= sizeof msg - off) break;
        off += (size_t)n;
    }

    aether_log(level, "%s", msg);
    if (!aether_log_using_daemon())
        fprintf(stderr, "aether-logctl: aether-logd unreachable — record went to /dev/kmsg fallback\n");
    return 0;
}
