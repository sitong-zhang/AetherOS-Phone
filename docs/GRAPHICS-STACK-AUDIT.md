# AetherOS 图形栈工程审计（P0-6）

> 审计目标：核对 P0-6 图形栈是否「真实驱动显示硬件、而非伪装」。
> 审计原则（AetherOS 铁律）：不假实现冒充真实现；不重造 Linux 内核已成熟子系统；必须看实际代码 / 调用路径 / 数据流 / 测试。
> 审计日期：2026-10-18。审计对象：内核 `aetheros-kernel-6.6.70-aarch64.Image` + 用户空间 `aether-drm.*` / `aether-render.*` / `aether-compositor.c`。

---

## 1. 架构决策与选型核对

| 选型项 | 决策 | 是否符合铁律 |
|---|---|---|
| 是否重造内核 KMS | **否**——直接吃 `drm` 字符设备，原子 KMS 提交 | ✅ 不重造成熟子系统 |
| 是否引入 Wayland/Weston | **否**——绕过 Android 兼容层，原生用户空间直驱 | ✅ 无第三方会话管理依赖 |
| 是否用 libdrm | **否**——裸 `ioctl` 封装（`aether-drm.c`） | ✅ 最小依赖，且精确掌握契约 |
| 渲染方式 | 软件光栅化（CPU 逐像素 XRGB8888） | ✅ 诚实标注为软件渲染，非 GPU 加速 |
| 字体来源 | DejaVu Sans Mono 经 `tools/gen_font.py` 二值化（可再生产） | ✅ 来源可溯，非手绘猜测 |

**结论**：架构选型与 AetherOS 铁律一致——底座是 Linux 内核，图形栈是其上的薄用户空间层，没有「假装成硬件/假装成 Android 图形栈」。

---

## 2. DRM ioctl 两轮式契约（核心取证）

内核 DRM 子系统对一批查询类 ioctl 采用统一的「**第一轮 `count=0` 探测数量 → 第二轮填指针数组回填**」契约。这条契约不是文档约定，而是**内核源码的硬行为**——第二轮对所有 `count>0` 的类别都执行 `put_user` / `copy_to_user`，任何指针数组漏填（保持 NULL）即返回 `-EFAULT`（Bad address）。

涉及的 ioctl 及取证路径（内核 6.6.70 源码）：

| ioctl | 两轮式数组 | 内核源码行为 |
|---|---|---|
| `DRM_IOCTL_MODE_GETRESOURCES` | `connector_id_ptr` / `crtc_id_ptr` / `encoder_id_ptr`（三者必须全填） | `drm_mode_getresources`：对每个 `count_*` 非零的数组 `put_user` 到用户指针 |
| `DRM_IOCTL_MODE_GETCONNECTOR` | `modes_ptr` / `encoders_ptr` / `props_ptr` / `prop_values_ptr` | `drm_mode_getconnector`：同上 |
| `DRM_IOCTL_MODE_GETPLANERESOURCES` | `plane_id_ptr` | `drm_mode_getplane_res` |
| `DRM_IOCTL_MODE_OBJ_GETPROPERTIES` | `props_ptr` / `prop_values_ptr` | `drm_mode_obj_get_properties` |

**属性读取路径取证**（为什么 primary plane 的 type 值能正确读回）：

```
drm_mode_obj_get_properties
  → __drm_object_property_get_value
       ├─ prop 为 DRM_MODE_PROP_IMMUTABLE → 读 obj->properties->values[idx]（attach 时的 init_val）
       └─ 否则 → drm_atomic_get_property（走 atomic 状态）
```

virtio-gpu 的 plane type 属性在 `drm_universal_plane_init` 中以 `DRM_MODE_PROP_IMMUTABLE` 创建，值取自 `plane->type`（内核 `enum drm_plane_type`：`OVERLAY=0` / `PRIMARY=1` / `CURSOR=2`）。因此 OBJ_GETPROPERTIES 对 IMMUTABLE type 属性回填的是 `values[]` 中的 `plane->type`，**内核无 bug，行为自洽**。

**cursor plane 过滤取证**：`drm_mode_getplane_res` 中——

```c
if (plane->type == DRM_PLANE_TYPE_CURSOR &&
    drm_core_check_feature(dev, DRIVER_CURSOR_HOTSPOT) &&
    file_priv->atomic && !file_priv->supports_virtualized_cursor_plane)
        continue;  /* 跳过 */
```

我们开启了 `DRIVER_CAP_ATOMIC` 能力，因此 virtio-gpu 的 cursor plane 被内核有意不报，GETPLANERESOURCES 只返回 primary plane。这与 debugfs `/sys/kernel/debug/dri/0/state` 的权威状态完全吻合（`plane[31]: plane-0` 绑定 `crtc=crtc-0`，`fb=37` 由 fbcon 分配；`plane[32]: plane-1` 为空闲 cursor）。

---

## 3. UAPI 常量与结构体陷阱（审计发现的致命陷阱）

### 3.1 本地补齐常量数值抄错（本审计最严重的一类人为错误）

Debian aarch64 sysroot 头文件**缺失** `DRM_PLANE_TYPE_*` 与 `DRM_MODE_CONNECTED`（`drm_connector.h` 不存在于 sysroot，libdrm 用户空间契约未提供）。`aether-drm.h` 用 `#ifndef` 本地补齐，但初版数值**抄反**：

| 常量 | 内核 `enum drm_plane_type`（drm_plane.h:563）真值 | 初版本地值 | 后果 |
|---|---|---|---|
| `DRM_PLANE_TYPE_OVERLAY` | **0** | （未定义） | — |
| `DRM_PLANE_TYPE_PRIMARY` | **1** | ~~0~~ ❌ | primary plane 回填值 1 被误判为非 primary |
| `DRM_PLANE_TYPE_CURSOR` | **2** | ~~1~~ ❌ | — |

**因果链**：virtio-gpu `plane->type = DRM_PLANE_TYPE_PRIMARY`（内核 enum=1）→ `drm_object_attach_property(..., plane->type)` 写入 `values[]` → OBJ_GETPROPERTIES 回填 `values[i]=1` → 代码用错误的本地宏 `0` 去比 → `1 != 0` → "no usable primary plane found"。

debugfs 铁证（`plane[31]` 是 primary）与内核源码自洽，但**内核无 bug，是我抄错 UAPI 常量数值**。修复：按 `drm_plane.h` 真实 enum 修正，并在 `aether-drm.h` 标注 ABI 出处注释。

### 3.2 必须用内核 UAPI 结构体

`DRM_IOCTL_MODE_OBJ_GETPROPERTIES` 的早期实现误用自定义 `struct obj_props` 当 ioctl 参数——布局与内核 `struct drm_mode_obj_get_properties` 完全不同，属纯未定义行为（此前几处"修好"是错位巧合）。修复：彻底改用内核 UAPI 结构体两轮式。

### 3.3 其他 sysroot 差异

- `drm_mode_fb_cmd2`：sysroot 无 `pitch` 单一成员，改用 `pitches[0]`（新布局 `handles[4]/pitches[4]/offsets[4]/modifier[4]`）；fourcc 需 `#include <drm/drm_fourcc.h>`。
- `drm_mode_obj_get_properties`：成员名为 `prop_values_ptr`（曾误写 `enum_values_ptr`）。

---

## 4. 已修复缺陷清单（全部根源可溯）

| # | 缺陷 | 现象 | 根因 | 修复 | 是否伪造 |
|---|---|---|---|---|---|
| 1 | 指针数组漏填 | GETRESOURCES/GETCONNECTOR `EFAULT` | 两轮式 ioctl 任意 `count>0` 指针数组漏填即内核 `put_user` 到 NULL | 全数组配套 alloc 填充；补 `free` 防泄漏 | 非伪造，内核契约 |
| 2 | UAPI 结构体误用 | OBJ_GETPROPERTIES `ENOENT` | 自定义结构体布局 ≠ `drm_mode_obj_get_properties` | 直接复用 UAPI 结构体两轮式 | 非伪造，UB |
| 3 | prop id 连续性假设 | first_modeset 错位 | `p_crtc=p_fb+1` 假设 id 连续；crtc 名写入相邻栈变量（依赖栈布局） | 显式数组按名接收 prop id | 非伪造，栈 UB |
| 4 | **plane type 常量数值** | 永远找不到 primary plane | 本地 `DRM_PLANE_TYPE_*` 数值抄错 | 按内核 enum 修正 | 非伪造，人为错误 |
| 5 | 双缓冲绑定 | J10 后屏显全黑 | `fb.px` 启动绑定 `map[0]` 后不更新，始终画旧 back buffer | `render_frame` 绘制前重绑当前 back buffer | 非伪造，渲染路径 |
| 6 | 客户端参数截断 | 写命令全 `unknown command` | `client_mode(argv[2])` 只取首词 | join 全部参数 | 非伪造，CLI |

**审计结论**：以上 6 类缺陷均为「契约误解 / 人为抄错 / 渲染状态机疏忽」，无任何一项是「用假数据冒充真实显示」。每处修复都回到内核源码或真实数据流核对，符合「看实际代码/数据流」铁律。

---

## 5. 验证证据

### 5.1 断言链（boot-smoke.py，QEMU 实跑）

| 段 | 断言主题 | 项数 | 结果 |
|---|---|---|---|
| A | 启动取证（boot diag 完成 / PID1 身份 / 服务拓扑） | 3 | ✅ |
| B | getty 登录 | 2 | ✅ |
| C | PID 1 comm=aether-init | 1 | ✅ |
| D | 服务拓扑（PID1/aetherosd/getty 存活且 PPID=1） | 3 | ✅ |
| E | aetherosd socket mode 0660 | 1 | ✅ |
| F | IPC 契约（schema/字段/活数据 uptime 递增） | 6 | ✅ |
| G | 9p hostshare 挂载 | 2 | ✅ |
| H | kill -9 崩溃自恢复（新 PID 与 init 报告一致 + 重启后 IPC 仍成立） | 7 | ✅ |
| I | 优雅停机（PSCI poweroff，QEMU 自行退出） | 3 | ✅ |
| J | **图形栈（card0 / comp socket / 进程 / IPC / mode / windows / hash / screenshot / close / 多进程 demo / 中文点阵上屏）** | 15 | ✅ |

正常通过 **44/44**（X0 异常守卫与 J8 失败中止路径不计正数）。图形栈 J 段全部通过。（P0-6 为 42/42；P1-1a 在 42/42 基础上将 J 段由 12 项扩展为 14 项并新增 J13/J14/J15，总成 43/43；P1-1b 新增 J16 中文点阵上屏，总成 **44/44**。）

### 5.2 视觉铁证

经 9p 取回的 `aether-comp-shot.ppm`（1280×800）目检为**真实桌面**：

- 深蓝→暗蓝垂直渐变背景；
- 左上 `aetherosd` 窗口：focused 态（亮青边框 + 焦点圆点）；
- 中央 `bootdiag` 窗口：非 focused 态（暗边框）；
- 8×16 字体清晰渲染出 `[OK] virtio-gpu KMS active` / `[OK] compositor serving IPC`；
- 底部带（x=40, y=752）可见中文状态行 `AetherOS 图形合成器 窗口系统已就绪`——16×16 CJK 位图真实上屏（P1-1b，J16 像素级核验 959 个近白汉字像素）；
- 像素采样：非黑比例约 18%，标题栏 / 正文 / focus 高亮 / 中文状态行四者 RGB 各不相同。

> 这是「真实逐像素绘制 + 真实 KMS page-flip 上屏」的直接证据，排除了占位画面与黑屏伪装。

### 5.3 数据流核对（排除「假画面」）

J10（focus 后帧哈希变化）与截图非黑证明：哈希变化来自**真实渲染**（`render_frame` 绘制 focus 高亮），且该帧经 `aether_drm_present`（原子 FB_ID 翻页 + `DRM_MODE_PAGE_FLIP_EVENT` + `wait_flip` poll `drm_event_vblank`）真实上屏；截图读 front buffer 得到非空内容。渲染 → 提交 → 上屏 → 取回，链路闭环无断点。

---

## 6. 诚实边界（明确未做 / 非伪造声明）

1. **软件渲染，非 GPU 加速**：光栅化为 CPU 逐像素。virtio-gpu 仅作 KMS 显示控制器，未启用 virgl/3D、未走 GEM 导入共享。不冒充硬件加速合成。
2. **单 plane 合成**：只用 primary plane，WM 自行叠加窗口；不支持多 plane 硬件合成 / overlays。
3. **字体为 8×16 ASCII（95 可打印字符）**：中文全角点阵（16×16+）未覆盖，标题/正文限 ASCII。
4. **evdev 输入未实接**：合成器预留 Tab/Esc poll 分支，但真实键盘事件采集未接入；焦点切换目前由 IPC 命令驱动。
5. **comm 截断已知**：内核 `TASK_COMM_LEN=16`（含 NUL），`aether-compositor`（16 字符）在 `ps` 中显示为 `aether-composit`；断言已用 `aether-composit(or)?` 正则兼容，非缺陷。

---

## 7. 审计结论

P0-6 图形栈**真实驱动显示硬件**，全程基于本机交叉编译的 Linux 6.6.70 (aarch64) + 真 QEMU 实跑，无模拟 / 伪装。架构遵循 AetherOS 铁律（不重造内核 KMS、不引入第三方图形会话）。开发过程中暴露的 6 类缺陷均属「契约误解 / 人为抄错 / 状态机疏忽」，已全部回到内核源码与真实数据流修复，并经 42/42 断言链 + 像素级视觉验证闭环。

**遗留项（如实登记，下一步路线）**：GPU 加速（virgl）、多 plane 合成、中文点阵字体、evdev 物理输入。

> **P1-1a 多进程窗口系统已于本轮落地**（独立进程经 SCM_RIGHTS 共享内存把像素上屏），详见 §8。

---

## 8. P1-1a 多进程窗口系统：SCM_RIGHTS 共享内存上屏（审计补遗）

> 审计对象（本轮新增）：`aether-demo.c`（独立客户端进程）、`aether-compositor.c` 的 `create/buf/commit` 三命令与 `recvmsg`/`SCM_RIGHTS` 接收路径、`aether-render.c` 的 shm blit。
> 审计日期：2026-10-18。验证手段：同 §5 的真 QEMU 实跑，`boot-smoke.py` 断言链升至 **43/43**。

### 8.1 选型核对（是否符合铁律）

| 选型项 | 决策 | 是否符合铁律 |
|---|---|---|
| 多进程窗口系统是否引入 Wayland 协议栈 | **否**——只取 `SCM_RIGHTS` 共享内存这一真机制，跳过 `wl_registry`/`xdg_surface` 会话管理 | ✅ 最小依赖、不重造 |
| 客户端像素上屏机制 | **Unix socket `SCM_RIGHTS` 传 memfd fd**（与 Wayland `wl_shm` 同一内核机制） | ✅ 真实机制、零拷贝、非自创协议 |
| 客户端与服务端是否同一进程 | **否**——`aether-demo` 是 `ps` 中独立 comm、PPID=1 的进程 | ✅ 真多进程，非 in-process 短路 |

**结论**：P1-1a 把 P0-6 的「合成器自绘固定窗口」升级为「独立进程经 IPC 注册窗口 + 共享内存上屏」，机制与 Wayland `wl_shm` 同源，符合「不假实现、不重造成熟子系统」铁律。

### 8.2 SCM_RIGHTS 机制取证（核心）

`SCM_RIGHTS` 是 Unix domain socket 的**辅助数据（ancillary data）**机制，定义在 `<sys/socket.h>`：发送方在 `sendmsg` 的 `msghdr.msg_control` 里挂一个 `cmsg`（`cmsg_level=SOL_SOCKET`、`cmsg_type=SCM_RIGHTS`、`cmsg_data` = `int[]` 文件描述符数组），内核在投递时把 fd **复制进接收方的 fd 表**（不是传数字，是传内核里的打开文件）。这正是 Wayland 客户端把 `wl_shm` 缓冲交给合成器的方式。

本次实现的取证点：

```
发送端 aether-demo.c:
  memfd_create("aether-demo", MFD_CLOEXEC | MFD_NOEXEC_SEAL)   // 6.6 内核要求 NOEXEC_SEAL
  ftruncate(memfd, CW*CH*4); mmap(MAP_SHARED)
  sendmsg: iov = "buf <id>\n"; cmsg(SCM_RIGHTS) = { memfd }    // 同一条消息带 fd

接收端 aether-compositor.c:
  recvmsg: CMSG_FIRSTHDR(msg) / CMSG_NXTHDR            // 提取 SCM_RIGHTS fd
  mmap(recv_fd, MAP_SHARED) -> win->shm                // 合成器映射同一块内存
  aether_render_window(): 若 win->has_shm 则 blit 进 fb  // 标题栏+边框预留，无越界
```

**memfd 内核约束取证**：Linux 6.6 引入 noexec memfd 强化（`MFD_NOEXEC_SEAL` / `MFD_EXEC`），`memfd_create` 不带二者之一会被内核拒绝或 warn。代码用 `MFD_CLOEXEC | MFD_NOEXEC_SEAL` 满足约束——这是 P1-1a 真实踩到的内核行为，非猜测。

**短连接模型取证**：合成器是「一连接一请求一应答后 `close`」的服务（与 `aethercompctl` 同款）。窗口状态存于合成器全局表、跨连接保留；因此客户端必须「每命令一条独立短连接」。这是 P0-6 早期 demo `SIGPIPE` 崩溃的根因（复用同 socket 发多条 → 第二条写到已关 socket），已在 `aether-demo.c` 按此模型重写（`connect_comp_retry` 250ms 退避 / 40 次；`signal(SIGPIPE, SIG_IGN)`）。

### 8.3 验证证据（QEMU 实跑，43/43）

在 §5.1 的 42/42 基础上，P1-1a 调整/新增 J 段断言，总断言数升至 **43/43（0 FAIL）**。关键新增项（取自 2026-10-18 `boot-smoke.summary.txt` 实跑）：

| 断言 | 期望 | 实跑结果 |
|---|---|---|
| J12 close 后 windows | ==2（剩 bootdiag+demo） | `windows=2` ✅ |
| J13 示例进程 PPID=1 | 独立进程上屏 | `68 1 S aether-demo` ✅ |
| J14 compositor 管理 demo 窗口 | list 含 `title=demo` | `list 含 title=demo` ✅ |
| J15 demo 像素真实上屏 | 截图红特征像素 >50 | **红色特征像素 1600 个** ✅ |

**像素级铁证**：J15 在 J11 已落盘的 `aether-comp-shot.ppm`（1280×800）上做全屏逐像素扫描，命中 `r>200 & g<100 & b<100` 的红色特征像素 **1600 个**（阈值 50）。该红色块是 demo 在它自己的 memfd 缓冲里画的 `0xFFFF3030`（明亮红），与 seeded 窗口的灰蓝色调完全不同；它出现在截图上，证明「**独立进程像素 → SCM_RIGHTS memfd → 合成器 mmap+blit → KMS 页翻上屏**」链路真实闭合，而非合成器内部写死的画面。

> CI 复现：`run-ci.sh --skip-core` 全链路（cross-demo 编译 → check-artifacts 4 文件 → rootfs 含 AETHER_DEMO → boot-smoke）→ `LOCAL_CI_OK`，与手动传参的 43/43 一致。

### 8.4 诚实边界（P1-1a）

1. **短连接模型是刻意取舍**：每命令一连接，简化服务端状态机（无长连接/心跳/鉴权）；代价是每命令一次 `connect` 开销，高频逐帧提交场景需改长连接——属下一步路线。
2. **零拷贝共享内存，非 GPU 合成**：客户端/合成器各自 `mmap` 同一 memfd，合成器仍 CPU blit 进 dumb buffer；未走 GEM 导入 / virgl。
3. **动画有限帧后静止**：demo 在 `ANIM_FRAMES=40` 帧后 `usleep` 保留末帧，既证动态提交又保证 J9 空闲哈希稳定可断言。
4. **无客户端协议版本协商**：`create/buf/commit` 为固定 schema，客户端与服务端须同源构建（同一 rootfs 内一致）。

## 9. P1-1b 中文点阵字体：UTF-8 混合渲染真实上屏（审计补遗）

> 审计对象（本轮新增）：`tools/gen_cjk_font.py`（来源可溯的字模生成器）、`aether-font-cjk.h`（生成产物）、`aether-render.c` 的 `aether_render_text_utf8` / `cjk_find` / `blit_glyph16`、桌面中文状态行 `AETHER_DESKTOP_CN` 渲染路径。
> 审计日期：2026-10-18。验证手段：同 §5 的真 QEMU 实跑，`boot-smoke.py` 断言链升至 **44/44**。

### 9.1 选型核对（是否符合铁律）

| 选型项 | 决策 | 是否符合铁律 |
|---|---|---|
| 中文 UI 字体来源 | **Noto Sans CJK SC 真实 TTF 光栅化**（Pillow），非手绘/猜测 | ✅ 来源可溯、非伪造 |
| 字体是否运行期依赖 FreeType / 字体文件 | **否**——构建期 bake 进 `.h` 位图，guest 静态单二进制零字体依赖 | ✅ 不重造、不加 guest 运行期依赖 |
| CJK 字体范围 | **精选子集 59 字**（覆盖 UI 字符串），非完整 CJK——与 ASCII 95 字同理 | ✅ 诚实范围声明 |
| 汉字上屏机制 | **UTF-8 解码 → 二分查表 → 16×16 位图 blit → KMS 页翻** | ✅ 真实渲染路径、非 ASCII 写死画面 |

**结论**：P1-1b 把 P0-6 以来「只能画 ASCII」的合成器升级为「能渲染 UTF-8 混合中文行」，字体来源可溯（Noto 真实光栅化）、构建期 bake 入头文件、运行期零字体依赖，符合「不假实现、不重造成熟子系统、来源可溯」铁律。

### 9.2 CJK 位图生成取证（核心）

CJK 字模**不是凭空捏造**——取证链如下：

```
来源: /usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc  (TTC, index=2 = SC 字面)
      ↑ Han unification 下各字面共享汉字字形，取 SC 字面即可
工具: tools/gen_cjk_font.py (Pillow ImageFont.truetype(TTC, 16, index=2))
      per 字: 32×32 离屏 → d.text() 光栅化 → getbbox() 墨迹包围盒
              → 在 16×16 单元内按包围盒居中 → 每字 16 行 × 16 位
              → 每行高位字节 (b>>8)&0xFF, 低位字节 b&0xFF  (MSB=最左像素)
输出: aether-font-cjk.h
      aether_cjk_cp[59]        升序 codepoint（U+4E00 一 … U+98CE 风）
      aether_cjk_glyphs[59][32] 每字 16 行 × 2 字节
验证: Python 往返解码 中(U+4E2D) → 「方框+居中竖笔」与字形一致
      生成器 stderr: "cjk glyphs: 59, missing: none"，无全空字模
```

**字节溢出陷阱（审计发现的关键缺陷，已修）**：初版生成器把 16 位扫描行存成单个 `0xXXXX` 元素塞进 `uint8_t[32]`，触发 `-Woverflow`（`0x7FFE`→254 截断），汉字高位笔画全丢、字形崩坏。**修复**：每行拆成两个 `0x%02X` 字节 `(b>>8)&0xFF, b&0xFF`，共 32 字节/字；C 端 `blit_glyph16` 用 `((uint32_t)g[2*gy]<<8)|g[2*gy+1]` 还原。修复后合成器交叉编译 **0 warnings**，且字模可被 Python 工具无损往返解码——这是「看实际二进制、不当面夸」纪律的直接产物。

### 9.3 UTF-8 解码与二分查找取证

`aether_render_text_utf8` 按字节类处理（与内核/标准库约定一致，非自创）：

| 字节类 | 前导特征 | 解码 | 渲染路径 | 字宽 |
|---|---|---|---|---|
| ASCII | `c < 0x80` | cp = c | `blit_glyph8`（`aether_font_8x16`，8×16） | 8 |
| 2 字节 | `c & 0xE0 == 0xC0` | `((c&0x1F)<<6) | (p[1]&0x3F)` | `blit_glyph8` | 16 |
| 3 字节（BMP 汉字） | `c & 0xF0 == 0xE0` | `((c&0x0F)<<12) | ((p[1]&0x3F)<<6) | (p[2]&0x3F)` | `cjk_find(cp)` → `blit_glyph16`（16×16） | 16 |
| 非法前导字节 | 不匹配上述 | 跳过 `p++` | 无 | 0 |

`cjk_find` 在升序 `aether_cjk_cp[]` 上做标准二分（O(log n)）；命中则 `blit_glyph16`，未命中（子集外汉字）显式画 **tofu 方框**（上/下/左/右 4 条 1px 线），而非崩坏或乱码——这是「诚实子集边界」的工程表达。光标按字宽（8/16）推进，ASCII 与汉字可在同一行交错。

### 9.4 验证证据（QEMU 实跑，44/44）

在 §8.3 的 43/43 基础上，P1-1b 新增 J16，总断言数升至 **44/44（0 FAIL）**。新增项（取自 2026-10-18 `boot-smoke.summary.txt` 实跑）：

| 断言 | 期望 | 实跑结果 |
|---|---|---|
| J16 中文点阵字体真实上屏 | 截图近白汉字像素 >400 | **近白汉字特征像素 959 个** ✅ |

**像素级铁证**：J16 只扫描桌面中文行矩形 `x∈[112,312], y∈[752,768]`（避开一切窗口），命中去底色近白像素（`r>180 & g>180 & b>180`）**959 个**——精确等于 12 个汉字字模点亮像素合计（阈值 400 留防抖余量）。这些近白像素只能源于 `AETHER_DESKTOP_CN`（`"AetherOS 图形合成器 窗口系统已就绪"`）那行 `0xE6EDF3` 的汉字笔画，证明「**UTF-8 字符串 → 二分查表 → 16×16 位图 blit → KMS 页翻上屏**」链路真实闭合。

> CI 复现：`run-ci.sh --skip-core` 全链路（cross-compositor 含 CJK 渲染 → check-artifacts → rootfs → boot-smoke）→ `LOCAL_CI_OK`，44/44 与手动传参一致；合成器交叉编译 0 warnings。

### 9.5 诚实边界（P1-1b）

1. **精选子集，非完整 CJK 字体**：59 字覆盖 UI/系统名/常用词；子集外汉字渲染为 tofu 方框（已实现，非故障），扩字符集改 `CHARS` 重生成即可。
2. **构建期 bake，非运行时 FreeType**：换取 guest 静态单二进制、零字体依赖；代价是字体不可运行期切换/缩放（16×16 固定）。
3. **UTF-8 仅覆盖 BMP（1/2/3 字节）**：未处理 4 字节辅助平面（emoji 等）；非法前导字节跳过而非报错——非通用文本排版引擎。
4. **MSB 最左约定固定**：与 ASCII 字模一致，二进制字模可被 Python 工具往返解码验证（已抽样校验 `中`）。

## 10. P1-2 批次审计补遗：真实输入 / 沙箱上屏 / OTA 交付 / 多 plane / virgl 能力（2026-10-18）

> 审计对象（本轮新增）：`aether-compositor.c` 的 `open_evdev`/`evdev_handle`（P1-2a）、`aether-sandbox.c` 的 `setresuid`+`capset`+seccomp 序列（P1-2b）、`aether-ota.c` + `ci/make_ab.sh` + `ci/ab-smoke.py`（P1-2c）、`aether-drm.c` 的 `pick_plane` 全量枚举与 `detect_gpu_caps`（P1-2d/2e）。
> 审计日期：2026-10-18。验证手段：同 §5 的真 QEMU 实跑——boot-smoke 断言链升至 **51/51**（含 K/S/J17 段），新增 ab-smoke **32/32**。

### 10.1 选型核对（是否符合铁律）

| 选型项 | 决策 | 是否符合铁律 |
|---|---|---|
| 键盘输入路径 | **客机内核 evdev 节点真实读取**（`EVIOCGBIT` 双重位查询），不经 QMP 注入伪造事件 | ✅ 真机制；QMP send-key 仅作测试驱动器，输入事件本身走 `/dev/input/eventX` |
| 应用隔离 | **真 Linux 用户隔离**：`setresgid/setresuid(1000)` + `PR_CAPBSET_DROP` + `capset(0)` + mnt namespace + seccomp BPF 允许列表 + `NoNewPrivs` | ✅ 不重造——全部是内核既有原语 |
| OTA 更新 | **A/B 双槽 + persist 分区标记 + initramfs 选择器**；切换前挂载目标槽校验 `/sbin/init`，失败拒绝翻转 | ✅ 与 Android A/B 同构但零依赖；失败可回退是硬保证 |
| 硬件 plane 清单 | **枚举驱动暴露的每个 plane**（`GETPLANERESOURCES`+`GETPLANE`+属性表），按 `possible_crtcs` 过滤后按类型计数 | ✅ 真实资源盘点，不猜硬件能力 |
| virgl 3D 能力 | **`DRM_IOCTL_VIRTGPU_GETPARAM(VIRTGPU_PARAM_3D_FEATURES)`**——内核回显 `has_virgl_3d` | ✅ 设备自报能力；非启发式（如 render 节点存在性） |

**结论**：P1-2 五个方向没有引入任何"模拟层"——输入走内核 evdev、隔离走内核 capability/seccomp/namespace、更新走磁盘槽位与 initramfs、能力盘点走 DRM ioctl。全部符合「不假实现冒充真实现、不重造内核成熟子系统」铁律。

### 10.2 P1-2a evdev 取证：怎么证明是"真键盘"而非自娱自乐

```
open_evdev(): 遍历 /dev/input/event*
  EVIOCGBIT(0, ...)      -> 设备必须宣告 EV_KEY 事件类型（排除纯鼠标/触摸的误配）
  EVIOCGBIT(EV_KEY, ...) -> 设备必须宣告 KEY_ENTER 位
                           （鼠标在 EV_KEY 下只报 BUTTON 位，永不上报 KEY_ENTER）
evdev_handle(): read() -> EV_KEY & value==1（按下）-> Tab 切换焦点
```

**双重位查询是刻意的**：只查 EV_KEY 不够（鼠标也报 EV_KEY 按钮位），叠加 KEY_ENTER 才唯一确定"这是键盘"。CI 断言 K1 从启动日志抓 `evdev keyboard attached: QEMU Virtio Keyboard`（内核经 virtio-input 上报的真实设备名），K2 经 QMP `send-key Tab` 驱动后断言 `focused 2 -> 102`——**焦点真的动了**，证明输入事件穿过了「evdev 节点 → poll → 合成器焦点表」整条路径。

### 10.3 P1-2b 沙箱取证：demo 非 root 却仍能上屏（含一个深坑）

沙箱的隔离序列（`aether-sandbox.c`，CI 实测顺序，不可交换）：

```
1. unshare(CLONE_NEWNS)               -> 私有挂载命名空间
2. PR_CAPBSET_DROP(逐位) + capset(0)  -> 清空 effective/permitted/inheritable
3. setresgid(1000)/setresuid(1000)    -> 非 root 身份（必须在 capset 之后：先降 uid 会丢降权能力得 EPERM）
4. prctl(PR_SET_SECCOMP) + seccomp BPF 允许列表
5. prctl(PR_SET_NO_NEW_PRIVS, 1)      -> 阻止 setuid 二进制逃逸
```

S1–S4 断言从 `/proc/<pid>/status` 直读 `Uid: 1000×4`、`CapEff: 0`、`Seccomp: 2`、`NoNewPrivs: 1`——内核报告的身份，不是脚本自述。

**审计发现并修复的深坑（sockfs `fchown` 静默无效）**：demo 降权后连合成器 socket 得 EACCES。根因不是权限模型错，是**unix socket 的 fd 走 `fchown()` 时 sockfs inode 没有 `setattr` 操作——返回 0 但不改 owner**（宿主最小复现实锤：`fchown` 后 `ls -la` 仍 `root root`）。修复：改 `chown(SOCK_PATH, -1, 1000)`（路径 chown，走真 setattr）。另：宿主实验里 root 子进程 `setresuid(1000)` 后仍能 connect，是 `SECURE_NOROOT` 默认关闭、setresuid 保留 capabilities 的 `CAP_DAC_OVERRIDE` 掩盖了问题——客机里 `capset(0)` 之后才暴露。**这类"宿主能过、客机失败"的差异只有真跑客机才能发现**。

### 10.4 P1-2c OTA A/B 取证：32 项断言钉死的四条不变式

```
选择器 initramfs /init: 读 /persist/slot -> 挂对应盘 (vda=A / vdb=B) -> switch_root
aether-ota --switch:    挂载目标槽 -> 校验 /sbin/init -> 翻 persist 标记 -> 请求 reboot
aether-ota --mark-recovery: 打恢复标志 -> reboot -> initramfs 见标志进恢复模式
恢复模式:               挂活跃槽校验 -> aether-ota --apply(validated=1, recovery_cleared=1) -> reboot 回正常槽
```

| 不变式 | 断言 | 实测 |
|---|---|---|
| 坏槽必拒（不会把系统切到烂盘） | B2：摘除 B 盘后 `--switch` | `failed to mount` → `refusing to switch to unvalidated slot B` → rc=1 → 标记保持 A |
| 热插拔真实生效 | B1/B4/B5：QMP `device_del`/`drive_add`+`device_add` | 轮询等 `/dev/vdb` 真消失再继续（vdb 重现实证） |
| 切槽后实启另一系统 | C1/C4/C6：`switched=B` → RB_AUTOBOOT → root=/dev/vdb | B 槽 rootfs 身份 + PID1=aether-init |
| 恢复后无回退 | D1–D5：mark → 恢复 banner → `validated=1 recovery_cleared=1` → 仍 slot B | `apply: active_slot=B validated=1` |

**审计记录的两个实现级教训**：(a) QEMU PCIe DFS 枚举序——root port 的 secondary bus 深度优先，只把 B 挂 root port 而 P 留 pcie.0 会让 vdb 变成 persist 盘（曾实锤"挂载成功却 `no init`"）；三盘全挂各自 root port 后枚举序归位。(b) QEMU `device_del` 完成时会释放命令行 `-drive if=none` 的 block backend，恢复必须经 HMP `drive_add` 重建。这两条都只存在于真实 QMP 交互里，文档层设计看不出来。

### 10.5 P1-2d/2e 取证：plane 清单与 virgl 能力的诚实边界

**多 plane（J17）**：`pick_plane()` 不再"找到 primary 就收工"——它枚举 `GETPLANERESOURCES` 返回的**每一个** plane，逐个 `GETPLANE` + 属性表解析 `type` 属性（两轮式：先 `count_props=0` 探数量，再按 `AETHER_DRM_MAX_PROPS` 上限分配缓冲，规避 virtio-gpu plane 属性 >10 的回写溢出），按 `possible_crtcs` 掩码过滤后按类型计数。实测 `total=1 primary=1 overlay=0 cursor=0`——**如实反映 virtio-gpu 只有 primary plane**。合成器状态 JSON 原样上报该清单，绝不宣称不存在的 overlay/cursor 硬件合成。

**virgl（G1/G2，宿主受限如实 SKIP）**：`detect_gpu_caps()` 用 `DRM_IOCTL_VIRTGPU_GETPARAM(VIRTGPU_PARAM_3D_FEATURES)`——与 Mesa virgl 驱动同源的查询，内核返回 `vgdev->has_virgl_3d`。默认冒烟（无 virgl 设备）下 `cap_3d=0`，合成器状态如实带 `"cap_3d":0`。带 `--gpu-virgl` 的 CI 段在沙箱宿主**如实 SKIP**：宿主无 `/dev/dri/renderD*` 且 mknod 被禁，QEMU `ui-opengl`/llvmpipe 都硬依赖 render node——SKIP 是唯一诚实选项（伪造 PASS 或记 FAIL 都是撒谎）。

### 10.6 验证证据汇总（2026-10-18 沙箱实跑）

| 套件 | 断言数 | 结果 | 关键新项 |
|---|---|---|---|
| boot-smoke（含 K/S/J17 段） | 51 | **51 PASS / 0 FAIL / 0 SKIP** | K1 evdev 识别、K2 Tab 焦点、S1–S4 沙箱四件套、J17 plane 清单 |
| boot-smoke-virgl | 2 | 2 SKIP（宿主无 render node）/ 0 FAIL | SKIP 理由逐条记录在 summary |
| ab-smoke（P1-2c 全链） | 32 | **32 PASS / 0 FAIL** | 坏槽拒绝、热插拔恢复、切槽实启、恢复无回退、优雅停机 |
| ipc-contract-smoke（宿主） | 13 | 全过 | 沙箱 demo 连合成器 socket 的回归亦覆盖 |

> CI 复现：`BUILD_DIR=/tmp/aether-final bash ci/run-ci.sh --skip-core` → `LOCAL_CI_OK`（boot-smoke 51/51 + virgl SKIP×2 + ab-smoke 32/32）。

### 10.7 诚实边界（P1-2）

1. **virgl 只检测未消费**：`cap_3d` 是能力上报，合成器渲染路径仍是 CPU blit + KMS 页翻；没有 EGL/GLES/virgl 用户态驱动，没有任何绘制搬到 GPU 命令流。
2. **plane 清单不改变合成策略**：枚举是盘点，合成仍只用 primary plane；多 plane 硬件合成（overlay 分配）未实现，也不假装实现。
3. **seccomp 是允许列表不是完整沙箱**：BPF 只放行 demo 实际使用的 syscall 集合；未做 syscall 参数级过滤（如 openat 路径白名单），不宣称达到完整应用沙箱标准。
4. **A/B 的 persist 是普通 ext4 镜像**：掉电原子性（翻转标记的 write barrier 语义）未做掉电注入测试；验的是逻辑正确性，不是电源故障鲁棒性。
5. **evdev 是单键盘单焦点**：未测多设备/设备热插拔/长按重复；焦点策略是 Tab 轮转的固定实现。

---

## 11. P2-13 可观测性三件套审计补遗：logd / crashd / watchdog（2026-10-19）

对应 Roadmap P2 第 13 项（Watchdog / Crash Manager / 序列化日志服务）。这是 P2 阶段的收官项——P2 其余各项（Security 沙箱=P1-2b、Package Manager=P2-11、OTA A/B=P1-2c）均已完成。

### 11.1 选型核对（是否符合铁律）

- **不重造内核子系统**：序列化日志直接复用 printk 环（kmsg 转发），轮转只做一代 rename（不假装 logrotate）；看门狗优先用内核 `/dev/watchdog`（缺席才降级软看门狗）。
- **不假实现**：watchdog 处决的是「IPC 僵死」（init waitpid 看不见的死法），不是万能重启；崩溃报告无 core、无栈回溯（/proc 已消失），报告里写死边界。
- **看实际数据流**：三服务均为 init 监管的常驻进程，日志/事件走 UDS datagram，落盘路径全部客机实证（见 11.3）。

### 11.2 架构与数据流

```
业务服务 ──aether_log()──> /run/aether-log.sock ──> aether-logd ──> /var/log/aether.log (+ kmsg)
   (init/compositor/...)        (datagram)              │
                                                       └─> 256KiB 轮转 .1
init(异常退出) ──crash event──> /run/aether-crash.sock ──> aether-crashd ──> /data/aether/crash/<svc>-<epoch>-<pid>.txt
watchdog ──5s 心跳(poll 2s)──> aetherosd/compositor IPC ──3 次失败──> SIGKILL ──> init restart=always 拉起
init ──状态变化──> /run/aether-services.state ──> watchdog 定位 pid
```

### 11.3 验证证据（QEMU 实跑，2026-10-19）

boot-smoke 76 PASS / 0 FAIL / 0 SKIP，其中 P2-13 新增 18 条：

- **L 段（logd）**：L1/L2 socket 0660 + PPID=1；L3 logctl→logd→落盘全链（标记行含 `up=35.21 INFO boot-smoke[128]`）；L4 行格式正则；L5 services.state 九服务全 running；L6 SIGUSR1 轮转出 .1。
- **W 段（watchdog）**：W1/W2 PPID=1 + 「software watchdog only」如实报告；**W3 核心价值**：SIGSTOP 冻结 aetherosd（pid=138，进程活着、IPC 无响应）→ `watchdog: killing aetherosd` → init 拉起 pid=150，全程约 12 秒。
- **C 段（crashd）**：C1 PPID=1 + socket；C2 H/W 段 SIGKILL 的报告 `cause: signal 9 (SIGKILL)`；C3 kill -SEGV demo → 报告 `cause: signal 11 (SIGSEGV)`；C4 诚实边界段在场；C5 demo 被 init 重启（161 1 demo）。

宿主侧另证：logd 降级路径（杀 logd 后 logctl 即刻走 kmsg 不阻塞，rc=0）、自动轮转（320 条 900B 记录 ≈300KB → .1）、crashd 报告内容（信号名映射 + 尾部日志缺失时如实标注 unavailable）。

### 11.4 诚实边界（P2-13）

1. **软看门狗 ≠ 硬件看门狗**：QEMU 无 `/dev/watchdog`，整机级死锁靠内核 panic 兜底；watchdog 只做服务级 IPC 僵死恢复。
2. **崩溃报告 ≠ core dump**：无寄存器/栈回溯；缓解 core 缺失需 ptrace 陪跑或 core_pattern，均未做。
3. **日志聚合有降级窗口**：早于 logd 启动的调用方（init 自身）首批记录走 kmsg，每 64 条重试主路径；本地日志文件（/run/aether-init.log）仍是完整真相。
4. **轮转只保留一代**：历史归档归 OTA/9p，不冒充日志系统。
5. **索引/事件无锁**：datagram 单发单收，事件丢失即丢失（init 本地日志有记录兜底）；crashd 重启期间的事件会丢——不假装可靠队列。

至此 Roadmap P2 全部完成（Security / Package Manager / OTA A/B / Watchdog+Crashd+logd）。

---

## 12. P3-14 SystemUI 上 Linux（审计补遗，2026-10-19）

Roadmap P3 第 14 项 / Phase 6：把"系统界面"迁到真 Linux 用户空间。AetherOS 首次具备系统界面层。

### 12.1 选型核对

- **不假实现**：SystemUI 是 compositor 的独立客户端进程（UDS 注册窗口 + SCM_RIGHTS 共享像素 + 自绘），不是合成器内置绘制、不是贴图；启动应用走真实 fork/exec + 沙箱降权。
- **不重造轮子**：绘制复用既有 `aether-render`（矩形/8x16/16x16 CJK），数据源全读 `/proc` 与既有系统文件（services.state / apps index），未引入任何第三方库。
- **看实际数据流**：状态条读数与客机 `/proc/meminfo` 独立实测交叉验证（±2% 内）。

### 12.2 与既有不变量的冲突处理（值得单列）

新增一个会动的系统界面，撞上了三条既有不变量，处理方式都是**去耦合而非降强度**：

| 冲突 | 处理 |
|---|---|
| 窗口基数不再是常量 2 | J12 改相对断言：close 使 windows 恰减 1 |
| 窗口 id 不再是常量 | K2 本就是相对断言（focused 前后不同），不动 |
| 定时重绘让全屏帧哈希持续变化 | J 段先 `refresh off` 冻结（确定化输入），测完 `refresh on`；另有 U8b 证明刷新确实在跑 |

### 12.3 验证证据（QEMU 实跑 89 PASS / 0 FAIL / 0 SKIP）

U 段 12 条要点：U2 窗口表 `[bootdiag, SystemUI, demo]`；U3b sysui 内存 90% = 客机实测 90%；U4 应用网格 = 真实包索引；U5/U6 两条启动链路都落地（pid 137/141，comm=hello，Uid=1000）；U7 青色特征像素 3889；U8b uptime 36.3→48.9s。

### 12.4 本轮两个实锤缺陷（均已修，写进代码注释）

1. **尺寸语义不一致导致合成器 SIGBUS**：`create` 的 w/h 是内容尺寸；SystemUI 误按窗口总尺寸传值，使 compositor mmap 长度超过 memfd 实际大小 → 访问越界页 → 合成器崩溃、客户端窗口全丢。教训：共享内存的尺寸语义必须两侧对齐，且 `cmd_buf` 必须校验响应 `ok:true`。
2. **合成器重启后窗口 id 失效**：SystemUI 加 `ensure_window()` 自愈（commit 被拒即重建窗口）。否则会出现"进程活着、日志说已提交、屏幕什么也没有"的假上屏——这正是"不假实现"铁律最容易破防的地方。

### 12.5 诚实边界

窗口仍带 WM chrome（无 layer-shell）；Launcher 无图标、无 pointer 输入；时钟只到分钟。SystemUI 是**普通客户端窗口**，不是系统层——不夸大。
