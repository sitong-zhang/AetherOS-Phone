/*
 * aether-notes.c - AetherOS 笔记（阶段 5 商店可安装的新应用示范）
 *
 * 真实存储于 /data/aether/notes.txt，多笔记：列表 / 新建 / 编辑 / 删除 / 保存。
 * 全部来自真实文件读写，无演示假数据。
 */
#include "aether-appdata.h"
#include "aether-apps.h"
#include "aether-render.h"
#include "aether-icons.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hit(int lx, int ly, int x, int y, int w, int h)
{ return lx >= x && ly >= y && lx < x + w && ly < y + h; }

#define NMAX 32
#define NFILE "/data/aether/notes.txt"

struct note { char title[40]; char body[1024]; };

static struct note g_notes[NMAX];
static int g_nn = 0;

static void notes_load(void)
{
    g_nn = 0;
    FILE *f = fopen(NFILE, "r");
    if (!f) return;
    char line[1100];
    int cur = -1;
    while (fgets(line, sizeof line, f)) {
        size_t L = strlen(line); while (L && line[L-1] < 32) line[--L] = 0;
        if (L == 0) continue;
        if (line[0] == '#' && (cur < 0 || g_nn == cur + 1)) {
            if (cur < 0) { if (g_nn < NMAX) { cur = g_nn; g_notes[cur].body[0]=0; } else continue; }
            snprintf(g_notes[cur].title, sizeof g_notes[cur].title, "%s", line + 1);
        } else {
            if (cur < 0) { if (g_nn < NMAX) { cur = g_nn; g_notes[cur].title[0]=0; } else continue; }
            int bl = (int)strlen(g_notes[cur].body);
            if (bl + L + 1 < (int)sizeof g_notes[cur].body) {
                if (bl) g_notes[cur].body[bl++] = '\n';
                memcpy(g_notes[cur].body + bl, line, L + 1);
            }
        }
    }
    if (cur >= 0) g_nn = cur + 1;
    fclose(f);
}

static void notes_save(void)
{
    FILE *f = fopen(NFILE, "w");
    if (!f) return;
    for (int i = 0; i < g_nn; i++)
        fprintf(f, "#%s\n%s\n", g_notes[i].title, g_notes[i].body);
    fclose(f);
}

void app_notes_open(struct app_state *st)
{
    notes_load();
    st->notes_sel = -1; st->notes_edit = 0;
    st->notes_title[0] = 0; st->notes_tlen = 0;
    st->notes_body[0] = 0; st->notes_blen = 0; st->notes_scroll = 0;
}

void app_notes_render(struct aether_win *w, int ox, int oy, int cw, int ch, struct app_state *st)
{
    aether_fill_rect(ox, oy, cw, ch, C_BG);
    int y = app_top_bar(w, ox, oy, cw, ch, st->notes_edit ? "编辑笔记" : "笔记");

    if (!st->notes_edit) {
        /* 新建按钮 */
        aether_draw_round_rect(ox + cw - 84, oy + 8, 72, 28, 14, C_ACCENT, 0xFF);
        aether_ui_label(ox + cw - 70, oy + 14, "+ 新建", 0xFFFFFFFF);
        y += 18;
        if (g_nn == 0) {
            aether_draw_icon(AE_ICON_FILE_TEXT, ox + cw/2 - 24, oy + ch/2 - 60, 48, C_DIM);
            aether_ui_label(ox + cw/2 - 40, oy + ch/2, "还没有笔记", C_DIM);
            aether_ui_label(ox + cw/2 - 60, oy + ch/2 + 24, "点击右上角新建", C_DIM);
            return;
        }
        int row_h = 64;
        for (int i = 0; i < g_nn; i++) {
            int ry = y + st->notes_scroll + i * (row_h + 8);
            if (ry > oy + ch - 8) break;
            if (ry + row_h < y) continue;
            aether_draw_round_rect(ox + 12, ry, cw - 24, row_h, 14, C_PANEL, 0xFF);
            aether_draw_border(ox + 12, ry, cw - 24, row_h, C_BORDER, 1);
            aether_ui_label(ox + 24, ry + 12, g_notes[i].title[0] ? g_notes[i].title : "(无标题)", C_TEXT);
            char prev[80]; int n = 0;
            const char *b = g_notes[i].body;
            while (*b && n < (int)sizeof prev - 1) { if (*b == '\n') { prev[n++] = ' '; b++; continue; } prev[n++] = *b++; }
            prev[n] = 0; if (!n) snprintf(prev, sizeof prev, "暂无内容");
            aether_ui_label(ox + 24, ry + 38, prev, C_DIM);
        }
    } else {
        /* 编辑：标题输入 + 正文输入 + 保存/删除 */
        aether_ui_label(ox + 24, y + 4, "标题", C_DIM);
        aether_draw_round_rect(ox + 24, y + 24, cw - 48, 32, 8, C_PANEL, 0xFF);
        aether_draw_border(ox + 24, y + 24, cw - 48, 32, C_BORDER, 1);
        aether_ui_label(ox + 32, y + 32, st->notes_title, st->notes_focus == 0 ? C_TEXT : C_TEXT);
        if (st->notes_focus == 0) aether_draw_icon(AE_ICON_TYPE, ox + cw - 44, y + 30, 18, C_ACCENT);
        y += 64;
        aether_ui_label(ox + 24, y + 4, "正文", C_DIM);
        aether_draw_round_rect(ox + 24, y + 24, cw - 48, ch - (y + 24) - 56, 8, C_PANEL, 0xFF);
        aether_draw_border(ox + 24, y + 24, cw - 48, ch - (y + 24) - 56, C_BORDER, 1);
        /* 正文按行绘制 */
        int tx = ox + 32, ty = y + 34;
        const char *b = st->notes_body;
        while (*b) {
            char buf[96]; int n = 0;
            while (*b && *b != '\n' && n < (int)sizeof buf - 1) buf[n++] = *b++;
            buf[n] = 0; if (*b == '\n') b++;
            aether_ui_label(tx, ty, buf, C_TEXT);
            ty += 22;
        }
        if (st->notes_focus == 1) aether_draw_icon(AE_ICON_TYPE, ox + cw - 44, y + 30, 18, C_ACCENT);
        /* 保存 / 删除 */
        int by = oy + ch - 44;
        aether_draw_round_rect(ox + 24, by, (cw-56)/2, 36, 18, C_OK, 0xFF);
        aether_ui_label(ox + 24 + (cw-56)/4 - 16, by + 10, "保存", 0xFFFFFFFF);
        aether_draw_round_rect(ox + 32 + (cw-56)/2, by, (cw-56)/2, 36, 18, C_BTN, 0xFF);
        aether_ui_label(ox + 32 + (cw-56)/2 + (cw-56)/4 - 16, by + 10, "删除", C_BTN_TX);
    }
}

void app_notes_click(struct aether_win *w, int lx, int ly, int cw, int ch, struct app_state *st)
{
    int ox = w->x, oy = w->y;
    if (st->notes_edit && hit(lx, ly, ox, oy, 56, 44)) { st->notes_edit = 0; return; }
    if (!st->notes_edit) {
        if (hit(lx, ly, ox + cw - 84, oy + 8, 72, 28)) { /* 新建 */
            st->notes_sel = -1; st->notes_edit = 1; st->notes_title[0] = 0; st->notes_tlen = 0;
            st->notes_body[0] = 0; st->notes_blen = 0; st->notes_focus = 0; return;
        }
        int y = oy + 44 + 18, row_h = 64;
        for (int i = 0; i < g_nn; i++) {
            int ry = y + st->notes_scroll + i * (row_h + 8);
            if (hit(lx, ly, ox + 12, ry, cw - 24, row_h)) {
                st->notes_sel = i; st->notes_edit = 1; st->notes_focus = 1;
                snprintf(st->notes_title, sizeof st->notes_title, "%s", g_notes[i].title);
                st->notes_tlen = (int)strlen(st->notes_title);
                snprintf(st->notes_body, sizeof st->notes_body, "%s", g_notes[i].body);
                st->notes_blen = (int)strlen(st->notes_body);
                return;
            }
        }
    } else {
        if (hit(lx, ly, ox + 24, oy + 44 + 24, cw - 48, 32)) { st->notes_focus = 0; return; }
        int y = oy + 44 + 18 + 64;
        if (hit(lx, ly, ox + 24, y + 24, cw - 48, ch - (y + 24) - 56)) { st->notes_focus = 1; return; }
        int by = oy + ch - 44;
        if (hit(lx, ly, ox + 24, by, (cw-56)/2, 36)) { /* 保存 */
            if (st->notes_sel < 0) { if (g_nn < NMAX) st->notes_sel = g_nn++; }
            snprintf(g_notes[st->notes_sel].title, sizeof g_notes[st->notes_sel].title, "%s", st->notes_title);
            snprintf(g_notes[st->notes_sel].body, sizeof g_notes[st->notes_sel].body, "%s", st->notes_body);
            notes_save(); st->notes_edit = 0; return;
        }
        if (hit(lx, ly, ox + 32 + (cw-56)/2, by, (cw-56)/2, 36)) { /* 删除 */
            if (st->notes_sel >= 0 && st->notes_sel < g_nn) {
                for (int i = st->notes_sel; i < g_nn - 1; i++) g_notes[i] = g_notes[i + 1];
                g_nn--;
            }
            notes_save(); st->notes_edit = 0; return;
        }
    }
}

void app_notes_key(struct aether_win *w, unsigned code, struct app_state *st)
{
    (void)w;
    if (!st->notes_edit) return;
    char c = aether_key_to_char(code, 0);
    if (c >= 32 && c != 127) {
        if (st->notes_focus == 0 && st->notes_tlen < (int)sizeof st->notes_title - 1) {
            st->notes_title[st->notes_tlen++] = c; st->notes_title[st->notes_tlen] = 0;
        } else if (st->notes_focus == 1 && st->notes_blen < (int)sizeof st->notes_body - 1) {
            st->notes_body[st->notes_blen++] = c; st->notes_body[st->notes_blen] = 0;
        }
    } else if (code == 14) { /* 退格 */
        if (st->notes_focus == 0 && st->notes_tlen > 0) st->notes_title[--st->notes_tlen] = 0;
        else if (st->notes_focus == 1 && st->notes_blen > 0) st->notes_body[--st->notes_blen] = 0;
    } else if (code == 28) { /* 回车：正文换行 */
        if (st->notes_focus == 1 && st->notes_blen < (int)sizeof st->notes_body - 1)
            st->notes_body[st->notes_blen++] = '\n';
    }
}

void app_notes_cmd(struct aether_win *w, const char *line, struct app_state *st)
{
    (void)w;
    char arg[24]; int v = 0;
    if (sscanf(line, "%*s %23s %d", arg, &v) >= 1) {
        if (!strcmp(arg, "new"))      { st->notes_sel = -1; st->notes_edit = 1; st->notes_title[0]=0; st->notes_tlen=0; st->notes_body[0]=0; st->notes_blen=0; st->notes_focus=0; return; }
        if (!strcmp(arg, "edit"))     { if (v>=0&&v<g_nn){ st->notes_sel=v; st->notes_edit=1; st->notes_focus=1; snprintf(st->notes_title,sizeof st->notes_title,"%s",g_notes[v].title); st->notes_tlen=strlen(st->notes_title); snprintf(st->notes_body,sizeof st->notes_body,"%s",g_notes[v].body); st->notes_blen=strlen(st->notes_body);} return; }
        if (!strcmp(arg, "back"))     { st->notes_edit = 0; return; }
        if (!strcmp(arg, "focus"))    { st->notes_focus = v ? 1 : 0; return; }
        if (!strcmp(arg, "save"))     { if (st->notes_sel<0){if(g_nn<NMAX)st->notes_sel=g_nn++;} snprintf(g_notes[st->notes_sel].title,sizeof g_notes[st->notes_sel].title,"%s",st->notes_title); snprintf(g_notes[st->notes_sel].body,sizeof g_notes[st->notes_sel].body,"%s",st->notes_body); notes_save(); st->notes_edit=0; return; }
        if (!strcmp(arg, "del"))      { if(v>=0&&v<g_nn){for(int i=v;i<g_nn-1;i++)g_notes[i]=g_notes[i+1];g_nn--;} notes_save(); st->notes_edit=0; return; }
    }
    const char *t = line + 4; while (*t == ' ') t++;
    if (!strncmp(arg, "type", 4) && t[0]) {
        for (int i = 0; t[i] && t[i] != '\r' && t[i] != '\n'; i++) {
            char c = t[i];
            if (st->notes_focus == 0 && st->notes_tlen < (int)sizeof st->notes_title - 1) {
                st->notes_title[st->notes_tlen++] = c; st->notes_title[st->notes_tlen] = 0;
            } else if (st->notes_blen < (int)sizeof st->notes_body - 1) {
                st->notes_body[st->notes_blen++] = c; st->notes_body[st->notes_blen] = 0;
            }
        }
    }
}
