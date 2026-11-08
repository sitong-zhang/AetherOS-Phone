#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_modules.py - 生成 AetherOS 发行版模块清单数据 (src/aether-modules.h)

前提对齐（用户确认）：Aether OS 基于 Linux 6.6.70 内核构建，内核原生已提供
调度/内存/文件系统/网络协议栈/总线驱动框架/虚拟化核心/安全原语等底层机制，
无需重复开发。本清单只覆盖【发行版需自研、定制整合、用户态实现】的模块。

每一项标注真实状态，不谎报：
  A = 本版已实现（自研或已落地的用户态能力）
  B = 已由上游/宿主提供并整合（Linux 内核、BusyBox、Limbo/QEMU）
  C = 基础具备或部分实现，尚未完整
  - = 规划中，明确未实现

运行：python3 tools/gen_modules.py
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "src", "aether-modules.h")

# 紧凑格式：以「一、」等开头的行是类别，含 | 的行是 "状态|项目名"
RAW = """
一、系统引导与启动链路
-|GRUB2 定制化引导加载器（主题、启动项、安全启动适配）
-|UEFI 安全启动签名与证书管理模块
-|系统启动参数生成与 grub.cfg 自动配置模块
-|初始内存盘 initramfs 定制构建模块
A|开机动画与启动进度展示模块
-|系统救援模式与单用户模式入口模块
-|恢复分区与系统重置引导模块
-|多系统引导项自动识别与管理模块
A|启动日志持久化与启动故障诊断模块
-|内核参数一键调优配置模块
二、内核定制与硬件适配层
B|Linux 6.6.70 内核裁剪与配置管理模块
-|内核补丁集管理与合入模块
C|内核固件全集集成与按需加载模块
-|第三方闭源驱动适配与 DKMS 管理模块
C|内核模块按需加载与黑名单管理模块
C|内核参数运行时调优与持久化配置模块
-|低延迟/性能/省电三种内核调度策略切换模块
C|异构硬件平台统一适配抽象层
三、基础运行时与核心库
B|Aether Libc 适配层（基于 glibc 定制）
-|核心系统库全集定制（libstdc++/libssl/zlib/curl）
-|动态链接库路径与版本管理模块
-|多架构二进制兼容运行层
A|运行时环境变量与系统全局配置模块
C|语言区域与本地化全量支持模块
C|时区与时间同步运行时模块
C|字符编码与国际化适配模块
四、系统初始化与服务管理
C|基于 systemd 的定制化 init 启动流程（本版为自研 aether-init）
A|系统服务分级启动与依赖管理模块
A|服务自启管控与运行状态监控模块
A|系统日志管理服务（日志分级与持久化）
-|用户会话管理服务（logind 定制）
C|设备事件守护服务（udev 规则定制）
-|定时任务调度服务（cron 定制封装）
C|系统启动速度优化与并行启动模块
A|守护进程异常重启与自愈模块
-|系统运行等级与模式切换模块
五、硬件驱动与外设管理
C|显卡驱动管理模块（DRM/fbdev 自动回退已实现，一键切换未做）
-|声卡驱动与音频设备拓扑管理模块
C|网卡驱动与硬件识别适配模块
C|USB 设备热插拔管理与权限控制模块
-|打印机驱动框架与自动匹配模块
-|蓝牙设备配对与连接管理模块
-|传感器设备适配与数据上报模块
-|摄像头设备管理与权限管控模块
A|磁盘设备热插拔与自动挂载模块
A|硬件健康状态监控模块
六、存储与文件系统管理层
A|系统分区方案与默认挂载配置模块
-|LVM 逻辑卷管理工具与可视化模块
-|全盘加密（LUKS）管理与解锁模块
-|软 RAID 阵列配置与管理模块
-|磁盘配额管理模块
C|文件系统检查与修复工具集
A|自动挂载服务（移动存储、共享目录）
-|磁盘碎片整理与空间优化工具
-|回收站与文件安全删除模块
-|网络存储挂载管理（NFS/SMB/WebDAV）
七、网络管理与通信服务
-|网络连接管理服务（有线/WiFi/蜂窝）
-|WiFi 扫描、连接与密码管理模块
-|网络代理全局配置模块
C|DNS 解析管理与缓存服务
C|DHCP 客户端与静态 IP 配置模块
-|防火墙前端管理模块（基于 Netfilter）
-|网络共享与热点模块
-|虚拟网卡与 VPN 管理模块
-|网络质量监控与故障诊断模块
-|蓝牙网络、USB 网络共享模块
八、安全与权限管控体系
B|用户与用户组管理模块
-|sudo 权限细粒度配置模块
C|文件权限与 ACL 可视化管理模块
-|应用沙箱运行管控模块
-|可执行文件签名校验模块
C|系统安全审计日志模块
-|强制访问控制前端配置模块
-|内核安全参数一键加固模块
-|TPM 可信计算与密钥管理模块
-|反调试与系统完整性校验模块
-|应用权限统一管理中心
-|系统安全漏洞扫描与修复模块
九、图形显示与桌面环境
C|显示服务核心（本版自研 UDS IPC，Wayland 协议未实现）
A|窗口合成器（自研合成引擎）
A|窗口管理器（浮动窗口 + 拖拽 + 最大化/最小化/关闭）
A|DRM/KMS 显示适配与显示后端模块
C|分辨率、刷新率与色域调节模块
A|2D 图形渲染引擎与控件库
-|3D 图形与 OpenGL/Vulkan 兼容适配层
A|全局主题引擎（配色、控件、动效）
A|桌面与图标管理模块
-|多工作区与虚拟桌面模块
C|窗口特效与开机动画渲染模块
A|全局光标与指针管理模块
-|高 DPI 缩放与分数缩放模块
C|桌面组件与任务栏小组件框架
A|通知中心与消息聚合模块
十、输入交互与外设框架
A|全局输入事件分发模块
A|键盘布局与键位映射模块
C|鼠标/触控板指针与点击交互模块
C|触摸屏与绝对坐标输入适配模块
-|游戏手柄外设适配框架
-|全局剪贴板管理模块（历史记录、跨应用）
-|输入法框架与多输入法管理模块
C|全局快捷键与开始菜单
-|无障碍交互模块（读屏、放大、高对比度）
十一、多媒体音视频服务
-|音频服务核心（PipeWire 定制）
-|音频设备切换与音量管理模块
-|音频混音与音效配置模块
-|系统音视频编解码全集集成
-|硬件编解码加速适配层
-|媒体播放器核心服务
-|摄像头预览与录像服务
-|麦克风与录音管理模块
-|流媒体播放框架
C|屏幕录制与截图服务（截图 IPC 已实现，录屏未做）
十二、电源管理与能效调控
-|电源管理守护服务
-|CPU 调频与性能模式切换模块
-|显卡动态功耗调控模块
-|硬盘节能与休眠管理模块
-|系统睡眠/休眠/唤醒管理模块
-|电池电量与充电策略管理模块
-|温度监控与散热调度模块
-|功耗统计与用电分析模块
-|唤醒源管理与误唤醒防护模块
-|节能/平衡/性能模式一键切换
十三、包管理与系统升级
-|Aether 包管理器（自研前端 + RPM/DEBIAN 后端）
-|软件仓库源管理模块
-|软件依赖自动解析与冲突处理模块
-|OTA 系统增量更新模块
-|系统版本管理与回滚模块
-|应用商店客户端与应用分发模块
-|软件安装/卸载/升级批量管理模块
-|第三方软件源安全校验模块
A|系统镜像构建与发布流水线模块
十四、虚拟化与容器支持
-|KVM 虚拟化用户态管理栈（libvirt 定制）
B|QEMU 设备模拟适配与优化（Limbo 宿主提供）
-|虚拟机图形化管理工具
B|虚拟机快照与备份还原（QEMU 宿主提供）
-|虚拟网络桥接与端口管理模块
-|容器运行时（Podman/Docker）集成
-|容器镜像管理与编排模块
C|9pfs 共享文件与主机互通模块
-|远程显示协议（SPICE/VNC）集成
B|虚拟机资源配额与性能调优（Limbo 配置）
十五、系统诊断与调试工具集
A|系统信息概览工具
A|进程与资源监控工具
A|系统性能采样分析工具
-|网络抓包与诊断工具
-|磁盘性能与健康检测工具
C|内存泄漏与占用分析工具
C|系统日志查看与筛选工具
A|崩溃日志收集与栈回溯模块
-|系统备份与还原工具
A|启动盘制作与系统部署工具
十六、内置应用与生态框架
A|系统设置中心
A|文件管理器
A|终端模拟器
A|文本编辑器
-|网页浏览器（定制内核版）
-|图像查看器
-|视频播放器
-|音乐播放器
A|计算器
C|日历与日程（任务栏时钟/日期）
-|联系人与邮件
C|截图工具
-|应用商店
-|安全中心
C|设备管理器
-|输入法客户端
十七、兼容与移植适配层
-|Windows 应用兼容层（Wine 定制优化）
B|Linux 旧版应用兼容适配（原生 Linux 用户态）
-|安卓应用运行兼容层
-|跨架构二进制转译适配模块
C|老旧硬件兼容驱动集
A|虚拟机环境专属优化模块
C|第三方应用开发 SDK 与文档
十八、系统体验与品牌定制
A|Aether OS 品牌视觉体系（图标、壁纸、开机画面）
C|全局主题与个性化模块（深浅主题）
-|首次开机引导配置向导
-|用户数据迁移工具
-|系统重置与出厂化模块
C|帮助与反馈中心
-|云端账号与数据同步模块
-|官方社区与更新公告模块
"""

STATE_DESC = {
    "A": "本版已实现（自研或已落地用户态能力）",
    "B": "已由上游/宿主提供并整合",
    "C": "基础具备或部分实现，尚未完整",
    "-": "规划中，明确未实现",
}


def cstr(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main():
    groups = []
    for line in RAW.strip().splitlines():
        line = line.strip()
        if not line:
            continue
        if "|" in line:
            st, name = line.split("|", 1)
            assert st in STATE_DESC, "未知状态: " + st
            groups[-1][1].append((name.strip(), st))
        else:
            groups.append((line, []))
    assert len(groups) == 18, "类别数应为 18，实际 %d" % len(groups)
    total = sum(len(items) for _, items in groups)

    lines = []
    lines.append("/*")
    lines.append(" * aether-modules.h - AetherOS 发行版模块清单（tools/gen_modules.py 生成，请勿手改）")
    lines.append(" *")
    lines.append(" * 前提：Linux 6.6.70 内核已提供全部底层机制，本清单只含发行版")
    lines.append(" *       需自研 / 定制整合 / 用户态实现的模块。状态为真实归属标注。")
    lines.append(" *   A 本版已实现 / B 上游或宿主已提供 / C 部分实现 / - 未实现")
    lines.append(" */")
    lines.append("#ifndef AETHER_MODULES_H")
    lines.append("#define AETHER_MODULES_H")
    lines.append("")
    lines.append("struct aether_mod_item { const char *name; char state; };")
    lines.append("struct aether_mod_group { const char *name; int count; const struct aether_mod_item *items; };")
    lines.append("")
    lines.append("#define AETHER_MOD_GROUP_COUNT 18")
    lines.append("#define AETHER_MOD_ITEM_COUNT  %d" % total)
    lines.append("")
    for gi, (gname, items) in enumerate(groups):
        lines.append("static const struct aether_mod_item aether_mod_g%d[] = {" % gi)
        for name, st in items:
            lines.append("    { %s, '%s' }," % (cstr(name), st))
        lines.append("};")
        lines.append("")
    lines.append("static const struct aether_mod_group aether_mod_groups[AETHER_MOD_GROUP_COUNT] = {")
    for gi, (gname, items) in enumerate(groups):
        lines.append("    { %s, %d, aether_mod_g%d }," % (cstr(gname), len(items), gi))
    lines.append("};")
    lines.append("")
    lines.append("static const char *aether_mod_state_text(char s)")
    lines.append("{")
    lines.append("    switch (s) {")
    for k, v in STATE_DESC.items():
        lines.append("    case '%s': return %s;" % (k, cstr(v)))
    lines.append("    default: return \"未知\";")
    lines.append("    }")
    lines.append("}")
    lines.append("")
    lines.append("#endif /* AETHER_MODULES_H */")

    with open(OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")

    counts = {}
    for _, items in groups:
        for _, st in items:
            counts[st] = counts.get(st, 0) + 1
    print("生成:", os.path.abspath(OUT))
    print("类别 %d / 项目 %d" % (len(groups), total))
    for k in ("A", "B", "C", "-"):
        print("  %s %-38s %3d" % (k, STATE_DESC[k], counts.get(k, 0)))


if __name__ == "__main__":
    main()
