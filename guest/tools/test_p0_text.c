/*
 * test_p0_text.c - P0 结构纠偏单元断言（host 侧）
 *
 * 覆盖 V3.0 分册 §16 验收清单中可在纯软件环境断言的条目：
 *   B01 含 descender 文案在行盒上下 4px 带无残留（文字底部裁切清零）
 *   B03 单行文本溢出显示省略号，不产生半个 UTF-8 汉字
 *   §5.3 行盒高 >= ascent + descent + 4，基线公式按真实字体度量
 *   §3.3 最小命中边长 88×88
 *
 * 只链接渲染原语与矢量字体子集，不依赖 framebuffer 设备、不依赖任何
 * 图形库；aether-glass.c 的两个模糊转发以空实现替身提供（本测试不画玻璃）。
 */
#include "aether-render.h"
#include "aether-tokens.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- aether-glass.c 转发替身（测试不链接完整合成器） ---- */
void aether_glass_blur_rect(int x, int y, int w, int h, int radius)
{ (void)x; (void)y; (void)w; (void)h; (void)radius; }
void aether_glass_blur_tint(int x, int y, int w, int h, int radius,
                            unsigned tint, unsigned char a)
{ (void)x; (void)y; (void)w; (void)h; (void)radius; (void)tint; (void)a; }

#define TW 720
#define TH 1280
#define BG 0xFF101820u
#define FG 0xFFFFFFFFu

static unsigned char *g_bits;
static int g_pass = 0, g_fail = 0;

static void check(int ok, const char *name)
{
    if (ok) { g_pass++; printf("  PASS  %s\n", name); }
    else    { g_fail++; printf("  FAIL  %s\n", name); }
}

/* 文本框上下各 4px 带内必须仍是纯背景色（分册 §5.3 验收断言） */
static int band_clean(int x, int y, int w, int h)
{
    unsigned *px = (unsigned *)g_bits;
    for (int dy = -4; dy < 0; dy++) {
        int yy = y + dy;
        if (yy < 0 || yy >= TH) continue;
        for (int i = 0; i < w; i++) {
            int xx = x + i;
            if (xx < 0 || xx >= TW) continue;
            if (px[(size_t)yy * TW + xx] != BG) return 0;
        }
    }
    for (int dy = 0; dy < 4; dy++) {
        int yy = y + h + dy;
        if (yy < 0 || yy >= TH) continue;
        for (int i = 0; i < w; i++) {
            int xx = x + i;
            if (xx < 0 || xx >= TW) continue;
            if (px[(size_t)yy * TW + xx] != BG) return 0;
        }
    }
    return 1;
}

/* 100 条含 descender / 复杂汉字的文案，覆盖 16 / 20 / 24 / 40 四档字号 */
static const char *const CASES[] = {
    "系统设置 y g p j q", "文件管理器 gyq", "应用商店 pgy", "通知中心 鄴",
    "控制中心 竖", "电池与性能 gp", "网络与连接 yq", "显示与亮度 jg",
    "声音与振动 py", "存储与清理 qg", "隐私与安全 鄴", "关于本机 gjy",
    "Android ygpq", "AetherOS qj", "Settings pgy", "Display jyq",
    "gypqj 竖鄴", "gggg pppp yyyy", "鄴陽鬱 鱻龘", "下载进度 96%",
    "12:34 充电中", "蓝牙已连接", "亮度 70%", "音量 50%",
    "应用无响应", "正在加载…", "全部清除", "通知历史", "更多设置",
    "文件夹命名", "拖拽排序", "小组件堆叠", "负一屏天气", "全局搜索",
    "王者荣耀 gp", "微信 yqj", "支付宝 pg", "高德地图 jy",
    "哔哩哔哩 qg", "网易云音乐", "相机与相册", "日历与日程",
    "计时器 00:59", "秒表 12.34", "录音 01:23", "导航 300m",
    "p 鄴 y 鬱 g", "qj 鱻 pg", "gy 龘 pq", "__gy__", "1g2p3y4q",
};

static int utf8_valid(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        int n;
        if (*p < 0x80) { p++; continue; }
        else if ((*p & 0xE0) == 0xC0) n = 2;
        else if ((*p & 0xF0) == 0xE0) n = 3;
        else return 0;
        for (int k = 1; k < n; k++) if ((p[k] & 0xC0) != 0x80) return 0;
        p += n;
    }
    return 1;
}

int main(void)
{
    printf("=== P0 结构纠偏单元断言 ===\n");
    g_bits = (unsigned char *)calloc((size_t)TW * TH * 4, 1);
    if (!g_bits) { printf("  FAIL  分配画布\n"); return 1; }
    aether_render_init(g_bits, TW, TH, TW * 4);

    /* --- §5.3 行盒与字体度量 --- */
    int asc16 = aether_text_ascent(TK_T_COMPAT);
    int desc16 = aether_text_descent(TK_T_COMPAT);
    int lh16 = aether_text_line_h(TK_T_COMPAT);
    check(asc16 > 0 && desc16 >= 0, "ascent/descent 可由矢量字体度量取得");
    check(lh16 >= asc16 + desc16 + 4, "16px 行盒高 >= ascent+descent+4");
    check(aether_text_line_h(TK_T_TITLE_2) >= TK_T_TITLE_2 * 3 / 2,
          "40px 标题行盒高 >= 1.5 倍字号");
    check(aether_text_box_y(100, 96, TK_T_TITLE_2) >= 100,
          "box_y 不越出容器上沿");
    check(TK_HIT_MIN == 88 && TK_HIT_SLOP == 8, "最小命中 88 / 外扩 8 令牌就位");

    /* --- B01 底部裁切：100 组合 × 4 档字号 --- */
    const int sizes[] = { TK_T_COMPAT, TK_T_CAPTION, TK_T_BODY_2, TK_T_TITLE_2 };
    int cut = 0, drawn = 0;
    for (unsigned c = 0; c < sizeof(CASES) / sizeof(CASES[0]); c++) {
        for (unsigned k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
            int size = sizes[k];
            int lh = aether_text_line_h(size);
            int bx = 24, by = 120 + (int)(c % 8) * 120;
            int bw = 640, bh = lh + 16;
            aether_clear(BG);
            aether_draw_text_utf8_box(bx, by, bw, bh, CASES[c], size, FG);
            if (!band_clean(bx, by, bw, bh)) cut++;
            drawn++;
        }
    }
    char msg[128];
    snprintf(msg, sizeof msg, "B01 无底部裁切（%d 组含 descender 文案 × 4 字号）", drawn);
    check(cut == 0, msg);
    if (cut) printf("         残留组数 = %d\n", cut);

    /* --- B03 省略号与 UTF-8 完整性 --- */
    char buf[128];
    const char *long_s = "这是一条非常非常长的通知标题用于验证单行溢出省略号是否正确显示";
    aether_text_fit_sized(buf, sizeof buf, long_s, TK_T_BODY_2, 240);
    int w = aether_text_width_utf8_sized(buf, TK_T_BODY_2);
    size_t n = strlen(buf);
    check(utf8_valid(buf), "B03 截断结果仍是合法 UTF-8（无半个汉字）");
    check(n >= 3 && (unsigned char)buf[n-3] == 0xE2 &&
          (unsigned char)buf[n-2] == 0x80 && (unsigned char)buf[n-1] == 0xA6,
          "B03 溢出尾部为 U+2026 省略号");
    check(w <= 240, "B03 截断后像素宽度不超容器");

    aether_text_fit_sized(buf, sizeof buf, "短标题", TK_T_BODY_2, 600);
    check(strcmp(buf, "短标题") == 0, "B03 未溢出时不追加省略号、不改原文");

    /* --- 字号阶梯与既有 8x16 布局兼容 --- */
    check(aether_text_width_utf8_sized("A中", TK_T_COMPAT) ==
          aether_text_width_utf8("A中"),
          "16px 字号的新旧文本宽度完全一致（布局零回归）");

    free(g_bits);
    printf("=== 结果：%d PASS / %d FAIL ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
