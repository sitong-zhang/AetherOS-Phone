# AGENTS.md —— 接手者操作手册（AetherOS：基于 Linux 内核的手机 OS）

## 0. 一句话定位

AetherOS = **基于 Linux 内核的手机操作系统（主打安全）**，ARM64 Linux 内核 + 完全自研用户态；
不基于 AOSP、不基于现成发行版。**目标真机**：开发板与谷歌 Pixel（刷入测试）。当前在云电脑上做
系统构建与调试，**不做 APK、不套壳、不交付预览包**——系统本体就是真 OS。

> ⚠️ 内核形态更正（2026-11-14）：早期若干 commit 曾把落地层改成「自研 x86_64 内核（非 Linux 复用）」，
> 与 V2 总文件 / 本文件「Linux 内核 + 自研用户态」矛盾，已纠正。仓库内 `kernel/` 是一段
> **研究性 R&D 代码**（从零学操作系统），**并非产品内核**，其安全 demo 经审计有 V1–V17 缺陷，
> 不代表产品安全能力。产品真实内核为 Linux。

## 1. 目录

| 路径 | 说明 |
| --- | --- |
| `secure/` | **用户态安全整机本体（产品安全核心资产）**：密码学基座、四区隔离存储、认证引擎、加密保险库、自毁引擎、审计链、完整性、TEE 锚点、设备自证、CLI 与全套自测（96/0） |
| `guest/` | 自研用户态上层：init、合成器、渲染、字体、图标、桌面与系统应用、参数源 `aether-tokens.h` |
| `l2/` | L2 硬件接口层：十五域统一接口、仿真实现、接口自测（在建） |
| `kernel/` | **自研 x86_64 内核（R&D 研究性，非产品内核）**：仅从零学 OS 的练习，不代表产品安全能力 |
| `tools/` | 基线采集、机检门禁、检测器、内核 QEMU 驱动/门禁 |
| `scripts/` | 一键操作：`secure.sh`（安全整机）、`kernel.sh`（R&D 内核）、`aether.sh`（上层） |
| `docs/` | 环境基线、架构（v2 口径）、安全设计、内核/阶段报告 |
| `evidence/` | 每轮演练证据（安全整机在 `round-002`，R&D 内核在 `round-003`） |
| `vm/` | **已归档停用**（旧 APK 套壳工程），不再是交付形态，见 `vm/DEPRECATED.md` |

## 2. 常用命令

```sh
# 安全整机：编译 + 96 项自测 + OpenSSL 密码学交叉验证 + 整机演示 + 导出证据
bash scripts/secure.sh build      # gcc -Werror 编译 aether-os + test_secure
bash scripts/secure.sh test       # 全套自测(96) + OpenSSL 交叉验证(4/4)
bash scripts/secure.sh demo       # 整机 shell 演示：解锁→存机密→输错3次→全盘销毁
bash scripts/secure.sh evidence   # 证据汇总到 evidence/round-002/

# R&D 内核（非产品，仅学 OS 用；其安全 demo 不代表产品安全）
bash scripts/kernel.sh build      # 编译自研 x86_64 内核（freestanding -Werror）
bash scripts/kernel.sh test       # QEMU TCG 真启动 + 断言（研究性）

# 自研用户态上层（B 轨，待重新取证）
bash scripts/aether.sh build
```

## 3. 已知坑 / 环境

- **云电脑无 `/dev/kvm`**：整机只能用 TCG 软件模拟，交互迟钝属环境限制，不影响逻辑正确性。
- **真机目标**：最终刷开发板 / 谷歌 Pixel 实机测试；`secure/` 的 TEE 锚点当前为软件后端，
  真机阶段切换为硬件后端（Pixel Titan M / eFuse），密钥由安全芯片驻留、不可导出。
- **密钥/凭据**：任何测试密钥、token 不得写入仓库；设备密钥由 `secure/` 在首次初始化时
  随机生成并持久化于 `sys/device.key`（软件后端），硬件后端由 TEE 提供。

## 4. 提交约定

- 未通过真机/真编译验证的功能不得标记为完成；状态文档如实标注「部分实现 / 未开始」。
- 凭证、密钥、token 不得写入仓库内任何文件。
- 交付渠道只有本 GitHub 仓库（源码与 GitHub Release），不维护独立公网发布页。
- 版本说明与仓库文档面向公众，使用客观陈述，不出现对话语气；不把 R&D 内核当产品安全吹。
- 不自改门禁：验收标准、门禁脚本、断言阈值不由执行方修改；每次改动留 diff 与回滚点。
