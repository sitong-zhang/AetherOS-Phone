/* aether-ui.c - 应用 UI 通用控件与主题调色板 */
#include "aether-appdata.h"
#include "aether-render.h"
#include <stdio.h>
#include <string.h>

unsigned C_BG, C_PANEL, C_TEXT, C_DIM, C_ACCENT, C_BORDER, C_BTN, C_BTN_TX;
unsigned C_OK, C_WARN, C_BAD;

void aether_ui_palette(void)
{
    if (aether_desktop_theme() == 1) {
        C_BG = pack_rgb(244, 246, 250); C_PANEL = pack_rgb(255, 255, 255);
        C_TEXT = pack_rgb(28, 34, 46); C_DIM = pack_rgb(105, 115, 132);
        C_ACCENT = pack_rgb(0, 100, 200); C_BORDER = pack_rgb(206, 214, 226);
        C_BTN = pack_rgb(228, 234, 244); C_BTN_TX = C_TEXT;
        C_OK = pack_rgb(20, 138, 76); C_WARN = pack_rgb(176, 118, 10); C_BAD = pack_rgb(188, 44, 44);
    } else {
        C_BG = pack_rgb(24, 29, 41); C_PANEL = pack_rgb(33, 40, 56);
        C_TEXT = pack_rgb(232, 238, 248); C_DIM = pack_rgb(142, 156, 178);
        C_ACCENT = pack_rgb(64, 156, 255); C_BORDER = pack_rgb(62, 74, 96);
        C_BTN = pack_rgb(50, 62, 84); C_BTN_TX = C_TEXT;
        C_OK = pack_rgb(80, 210, 130); C_WARN = pack_rgb(240, 190, 80); C_BAD = pack_rgb(245, 110, 110);
    }
}

unsigned aether_ui_state_color(char s)
{
    switch (s) {
    case 'A': return C_OK;
    case 'B': return C_ACCENT;
    case 'C': return C_WARN;
    default:  return C_DIM;
    }
}

int aether_ui_hit(int lx, int ly, int x, int y, int w, int h)
{
    return lx >= x && ly >= y && lx < x + w && ly < y + h;
}

static int ulen(unsigned char c)
{
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    return 1;
}

/* 按像素宽度截断 UTF-8 文本，防止文字溢出窗口 */
void aether_ui_fit(char *dst, size_t cap, const char *src, int maxpx)
{
    int w = 0;
    size_t o = 0;
    const char *p = src;
    while (*p && o + 5 < cap) {
        int n = ulen((unsigned char)*p);
        if (n > 1 && (!p[1] || (n > 2 && !p[2]))) break;
        char tmp[5];
        memcpy(tmp, p, (size_t)n); tmp[n] = 0;
        int cw = aether_text_width_utf8(tmp);
        if (w + cw > maxpx) break;
        memcpy(dst + o, p, (size_t)n);
        o += (size_t)n; w += cw; p += n;
    }
    dst[o] = 0;
}

void aether_ui_label(int x, int y, const char *s, unsigned c)
{
    aether_draw_text_utf8(x, y, s, c);
}

void aether_ui_button(int x, int y, int w, int h, const char *s, unsigned bg, unsigned fg)
{
    aether_draw_round_rect(x, y, w, h, 6, C_BORDER, 0xFF);
    aether_draw_round_rect(x + 1, y + 1, w - 2, h - 2, 5, bg, 0xFF);
    char t[64];
    aether_ui_fit(t, sizeof t, s, w - 8);
    int tw = aether_text_width_utf8(t);
    aether_ui_label(x + (w - tw) / 2, y + (h - 16) / 2, t, fg);
}

void aether_ui_bar(int x, int y, int w, int h, double pct, unsigned c)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    aether_draw_round_rect(x, y, w, h, 4, C_BORDER, 0xFF);
    int fw = (int)(w * pct / 100.0);
    if (fw > 2) aether_draw_round_rect(x + 1, y + 1, fw - 2, h - 2, 3, c, 0xFF);
}

void aether_ui_status(int ox, int oy, int cw, int ch, struct app_state *st)
{
    aether_fill_rect(ox, oy + ch - 24, cw, 24, C_PANEL);
    aether_fill_rect(ox, oy + ch - 24, cw, 1, C_BORDER);
    char b[128];
    aether_ui_fit(b, sizeof b, st->status[0] ? st->status : "就绪", cw - 16);
    aether_ui_label(ox + 8, oy + ch - 20, b, C_DIM);
}
