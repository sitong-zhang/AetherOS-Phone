/*
 * aether-android-util.c - 兼容层共享工具（无 main，服务与合成器都可链接）
 *
 * 提供状态名、/proc/cmdline 参数读取、状态文件读取。放在独立编译单元，
 * 避免与 aether-android.c 的 main 冲突，也让 compositor 只依赖这一小份代码，
 * 不把容器启动与服务控制逻辑链进显示路径。
 */
#include "aether-android.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *aether_android_state_name(int state)
{
    switch (state) {
    case AANDROID_UNAVAILABLE: return "unavailable";
    case AANDROID_PROBING:     return "probing";
    case AANDROID_RUNNING:     return "running";
    case AANDROID_FAILED:      return "failed";
    default:                   return "unknown";
    }
}

int aether_android_cmdline_int(const char *key, int def)
{
    FILE *f = fopen("/proc/cmdline", "r");
    if (!f) return def;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    char *p = strstr(buf, key);
    if (!p) return def;
    p += strlen(key);
    return (int)strtol(p, NULL, 10);
}

int aether_android_status_int(const char *key, int def)
{
    FILE *f = fopen(AETHER_ANDROID_STATUS, "r");
    if (!f) return def;
    char line[256];
    int val = def;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, strlen(key)) == 0 && line[strlen(key)] == '=') {
            val = (int)strtol(line + strlen(key) + 1, NULL, 10);
            break;
        }
    }
    fclose(f);
    return val;
}

int aether_android_status_read(char *buf, size_t sz)
{
    FILE *f = fopen(AETHER_ANDROID_STATUS, "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, sz - 1, f);
    fclose(f);
    buf[n] = 0;
    return 0;
}
