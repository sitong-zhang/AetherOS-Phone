/*
 * test_p1_motion.c - V3.0 分册 P1「材质与动效体系」单元断言
 *
 * 链接真代码（aether-anim.c / aether-material.c / aether-render.c），
 * 不复制实现、不打桩，覆盖分册 §8.1~§8.3、§7.2/§7.3/§7.7 与
 * 第 16 章 C01 / C02 / D03 / D04 / E02 的可计算部分：
 *   - 五曲线端点与形态
 *   - 可打断转场（from/to 视觉值续接、anim_set 钉终值）
 *   - 距离联动时长、减少动效降级
 *   - 弹簧三档有限步收敛
 *   - M1–M12 配方表与准入
 *   - 同帧模糊预算 ≤1
 *   - 全局按压 down 当帧可见、爬升/回落、命中
 *   - 缩放字标宽度公式（开机字标右缘裁切修复）
 */
#include "aether-anim.h"
#include "aether-material.h"
#include "aether-render.h"
#include "aether-tokens.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

/* ---- aether-glass.c 转发替身（测试不链接完整合成器） ----
 * aether-render.c 的模糊入口会转发到玻璃层，这里只需满足链接。 */
void aether_glass_blur_rect(int x, int y, int w, int h, int radius)
{ (void)x; (void)y; (void)w; (void)h; (void)radius; }
void aether_glass_blur_tint(int x, int y, int w, int h, int radius,
                            unsigned tint, unsigned char a)
{ (void)x; (void)y; (void)w; (void)h; (void)radius; (void)tint; (void)a; }

static int g_pass = 0, g_fail = 0;
#define CK(c, m) do { \
    if (c) { printf("  PASS  %s\n", (m)); g_pass++; } \
    else   { printf("  FAIL  %s\n", (m)); g_fail++; } \
} while (0)
static int near(double a, double b, double eps) { return fabs(a - b) <= eps; }

/* 8×16 默认行盒，来自 aether-font.h；这里不重复 include，避免与
 * aether-render.c 内部同名宏冲突，只作为期望值常量使用。 */
#define FONT_W 8
#define FONT_H 16

static void test_curves(void)
{
    printf("=== 五曲线（分册 §8.1） ===\n");
    CK(near(aether_ease_out(0.0), 0.0, 1e-9) && near(aether_ease_out(1.0), 1.0, 1e-9),
       "C-OUT 端点 0→0 / 1→1");
    CK(near(aether_ease_in_out(0.0), 0.0, 1e-9) && near(aether_ease_in_out(1.0), 1.0, 1e-9),
       "C-INOUT 端点 0→0 / 1→1");
    CK(near(aether_ease_linear(0.0), 0.0, 1e-9) && near(aether_ease_linear(1.0), 1.0, 1e-9),
       "C-LINEAR 端点 0→0 / 1→1");
    CK(aether_ease_out(0.5) > 0.5, "C-OUT 起步快（0.5 处 > 0.5）");
    CK(aether_ease_in_out(0.25) < 0.25, "C-INOUT 起步缓（0.25 处 < 0.25）");
    CK(aether_ease_back(0.8) > 1.0, "C-BACK 尾部轻微过冲（> 1）");
    CK(near(aether_ease_elastic(1.0), 1.0, 1e-9) && near(aether_ease_elastic(0.0), 0.0, 1e-9),
       "C-ELASTIC 端点 0→0 / 1→1");
}

static void test_interrupt(void)
{
    printf("=== 可打断续接 / 终值语义（分册 §8.5 / C02） ===\n");
    struct aether_anim a;
    memset(&a, 0, sizeof a);
    aether_anim_set(&a, 0.42);
    CK(near(aether_anim_value(&a, aether_ease_in_out), 0.42, 1e-9),
       "anim_set 把视觉值钉在 0.42，value() 返回该值");
    CK(!aether_anim_running(&a) && !aether_anim_active(),
       "anim_set 不留活跃计数（E06 静态页不持续重绘）");

    struct aether_anim b;
    memset(&b, 0, sizeof b);
    aether_anim_begin_from(&b, 300, 0.0, 1.0);
    b.t0 -= 150;                       /* 模拟推进到半程 */
    double mid = aether_anim_visual(&b, aether_ease_in_out);
    CK(mid > 0.05 && mid < 0.95, "半程视觉值落在 0..1 区间内");
    aether_anim_begin_from(&b, 300, mid, 0.0);   /* 反向打断 */
    double first = aether_anim_visual(&b, aether_ease_in_out);
    CK(near(first, mid, 0.02), "反向打断首帧从当前视觉值续接，无跳变");
    CK(b.dur < 300, "剩余距离缩短时时长按 §8.3 压缩");

    b.t0 -= 1000;                     /* 推进到结束 */
    double last = aether_anim_value(&b, aether_ease_in_out);
    CK(near(last, 0.0, 1e-9), "到点后 value() 返回终值 0，不闪回 1");
}

static void test_duration_and_reduce(void)
{
    printf("=== 时长阶梯 / 距离联动 / 减少动效（分册 §8.3 / C09） ===\n");
    CK(TK_D_INSTANT == 90 && TK_D_MATERIALIZE == 250 && TK_D_MOVE == 300
       && TK_D_DEMATERIALIZE == 350 && TK_D_SPRING == 500 && TK_D_SLOW == 600,
       "TK_D_* 阶梯 90/250/300/350/500/600 就位");
    CK(aether_anim_duration(300, 100, 200) == 180, "短位移压缩到 0.6×（300→180）");
    CK(aether_anim_duration(300, 200, 200) == 300, "基准位移不缩放");
    CK(aether_anim_duration(300, 400, 200) == 345, "超长位移最多放大 1.15×（300→345）");

    aether_anim_set_reduce(1);
    struct aether_anim r;
    memset(&r, 0, sizeof r);
    aether_anim_begin(&r, 300);
    CK(r.dur <= TK_D_INSTANT, "减少动效时所有过渡 ≤90ms");
    aether_anim_set_reduce(0);
}

static void test_spring(void)
{
    printf("=== 二阶弹簧三档（分册 §8.2） ===\n");
    const double bounces[3] = { 0.15, 0.30, 0.00 };
    const char *names[3] = { "SNAPPY", "BOUNCY", "SMOOTH" };
    for (int i = 0; i < 3; i++) {
        struct aether_spring s;
        memset(&s, 0, sizeof s);
        aether_spring_init(&s, 0.0, bounces[i]);
        aether_spring_to(&s, 1.0);
        int steps = 0;
        while (aether_spring_running(&s) && steps < 400) {
            s.last_ms -= 40;          /* 模拟 40ms 固定步进 */
            (void)aether_spring_step(&s);
            steps++;
        }
        char msg[96];
        snprintf(msg, sizeof msg, "%s 有限步内吸附（%d 步）", names[i], steps);
        CK(!aether_spring_running(&s), msg);
        snprintf(msg, sizeof msg, "%s 收敛到目标值", names[i]);
        CK(near(s.value, 1.0, 0.01), msg);
    }
}

static void test_material(void)
{
    printf("=== M1–M12 配方表 / 同帧模糊预算（分册 §7.2 / E02） ===\n");
    CK(aether_material_count() == AE_MAT_COUNT && AE_MAT_COUNT == 12,
       "配方表共 12 种材质");
    int ids_ok = 1;
    for (int i = 1; i <= 12; i++) {
        const struct aether_mat_recipe *r = aether_material(i);
        if (!r || r->id != i) ids_ok = 0;
    }
    CK(ids_ok, "12 个 id 连续可查且自描述一致");
    CK(aether_material(0) == NULL && aether_material(13) == NULL,
       "越界 id 返回 NULL");
    const struct aether_mat_recipe *wall = aether_material(AE_M1_WALL);
    const struct aether_mat_recipe *notif = aether_material(AE_M5_NOTIF);
    const struct aether_mat_recipe *sheet = aether_material(AE_M10_SHEET);
    CK(wall && wall->blur_px == 0, "M1 壁纸底不模糊（条件式遮罩）");
    CK(notif && notif->blur_px == TK_BLUR_MED, "M5 通知中心中模糊");
    CK(sheet && sheet->blur_px == TK_BLUR_HEAVY && sheet->downsample == 3,
       "M10 二级面板重模糊 + 3× 降采样（仅模态矩形内）");
    CK(aether_material(AE_M7_TILE_OFF)->pressable
       && aether_material(AE_M8_TILE_ON)->pressable,
       "M7/M8 磁贴参与全局按压");
    CK(!aether_material_allows_glass(AE_M2_CARD),
       "M2 内容卡禁止玻璃（上游层级规则）");
    CK(aether_material_allows_glass(AE_M6_CONTROL),
       "M6 控制中心允许玻璃");

    aether_material_begin_frame();
    CK(aether_material_request_blur(0, 0, 720, 1152, 20) == 1,
       "同帧首次真算模糊允许");
    CK(aether_material_request_blur(0, 0, 720, 1152, 20) == 0,
       "同帧第二次请求被合并复用");
    CK(aether_material_blur_budget() == 1, "E02 本帧真算次数 = 1（≤1）");
    aether_material_begin_frame();
    CK(aether_material_blur_budget() == 0, "新一帧预算清零");
}

static void test_press(void)
{
    printf("=== 全局按压反馈（分册 §7.7 / C01） ===\n");
    aether_anim_set_reduce(0);
    aether_material_set_touch(-1, -1, 0);
    aether_material_tick();
    aether_material_set_touch(100, 100, 1);
    float k0 = aether_material_press_k(0);
    CK(k0 > 0.0f, "down 当帧即有可见按压量（不等 150ms）");
    CK(aether_material_pressed(90, 90, 40, 40) == 1, "触点命中矩形");
    CK(aether_material_pressed(300, 300, 40, 40) == 0, "远处矩形不命中");
    CK(aether_material_press_inflate() > 0, "down 当帧膨胀量 > 0");
    CK(aether_material_press_lift(1) > aether_material_press_lift(0),
       "浅色提亮基准高于深色（0.30 > 0.14）");
    usleep(170000);
    aether_material_tick();
    CK(aether_material_press_k(0) > 0.9f, "按住 170ms 后爬升到接近 1");
    aether_material_set_touch(100, 100, 0);
    usleep(70000);
    aether_material_tick();
    CK(aether_material_press_k(0) < 0.05f, "松手 70ms 后回落到 0");
}

static void test_boot_logo_width(void)
{
    printf("=== 开机字标缩放宽度（品牌字右缘裁切修复） ===\n");
    const char *logo = "Aether OS";
    int w16 = aether_text_width_scale(logo, FONT_H);
    int w88 = aether_text_width_scale(logo, 88);
    int expect_scale = (88 + FONT_H - 1) / FONT_H;
    CK(w16 == (int)strlen(logo) * FONT_W, "16px 时宽度 = 字符数 × 8");
    CK(w88 == (int)strlen(logo) * FONT_W * expect_scale,
       "88px 时宽度按 ceil(h/16) 倍缩放");
    CK(w88 > w16 && w88 < TK_SCREEN_W,
       "缩放后宽度大于未缩放值且未溢出 720px 画布");
    CK(aether_text_width_scale(NULL, 88) == 0 && aether_text_width_scale(logo, 0) == 0,
       "空指针 / 非正字号返回 0");
}

int main(void)
{
    printf("=== P1 材质与动效单元断言 ===\n");
    test_curves();
    test_interrupt();
    test_duration_and_reduce();
    test_spring();
    test_material();
    test_press();
    test_boot_logo_width();
    printf("=== 结果：%d PASS / %d FAIL ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
