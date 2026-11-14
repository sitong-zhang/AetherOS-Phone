/*
 * AetherOS Kernel · 参数单一源（v2 §5.6 第五门 / §11.3）
 *
 * 所有内核与安全子系统的可调数值 ONLY 在此定义。
 * 任何 .c/.S 文件不得再硬编码下列数值的字面量；tools/gate_kernel.py 会扫描。
 * 修改参数只改这里，不改实现。
 */
#ifndef AETHER_KCONFIG_H
#define AETHER_KCONFIG_H

/* ---- 内核标识 ---- */
#define K_VERSION_MAJOR        1
#define K_VERSION_MINOR        0
#define K_VERSION_PATCH        0
#define K_CODENAME             "Changming-Aether"
#define K_BUILD_MAGIC          0x41455448u   /* 'AETH' */

/* ---- 引导 / 链接 ---- */
#define K_LOAD_PHYS            0x00100000u   /* 1 MiB，multiboot 约定 */
#define K_BOOT_STACK_BYTES    16384u
#define K_IDT_ENTRIES         256u
#define K_GDT_ENTRIES         6u
#define K_PDE_HUGE_FLAG       0x83u         /* P|RW|PS(2MiB) */
#define K_PTE_KERNEL_FLAG     0x03u         /* P|RW */
#define K_PTE_USER_FLAG       0x07u         /* P|RW|US */
#define K_EFER_MSR            0xC0000080u
#define K_EFER_LME_BIT        8u
#define K_CR4_PAE_BIT         5u
#define K_CR0_PG_BIT          31u
#define K_MB_MAGIC_LOADER     0x2BADB002u
#define K_MB_HEADER_MAGIC     0x1BADB002u
#define K_MB_HEADER_FLAGS     0x00000003u   /* ALIGN_4K | MEMINFO */

/* ---- 显示 / 控制台 ---- */
#define K_VGA_COLS            80u
#define K_VGA_ROWS            25u
#define K_VGA_FB_PHYS         0x000B8000u
#define K_SERIAL_COM1         0x03F8u
#define K_QEMU_EXIT_PORT      0x00F4u       /* isa-debug-exit */
#define K_QEMU_EXIT_OK       0x10u

/* ---- 中断 / 时钟 ---- */
#define K_PIC1_CMD            0x0020u
#define K_PIC1_DATA           0x0021u
#define K_PIC2_CMD            0x00A0u
#define K_PIC2_DATA           0x00A1u
#define K_PIT_CH0             0x0040u
#define K_PIT_CMD             0x0043u
#define K_PIT_HZ              100u
#define K_PIT_BASE_HZ         1193182u
#define K_PS2_DATA            0x0060u
#define K_PS2_STATUS          0x0064u

/* ---- 内存 ---- */
#define K_PAGE_SIZE           4096u
#define K_HUGE_PAGE_SIZE      (2u * 1024u * 1024u)
#define K_IDENTITY_MAP_MB     1024u         /* identity map 低 1 GiB */
#define K_HEAP_START          0x00200000u   /* 2 MiB */
#define K_HEAP_BYTES          (8u * 1024u * 1024u)
#define K_PMM_MAX_MB          512u          /* 管理上限，按基线 123 GB 保守取 512 MiB */
#define K_USER_LOAD_VADDR     0x0000000000400000ull  /* 用户态 ELF 链接基址 4 MiB */
#define K_USER_STACK_TOP      0x0000000000300000ull  /* 用户栈 3 MiB 向下 */
#define K_USER_STACK_BYTES    (64u * 1024u)
/* 第二 ring3 任务（演示抢占式多任务）：独立代码段与栈，共享内核页表 */
#define K_TASK2_LOAD_VADDR    0x0000000000600000ull  /* 任务2 ELF 链接基址 6 MiB */
#define K_TASK2_STACK_TOP     0x0000000000500000ull  /* 任务2 栈 5 MiB 向下 */
#define K_TASK2_STACK_BYTES   (64u * 1024u)
#define K_SCHED_QUANTUM       4u    /* 每个任务时间片（PIT 节拍，100Hz 下 = 40ms） */

/* ---- 块设备 / 数据区（可擦除） ---- */
#define K_ATA_IO_BASE         0x01F0u
#define K_ATA_CTRL            0x03F6u
#define K_SECTOR_BYTES        512u
#define K_DISK_SUPER_LBA      0u            /* 超级块：失败计数 / 审计锚 / 凭据 */
#define K_DISK_VAULT_LBA      8u            /* 加密保险库数据区起始 */
#define K_DISK_VAULT_SECTORS  2048u        /* 1 MiB 数据区 */
#define K_DISK_JOURNAL_LBA    2064u        /* 审计链区 */
#define K_DISK_JOURNAL_SECTORS 64u
#define K_DISK_MAGIC          0x41564F4Cu  /* 'AVOL' */
#define K_DISK_TOTAL_SECTORS  4096u

/* ---- 安全 ---- */
#define K_PW_MAX_LEN          63u
#define K_NAME_MAX_LEN        31u
#define K_SALT_BYTES          16u
#define K_HASH_BYTES          32u           /* SHA-256 */
#define K_KEY_BYTES           32u
#define K_NONCE_BYTES         12u
#define K_TAG_BYTES           32u           /* HMAC-SHA256 截断用全长 */
#define K_PBKDF2_ITER         1024u         /* TCG 软模拟下的实测折中，见报告 */
#define K_MAX_FAILS_DEFAULT   3u            /* 输错 N 次即抹除 */
#define K_WIPE_PASSES         4u            /* 覆写轮数 */
#define K_WIPE_PATTERNS       4u            /* 0x00 / 0xFF / RNG / 0x00 */
#define K_AUDIT_MAX_EVENTS    256u
#define K_VAULT_MAX_FILES     32u
/* 单条保险库记录必须能装进 1 个 512B 扇区：
 * used4+name32+len4+nonce12+tag32+data384 = 468B <= 512B（v2 声明-实现一致性） */
#define K_VAULT_FILE_BYTES    384u
#define K_SESSION_IDLE_TICKS  (K_PIT_HZ * 60u)

/* ---- 系统调用号（内核与用户态共享） ---- */
#define K_SYS_WRITE           1u   /* (fd, buf, len) -> n */
#define K_SYS_READ            2u   /* (fd, buf, len) -> n；fd=0 串口/键盘 */
#define K_SYS_GET_TICKS       3u   /* () -> ticks */
#define K_SYS_SYSINFO         4u   /* (buf, len) -> n */
#define K_SYS_LOGIN           5u   /* (pw, len) -> 状态 */
#define K_SYS_VAULT_PUT       6u   /* (name, data, len) -> 0/err */
#define K_SYS_VAULT_GET       7u   /* (name, buf, len) -> n */
#define K_SYS_VAULT_LIST      8u   /* (buf, len) -> n */
#define K_SYS_LOCK            9u   /* () -> 0 */
#define K_SYS_UNLOCK          10u  /* (pw,len) -> 状态 */
#define K_SYS_WIPE_STATUS     11u  /* (buf,len) -> n */
#define K_SYS_AUDIT_DUMP      12u  /* (buf,len) -> n */
#define K_SYS_SELFTEST        13u  /* (buf,len) -> n */
#define K_SYS_EXIT            14u  /* (code) -> 不返回 */
#define K_SYS_BOOT_MAGIC      15u  /* () -> 内核启动魔数，证明出自 ring0 */
#define K_SYS_GETPID          16u  /* () -> 当前任务 pid */
#define K_SYS_YIELD           17u  /* () -> 主动让出（仍由时钟抢占驱动切换） */

/* ---- 解锁返回状态 ---- */
#define K_AUTH_OK             0
#define K_AUTH_FAIL           1
#define K_AUTH_WIPED          2
#define K_AUTH_DURESS         3
#define K_AUTH_LOCKED         4

#endif /* AETHER_KCONFIG_H */
