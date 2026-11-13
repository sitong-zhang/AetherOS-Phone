# 第 1 门存量命中归因（v2 §5.6）

> 由 `tools/gate_l2.py` 扫描生成，机器可判；命中即失败，不设豁免（§11.3）。
> 本文件是**已知不合格项清单**，S2 阶段待整改；不许用「已知问题」糊过去，
> 必须逐项写清卡在哪、需要什么条件（§2.7 规矩 2）。

合计 **56 处 / 13 个文件**。

## 按文件

| 文件 | 命中数 |
|---|---|
| `guest/src/aether-settings.c` | 17 |
| `guest/src/aether-apps.c` | 7 |
| `guest/src/aether-apps2.c` | 5 |
| `guest/src/aether-android.c` | 5 |
| `guest/src/aether-compositor.c` | 4 |
| `guest/src/aether-display.c` | 4 |
| `guest/src/aether-init.c` | 4 |
| `guest/src/aetherosd.c` | 3 |
| `guest/src/aether-phone.c` | 2 |
| `guest/src/aether-android.h` | 2 |
| `guest/src/aether-discover.c` | 1 |
| `guest/src/aether-android-bridge.c` | 1 |
| `guest/src/aether-android-util.c` | 1 |

## 按路径类型

| 类型 | 命中数 |
|---|---|
| `/proc/` | 30 |
| `/dev/` | 18 |
| `/sys/class/` | 8 |

## 整改条件（S2，按一类问题逐轮处理，§2.7 规矩 1）

1. `/dev/fb0`、`/dev/dri/*`：当前在 `aether-display.c`/`aether-drm.c` 直开。
   需把「打开/模式设置/帧提交/同步事件」收敛到 L2 `AETHER_DOM_DISPLAY`，
   上层只调 `aether_hal_submit/poll`，不得再出现设备节点字面量。
2. `/proc/*`（stat/meminfo/version/uptime/cpuinfo 等）：当前散落在 `aether-apps*.c`、
   `aether-settings.c`、`aether-compositor.c` 等读系统指标。
   需统一由 L2 提供「系统信息」能力接口，云端由仿真注入，真机由实现层读取。
3. `/sys/class/*`：当前用于电源/热/亮度等。需收敛到 `AETHER_DOM_POWER`、
   `AETHER_DOM_THERMAL`、`AETHER_DOM_DISPLAY` 的能力查询与状态接口。
4. 每轮只改一类；改完后 `scripts/aether.sh test` 重跑第 1 门，
   新建层与存量都零命中才算该门通过。

## 逐条明细

| 文件 | 行 | 代码 |
|---|---|---|
| `guest/src/aether-apps.c` | 512 | `unsigned long tot = read_kv("/proc/meminfo", "MemTotal:");` |
| `guest/src/aether-apps.c` | 513 | `unsigned long av  = read_kv("/proc/meminfo", "MemAvailable:");` |
| `guest/src/aether-apps.c` | 516 | `FILE *f = fopen("/proc/version", "r");` |
| `guest/src/aether-apps.c` | 519 | `f = fopen("/proc/uptime", "r");` |
| `guest/src/aether-apps.c` | 521 | `f = fopen("/proc/loadavg", "r");` |
| `guest/src/aether-apps.c` | 523 | `f = fopen("/proc/cpuinfo", "r");` |
| `guest/src/aether-apps.c` | 574 | `snprintf(p, sizeof p, "/proc/%s/comm", de->d_name);` |
| `guest/src/aether-apps2.c` | 311 | `FILE *f = fopen("/proc/version", "r");` |
| `guest/src/aether-apps2.c` | 314 | `f = fopen("/proc/cpuinfo", "r");` |
| `guest/src/aether-apps2.c` | 327 | `f = fopen("/proc/meminfo", "r");` |
| `guest/src/aether-apps2.c` | 338 | `f = fopen("/proc/uptime", "r");` |
| `guest/src/aether-apps2.c` | 340 | `f = fopen("/proc/loadavg", "r");` |
| `guest/src/aether-compositor.c` | 74 | `int con = open("/dev/console", O_WRONLY);` |
| `guest/src/aether-compositor.c` | 102 | `FILE *f = fopen("/proc/stat", "r");` |
| `guest/src/aether-compositor.c` | 124 | `f = fopen("/proc/meminfo", "r");` |
| `guest/src/aether-compositor.c` | 172 | `snprintf(p, sizeof p, "/dev/input/event%d", i);` |
| `guest/src/aether-discover.c` | 464 | `FILE *uf = fopen("/proc/uptime", "r");` |
| `guest/src/aether-display.c` | 61 | `int fd = open("/dev/fb0", O_RDWR);` |
| `guest/src/aether-display.c` | 204 | `if (aether_drm_open(dr, "/dev/dri/card0", res) == 0) {` |
| `guest/src/aether-display.c` | 220 | `if (aether_drm_open(dr, "/dev/dri/card0", res) == 0) {` |
| `guest/src/aether-display.c` | 245 | `if (dr && aether_drm_open(dr, "/dev/dri/card0", res) == 0) {` |
| `guest/src/aether-init.c` | 68 | `int c = open("/dev/console", O_RDWR);` |
| `guest/src/aether-init.c` | 88 | `mkdir("/dev/pts", 0755);` |
| `guest/src/aether-init.c` | 89 | `mount_one("devpts", "/dev/pts", "devpts");` |
| `guest/src/aether-init.c` | 200 | `int c = open("/dev/console", O_RDWR);` |
| `guest/src/aether-phone.c` | 747 | `FILE *f = fopen("/sys/class/power_supply/BAT0/capacity", "r");` |
| `guest/src/aether-phone.c` | 748 | `if (!f) f = fopen("/sys/class/power_supply/BAT1/capacity", "r");` |
| `guest/src/aether-settings.c` | 159 | `int c = read_int_path("/sys/class/power_supply/BAT0/capacity", -1);` |
| `guest/src/aether-settings.c` | 160 | `if (c < 0) c = read_int_path("/sys/class/power_supply/BAT1/capacity", -1);` |
| `guest/src/aether-settings.c` | 161 | `if (c < 0) c = read_int_path("/sys/class/power_supply/battery/capacity", -1);` |
| `guest/src/aether-settings.c` | 236 | `FILE *f = fopen("/proc/net/dev", "r");` |
| `guest/src/aether-settings.c` | 282 | `FILE *f = fopen("/proc/asound/cards", "r");` |
| `guest/src/aether-settings.c` | 292 | `read_first_line("/sys/class/power_supply/BAT0/status", b, sizeof b);` |
| `guest/src/aether-settings.c` | 293 | `if (!b[0]) read_first_line("/sys/class/power_supply/battery/status", b, sizeof b);` |
| `guest/src/aether-settings.c` | 296 | `int temp = read_int_path("/sys/class/thermal/thermal_zone0/temp", -1);` |
| `guest/src/aether-settings.c` | 300 | `read_first_line("/proc/uptime", b, sizeof b);` |
| `guest/src/aether-settings.c` | 302 | `read_first_line("/proc/loadavg", b, sizeof b);` |
| `guest/src/aether-settings.c` | 427 | `read_first_line("/proc/uptime", b, sizeof b);` |
| `guest/src/aether-settings.c` | 453 | `if (read_first_line("/proc/version", ver, sizeof ver)) {` |
| `guest/src/aether-settings.c` | 490 | `FILE *f = fopen("/proc/cpuinfo", "r");` |
| `guest/src/aether-settings.c` | 505 | `if (read_first_line("/proc/version", ver, sizeof ver)) ver[80] = 0;` |
| `guest/src/aether-settings.c` | 690 | `if (read_first_line("/proc/version", v, sizeof v)) fprintf(f, "内核: %s\n", v);` |
| `guest/src/aether-settings.c` | 691 | `if (read_first_line("/proc/uptime", v, sizeof v)) fprintf(f, "运行时间: %s\n", v);` |
| `guest/src/aether-settings.c` | 692 | `if (read_first_line("/proc/loadavg", v, sizeof v)) fprintf(f, "负载: %s\n", v);` |
| `guest/src/aetherosd.c` | 42 | `FILE *f = fopen("/proc/stat", "r");` |
| `guest/src/aetherosd.c` | 78 | `FILE *f = fopen("/proc/meminfo", "r");` |
| `guest/src/aetherosd.c` | 102 | `FILE *f = fopen("/proc/uptime", "r");` |
| `guest/src/aether-android.h` | 37 | `#define AETHER_ANDROID_UINPUT   "/dev/uinput"` |
| `guest/src/aether-android.h` | 38 | `#define AETHER_ANDROID_BINDERFS "/dev/binderfs"` |
| `guest/src/aether-android.c` | 95 | `path_exists("/dev/binder") \|\| path_exists("/dev/binderfs/binder")) ? 1 : 0;` |
| `guest/src/aether-android.c` | 96 | `c->fb0    = path_exists("/dev/fb0") ? 1 : 0;` |
| `guest/src/aether-android.c` | 98 | `path_exists("/dev/input/uinput")) ? 1 : 0;` |
| `guest/src/aether-android.c` | 146 | `if (path_exists("/dev/fb0")) bind_dev("/dev/fb0", "dev/fb0");` |
| `guest/src/aether-android.c` | 148 | `else if (path_exists("/dev/input/uinput")) bind_dev("/dev/input/uinput", "dev/uinput");` |
| `guest/src/aether-android-bridge.c` | 82 | `const char *paths[] = { AETHER_ANDROID_UINPUT, "/dev/input/uinput", NULL };` |
| `guest/src/aether-android-util.c` | 27 | `FILE *f = fopen("/proc/cmdline", "r");` |
