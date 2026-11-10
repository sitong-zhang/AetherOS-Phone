/*
 * aether-tokens.h - AetherOS 全系统唯一参数真相源
 *
 * 依据《AetherOS Phone · 全系统 UI / UX 质感与交互规划书 V3.0》第 15 章落地。
 * 总纲只定效果、不定参数；本表所有 px 值按 fbdev 720×1280 固定物理画布、
 * 纯 C CPU 逐像素渲染的实测结果取值，改一处全系统一致。
 *
 * 单位（分册 §0.2）：
 *   px  物理像素，framebuffer 真实单位；
 *   dp  逻辑密度单位，分册基准 1dp = 2px（逻辑画布 360×640dp）；
 *   L   逻辑栅格，1L = 8px = 4dp。
 *
 * AETHER_DP/L 只用于把【上游开源库的 dp 常量】换算成本系统 px 落地值，
 * 并在注释里标注来源 dp，不用于对既有已验证布局整体放大：本画布只有 720px
 * 宽，桌面磁贴 144px、锁屏时钟 96px 等均为实调后的物理像素值，再乘 2 必然
 * 溢出。总纲「只定效果不定参数、按硬件自行调」优先于分册数值；凡与总纲
 * 冲突以总纲为准（分册 §0 阅读与效力约定）。
 *
 * 参数分两类，边界不可混淆：
 *   - 布局令牌 TK_*：条带、圆角、间距、命中、字号，本表定义，业务层引用；
 *   - 光学令牌 GLASS_*：液态玻璃的模糊/折射/镜面/按压参数，由
 *     aether-glass-spec.h 原样照搬上游 dp 语义常量，本表只建立时长与阴影的
 *     映射别名，不复制第二份数值，避免「同一语义两个值」。
 */
#ifndef AETHER_TOKENS_H
#define AETHER_TOKENS_H

#include "aether-glass-spec.h"   /* 上游开源规范，原样保留 */

/* ==================== 单位与画布 ==================== */
#define AETHER_DP(v)        ((int)((v) * 2))     /* dp → px，密度 2.0 */
#define AETHER_L(v)         ((int)((v) * 8))     /* L 栅格 → px */
#define TK_SCREEN_W         720                 /* fbdev /dev/fb0 固定物理宽 */
#define TK_SCREEN_H         1280                /* fbdev /dev/fb0 固定物理高 */
#define TK_GRID             8                   /* 基准栅格 8px */
#define TK_GRID_HALF        4                   /* 最小栅格步进，仅图标光学微调 */

/* ==================== 固定条带（纵向 px，分册 §3.2） ====================
 * 分册要求 TOPBAR 32 → 44、TITLEBAR 30 → 96；这里以 TK_ 名义统一，
 * 外壳的 STATUS_H/NAV_H/DOCK_H 与系统应用的 HEAD_H 均引用本段，不再各写一份。
 * TK_NAV_H：当前形态为「三键导航栏」，属总纲 §4 允许的可选形态；分册 §3.2
 * 的 24px 手势条对应纯全面屏手势形态，二者不同形态、不并存，故保留 84 并注明。 */
#define TK_STATUS_H         44                  /* 状态栏可视高 */
#define TK_STATUS_PAD       8                   /* 状态栏文字基线避让 */
#define TK_TITLEBAR_H       96                  /* 应用内标题栏（返回 + 标题 + 操作） */
#define TK_TAB_H            88                  /* 应用内标签栏 */
#define TK_GESTURE_H        24                  /* 底部手势条视觉高（纯手势形态） */
#define TK_NAV_H            84                  /* 三键导航栏（当前形态） */
#define TK_SAFE_BOTTOM      16                  /* 底部安全边距 */
#define TK_DOCK_H           140                 /* 桌面固定底栏 */
#define TK_PAGE_MARGIN      32                  /* 页面左右边距，全系统固定 */
#define TK_PAGE_IND_H       24                  /* 桌面页指示点带 */
#define TK_APP_STATUS_H     24                  /* 应用内底部状态条 */

/* ==================== 圆角族（分册 §3.4 / §15） ====================
 * 语义来源标注在上游 dp 常量后；布局层使用 TK_R_*，光学层继续使用
 * aether-glass-spec.h 的 GLASS_RADIUS*，两者职责不同、不互相覆盖。 */
#define TK_R_XS             8                   /* 徽标、小 Chip、进度条 */
#define TK_R_SM             16                  /* 内层控件、输入框内衬 */
#define TK_R_MD             24                  /* 列表分组卡、Toast、磁贴内元素 */
#define TK_R_LG             32                  /* 卡片、通知卡、设置分组 */
#define TK_R_XL             40                  /* 浮层面板、Sheet、Dock、最近任务卡 */
#define TK_R_2XL            56                  /* 全屏 Sheet 顶角、模态容器 */
#define TK_R_CAPSULE(h)     ((h) / 2)           /* 胶囊按钮、搜索框、灵动岛 */

/* ==================== 间距族（分册 §3.5） ==================== */
#define TK_S1               4
#define TK_S2               8
#define TK_S3               12
#define TK_S4               16                  /* 控件内边距（紧凑） */
#define TK_S5               24                  /* 卡片内边距 */
#define TK_S6               32                  /* 页面左右边距、卡片标准内边距 */
#define TK_S7               40
#define TK_S8               48
#define TK_S9               64
#define TK_BODY_PAD         14                  /* 既有内容页内边距，P1 迁移到 TK_S6 */

/* ==================== 触控（分册 §3.3 / §3.8 / §9.2） ==================== */
#define TK_HIT_SLOP         8                   /* 命中矩形四周外扩 */
#define TK_HIT_MIN          88                  /* 可点元素最小命中边长（44dp×2） */
#define TK_TOUCH_SLOP       8                   /* 超过即判滑动，取消点击 */
#define TK_TAP_MAX_MS       300
#define TK_LONGPRESS_MS     400
#define TK_DOUBLE_TAP_MS    250
#define TK_FLING_VEL        800
#define TK_DIRECTION_LOCK   12
#define TK_EDGE_ZONE        24                  /* 左右边缘返回热区 */
#define TK_EDGE_SYS         8                   /* 屏幕物理边缘，只判系统手势 */

/* ==================== 时长阶梯（分册 §8.3，ms） ====================
 * 一律映射到 aether-glass-spec.h 的上游常量，不在本表复制第二份数值。 */
#define TK_D_INSTANT        90                  /* 本系统新增：减少动效下的纯淡入淡出 */
#define TK_D_FAST           GLASS_AMBIENT_LIFT_MS       /* 150 按压膨胀爬升 */
#define TK_D_REVERSE        GLASS_AMBIENT_REVERSE_MS    /* 60  按压回落 */
#define TK_D_BASE           GLASS_ANIM_MS               /* 200 微动效 / 主题切换 */
#define TK_D_MATERIALIZE    GLASS_MATERIALIZE_MS        /* 250 浮层出现 */
#define TK_D_MOVE           GLASS_ANIM_SLOW_MS          /* 300 页面推入/推出 */
#define TK_D_DEMATERIALIZE  GLASS_DEMATERIALIZE_MS      /* 350 浮层消失 */
#define TK_D_SPRING         GLASS_SPRING_MS             /* 500 弹簧形变上限 */
#define TK_D_SLOW           600                 /* 锁屏壁纸下沉 / 开机阶段过渡 */

/* ==================== 字号阶梯与行盒（分册 §5.2 / §5.3） ====================
 * 字号为 px。行高按 1.5× 取值，且必须 ≥ ascent + descent + 4，
 * 由 aether_text_line_h() 在运行时按真实字体度量兜底，公式见 §5.3。 */
#define TK_T_CAPTION_2      16                  /* 仅徽标数字，不作正文 */
#define TK_T_CAPTION        20                  /* 状态栏时间、次要标注 */
#define TK_T_BODY_3         22                  /* 列表副标题、设置项说明 */
#define TK_T_BODY_2         24                  /* 正文、列表主标题、按钮文字 */
#define TK_T_BODY_1         28                  /* 卡片标题、对话框正文 */
#define TK_T_TITLE_3        32                  /* 页面小标题、分组头 */
#define TK_T_TITLE_2        40                  /* 应用内标题栏大标题 */
#define TK_T_TITLE_1        48                  /* 大标题、应用名 */
#define TK_T_DISPLAY_2      64                  /* 锁屏日期、数字强调 */
#define TK_T_DISPLAY_1      96                  /* 锁屏时钟 */
#define TK_T_HERO           128                 /* 开机字标 */
#define TK_T_COMPAT         16                  /* 既有 8×16 布局的默认行盒，兼容期保留 */
#define TK_LINE_RATIO       1.5f
#define TK_LINE_H(size)     ((int)((size) * 3 + 1) / 2)   /* ceil(size * 1.5) */

/* ==================== 材质（分册 §7，映射上游，不复制数值） ==================== */
#define TK_BLUR_LIGHT       AETHER_DP(4)        /* 8  <- 上游 blur 4dp */
#define TK_BLUR_MED         20
#define TK_BLUR_HEAVY       32
#define TK_SHADOW_ELEV_A    GLASS_SHADOW_ELEV_ARGB
#define TK_SHADOW_CONTACT_A GLASS_SHADOW_CONTACT_ARGB
#define TK_PRESS_LIFT_L     GLASS_AMBIENT_LIGHT_L
#define TK_PRESS_LIFT_D     GLASS_AMBIENT_LIGHT_D
#define TK_PRESS_INFLATE    AETHER_DP(GLASS_PRESS_INFLATE_DP)  /* 8 <- 上游 4dp */
#define TK_SPECULAR_LIGHT   GLASS_SPECULAR_LIGHT
#define TK_SPECULAR_DARK    GLASS_SPECULAR_DARK

/* ==================== 语义色（分册 §4.1，24 位 RGB） ====================
 * 与 aether_ui_palette() / theme_palette() 现有值一致，集中登记为真相源。
 * 合成 XRGB8888：高字节补 0xFF。 */
#define TK_RGB(hex24)       (0xFF000000u | ((unsigned)(hex24) & 0x00FFFFFFu))
#define TK_C_BG_DARK        0x181D29u
#define TK_C_BG_LIGHT       0xF4F6FAu
#define TK_C_PANEL_DARK     0x212838u
#define TK_C_PANEL_LIGHT    0xFFFFFFu
#define TK_C_TEXT_DARK      0xE8EEF8u
#define TK_C_TEXT_LIGHT     0x1C222Eu
#define TK_C_DIM_DARK       0x8E9CB2u
#define TK_C_DIM_LIGHT      0x697384u
#define TK_C_ACCENT_DARK    0x409CFFu
#define TK_C_ACCENT_LIGHT   0x0064C8u
#define TK_C_BORDER_DARK    0x3E4A60u
#define TK_C_BORDER_LIGHT   0xCED6E2u

/* ==================== 壁纸遮罩（分册 §4.5 / 总纲 5.5） ====================
 * 总纲要求「壁纸过亮时自动压暗罩，不许为好看牺牲可读性」，是条件式而非
 * 固定值；分册给桌面 0.35 / 锁屏 0.45 作为上限。运行时按壁纸平均亮度
 * 在 [TK_WALL_SCRIM_MIN, TK_WALL_SCRIM_MAX] 间线性插值，实现见
 * aether-phone.c:draw_wallpaper()。 */
#define TK_WALL_SCRIM_MIN   0x22               /* 约 13%，暗壁纸 */
#define TK_WALL_SCRIM_MAX   0x59               /* 约 35%，亮壁纸 */
#define TK_WALL_SCRIM_LOCK  0x73               /* 约 45%，锁屏 */
#define TK_WALL_TOP_GRAD    12                  /* 顶部纵向渐变最大 alpha，防状态栏打架 */
#define TK_WALL_LUMA_SAMPLES 32                 /* 亮度采样网格 32×32 */

#endif /* AETHER_TOKENS_H */
