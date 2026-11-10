/*
 * aether-anim.c - 统一动效曲线与动画时钟实现
 */
#include "aether-anim.h"
#include "aether-tokens.h"   /* TK_D_INSTANT：减少动效时长唯一真相源 */
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

/* 已在运动的句柄被重新 begin 时，先回收旧计数，避免 g_active_cnt 泄漏
 * 导致 aether_anim_active() 恒为 1、静态页面持续重绘（E06）。 */
static void begin_common(struct aether_anim *a, long dur_ms,
                         double from, double to)
{
    if (a->active) { a->active = 0; if (g_active_cnt > 0) g_active_cnt--; }
    a->t0 = aether_anim_now_ms();
    a->dur = g_reduce ? TK_D_INSTANT : dur_ms;   /* 减少动效 ≤90ms */
    if (a->dur < 0) a->dur = 0;
    a->from = from; a->to = to;
    a->active = 1;
    g_active_cnt++;
    g_last_kick = a->t0;
}
void aether_anim_begin(struct aether_anim *a, long dur_ms)
{
    begin_common(a, dur_ms, 0.0, 1.0);
}
void aether_anim_set(struct aether_anim *a, double v)
{
    if (a->active) { a->active = 0; if (g_active_cnt > 0) g_active_cnt--; }
    a->t0  = aether_anim_now_ms();   /* 非 0：value() 走真实 from/to，不回退 0→1 */
    a->dur = 0;
    a->from = a->to = v;
    a->active = 0;
}
void aether_anim_begin_from(struct aether_anim *a, long dur_ms,
                            double from, double to)
{
    /* 剩余距离占比压缩时长：只差 20% 时走 20% 的完整时长，
     * 但下限 60% 保证不出现「一闪而过」的突兀感（分册 §8.3）。 */
    double remain = (to - from); if (remain < 0) remain = -remain;
    if (remain > 1.0) remain = 1.0;
    double scale = 0.6 + 0.4 * remain;
    begin_common(a, (long)(dur_ms * scale), from, to);
}
void aether_anim_reset(struct aether_anim *a)
{
    /* 只回收活跃计数，保留 from/to：动画到点后 value() 必须仍返回终值 to，
     * 否则「被打断反向」的句柄在最后一帧会闪回 1.0。from/to 只由 begin 系列写。 */
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
    double e = curve ? curve(p) : p;
    double from = a->from, to = a->to;
    /* 全局零初始化的句柄（从未 begin）语义为「已完成 / 显示终态」，
     * 与 P0 之前 value() 恒返回 1 的行为一致，避免首页未启动动画时
     * 被误判成起点态而只画出来源矩形。 */
    if (a->t0 == 0) { from = 0.0; to = 1.0; }
    return from + (to - from) * e;
}
double aether_anim_visual(const struct aether_anim *a, double (*curve)(double))
{
    /* 复制句柄，只读查询当前视觉值；不推进、不回收原句柄。 */
    struct aether_anim tmp = *a;
    return aether_anim_value(&tmp, curve);
}

long aether_anim_duration(long token_ms, double dist, double ref)
{
    if (ref <= 0) ref = 1.0;
    double s = dist / ref;
    if (s < 0.6) s = 0.6;
    if (s > 1.15) s = 1.15;
    return (long)(token_ms * s + 0.5);
}

/* ---- 二阶弹簧积分 ---- */
void aether_spring_init(struct aether_spring *s, double value, double bounce)
{
    double zeta = 1.0 - bounce;
    if (zeta < 0.05) zeta = 0.05;     /* 防过零导致发散 */
    if (zeta > 1.0)  zeta = 1.0;
    s->value = value; s->vel = 0.0; s->target = value;
    s->k = 180.0;
    s->c = 2.0 * zeta * sqrt(s->k);
    s->last_ms = aether_anim_now_ms();
    s->active = 0;
}
void aether_spring_to(struct aether_spring *s, double target)
{
    if (s->target == target && !s->active) return;
    s->target = target;
    s->active = 1;
    s->last_ms = aether_anim_now_ms();
    aether_anim_kick();
}
double aether_spring_step(struct aether_spring *s)
{
    if (!s->active) return s->target;
    long now = aether_anim_now_ms();
    long dt = now - s->last_ms;
    s->last_ms = now;
    if (dt < 0) dt = 0;
    if (dt > 160) dt = 160;           /* 后台切回时钳制，避免一步跳飞 */
    /* 固定 40ms 步进积分（分册 §8.2），dt 拆成若干子步保证稳定 */
    int steps = (int)(dt / 40);
    int rem = (int)(dt % 40);
    if (steps == 0 && rem > 0) { steps = 1; rem = 0; }
    for (int i = 0; i < steps; i++) {
        double step = 0.04;
        if (i == steps - 1 && rem > 0) step = (double)dt / 1000.0 / steps;
        double acc = -s->k * (s->value - s->target) - s->c * s->vel;
        s->vel += acc * step;
        s->value += s->vel * step;
    }
    /* 接近目标且速度极小 → 吸附（防长尾）；bounce=0 时更早收敛 */
    if (fabs(s->value - s->target) < 0.0015 && fabs(s->vel) < 0.02) {
        s->value = s->target; s->vel = 0.0; s->active = 0;
    }
    if (s->active) aether_anim_kick();
    return s->value;
}
int aether_spring_running(const struct aether_spring *s) { return s->active; }
void aether_spring_settle(struct aether_spring *s)
{
    s->value = s->target; s->vel = 0.0; s->active = 0;
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
