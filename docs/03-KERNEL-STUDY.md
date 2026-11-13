# 03 · 开源教程学习笔记（动手前先学）

> 用户指令：「不管你会不会做，你都先去开源仓库先去收别人的教程学习」。
> 本文档是**动手写内核之前**先做的资料收集与消化记录，全部来源为公开开源社区/仓库，
> 检索日期 2026-11-13。学完才开工；借鉴点、自研边界、未采纳方案都如实写明。

## 1. 收到的资料清单

| # | 来源 | 类型 | 学到什么 |
|---|---|---|---|
| [[1]][r1] | OSDev Wiki · Bare Bones | 官方入门教程 | 标准链路 GRUB→Multiboot→ELF kernel→QEMU；freestanding 编译；链接脚本 1 MiB 加载 |
| [[2]][r2] | OSDev Wiki · Setting Up Long Mode | 官方教程 | 32→64 位切换的精确步骤与寄存器位；4 级页表；GDT 64 位描述符 |
| [[3]][r3] | OSDev Wiki · Meaty Skeleton | 官方骨架 | 厂商式目录结构：`arch/` 隔离架构相关代码、sysroot、libc/libk 分层、kernel 与用户态分离 |
| [[4]][r4] | OSDev Wiki · Creating a 64-bit kernel (separate loader) | 官方教程 | multiboot 只交 32 位环境，64 位内核需 loader/自切换；ELF64 加载与 `-z max-page-size=0x1000` |
| [[5]][r5] | OSDev Wiki · Loading ELF Binaries | 官方教程 | PT_LOAD 段解析、`p_vaddr/p_filesz/p_memsz`、按 base 计算镜像、入口 `e_entry` |
| [[6]][r6] | GitHub · milescoviello/OS-DEV | 可运行开源 OS | **关键技巧**：QEMU `-kernel` 的 multiboot 加载器拒绝 ELF64，解法是链接成 ELF64 后用 `objcopy -O elf32-i386` 重框成 32 位容器，内部 64 位代码字节不变；无需 GRUB/xorriso |
| [[7]][r7] | GitHub · davmac314/multiboot-kernel64 | 可运行开源 OS | 同样的「32 位容器装 64 位内核」+ higher-half 映射的独立实现，交叉印证 |
| [[8]][r8] | GitHub · Amit123103/Basic_kernel (MyKernel) | 可运行开源 OS | 子系统清单对照：multiboot2 / 长模式 / GDT+TSS / IDT / PMM 位图 / VMM 4 级页表 / 堆 / VGA / COM1 / PS2 键盘 / PIT 100Hz / RTC / 系统调用 / 交互 shell |
| [[9]][r9] | GitHub · phil-opp/blog_os (Multiboot Kernel) | 教程源码 | 链接脚本 `.boot` 段置顶、`ld -n` 禁止自动对齐、multiboot header 必须在文件前 8 KiB |
| [[10]][r10] | GitHub · Cinux-Book（中文，从零手搓 x86_64） | 中文教程 | 从 512B MBR→保护模式→长模式→内存→进程调度→系统调用→文件系统→GUI 的完整「一站一站长出来」路线；QEMU 里跑 |
| [[11]][r11] | GitHub · 田宇《一个64位操作系统的设计与实现》随书源码 | 中文教材 | 国内厂商式分层：引导层/内核层/应用层；BootLoader→内核头→主程序→异常→内存→中断→键盘→进程→系统调用→Shell |
| [[12]][r12] | GitHub · Vac011/MyOS | 中文开源 OS | 同样是「引导层/内核层/应用层」三段式，明确列出参考 LFS、OSDev、田宇书 |
| [[13]][r13] | OSDev Wiki · D Bare Bones With LDC2（检索命中） | 官方教程变体 | 直接给出可用的 32 位 multiboot header 常量、`CR4.PAE`/`EFER.LME`/`CR0.PG` 汇编序列与 64 位 GDT 布局 |

[r1]: https://wiki.osdev.org/Bare_Bones
[r2]: https://wiki.osdev.org/Setting_Up_Long_Mode
[r3]: https://wiki.osdev.org/Meaty_Skeleton
[r4]: https://wiki.osdev.org/Creating_a_64-bit_kernel_using_a_separate_loader
[r5]: https://wiki.osdev.org/Loading_ELF_Binaries
[r6]: https://github.com/milescoviello/OS-DEV
[r7]: https://github.com/davmac314/multiboot-kernel64
[r8]: https://github.com/Amit123103/Basic_kernel
[r9]: https://github.com/phil-opp/blog_os
[r10]: https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book
[r11]: https://github.com/cpp-codes/The-design-and-implementation-of-a-64-bit-os
[r12]: https://github.com/Vac011/MyOS
[r13]: https://wiki.osdev.org/D_Bare_Bones_With_LDC2

## 2. 厂商/社区做操作系统的标准链路（照着学）

```
固件(BIOS/UEFI)
  └─ Bootloader(GRUB / Multiboot)        把内核 ELF 搬到内存、交出内存映射与 cmdline
       └─ 内核引导汇编                    实模式/32位保护模式 → 开分页 → 长模式
            └─ 内核主程序 kmain           GDT/IDT/TSS、异常、PIC、内存管理、驱动
                 └─ 驱动层               串口/显示/键盘/定时器/块设备
                      └─ initrd + ELF 加载器
                           └─ PID 1 init（ring3 用户态）
                                └─ 系统服务 / shell / 应用
```

对应到本项目的落点：

| 厂商环节 | 本项目实现 |
|---|---|
| Bootloader | QEMU 内置 multiboot 加载器（`-kernel`），内核自带 Multiboot1 header |
| 32→64 切换 | `boot.S` 自写：临时 4 级页表 identity map → PAE → EFER.LME → PG → 64 位 GDT → far jump |
| 描述符与中断 | GDT(内核/用户代码数据 + TSS)、IDT 256 门、8259 PIC 重映射、异常打印 |
| 内存 | PMM 位图（来自 multiboot mmap）+ VMM 4 级页表 map/unmap + 内核堆 |
| 驱动 | COM1 串口、VGA text、PS/2 键盘、PIT 100 Hz、ATA PIO 块设备 |
| initrd / 加载器 | multiboot module → 解析 ELF64 PT_LOAD → 建进程页表 → iretq 切 ring3 |
| PID 1 | 用户态 `init`，通过 `int 0x80` 系统调用访问内核服务与保险库 |
| 根文件系统/数据 | 虚拟磁盘（`-drive if=ide`）作为**可擦除数据区**；内核 ELF 作为**只读系统区** |
| 构建 | freestanding `gcc -ffreestanding -nostdlib -mno-red-zone` + 自写 linker script + `objcopy` 重框 |

## 3. 学到的硬事实（写代码要用到的精确值）

- Multiboot1 magic `0x1BADB002`，bootloader 交出 `EAX=0x2BADB002`、`EBX=multiboot_info*`；
  header 必须 4 字节对齐且位于文件前 8 KiB [[1]][r1][[9]][r9]。
- `FLAGS` 常用 `0x00000003`（4 KiB 页对齐 + 传递内存映射）；`mmap` tag 给出可用 RAM 区间。
- 进长模式顺序：建 PML4/PDPT/PD/PT → `CR3=页表` → `CR4.PAE(bit5)=1` →
  `MSR 0xC0000080 (EFER).LME(bit8)=1` → `CR0.PG(bit31)=1` → `lgdt` 64 位 GDT →
  远跳到 64 位代码段 [[2]][r2][[13]][r13]。页表项 `PRESENT=1`、`RW=2`、`HUGE=0x80`。
- 64 位 GDT 代码段 access `0x9A`、flags `LONG_MODE(bit5)`；数据段 access `0x92`；
  用户段 DPL=3（代码 `0xFA`、数据 `0xF2`）[[2]][r2]。
- freestanding 编译：`-ffreestanding -fno-builtin -nostdlib -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -fno-pic -fno-pie`，
  链接 `-z max-page-size=0x1000`，且 GCC 会隐式引用 `memset/memcpy/memmove`，必须自提供 [[4]][r4][[3]][r3]。
- ELF64 加载：校验 `\x7fELF`、`EI_CLASS=2`、`ET_EXEC/ET_DYN`、`EM_X86_64=62`，
  遍历 `PT_LOAD` 按 `p_vaddr` 映射、`p_filesz` 拷贝、`p_memsz-p_filesz` 清零，跳转 `e_entry` [[5]][r5]。
- ring3：IDT 中断门 DPL=3（`0xEE`）、TSS.RSP0 供 ring3→ring0 切内核栈、构造 iretq 帧
  （SS/RSP/RFLAGS/CS/RIP）；系统调用用 `int 0x80` 或 `syscall`（MSR STAR/LSTAR）[[8]][r8]。
- **QEMU 的坑（决定架构选型）**：`-kernel` 的 multiboot 加载器只接受 ELF32 容器。
  解法是链接成真 ELF64 再用 `objcopy -O elf32-i386` 重框，64 位机器码字节不变，
  「No GRUB, no ISO, no xorriso」——正好本机没有 GRUB/xorriso/nasm [[6]][r6][[7]][r7]。

## 4. 借鉴 vs 自研边界（不抄代码，抄结构）

- **借鉴**：标准链路、寄存器位与魔数（硬件事实，不存在「原创」空间）、目录分层 `arch/`+`core/`+`user/`、
  freestanding 构建参数、QEMU 重框技巧、ELF/页表/描述符的规范做法。这些来自公开规范与上述教程。
- **自研**：内核全部源码本轮从零编写；安全子系统（口令失败计数 + 加密保险库 + 自毁引擎 +
  审计哈希链 + 销毁证明）是本项目独有设计，复用同仓库 `secure/` 里已经过 81 项自测的算法思路，
  但按 freestanding 内核环境重写，不链接任何外部库。
- **不抄**：不 fork / 不复制任何仓库源码文件；不引入 Linux 内核源码；不使用现成镜像冒充自研内核
  （历史归档里的 Android 内核镜像本轮明确不复用）。
- **许可**：仅为学习其公开教程与规范事实，未纳入第三方代码，仓库许可证沿用本项目原有 LICENSE。

## 5. 与 v2 总纲的对照（如实标注差异）

| 总纲条目 | 本轮状态 |
|---|---|
| §2.1 环境基线重填 | 已沿用 `docs/00-BASELINE.md`（32 核 / 123 GB / 无 KVM → 只能 TCG） |
| §2.2 六种一键脚本 | `scripts/kernel.sh` 提供 build/run/test/evidence/rebuild/status，并接入 `aether.sh` |
| §2.5 可复现构建 | 固化编译器参数、链接脚本、两次构建产物哈希比对 |
| §2.6 双轨 | **A 轨（TCG 整机）本轮原型选 x86_64**，因为 QEMU x86_64 的 multiboot 链路最成熟且教程完备；总纲 A 轨原文为 ARM64，此处如实标为**架构偏差**，ARM64 端口列为后续，不假称已满足 |
| §5.6 五门机检 | 新增内核专项机检：设备路径不越层、参数单一源、声明-实现一致性、freestanding 无 libc、引导链自检 |
| §11.3 不自改门禁 | 规则写死、命中即 FAIL，不为凑通过放宽 |
| 证据四件套 | `evidence/round-003/` 出用例结果 / 异常报告 / 改动 diff / 回归对比 |
| 三档状态标注 | 报告按「已跑通 / 部分实现 / 未开始」如实标注 |

## 6. 未采纳的方案与原因

- **GRUB + ISO 镜像**：本机无 `grub-mkrescue`/`xorriso`；QEMU `-kernel` 直接加载更省一层，且 [[6]][r6] 验证过。
- **NASM**：本机无 nasm，改用 GNU `as`（AT&T 语法），与 gcc/gas 工具链一致。
- **UEFI/OVMF**：本机无 OVMF 固件文件；Multiboot1 + BIOS 路径足够验证完整引导链。
- **直接用归档的 Linux/Android 内核镜像**：那是别人的内核，不能叫「自研系统」；本轮从零写内核。
- **ARM64 直上**：无 multiboot，需自写 GIC/PL011/异常向量/设备树，风险高且违背「先跑通再扩展」；列为后续端口。
