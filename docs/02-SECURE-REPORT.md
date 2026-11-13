# 阶段报告 · 安全整机本体（round-002）

> 四件套：改了什么 / 为什么 / 怎么验证 / 未解决。全部结论来自本机真实编译与真实运行。
> 阶段定义与三档标注见 `README.md`；安全设计细节见 `docs/SECURITY.md`。

## 1. 本轮目标

把系统做成一套**主打安全**、能在云电脑上软件模拟真实跑起来的整机，核心诉求是：
口令连续错误达到阈值就**抹除全部用户数据**（不是恢复出厂），但**系统本体不抹除**；
并自主补齐一批有实际防护意义的安全特性。

交付形态沿用 v2 口径：系统本体不是 APK、不是套壳、不发布、不刷真机。

---

## 2. 改了什么（新增文件）

| 文件 | 作用 |
| --- | --- |
| `secure/include/aether_crypto.h` / `secure/core/aether_crypto.c` | 自包含密码学基座：SHA-256、HMAC、PBKDF2、ChaCha20、encrypt-then-MAC、CSPRNG、常量时间比较、安全清零 |
| `secure/include/aether_store.h` / `secure/core/aether_store.c` | 四区隔离存储、原子写（fsync+rename）、目录穿越防护、定长卷创建与多轮覆写、区摘要 |
| `secure/include/aether_audit.h` / `secure/core/aether_audit.c` | 只追加防篡改哈希链（追加/校验/导出） |
| `secure/include/aether_integrity.h` / `secure/core/aether_integrity.c` | 系统区完整性基线封存与自检 |
| `secure/include/aether_secure.h` / `secure/core/aether_secure.c` | 统一门面：认证引擎、加密保险库、自毁引擎、会话管理、销毁证明、状态输出 |
| `secure/cli/aether_os.c` | 整机主程序：独立子命令 + 常驻交互 shell + 真实状态快照导出 |
| `secure/tests/test_secure.c` | 全套自测（10 组，81 断言） |
| `tools/render_secure_ui.py` | 把真实状态快照渲染成界面图（自查/取证） |
| `scripts/secure.sh` | 安全整机一键入口：build/test/demo/evidence/status/shell/all |
| `scripts/aether.sh` | 新增 `secure` 转发分支（原 6 种一键命令不变） |
| `docs/SECURITY.md` | 安全系统设计说明（威胁模型、架构、14 项特性、边界） |
| `evidence/round-002/` | 本轮证据四件套 + 4 张界面图 + 3 份状态快照 + 销毁证明 |

`README.md`、`docs/README.md` 同步更新索引与快速开始。

---

## 3. 为什么这样设计

- **四区隔离**是「数据全抹除、系统不抹除」能成立的前提：把系统本体、用户数据、
  安全日志、取证证明放在不同语义区，擦除引擎的作用域被物理地限制在 `data/`。
- **crypto-shred + 多轮覆写**双管齐下，才配得上「不只是恢复出厂设置」：
  只删索引不算销毁，销毁密钥让密文永不可解，覆写让旧字节不可恢复。
- **计数三重防护**是因为单纯「重启清零」或「改文件清零」都能轻易绕过暴力破解防护：
  原子落盘挡重启，HMAC 挡改文件，审计链交叉核对挡「连 MAC 一起重算」。
- **sys/ 摘要硬断言**把「系统不抹除」变成机器可判的断言，而不是一句口号。
- **密码学用公开标准 + OpenSSL 交叉验证**：安全系统不能靠自己撸的玩具算法，
  也不该宣称「自研」就等于「可信」，所以用独立实现复算比对来证明正确性。

---

## 4. 怎么验证（真实命令与结果）

```bash
scripts/secure.sh all
```

| 验证项 | 结果 | 证据 |
| --- | --- | --- |
| 全量自测（10 组 / 81 断言） | **81 PASS / 0 FAIL** | `test_secure.stdout.txt` |
| OpenSSL 3.x 密码学交叉验证 | **4/4 逐字节一致** | `openssl-crosscheck.txt` |
| 输错 3 次 → 全盘销毁 | `WIPED … 系统完好` | `demo-session.log` |
| `sys/` 摘要擦除前后一致 | 逐字节相同 | 组 D 断言 + `wipe-receipt.json` |
| 凭据 / 密钥 / 卷全部清除 | 三个文件均不存在 | 组 D 断言 |
| 审计链完整且记录销毁全过程 | INTACT，含 3 个销毁事件 | 组 D 断言 |
| 销毁证明 `sys_unchanged: true` | 通过 | `wipe-receipt.json` |
| 旧数据不可恢复 | 重新初始化后读不到旧文件 | 组 D 断言 |
| 防重启绕过 | 重启后 `fails` 仍为 2 | 组 D 断言 |
| 防「改计数文件」绕过 | 不重算 MAC → 篡改即擦；重算 MAC → 链把计数恢复为 1 | 组 E 断言 |
| 胁迫口令 / 紧急销毁 / 篡改即擦 | 三条路径均触发销毁且 `sys/` 不变 | 组 F/G/H 断言 |
| 静态加密（卷内无明文） | 全卷 `memmem` 搜索不到明文标记 | 组 D 断言 |
| 空闲锁定 / 指数退避 | 超时拒绝写入；实测退避 1 秒 | 组 I/J 断言 |
| 整机 shell 端到端演示 | 解锁 → 存 3 个加密文件 → 输错 3 次 → 销毁 | `demo-session.log` |
| 界面图自查 | 4 张，中文/布局/状态数字正常，无缺字方框 | `01~04 PNG` |

**OpenSSL 交叉验证明细**（本机 `openssl 3.0.13` 独立复算）：

```
SHA256(abc)              = ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
HMAC-SHA256(key,... )    = f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8
PBKDF2-HMAC-SHA256(4096) = c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a
ChaCha20 keystream[:16]  = 10f1e7e4d13b5915500fdd1fa32071c4
```

---

## 5. 过程中发现并修复的真实缺陷（均为跑出来的，非推断）

| # | 缺陷 | 后果 | 修复 |
| --- | --- | --- | --- |
| 1 | SHA-256 尾块构造把 padding 覆盖成长度字段 | 所有哈希错误 | 追加长度到 padding 块尾部 |
| 2 | `ae_zero` 依赖 `explicit_bzero`，与 `-include` 冲突 | 编译失败 | 改用 volatile 函数指针 memset，无 libc 扩展依赖 |
| 3 | `ae_store_read` 缓冲区未补 `\0` | 审计链 `strtok_r` 越界读，把完好链误判为篡改 → 第一次输错就误触发销毁 | `malloc(sz+1)` 并补终止符 |
| 4 | `ae_store_volume_overwrite(..., NULL)` 无条件写 `out_hash` | 重新初始化时崩溃 | 支持 NULL 输出 |
| 5 | 审计链 `verify` 拿调用者未初始化的输出指针比对 | 误判断链 | 始终与持久化的 head 比对 |
| 6 | 空 `detail` 产生空字段被 `strtok_r` 跳过 | 误判断链 | 空 detail 填中性占位 |
| 7 | 桩实现错误码传不到上层（缺实现会静默成功） | 安全语义错误 | 核心层真正调用 `impl->submit` |
| 8 | `status` 出厂态显示 `fails 0/0` | 状态显示不一致 | 改走统一 `ae_secure_max_fails()` API |
| 9 | 擦除消息显示「连续错误 0 次」 | 回报不实 | wipe 前留存触发时计数 |
| 10 | `-Werror` 下 4 处缓冲截断/fortify 溢出推导告警 | 编译失败 | 复用带边界检查的路径构造、限长 memcpy、固定块读回、加大重建缓冲 |
| 11 | CLI 直接访问不完整类型内部字段 | 编译失败 | 新增公开 API，全部改走门面接口 |
| 12 | 测试在 `put` 之前采卷哈希，与证明取样点不一致 | 1 条误报 FAIL | 采样点移到擦除前一刻 |

第 3 条是本轮最关键的一次定位：它一度让「篡改即擦」在正常输错口令时被误触发，
修复后核心链路才真正跑通。**没有通过放宽门禁或改断言来掩盖任何失败。**

---

## 6. 未解决 / 诚实边界

| 项 | 状态 | 说明 |
| --- | --- | --- |
| 真机内核与 S6 移植 | 未开始 | 当前是云电脑软件模拟的用户态整机，不是刷机内核 |
| 硬件根信任锚定计数 | 设计等价物 | 真机应由 TEE/安全芯片锚定；当前用 `device.key` + 审计链模拟，强度不等同 |
| SSD 物理安全擦除 | 待真机 | 当前覆写针对文件系统上的定长卷文件；真机需 `BLKDISCARD` 等 |
| 安卓兼容层 | 未坐实、未接入 | `android-compat/` 已能 `boot_completed=1`，`pm install` 第三方 APK 出帧/可交互未坐实；本轮聚焦安全本体，未改动、未纳入擦除引擎范围 |
| 图形桌面帧缓冲与输入桥 | 未开始 | 界面图是真实状态快照的可视化取证，不是已跑通的图形桌面 |
| L2 五门机检第 1 门 | **仍 FAIL** | 存量 `guest/src` 56 处设备路径直连，见 `evidence/round-001/gate1-debt.md`，不因本轮成果掩盖 |
| S3 软键盘（含中文） | 未开始 | v2 的一票否决硬门，与安全整机正交 |
| 命名「长明」 | 预留 | 未注册、未对外宣称 |
| 本轮是否推送 | 见交付说明 | 本地改动在 `AetherOS-Phone` 工作区，推送前已核对无凭证/大二进制入库 |

---

## 7. 证据索引

`evidence/round-002/INDEX.md` 列出全部证据文件；`secure-sources.sha256.txt`
提供源码哈希用于可复现核对。阶段状态提升只以这些证据为准。
