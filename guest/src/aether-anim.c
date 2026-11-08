/*
 * aether-anim.c - 统一动效曲线与动画时钟实现
 */
#include "aether-anim.h"
#include <time.h>
#include <math.h>

static int g_reduce = 0;      /* 减少动效 */
static int g_fx = 2;          /* 材质档位，默认标准实时模糊 */
static int g_active_cnt = 0;  /* 活跃动画计数 */
static long g_last_kick = 0;  /* 最近一次活动 */

long aether_anim_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}
double aether_anim_now_s(void) { return (double)aether_anim_now_ms() / 1000.0; }

static double clamp01(double t) { return t < 0 ? 0 : (t > 1 ? 1 : t); }

double aether_ease_out(double t)
{
    t = clamp01(t);
    double u = 1.0 - t;
    return 1.0 - u * u * u;                 /* cubic out */
}
double aether_ease_in_out(double t)
{
    t = clamp01(t);
    return t < 0.5 ? 4.0 * t * t * t
                   : 1.0 - pow(-2.0 * t + 2.0, 3.0) / 2.0;
}
double aether_ease_back(double t)
{
    t = clamp01(t);
    const double c1 = 1.70158, c3 = c1 + 1.0;
    double u = t - 1.0;
    return 1.0 + c3 * u * u * u + c1 * u * u;
}
double aether_ease_elastic(double t)
{
    t = clamp01(t);
    if (t == 0.0 || t == 1.0) return t;
    const double c4 = (2.0 * 3.141592653589793) / 3.0;
    return pow(2.0, -10.0 * t) * sin((t * 10.0 - 0.75) * c4) + 1.0;
}

void aether_anim_set_reduce(int on) { g_reduce = on ? 1 : 0; }
int  aether_anim_reduce(void) { return g_reduce; }
void aether_anim_set_fx(int level) { g_fx = level < 0 ? 0 : (level > 2 ? 2 : level); }
int  aether_anim_fx(void) { return g_fx; }

void aether_anim_begin(struct aether_anim *a, long dur_ms)
{
    a->t0 = aether_anim_now_ms();
    a->dur = g_reduce ? 90 : dur_ms;
    if (a->dur < 0) a->dur = 0;
    a->active = 1;
    g_active_cnt++;
    g_last_kick = a->t0;
}
void aether_anim_reset(struct aether_anim *a)
{
    if (a->active) { a->active = 0; if (g_active_cnt > 0) g_active_cnt--; }
}

double aether_anim_progress(struct aether_anim *a)
{
    if (!a->active) return 1.0;
    if (a->dur <= 0) { aether_anim_reset(a); return 1.0; }
    long dt = aether_anim_now_ms() - a->t0;
    if (dt >= a->dur) { aether_anim_reset(a); return 1.0; }
    return (double)dt / (double)a->dur;
}

double aether_anim_value(struct aether_anim *a, double (*curve)(double))
{
    double p = aether_anim_progress(a);
    return curve ? curve(p) : p;
}

int aether_anim_running(const struct aether_anim *a)
{
    if (!a->active) return 0;
    if (a->dur <= 0) return 0;
    return (aether_anim_now_ms() - a->t0) < a->dur;
}

int aether_anim_active(void)
{
    /* 动画计数 > 0 或最近 200ms 内有活动，保证末帧与回落被绘制 */
    if (g_active_cnt > 0) return 1;
    return (aether_anim_now_ms() - g_last_kick) < 200;
}
void aether_anim_kick(void) { g_last_kick = aether_anim_now_ms(); }
void aether_anim_tick(void) { /* 当前实现无需全局清理，句柄自行回收 */ }
