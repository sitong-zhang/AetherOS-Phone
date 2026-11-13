# AetherOS 内核（x86_64 自研）

> 这是「完整的操作系统」落地层：引导 → 内核 → 驱动 → 系统调用 → 用户态 init → 安全服务，
> 全部在本机 QEMU TCG **纯软件模拟**真启动、真出图、真交互、真自检。
> 不是用户态仿真，不是套壳，不是 APK。

## 0. 怎么跑

```bash
scripts/kernel.sh build     # 编译内核（freestanding -Werror）+ 用户态 init + 嵌入 initrd + 虚拟盘
scripts/kernel.sh test      # QEMU TCG 真启动，自动交互测试 + 真实帧缓冲取证 + 断言
scripts/kernel.sh evidence  # 重新取证（截图 + 串口日志 + 销毁证明）
scripts/kernel.sh gate      # 内核门禁六门机检
scripts/aether.sh secure    # 安全整机（用户态）一键
scripts/aether.sh kernel    # 转发到 kernel.sh
```

## 1. 架构（按厂商标准链路，已学开源教程后实现）

```
boot.S  (Multiboot1 + PAE/EFER/CR0.PG 切 IA-32e 长模式 + GDT/TSS)
  └─> kmain() 开机自检链
        ├─ console   : COM1 串口 + VGA 文本帧缓冲
        ├─ pmm       : multiboot mmap 位图，4 KiB 帧
        ├─ vmm       : 4 级页表（拆 2 MiB 大页为 4 KiB 页，置 U/S 位供 ring3）
        ├─ cpu/int   : GDT/TSS/IDT/8259 PIC，256 个中断桩，异常→内核 Fault 日志
        ├─ devices   : PIT(100Hz)/PS2 键盘/ATA PIO 块设备/CSPRNG
        ├─ crypto     : SHA-256/HMAC/PBKDF2/ChaCha20（标准向量 + OpenSSL 交叉验证）
        ├─ secure     : 内核态安全子系统（认证/保险库/自毁/审计/销毁证明）
        └─ elf_load_and_run() 加载 init.elf（独立 ELF64）→ task_enter_user iretq 切 ring3
              └─> init.c (PID 1, ring3) 经过 int 0x80 系统调用访问内核服务
```

四区磁盘布局（虚拟 IDE 盘 = 可擦除「用户数据区」；内核本身由 `-kernel` 加载 = 系统区）：

| LBA | 内容 |
|---|---|
| 0 | 超级块（口令盐/哈希、胁迫哈希、DEK 密封、失败计数、审计锚、MAC） |
| 8.. | 加密保险库数据卷（ChaCha20 + HMAC 密封记录） |
| 2064.. | 审计链记录（只追加、哈希链） |

## 2. 引导链（关键坑，已踩过）

- QEMU `-kernel` 内置 Multiboot 加载器只吃 **32 位 ELF 容器**，但内部可装 64 位代码。
  故链接成 ELF64 后用 `objcopy -O elf32-i386` 重框（`scripts/kernel.sh`），代码字节不变。
- 清 BSS 绝不可覆盖正在使用的引导页表：页表放进独立 `.boot_scratch` 段，不在 BSS 范围。
  （实测：清 BSS 把 PDPT 自己清零 → 立刻 `#PF → #DF → triple fault`。）
- **ring3 取指 `#PF err=0x5` 的真正根因是 U/S 位两级都没置**（非 TLB）：x86 分页在
  PML4/PDPT/PD/PT **每一级**都查 U/S 位。① `boot.S` 的上层项 `PML4[0]`/`PDPT[0]` 原用
  `K_PTE_KERNEL_FLAG=0x03`（无 US）→ ring3 走不到任何用户页；② `memory.c` 拆 2 MiB 内核大页
  时 512 个 4 KiB 叶项漏 `PE_US`。两处都补上 `PE_US` 后 `DIAG PTE(0x400000)=0x400067 P=1 RW=1 US=1`，
  用户态取指即通。内核自身 2 MiB 大页（PD[0]，覆盖 0–2MiB 内核区）保持 US=0，用户访问在 PD 层
  被挡 → 隔离仍成立。`isr.S::task_enter_user` 切 ring3 前仍保留一次 CR3 重载（无害，作为 TLB 保险）。
  （实测：`[KERNEL FAULT] vec=14(PF) err=0x5 RIP=0x400000 CR2=0x400000 CS=0x1b` → 修复后消失。）
- **系统调用参数映射**：`usyscall` 把参数排成 `rdi/rsi/rdx/rcx`，分发侧早期误从错误寄存器取长度
  （`K_SYS_WRITE` 把长度取了 `a3`=rdx=0，导致 `kmemcpy` 拷 0 字节、所有用户输出变空串），并漏改
  `K_SYS_READ`/`K_SYS_VAULT_PUT` 的取参。已按真实映射修正（长度取 `a2`）。
- **ATA PIO 写时序**：`ata_select` 发命令前必须等驱动器就绪（`BSY=0 & DRQ=1`），否则写命令被拒、
  出厂配置（超级块）落盘失败（读命令侥幸成功）。已加 `ata_wait_ready()`。
- **系统摘要只哈希不可变区**：`ksec_sys_digest()` 原哈希 `__kernel_start..__kernel_phys_end`，该段含
  `.boot_scratch` 页表（拆页时改写）与 `.data/.bss`（运行期全局变量），导致销毁前后摘要必不同、
  误报「系统被破坏」。改为只哈希 `__kernel_text_start..__kernel_ro_end`（代码+只读数据）。

## 3. 系统调用

`int 0x80`（IDT[0x80]，DPL=3）。约定：`rax=调用号`，`rdi/rsi/rdx/rcx=参数`，返回写 `rax`。
覆盖：write/read(非阻塞)/get_ticks/sysinfo/login/unlock/lock/vault_put/vault_get/vault_list/
wipe_status/audit_dump/selftest/boot_magic/exit。用户态不能直接访问设备或内核内存。

## 4. 安全子系统（主打安全，内核态实现）

与 `secure/` 用户态安全整机同源设计：输错口令达阈值 → 销毁 DEK(crypto-shred) +
多轮覆写整卷 + 清除全部凭据，而 `.text/.rodata` 摘要硬断言前后一致（系统不抹除）。
内核态额外做：设备密钥 MAC（防篡改计数/防「改计数文件」绕过）、只追加审计哈希链、
系统区完整性自检（篡改即擦）、ring3 输入链路（int 0x80）贯穿认证/保险库/自毁。

## 5. 测试与取证

- `kernel/tests/test_kernel_crypto.c`：host 端编译内核 crypto 跑标准向量，12 PASS / 0 FAIL，
  并与 OpenSSL 命令行交叉验证。
- `tools/kernel_qemu_driver.py`：自己 listen unix socket 让 QEMU 连入（早期串口不丢），
  QMP `screendump` 导出真实 VGA 帧缓冲 PPM→PNG，解析结构化串口日志断言完整闭环。
- 证据 `evidence/round-003/`：串口日志、QMP 截图、销毁证明、测试结果 JSON。

## 6. 边界（如实标注，不实称完成）

- **纯软件模拟**：本机无 KVM，QEMU TCG 跑，性能远低于真机；覆写/派生耗时以秒计。
- **未上真机**：无 TEE/安全芯片；`DEV_KEY` 是内核只读常量，模拟 TEE 锚点，**非真 TEE**。
  真机阶段需把失败计数/设备密钥锚定到硬件安全区，块设备用 SECURE ERASE 命令。
- **图形桌面**：init 是文本 + 系统调用演示，非 GUI；VGA 文本帧缓冲为取证截图来源，
  非像素级窗口系统。
- 引导/内核/驱动/系统调用/用户态/安全服务均真实跑通；安卓兼容层未接入本内核。
