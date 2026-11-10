/*
 * aether-anim.h - AetherOS 全系统统一动效曲线与动画时钟
 *
 * 总纲 6.2/5.13：全系统只允许一套控件语言、一套曲线体系；
 * 转场必须有方向感；减少动效时降为极短淡入淡出（5.20 无障碍）。
 *
 * 设计：单调毫秒时钟 + 缓动函数 + 轻量「过渡句柄」。UI 层用
 * aether_anim_begin() 记录起点，aether_anim_value() 取当前缓动进度，
 * 合成器用 aether_anim_active() 决定是否需要连续重绘（跟手动效）。
 * 不依赖任何图形库，纯 C，可在交叉编译的静态二进制中使用。
 */
#ifndef AETHER_ANIM_H
#define AETHER_ANIM_H

#include <stddef.h>

/* ---- 单调时钟（毫秒，不受系统时间调整影响） ---- */
long aether_anim_now_ms(void);
double aether_anim_now_s(void);

/* ---- 统一曲线体系 ---- */
/* 标准减速：面板进入、图标浮现、页面推入（0..1） */
double aether_ease_out(double t);
/* 标准加减速：位置移动、缩放（0..1） */
double aether_ease_in_out(double t);
/* 轻微回弹：落位、胶囊收放（0..1，可略超 1） */
double aether_ease_back(double t);
/* 弹性回弹：下拉回弹、拖拽归位 */
double aether_ease_elastic(double t);
/* 线性：进度条、计时数字 */
static inline double aether_ease_linear(double t) { return t < 0 ? 0 : (t > 1 ? 1 : t); }

/* ---- 过渡句柄 ----
 * from/to 是「视觉值区间」，默认 0→1。分册 §8.5 硬性要求：转场中途被
 * 反向打断时必须从【当前视觉值】续接，不许跳回起点重播。做法是把当前
 * 值作为新句柄的 from，目标作 to，value() 返回 from+(to-from)*curve(p)。 */
struct aether_anim {
    long   t0;        /* 起始毫秒 */
    long   dur;       /* 时长毫秒 */
    int    active;
    double from, to;  /* 视觉值区间（默认 0 → 1） */
};

/* 启动一个过渡；dur_ms<=0 或「减少动效」时立即完成 */
void aether_anim_begin(struct aether_anim *a, long dur_ms);
/* 把句柄直接钉在某个视觉值（无动画、无活跃计数）：用于 IPC/settle
 * 把两段式吸附点或浮层终态一步推到位，之后 value() 恒返回该值。 */
void aether_anim_set(struct aether_anim *a, double v);
/* 从指定视觉值续接到目标值（可打断转场用）；dur_ms 为完整时长，
 * 会按剩余距离占比压缩，避免「只差一点却走满 300ms」。 */
void aether_anim_begin_from(struct aether_anim *a, long dur_ms,
                            double from, double to);
/* 重置为未开始 */
void aether_anim_reset(struct aether_anim *a);
/* 取线性进度 0..1（未经缓动），到点返回 1 并把 active 置 0 */
double aether_anim_progress(struct aether_anim *a);
/* 取指定缓动后的视觉值；curve 传 aether_ease_* */
double aether_anim_value(struct aether_anim *a, double (*curve)(double));
/* 只读查询当前视觉值（不推进、不回收句柄），供打断时记录起点 */
double aether_anim_visual(const struct aether_anim *a, double (*curve)(double));
/* 是否仍在运动（用于合成器连续重绘判定） */
int aether_anim_running(const struct aether_anim *a);

/* ---- 距离联动时长（分册 §8.3） ----
 * 实际时长 = token_ms * clamp(dist / ref, 0.6, 1.15)，防止「短位移长动画」。
 * ref：页面位移传全屏高，控件位移传 200px。 */
long aether_anim_duration(long token_ms, double dist, double ref);

/* ---- 二阶弹簧（分册 §8.2，连续形变类元素） ----
 * a'' = -k(a-target) - c*a'，k=180，c=2ζ√k，ζ=1-bounce。
 * 固定 40ms 步进积分；接近目标且速度极小时吸附（防长尾）。
 * 仅用于灵动岛形变 / 卡片吸附 / 下拉超限回位，不用于页面进入。 */
struct aether_spring {
    double value, vel, target;
    double k, c;
    long   last_ms;
    int    active;
};
void   aether_spring_init(struct aether_spring *s, double value, double bounce);
void   aether_spring_to(struct aether_spring *s, double target);
double aether_spring_step(struct aether_spring *s);
int    aether_spring_running(const struct aether_spring *s);
void   aether_spring_settle(struct aether_spring *s);

/* ---- 无障碍 / 性能降级 ---- */
/* 减少动效：所有过渡降为极短淡入淡出；可在设置页与 IPC 切换 */
void aether_anim_set_reduce(int on);
int  aether_anim_reduce(void);
/* 材质档位：0=实色降级 1=轻模糊 2=标准实时模糊（总纲 7 自动降级） */
void aether_anim_set_fx(int level);
int  aether_anim_fx(void);
/* 全局：当前是否有任一过渡在跑（合成器据此做连续帧） */
int  aether_anim_active(void);
void aether_anim_kick(void);       /* 有动画开始/进行中时调用 */
void aether_anim_tick(void);       /* 每帧推进，回收结束的动画 */

/* 数值插值 */
static inline double aether_lerp(double a, double b, double t) { return a + (b - a) * t; }
static inline int aether_lerpi(int a, int b, double t) { return (int)(a + (b - a) * t + 0.5); }

#endif /* AETHER_ANIM_H */
