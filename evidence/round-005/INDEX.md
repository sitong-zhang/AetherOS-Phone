# round-005 证据清单 · 熔断「攻击意图分级」+ 仓库整理

- 触发：用户 2026-11-14 澄清「正常维修短接不删系统；触及用户数据/系统核心区且检测到攻击意图，
  立刻摧毁系统 + 烧毁硬件」，并指出《系统评估报告 2026-11-14》§4.1 与其意思一致（更正上一轮误判）。
- 环境：云电脑 x86_64 用户态 CLI（软件仿真）；物理事件为 `aether_hal` 故障注入，非真机电气检测；
  「烧毁硬件」为**仿真熔断指令锁存**（`simulated(cloud-no-physical-fuse)`），真机 eFuse 属 S6。
- 复现：`bash scripts/secure.sh build && bash scripts/secure.sh test`；CLI 序列见本目录 `lockdown-grading-cli.log`。

| 文件 | 内容 |
|---|---|
| `lockdown-grading-cli.log` | 端到端 6 组：A1–A3 无害零删除 → A4 可疑进 SUSPECT → A5 核心区导出才熔断 → A6 维修白名单 → L3 用户红线(USB破解)熔断 → 变砖终态/零存储/sys 抹除 |
| `test_secure.stdout.txt` | 全套自测 **252 PASS / 0 FAIL** + OpenSSL 交叉验证 4/4（含新增组 M = 报告 A1–A8） |
| `secure-sources.sha256.txt` | `secure/**/*.c\|*.h` 逐文件 sha256，可复现 |

**关键结论（外部可观测）**
1. **事件只作证据、攻击意图才触发**：充电/开盖/维修短接(L1)零删除、状态仍 LOCKED；
   未授权 ADB(L2) 断连取证进 SUSPECT、零删除；仅核心区导出/USB破解强插(L3) 才立即熔断。
2. **软件熔断 vs 硬件熔断分开**：软件层抹除含 `sys/` 的全部文件 + BRICKED 标记（刷机可重装）；
   硬件层为 `ae_hal_hw_fuse_burn()` 仿真指令锁存（真机 eFuse 才物理永久变砖）。
3. **维修白名单**（签名凭证）下开盖/短接/ADB/充电全程不熔断；但不豁免 L3 核心区导出（防伪造凭证）。
4. 零存储成立：全盘扫描运行根 0 处密码明文；变砖后 `sys/` 保持 0 文件、引导级拦截、状态跨重启持久。

**本阶段配套文档**
- `docs/ASSESSMENT-RESPONSE-001.md`：评估报告 V1–V17/§4/§5 逐条采纳·修正采纳·驳回 + 依据（含对象错位纠正）。
- `docs/SYSTEM-BLUEPRINT.md`：系统蓝图单一真相源（要构建什么系统/整体思路/分层/状态/路线/边界/文档地图）。
- README / docs/README.md / SECURITY-BASELINE.md / 04-KERNEL-REPORT.md / round-004 INDEX 口径订正。

**诚实边界**：物理检测为云端仿真；「烧毁硬件」云端仅仿真指令、真机 S6 才物理执行；
软件熔断=刷机可重装，不虚称「永久变砖」；`secure/` 仍是本机 CLI，非随 Linux 手机 OS 启动的服务；
H1–H4（验证启动/故障隔离/地址隔离/内核硬化）属 Linux 内核层，未开始。
