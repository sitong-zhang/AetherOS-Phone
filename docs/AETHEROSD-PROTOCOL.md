# aetherosd IPC 契约（AetherBus on Linux）

> 本文档定义 AetherOS 在**真 Linux 内核**上用户空间守护进程 `aetherosd` 的 IPC 契约。
> 它是 `AetherOS.md` 中 `AetherBus` / `AetherService` 在 Linux 后端的真实落地形态，也是外壳「系统自检」页在真 OS 运行时对接实时状态的规范来源。
> 设计纪律（原则 7：禁止假实现冒充真实现）：**读不到的字段一律保持 `"unknown"` / `-1` / `false`，绝不编造值。**

---

## 1. 部署形态

| 项 | 值 |
|---|---|
| 二进制 | `aetherosd`（由 `aetherosd.c` 编译；真 OS 用 `aetherosd-aarch64-static`，静态链接） |
| 宿主 | 已启动的 AetherOS Linux 内核（QEMU `-M virt` 或真 ARM64 硬件） |
| 启动 | 由 AetherOS 原生 init（`aether-init`，PID 1）按 `/etc/aether/services` 服务清单拉起 |
| 状态文件 | `/run/aetheros.state`（JSON，周期刷新，供 init 早期取证 / 非 socket 消费方） |
| IPC 通道 | Unix domain socket `AF_UNIX` 流式，**文件系统路径** `/run/aetheros.sock` |

---

## 2. 运行模式

| 调用 | 行为 |
|---|---|
| `aetherosd` | 守护进程：写 `/run/aetheros.state` + 监听 `/run/aetheros.sock`，每连接返回一次实时 JSON；每 5 轮（~10s）刷新状态文件。 |
| `aetherosd -1` | 单次采集：写 JSON 到状态文件并退出（init 早期取证用）。 |
| `aetherosd -c` | 客户端：连接 socket，打印一次实时 JSON；不可达则退出码 1 + `cannot reach /run/aetheros.sock`。 |

---

## 3. 快照 JSON Schema（`aetherosd.state/1`）

每个字段均由 `/proc` / `/sysfs` / 系统文件**实时读取**。类型固定，消费方可稳定解析。

```json
{
  "schema": "aetherosd.state/1",
  "backend": "linux-native",
  "collected_at": 1791700440,
  "build": "dev",
  "kernel": "Linux version 6.6.70 ...",
  "hostname": "a2761c2c76a9",
  "os_release": "PRETTY_NAME=\"Ubuntu 24.04.3 LTS\"",
  "uptime_s": 1411,
  "memory": { "total_kb": 129308484, "available_kb": 109467416, "cached_kb": 49119868 },
  "cpus": 32,
  "mounts": 108,
  "root_volume": { "mount": "/", "fstype": "overlay" },
  "net_ifaces": 3,
  "virtio_disk": false,
  "share_mounted": false
}
```

| 字段 | 类型 | 来源 | 读不到时 |
|---|---|---|---|
| `schema` | string | 常量 `aetherosd.state/1` | 恒有 |
| `backend` | string | 常量 `linux-native` | 恒有 |
| `collected_at` | integer (unix sec) | `time(NULL)` | 恒有 |
| `build` | string | 编译宏 `AETHEROS_BUILD`（默认 `dev`；真 OS 注入版本号） | `dev` |
| `kernel` | string | `/proc/version` 首行 | `"unknown"` |
| `hostname` | string | `/etc/hostname` 首行 | `"unknown"` |
| `os_release` | string | `/etc/os-release` 首行 | `"unknown"` |
| `uptime_s` | number | `/proc/uptime` 首字段 | `-1` |
| `memory.total_kb` | integer | `/proc/meminfo` `MemTotal` | `-1` |
| `memory.available_kb` | integer | `/proc/meminfo` `MemAvailable` | `-1` |
| `memory.cached_kb` | integer | `/proc/meminfo` `Cached` | `-1` |
| `cpus` | integer | `count_lines(/proc/cpuinfo,"processor")` | `-1` |
| `mounts` | integer | `count_lines(/proc/mounts)` | `-1` |
| `root_volume.mount` | string | 常量 `/` | 恒有 |
| `root_volume.fstype` | string | `find_mount("/",...)` | `"unknown"` |
| `net_ifaces` | integer | `count_lines(/proc/net/dev) - 2` | `-1` |
| `virtio_disk` | boolean | `access("/dev/vda", F_OK)==0` | `false` |
| `share_mounted` | boolean | `find_mount("/mnt/share",...)` | `false` |

**采集原语约束（踩坑后定）**：只使用对常规小文件的 `fopen`/`fgets`，不使用 `statvfs()`，不递归遍历 procfs 目录树——`statvfs` 遇 fuse/不可达挂载会永久阻塞，`readdir("/proc")` 在进程多时开销失控。故本契约「最小且必然返回」，永不因采集卡死。

---

## 4. 消费方对接

### 4.1 native SystemUI / 系统监视器（真 OS 内）
- 直连 `/run/aetheros.sock`（`AF_UNIX` `SOCK_STREAM`），读一次 JSON 即关闭；或轮询 `/run/aetheros.state`。
- 解析 `memory.*_kb`、`cpus`、`uptime_s` 等渲染状态栏 / 系统监视器。

### 4.2 外壳「系统自检」页（Android 宿主 / 真 OS 双场景）
- 在 Android 宿主（Simulator）上：`/run/aetheros.sock` 通常不可达 → 降级到读宿主 `/proc/version`、`ActivityManager`、`StatFs`、电量（即 `SystemCheckActivity` 现有真实读取），并展示 `aether-linux` 真实内核证据（缓存自 `android-boot.log`，已 `[LINUX BACKEND]` 标注）。
- 在真 OS（外壳作为原生应用运行于 AetherOS Linux）上：尝试 `LocalSocket`(`/run/aetheros.sock`, `Namespace.FILESYSTEM`)；可达则解析 JSON 实时展示并标 `[LINUX BACKEND] live`；不可达则同上降级。
- 无论哪种场景：**绝不**用 socket 失败来伪造「无系统」，只切换数据来源并诚实标注。

### 4.3 契约稳定性承诺
- `schema` 字段用于版本协商；消费方应校验 `schema == "aetherosd.state/1"` 再解析。
- 新增字段为**向后兼容追加**（不删不改既有字段类型）。破坏性变更将提升 `schema` 次版本（如 `aetherosd.state/2`）。

---

## 5. 访问控制（2026-10-18，P0-3）

| 层 | 机制 | 强度 |
|---|---|---|
| 内核准入 | socket 文件 mode **0660**（`bind` 后 `chmod`）；无权用户 `connect()` 直接 EACCES | 最高，不依赖应用代码 |
| 客户端校验 | aether-core `SameUidAccessPolicy`：连接前校验 socket 属主 uid 与自身一致（`unix:uid` 扩展属性），防伪 socket 中间人 | 中 |
| 服务端对端凭证 | SO_PEERCRED 按对端 uid/gid/pid 精细准入 | **未实现**（纯 JDK 不暴露，需 JNI）——如实标注，不冒充 |

QEMU 取证（2026-10-18）：guest 内 `aetherosd: IPC socket online at /run/aetheros.sock (mode 0660)`，
`ls -la /run/aetheros.sock` → `srw-rw----`。

## 6. 传输层（2026-10-18，P0-3：AetherBus → UDS）

`aether-core` 模块（`org.aetheros.core.ipc.transport`）提供跨进程传输，**零新增依赖**
（仅 kotlin-stdlib + kotlinx-coroutines，UNIX socket 用 JDK 原生 JEP 380 API）：

| 组件 | 职责 |
|---|---|
| `BusMessage` + `BusFrame` | 线上消息（`publish`/`snapshot`/`ping`）；帧 = 4 字节大端长度 + UTF-8 JSON，上限 16 MiB |
| `BusTransport` | 连接抽象：`incoming: SharedFlow` + `suspend send` + `close` |
| `InMemoryBusTransport` | 进程内实现（测试/同进程多模块）；发送者不收自己的发布 |
| `UdsBusTransport` / `UdsBusServer` | JDK 原生 UDS 客户端/服务端；服务端 bind 后设 0660 |
| `AccessPolicy` | 访问策略接口（`SameUidAccessPolicy` / `AllowAllPolicy`） |
| `AetherosdClient` | 本契约的 JVM 客户端：连接 → 读至 EOF → 校验 schema → `AetherosdSnapshot` |

**跨语言现场取证**（`LiveAetherosdContractTest`，59 项测试全绿）：真实 C `aetherosd`
（宿主编译）× Kotlin `AetherosdClient` 经真 UDS 互通，uptime 6002→6006s 递增，
C 侧 `aetherosd -c` 与 Kotlin 侧读同一总线数据一致；daemon 退出后 socket 被清理。

**踩坑记录**：JDK 的 `unix:mode` 扩展属性只接受 `Integer`（八进制），传
`Set<PosixFilePermission>` 会 `ClassCastException`——已改用 Integer 并留测试防回归。

## 7. 真机验证证据（2026-10-11）

在本 Linux 宿主原生编译并运行 `aetherosd -1` / `-c`，确认：

```
$ aetherosd -1
  kernel        : Linux version 6.6.117-45.11.6.tl4.x86_64 ... #1 SMP ...
  hostname      : a2761c2c76a9
  os-release    : PRETTY_NAME="Ubuntu 24.04.3 LTS"
  uptime_s      : 1405
  memory        : total 129308484 kB / available 109397580 kB / cached 49118688 kB
  cpus          : 32
  root_volume   : / (overlay)
$ aetherosd -c            # 经 UDS 取到实时 JSON（collected_at / uptime 每次递增）
$ aetherosd -c            # 不可达时：exit 1, "cannot reach /run/aetheros.sock"
```

> 结论：`aetherosd` 是真 Linux 后端的**可验证实体重**，非纸面架构。其数据来自本机 `/proc`/`/sysfs`，与 QEMU 内启动的 AetherOS 内核一致。
