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

/* ---- 过渡句柄 ---- */
struct aether_anim {
    long   t0;        /* 起始毫秒 */
    long   dur;       /* 时长毫秒 */
    int    active;
};

/* 启动一个过渡；dur_ms<=0 或「减少动效」时立即完成 */
void aether_anim_begin(struct aether_anim *a, long dur_ms);
/* 重置为未开始 */
void aether_anim_reset(struct aether_anim *a);
/* 取线性进度 0..1（未经缓动），到点返回 1 并把 active 置 0 */
double aether_anim_progress(struct aether_anim *a);
/* 取指定缓动后的进度；curve 传 aether_ease_* */
double aether_anim_value(struct aether_anim *a, double (*curve)(double));
/* 是否仍在运动（用于合成器连续重绘判定） */
int aether_anim_running(const struct aether_anim *a);

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
