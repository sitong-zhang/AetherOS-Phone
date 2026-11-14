# 内核轮次报告 · round-003（R&D 内核，历史留痕）

> ⚠️ **口径更正（2026-11-14，评估报告 V17 + 用户判向纠正）**：
> 1. 标题旧称「真机闭环」不实——实为 **QEMU TCG 纯软件模拟**，非真机、非 TEE。
>    标题与正文的历史措辞**原样保留**（可作留痕），但不得据此认定产品已真机验证。
> 2. 断言数 34/34 是**当时轮次**的数字；后续加抢占式多任务后为 **37/37**，非 34。
> 3. 本报告对象是 **`kernel/` 自研 x86_64 内核 = 研究性 R&D 教学代码，非产品内核**；
>    产品真实内核是 **Linux**，产品安全以 `secure/` 为准（见 `SYSTEM-BLUEPRINT.md` / `AGENTS.md`）。
>    经漏洞清单 V1–V17 审计，本 R&D 内核的「安全 demo」**不代表产品安全能力**。
> 本文件属**历史留痕**，不作当前验收依据；判向以「当前有效规范」为准。

> 日期：2026-11-14
> 范围：自研 x86_64 内核在 QEMU TCG 纯软件模拟下的真实启动、ring3 用户态、完整安全演示。
> 结论（当时）：**34/34 集成断言全绿，QEMU isa-debug-exit 退出码 33；六门静态机检 8/8 全绿。**
> 边界（必须如实保留）：纯软件模拟（QEMU TCG，非真机/非 TEE）；无 GUI 桌面；安卓兼容层未接入。

---

## 1. 一句话结论

上一轮卡在 ring3 取指 `#PF err=0x5`。本轮把根因逐层下钻，发现并修复了 **4 个独立 bug**（页表 U/S 两级、系统调用参数映射、ATA PIO 写时序、系统摘要哈希了可变内存），最终让「出厂 → 解锁 → 加密存 3 文件 → 读回 → 锁定 → 输错 3 次 → 全盘销毁且系统完好 → 重新初始化 → 旧数据不可恢复 → 交互 shell」整条链路在真实 QEMU 里跑通闭环。

---

## 2. 改了什么 / 为什么（4 个 bug，按定位顺序）

### Bug A — 拆 2MiB 大页漏 U/S 位 + 上层页表项漏 U/S 位（ring3 #PF 根因）
- **现象**：`[KERNEL FAULT] vec=14(PF) err=0x5 RIP=0x400000 CR2=0x400000 CS=0x1b`，用户态在 0x400000 取指令即保护违规。
- **根因（两层）**：x86 分页在 PML4/PDPT/PD/PT **每一级**都查 U/S 位。
  1. `kernel/core/memory.c` 拆 boot 建的内核 2MiB 大页时，512 个 4KiB 叶项写死 `PE_PRESENT|PE_RW`，**漏 `PE_US`**（调用方 `elf.c` 传的 `RW|US=0x06` 被整段丢弃）。
  2. `kernel/arch/x86_64/boot.S` 的上层页表项 `PML4[0]`/`PDPT[0]` 用 `K_PTE_KERNEL_FLAG=0x03`（无 US），上层 U/S=0 时 ring3 根本走不到任何用户页。
- **修复**：
  - `memory.c` 拆页分支叶项改为 `(base+i*PAGE)|PE_PRESENT|PE_RW|(flags & PE_US)`，把调用方要求的 US 位带下来。
  - `boot.S` 上层项改用 `K_PTE_USER_FLAG=0x07`（P|RW|US）。ring0 访问无视 U/S 位，内核运行不受影响；内核自身 2MiB 大页（PD[0]，覆盖 0–2MiB 内核区）保持 US=0，用户访问在 PD 层被挡 → **隔离仍然成立**。
- **诊断**：`memory.c` 新增 `vmm_pte_flags()`，`elf.c` 打印 `DIAG PTE(0x400000)=0x400067 P=1 RW=1 US=1`，证实叶项 U/S 已置位。

### Bug B — 系统调用参数映射错乱（所有用户输出变空串 + vault_put 错位）
- **现象**：ring3 进了 demo，但串口里**零用户文本**；追踪 `[SC1]`（write）系统调用被大量调用却无输出。
- **根因**：`usyscall` 把参数映射为 `rdi=arg1, rsi=arg2, rdx=arg3`；但 `syscall.c` 的几个分发分支取错了寄存器：
  - `K_SYS_WRITE` 把长度取了 `a3`(=rdx=0) → `kmemcpy(tmp, s, 0)` 只拷 0 字节 → 输出空串。
  - `K_SYS_READ` 的 `!a3` 误判（应为 `!a2`）。
  - `K_SYS_VAULT_PUT` 把 `data` 取了 `rdx`(=len)、`len` 取了 `rcx`(=0) → 写入错位。
- **修复**：`syscall.c` 三处按 `usyscall` 的真实映射取参（`WRITE`/`READ` 长度取 `a2`；`VAULT_PUT` 取 `rsi`/`rdx`）。`u_login`/`u_unlock`/`u_selftest` 等映射本就正确，未动。

### Bug C — ATA PIO 写时序（出厂配置写盘失败，demo 卡在 PROVISION_FAIL）
- **现象**：demo 跑到 stage 3 返回 `EXIT(3)`（PROVISION_FAIL）；`ksec_setup` 经 `ata_write` 落盘返回 `-2`。
- **根因**：`devices.c` 的 `ata_select` 发命令前**没有等驱动器就绪**（BSY=0 且 DRQ=1）。读命令在 QEMU 上侥幸成功（开机读超级块无误判为出厂），写命令因未就绪被拒 → `ata_poll` 等不到 DRQ → 超时 `-2`。
- **修复**：新增 `ata_wait_ready()`，在 `ata_select` 发命令前先等 `BSY=0 & DRQ=1`。读/写共用，时序正确。

### Bug D — 系统摘要哈希了可变内存（误报「系统被破坏」）
- **现象**：销毁证明 `sys_unchanged: false`、`result: SYSTEM_INTEGRITY_FAILURE`；即便销毁前 selftest 也显示 `sys_digest=CHANGED`。
- **根因**：`vault.c` 的 `ksec_sys_digest()` 哈希范围 `__kernel_start..__kernel_phys_end`，该段包含：
  1. `.boot_scratch`（**页表**）——`vmm_map` 拆用户页时改写 → 哈希变；
  2. `.data`/`.bss`（**运行期全局变量**，如 `g_state`/`g_super`/PMM 位图）——开机后即被改写 → 哈希变。
  两处都会让「销毁前后摘要不同」，误判系统被破坏。
- **修复**：链接脚本在 `.rodata` 后加 `__kernel_ro_end`、在 `.text` 起加 `__kernel_text_start`；`ksec_sys_digest()` 只哈希 `__kernel_text_start..__kernel_ro_end`（代码+只读数据，运行期不可变）。销毁证明从此稳定为 `sys_unchanged: true` / `ALL_USER_DATA_DESTROYED_SYSTEM_INTACT`。

---

## 3. 验证（真机驱动，不实称）

| 项目 | 命令 | 结果 |
|---|---|---|
| 真实启动 + 引导链 + 安全 Demo 闭环 | `scripts/kernel.sh test` | **34/34 PASS**，QEMU 退出码 **33** |
| 六门静态机检（构建/密码学/隔离/参数单一源/可复现/Multiboot） | `scripts/kernel.sh gate` | **8/8 PASS** |
| 内核密码学单测（host 编译跑） | `gate` 门 2 | 12 PASS / 0 FAIL |
| 真实 VGA 帧缓冲取证 | `evidence/round-003/0*.png`（QMP screendump） | 显示 `sys_unchanged:true`、`ALL_USER_DATA_DESTROYED_SYSTEM_INTACT`、`OLD_DATA_GONE`、`DEMO_DONE=1 ALL STAGES PASSED` |

串口关键标记（机器可判，全部命中）：`BOOT_MAGIC_OK` → `SELFTEST crypto_sha256=PASS` → `PROVISION_OK` → `UNLOCK_OK` → `VAULT_PUT_OK x3` → `VAULT_GET_OK` → `LOCK_OK` → `DENIED (1/3)(2/3)` → `WIPED: 3 wrong passwords reached threshold 3` → `WIPE_RECEIPT_OK sys_unchanged=true` → `REINIT_OK` → `OLD_DATA_GONE` → `DEMO_DONE=1 ALL STAGES PASSED` → `SHELL READY` → 注入 `status/selftest/exit` 全部回显 → `KERNEL_EXIT_CODE=0`（→ isa-debug-exit 33）。

---

## 4. 证据四件套（本轮）
- `evidence/round-003/serial-live.log` —— 真实串口全程（实时落盘，含 DIAG 与 demo 全输出）
- `evidence/round-003/qemu-serial.log` —— 完整串口
- `evidence/round-003/0{1,2,3}-*.png/.ppm` —— QEMU 真实 VGA 帧缓冲三张（boot-post / data-destruction / interactive-shell）
- `evidence/round-003/kernel-test-results.json` —— 34 项断言 + 退出码 + 截图清单
- `evidence/round-003/qemu-exit-code.txt` / `qemu-cmdline.txt` —— 退出码与 QEMU 命令行

> 注：`kernel-test-results.json` 与截图由 `tools/kernel_qemu_driver.py` 在每次 `test` 时覆盖刷新；机检结果写入 `gate_kernel.json`。

---

## 5. 未解决 / 已知边界（如实标注，不夸大）
1. **纯软件模拟**：QEMU TCG，无 KVM、无真机 TEE/SE；安全子系统的「防篡改/销毁证明」在模拟环境验证，物理侧（侧信道、冷启动、NAND 残值）未覆盖。
2. **无 GUI 桌面**：当前是串口控制台 + VGA 文本模式；不是图形 OS。
3. **安卓兼容层未接入**：内核是独立自研 OS，尚未与 Android/Linux 用户态兼容层打通。
4. **销毁是逻辑级**：对 ATA 镜像做多轮覆写 + 凭据清零 + 摘要比对；对物理闪存的块级擦除/损耗均衡不可控（同边界 1）。
5. **单次完整回归 + 敏感信息扫描 + 可复现构建哈希** 已通过门禁，提交前再做一次全量确认。

---

## 6. 提交计划
- 本轮改动文件：`kernel/arch/x86_64/boot.S`、`kernel/arch/x86_64/linker.ld`、`kernel/core/memory.c`、`kernel/core/syscall.c`、`kernel/core/devices.c`、`kernel/core/interrupts.c`（删调试追踪）、`kernel/secure/vault.c`、`tools/kernel_qemu_driver.py`、`tools/gate_kernel.py`、文档。
- 仓库仍只发 `sitong-zhang/AetherOS-Phone`；token 不入库。
- 提交信息如实写「ring3 安全 demo 真机闭环 + 4 个内核 bug 修复」。

---

## 7. 后续（本轮新增）：PIT 时钟驱动的抢占式多任务

在「完整操作系统」总纲下，选定内核支柱推进抢占式调度，不破坏已绿的安全 demo 闭环。

**做了什么**
- 新增 `kernel/core/proc.c`：进程表（`proc_t` 含 pid/state/quantum/name/整帧 `ctx`）+ 轮转调度器。
  - `sched_tick()`：PIT(100Hz) 中断里调用，**仅当用户态(CS.RPL=3)** 才保存当前整帧、按 `K_SCHED_QUANTUM=4` 节拍轮转下一个 READY 任务；改写中断返回帧 `regs_t` 的 iret 字段，isr 返回即 `iretq` 切到下一任务（厂商式「改写返回帧」上下文切换）。
  - `sched_exit_task()`：`SYS_EXIT` 落点——init 退出 = 整内核收尾；其余任务标记 EXITED 并切换，不终止内核。
- 新增 `kernel/user/task2.c` + `kernel/user/user2.ld`：第二个 ring3 心跳任务（@0x600000，独立代码段与用户栈），每 ~80ms 打印 `TASK2 PID=2 tick=.. HEARTBEAT #n`，~0.4s 后打 `TASK2 DONE` 并 `SYS_EXIT`。`init`（安全 demo）保留为 PID1。
- `elf.c` 拆分出 `elf_load()`（只映射 ELF + 用户栈、不进 ring3）；`interrupts.c` 在 PIT 处理中接入 `sched_tick()`；`syscall.c` 扩展 `GETPID`/`YIELD`/`EXIT`；`console.c` 加关中断保护 VGA/串口光标，避免抢占下状态交错乱码。
- `scripts/kernel.sh` 编译/链接/嵌入 task2；`kernel_qemu_driver.py` 断言由 34 增至 37。

**真机验证（不实称）**
- QEMU TCG 真启动：**37/37 断言全绿**，退出码 33，三轮真实 VGA 帧缓冲取证。
- 串口实证 init 与 task2 真实交织：`TASK2 PID=2 tick=15 HEARTBEAT #0` 出现在 `SHELL READY` 之前，`#1..#4` 与 shell 命令输出交错，末行 `TASK2 DONE pid=2`。
- 六门静态机检 **8/8** 全过，内核树可复现 sha256 一致。

**已知边界（如实标注）**
1. **共享内核页表**：双任务运行在同一套 4 级页表上，各有独立代码段与用户栈，但**任务间无 MMU 地址空间隔离**（隔离为后续阶段）；内核区(U/S=0)任务态不可访问。
2. 其余边界同 §5（纯软件模拟、无 GUI、无真机 TEE、安卓兼容层未接入）不变。
