/*
 * aether-apps.c - AetherOS 内置应用（一）：文件管理器 / 终端 / 编辑器 / 计算器
 *
 * 所有内容来自真实数据源：opendir+stat、popen 执行 BusyBox、键盘输入、
 * 真实运算。不使用硬编码假数据。
 */
#include "aether-appdata.h"
#include "aether-apps.h"
#include "aether-render.h"
#include "aether-modules.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct app_state aether_app_states[MAX_WINDOWS];
static int g_shift;

/* Linux input-event-codes 常用键 */
#define K_ESC        1
#define K_BACKSPACE 14
#define K_TAB       15
#define K_ENTER     28
#define K_SPACE     57
#define K_UP       103
#define K_LEFT     105
#define K_RIGHT    106
#define K_DOWN     108

struct app_state *aether_app_state(struct aether_win *win)
{
    if (!win || win->id < 1 || win->id > MAX_WINDOWS) return NULL;
    return &aether_app_states[win->id - 1];
}

static const char *NAMES[APP_COUNT] = {
    "文件管理器", "终端", "文本编辑器", "计算器",
    "系统监视器", "系统设置", "关于本机", "系统模块清单",
    "浏览器", "图片查看器", "视频播放器", "音乐播放器", "时钟", "日历",
    "应用商店", "笔记"
};
static const char *SHORTS[APP_COUNT] = {
    "文件", "终端", "编辑", "计算", "监视", "设置", "关于", "模块",
    "浏览", "图片", "视频", "音乐", "时钟", "日历", "商店", "笔记"
};
static const unsigned COLORS[APP_COUNT] = {
    0xE8A33D, 0x2E2E38, 0x4A90D9, 0x7B68AE,
    0x35A779, 0x607D8B, 0x3F6FB5, 0xB5615A,
    0x2E7DB8, 0xC0607A, 0x6A5AA8, 0xD07040, 0x3C8C8C, 0xB08830,
    0x4A6FE0, 0x8A6FC0
};

const char *aether_app_name(enum app_kind k)  { return NAMES[(int)k % APP_COUNT]; }
const char *aether_app_short(enum app_kind k) { return SHORTS[(int)k % APP_COUNT]; }
unsigned    aether_app_color(enum app_kind k) { return pack_rgb((COLORS[k] >> 16) & 0xFF,
                                                                 (COLORS[k] >> 8) & 0xFF, COLORS[k] & 0xFF); }

/* ==================== 文件管理器 ==================== */
static void files_refresh(struct app_state *st)
{
    st->nent = 0;
    DIR *d = opendir(st->path);
    if (!d) { snprintf(st->status, sizeof st->status, "无法打开目录：%s", st->path); return; }
    struct dirent *de;
    struct file_entry dirs[MAX_ENTRIES], files[MAX_ENTRIES];
    int nd = 0, nf = 0;
    char full[512];
    while ((de = readdir(d))) {
        if (strcmp(de->d_name, ".") == 0) continue;
        struct stat sb;
        snprintf(full, sizeof full, "%s/%s", st->path, de->d_name);
        if (stat(full, &sb) != 0) continue;
        int isdir = S_ISDIR(sb.st_mode) ? 1 : 0;
        struct file_entry *dst = isdir ? (nd < MAX_ENTRIES ? &dirs[nd++] : NULL)
                                       : (nf < MAX_ENTRIES ? &files[nf++] : NULL);
        if (!dst) continue;
        snprintf(dst->name, sizeof dst->name, "%s", de->d_name);
        dst->isdir = isdir;
        dst->size = isdir ? 0 : (unsigned long)sb.st_size;
    }
    closedir(d);
    for (int i = 0; i < nd && st->nent < MAX_ENTRIES; i++) st->ent[st->nent++] = dirs[i];
    for (int i = 0; i < nf && st->nent < MAX_ENTRIES; i++) st->ent[st->nent++] = files[i];
    snprintf(st->status, sizeof st->status, "%s   共 %d 项（目录 %d / 文件 %d）", st->path, st->nent, nd, nf);
}

void app_files_open(struct app_state *st)
{
    snprintf(st->path, sizeof st->path, "/");
    files_refresh(st);
}

void app_files_render(struct aether_win *w, int ox, int oy, int cw, int ch, struct app_state *st)
{
    char p[256];
    aether_ui_fit(p, sizeof p, st->path, cw - 200);
    aether_draw_round_rect(ox + 8, oy + 8, cw - 186, 30, 5, C_PANEL, 0xFF);
    aether_draw_border(ox + 8, oy + 8, cw - 186, 30, C_BORDER, 1);
    aether_ui_label(ox + 16, oy + 15, p, C_TEXT);
    aether_ui_button(ox + cw - 170, oy + 8, 76, 30, "向上一级", C_BTN, C_BTN_TX);
    aether_ui_button(ox + cw - 88, oy + 8, 80, 30, "刷新", C_BTN, C_BTN_TX);

    int ly = oy + 48;
    aether_fill_rect(ox + 8, ly, cw - 16, 22, C_PANEL);
    aether_ui_label(ox + 16, ly + 3, "名称", C_DIM);
    aether_ui_label(ox + cw - 158, ly + 3, "类型", C_DIM);
    aether_ui_label(ox + cw - 82, ly + 3, "大小", C_DIM);
    ly += 22;

    int row_h = 24;
    int visible = (ch - 48 - 22 - 24 - 8) / row_h;
    if (visible < 1) visible = 1;
    for (int i = 0; i < visible; i++) {
        int idx = w->scroll + i;
        if (idx >= st->nent) break;
        int yy = ly + i * row_h;
        if (idx % 2 == 0) aether_fill_rect(ox + 8, yy, cw - 16, row_h, C_PANEL);
        char n[128];
        aether_ui_fit(n, sizeof n, st->ent[idx].name, cw - 186);
        aether_ui_label(ox + 16, yy + 4, n, st->ent[idx].isdir ? C_ACCENT : C_TEXT);
        aether_ui_label(ox + cw - 158, yy + 4, st->ent[idx].isdir ? "文件夹" : "文件", C_DIM);
        char sz[32];
        if (st->ent[idx].isdir) snprintf(sz, sizeof sz, "-");
        else if (st->ent[idx].size < 1024) snprintf(sz, sizeof sz, "%lu B", st->ent[idx].size);
        else if (st->ent[idx].size < 1048576UL) snprintf(sz, sizeof sz, "%lu KB", st->ent[idx].size / 1024);
        else snprintf(sz, sizeof sz, "%lu MB", st->ent[idx].size / 1048576UL);
        aether_ui_label(ox + cw - 82, yy + 4, sz, C_DIM);
    }
    aether_ui_status(ox, oy, cw, ch, st);
}

void app_files_click(struct aether_win *w, int lx, int ly, int cw, int ch, struct app_state *st)
{
    if (aether_ui_hit(lx, ly, cw - 170, 8, 76, 30)) {
        char *p = strrchr(st->path, '/');
        if (!p || p == st->path) snprintf(st->path, sizeof st->path, "/");
        else *p = 0;
        w->scroll = 0; files_refresh(st); return;
    }
    if (aether_ui_hit(lx, ly, cw - 88, 8, 80, 30)) { files_refresh(st); return; }
    int row_h = 24, ly0 = 70;
    int visible = (ch - 48 - 22 - 24 - 8) / row_h;
    if (ly >= ly0 && ly < ly0 + visible * row_h) {
        int idx = w->scroll + (ly - ly0) / row_h;
        if (idx >= 0 && idx < st->nent) {
            if (st->ent[idx].isdir) {
                char np[512];
                snprintf(np, sizeof np, "%s/%s", st->path, st->ent[idx].name);
                char *q = np;
                while ((q = strstr(q, "//"))) memmove(q, q + 1, strlen(q));
                snprintf(st->path, sizeof st->path, "%s", np);
                w->scroll = 0; files_refresh(st);
            } else {
                snprintf(st->status, sizeof st->status, "已选择：%s（%lu 字节）",
                         st->ent[idx].name, st->ent[idx].size);
            }
        }
    }
}

void app_files_key(struct aether_win *w, unsigned code, struct app_state *st)
{
    (void)st;
    int row_h = 24;
    int visible = 10;
    if (code == K_DOWN) { w->scroll++; if (w->scroll > st->nent - visible) w->scroll = st->nent - visible; }
    else if (code == K_UP) w->scroll--;
    if (w->scroll < 0) w->scroll = 0;
    (void)row_h;
}

/* ==================== 终端 ==================== */
static void term_push(struct app_state *st, const char *s)
{
    if (st->nlines >= MAX_TERM_LINES) {
        for (int i = 1; i < MAX_TERM_LINES; i++)
            snprintf(st->lines[i - 1], sizeof st->lines[0], "%s", st->lines[i]);
        st->nlines = MAX_TERM_LINES - 1;
    }
    snprintf(st->lines[st->nlines++], sizeof st->lines[0], "%s", s);
}

static void term_exec(struct app_state *st, const char *raw)
{
    char cmd[128];
    snprintf(cmd, sizeof cmd, "%s", raw);
    char *s = cmd;
    while (*s == ' ') s++;
    char *e = s + strlen(s);
    while (e > s && e[-1] == ' ') *--e = 0;

    char prompt[140];
    snprintf(prompt, sizeof prompt, "aetheros:~# %s", s);
    term_push(st, prompt);
    if (!s[0]) return;

    if (!strcmp(s, "clear")) { st->nlines = 0; return; }
    if (!strcmp(s, "help")) {
        term_push(st, "AetherOS 终端 · 内建: help / clear / about / modules");
        term_push(st, "其余命令由 BusyBox 真实执行，例如:");
        term_push(st, "  uname -a   ls /   free   ps   date   cat /proc/version");
        return;
    }
    if (!strcmp(s, "about")) {
        term_push(st, "AetherOS 以太操作系统 手机版");
        term_push(st, "Linux 6.6.70 内核 + BusyBox + 自研图形桌面");
        return;
    }
    if (!strcmp(s, "modules")) {
        term_push(st, "发行版模块清单 18 类 182 项，见「系统模块清单」应用");
        return;
    }
    char full[160];
    snprintf(full, sizeof full, "%s 2>&1", s);
    FILE *fp = popen(full, "r");
    if (!fp) { term_push(st, "执行失败：无法创建进程"); return; }
    char line[128];
    int got = 0;
    while (fgets(line, sizeof line, fp)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (n > 96) line[96] = 0;
        term_push(st, line);
        if (++got > 16) { term_push(st, "... 输出已截断 ..."); break; }
    }
    pclose(fp);
    if (!got) term_push(st, "(无输出)");
}

void app_term_open(struct app_state *st)
{
    st->nlines = 0; st->inlen = 0; st->input[0] = 0;
    term_push(st, "AetherOS 终端 · 输入 help 查看帮助，或点下方快捷命令");
    snprintf(st->status, sizeof st->status, "可直接输入命令并执行真实 BusyBox 程序");
}

void app_term_render(struct aether_win *w, int ox, int oy, int cw, int ch, struct app_state *st)
{
    (void)w;
    unsigned bg = aether_desktop_theme() == 1 ? pack_rgb(22, 24, 30) : pack_rgb(12, 15, 22);
    unsigned fg = pack_rgb(208, 230, 212), acc = pack_rgb(110, 220, 150);
    aether_fill_rect(ox, oy, cw, ch - 62, bg);
    int base = oy + 8;
    int maxrows = (ch - 62 - 16) / 18;
    if (maxrows < 1) maxrows = 1;
    int start = st->nlines > maxrows - 1 ? st->nlines - (maxrows - 1) : 0;
    for (int i = start; i < st->nlines; i++) {
        char b[100];
        aether_ui_fit(b, sizeof b, st->lines[i], cw - 16);
        aether_ui_label(ox + 8, base + (i - start) * 18, b, fg);
    }
    char inp[140], bi[140];
    snprintf(inp, sizeof inp, "aetheros:~# %s_", st->input);
    aether_ui_fit(bi, sizeof bi, inp, cw - 16);
    aether_ui_label(ox + 8, base + (st->nlines - start) * 18, bi, acc);

    static const char *quick[] = { "uname -a", "ls /", "free", "ps", "date", "help", "clear" };
    int n = 7, bw = (cw - 16 - (n - 1) * 6) / n, by = ch - 56;
    for (int i = 0; i < n; i++)
        aether_ui_button(ox + 8 + i * (bw + 6), oy + by, bw, 28, quick[i], C_BTN, C_BTN_TX);
    aether_ui_status(ox, oy, cw, ch, st);
}

void app_term_click(struct aether_win *w, int lx, int ly, int cw, int ch, struct app_state *st)
{
    (void)w; (void)ch;
    static const char *quick[] = { "uname -a", "ls /", "free", "ps", "date", "help", "clear" };
    int n = 7, bw = (cw - 16 - (n - 1) * 6) / n, by = ch - 56;
    for (int i = 0; i < n; i++) {
        if (aether_ui_hit(lx, ly, 8 + i * (bw + 6), by, bw, 28)) {
            term_exec(st, quick[i]);
            snprintf(st->status, sizeof st->status, "已执行真实命令：%s", quick[i]);
            return;
        }
    }
}

void app_term_key(struct aether_win *w, unsigned code, struct app_state *st)
{
    (void)w;
    if (code == K_ENTER) {
        term_exec(st, st->input);
        st->inlen = 0; st->input[0] = 0;
        return;
    }
    if (code == K_BACKSPACE) {
        if (st->inlen > 0) st->input[--st->inlen] = 0;
        return;
    }
    char c = aether_key_to_char(code, g_shift);
    if (c && st->inlen < (int)sizeof st->input - 2) {
        st->input[st->inlen++] = c;
        st->input[st->inlen] = 0;
    }
}

/* ==================== 文本编辑器 ==================== */
static void ed_save(struct app_state *st)
{
    FILE *f = fopen("/tmp/aether-note.txt", "w");
    if (!f) { snprintf(st->status, sizeof st->status, "保存失败：无法写入 /tmp"); return; }
    for (int i = 0; i < st->ed_n; i++) fprintf(f, "%s\n", st->ed[i]);
    fclose(f);
    snprintf(st->status, sizeof st->status, "已保存到 /tmp/aether-note.txt（%d 行）", st->ed_n);
}

void app_editor_open(struct app_state *st)
{
    memset(st->ed, 0, sizeof st->ed);
    st->ed_n = 1; st->ed_row = 0; st->ed_col = 0;
    snprintf(st->status, sizeof st->status, "键盘输入文字，回车换行，可保存到 /tmp");
}

void app_editor_render(struct aether_win *w, int ox, int oy, int cw, int ch, struct app_state *st)
{
    aether_ui_button(ox + 8, oy + 8, 84, 28, "新建", C_BTN, C_BTN_TX);
    aether_ui_button(ox + 98, oy + 8, 84, 28, "保存", C_BTN, C_BTN_TX);
    aether_ui_button(ox + 188, oy + 8, 84, 28, "清空", C_BTN, C_BTN_TX);
    aether_ui_label(ox + 284, oy + 14, "键盘输入 · 回车换行", C_DIM);
    aether_fill_rect(ox + 8, oy + 44, cw - 16, ch - 44 - 24, C_PANEL);
    aether_draw_border(ox + 8, oy + 44, cw - 16, ch - 44 - 24, C_BORDER, 1);
    int rows = (ch - 44 - 24 - 8) / 20;
    for (int i = 0; i < rows && i < MAX_ED_LINES; i++) {
        int idx = w->scroll + i, yy = oy + 50 + i * 20;
        char ln[8];
        snprintf(ln, sizeof ln, "%2d", idx + 1);
        aether_ui_label(ox + 14, yy + 2, ln, C_DIM);
        if (idx >= st->ed_n) continue;
        char t[MAX_ED_COLS + 1];
        aether_ui_fit(t, sizeof t, st->ed[idx], cw - 60);
        aether_ui_label(ox + 40, yy + 2, t, C_TEXT);
        if (idx == st->ed_row) {
            int cx = ox + 40 + aether_text_width(st->ed[idx]);
            if (cx > ox + cw - 20) cx = ox + cw - 20;
            aether_fill_rect(cx, yy, 2, 16, C_ACCENT);
        }
    }
    aether_ui_status(ox, oy, cw, ch, st);
}

void app_editor_click(struct aether_win *w, int lx, int ly, int cw, int ch, struct app_state *st)
{
    (void)cw; (void)ch; (void)ly;
    if (aether_ui_hit(lx, ly, 8, 8, 84, 28) || aether_ui_hit(lx, ly, 188, 8, 84, 28)) {
        memset(st->ed, 0, sizeof st->ed);
        st->ed_n = 1; st->ed_row = 0; st->ed_col = 0; w->scroll = 0;
        snprintf(st->status, sizeof st->status, "已新建空白文档");
        return;
    }
    if (aether_ui_hit(lx, ly, 98, 8, 84, 28)) ed_save(st);
}

void app_editor_key(struct aether_win *w, unsigned code, struct app_state *st)
{
    (void)w;
    char *line = st->ed[st->ed_row];
    int len = (int)strlen(line);
    if (code == K_ENTER) {
        if (st->ed_n < MAX_ED_LINES) {
            for (int i = st->ed_n; i > st->ed_row + 1; i--)
                snprintf(st->ed[i], sizeof st->ed[0], "%s", st->ed[i - 1]);
            st->ed_n++;
            st->ed_row++;
            st->ed[st->ed_row][0] = 0;
        }
        return;
    }
    if (code == K_BACKSPACE) {
        if (len > 0) line[len - 1] = 0;
        else if (st->ed_row > 0) { st->ed_row--; st->ed_n--; }
        return;
    }
    if (code == K_UP && st->ed_row > 0) { st->ed_row--; return; }
    if (code == K_DOWN && st->ed_row < st->ed_n - 1) { st->ed_row++; return; }
    char c = aether_key_to_char(code, g_shift);
    if (c && len < MAX_ED_COLS) { line[len] = c; line[len + 1] = 0; }
}

/* ==================== 计算器 ==================== */
static void calc_reset(struct app_state *st)
{
    snprintf(st->disp, sizeof st->disp, "0");
    st->acc = 0; st->op = 0; st->fresh = 1;
}

static void calc_press(struct app_state *st, const char *k)
{
    char c = k[0];
    if (c >= '0' && c <= '9') {
        if (st->fresh || !strcmp(st->disp, "0")) { snprintf(st->disp, sizeof st->disp, "%c", c); st->fresh = 0; }
        else if (strlen(st->disp) < 15) { size_t n = strlen(st->disp); st->disp[n] = c; st->disp[n + 1] = 0; }
        return;
    }
    if (c == '.') {
        if (st->fresh) { snprintf(st->disp, sizeof st->disp, "0."); st->fresh = 0; }
        else if (!strchr(st->disp, '.')) strncat(st->disp, ".", 2);
        return;
    }
    if (c == 'C') { calc_reset(st); return; }
    if (c == '<') { size_t n = strlen(st->disp); if (n > 1) st->disp[n - 1] = 0; else st->disp[0] = '0', st->disp[1] = 0; return; }
    if (c == '=' || c == '+' || c == '-' || c == '*' || c == '/') {
        double v = atof(st->disp), r = v;
        if (st->op) {
            switch (st->op) {
            case '+': r = st->acc + v; break;
            case '-': r = st->acc - v; break;
            case '*': r = st->acc * v; break;
            case '/': r = v != 0 ? st->acc / v : 0; break;
            }
            snprintf(st->disp, sizeof st->disp, "%.10g", r);
            st->acc = r;
        } else st->acc = v;
        st->op = (c == '=') ? 0 : c;
        st->fresh = 1;
        return;
    }
}

static const char *CALC_KEYS[5][4] = {
    { "C", "<", "/", "*" }, { "7", "8", "9", "-" }, { "4", "5", "6", "+" },
    { "1", "2", "3", "=" }, { "0", ".", "=", "C" },
};

void app_calc_open(struct app_state *st)
{
    calc_reset(st);
    snprintf(st->status, sizeof st->status, "点击按钮或键盘输入，进行真实运算");
}

void app_calc_render(struct aether_win *w, int ox, int oy, int cw, int ch, struct app_state *st)
{
    (void)w;
    aether_draw_round_rect(ox + 10, oy + 10, cw - 20, 54, 6, C_PANEL, 0xFF);
    aether_draw_border(ox + 10, oy + 10, cw - 20, 54, C_BORDER, 1);
    char d[40];
    aether_ui_fit(d, sizeof d, st->disp, cw - 36);
    int tw = aether_text_width_utf8(d);
    aether_ui_label(ox + cw - 18 - tw, oy + 28, d, C_TEXT);
    int gx = ox + 10, gy = oy + 76;
    int bw = (cw - 20 - 18) / 4;
    int bh = (ch - 76 - 24 - 10 - 24) / 5;
    if (bh < 26) bh = 26;
    for (int r = 0; r < 5; r++)
        for (int c = 0; c < 4; c++) {
            int x = gx + c * (bw + 6), y = gy + r * (bh + 6);
            char k = CALC_KEYS[r][c][0];
            int isnum = (k >= '0' && k <= '9') || k == '.';
            aether_ui_button(x, y, bw, bh, CALC_KEYS[r][c],
                             isnum ? C_BTN : C_ACCENT,
                             isnum ? C_BTN_TX : pack_rgb(255, 255, 255));
        }
    aether_ui_status(ox, oy, cw, ch, st);
}

void app_calc_click(struct aether_win *w, int lx, int ly, int cw, int ch, struct app_state *st)
{
    (void)w;
    int bw = (cw - 20 - 18) / 4;
    int bh = (ch - 76 - 24 - 10 - 24) / 5;
    if (bh < 26) bh = 26;
    for (int r = 0; r < 5; r++)
        for (int c = 0; c < 4; c++) {
            int x = 10 + c * (bw + 6), y = 76 + r * (bh + 6);
            if (aether_ui_hit(lx, ly, x, y, bw, bh)) {
                calc_press(st, CALC_KEYS[r][c]);
                snprintf(st->status, sizeof st->status, "按键 %s  →  当前结果 %s", CALC_KEYS[r][c], st->disp);
                return;
            }
        }
}

void app_calc_key(struct aether_win *w, unsigned code, struct app_state *st)
{
    (void)w;
    if (code == K_ENTER) { calc_press(st, "="); return; }
    if (code == K_BACKSPACE) { calc_press(st, "<"); return; }
    char c = aether_key_to_char(code, g_shift);
    if (!c) return;
    char k[2] = { c, 0 };
    if (c == 'c' || c == 'C') k[0] = 'C';
    else if (c == 'x' || c == 'X') k[0] = '*';
    calc_press(st, k);
}

/* ==================== 系统监视器 ==================== */
static unsigned long read_kv(const char *path, const char *key)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[256];
    unsigned long v = 0;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, key, strlen(key))) { sscanf(line + strlen(key), "%lu", &v); break; }
    }
    fclose(f);
    return v;
}

static void monitor_refresh(struct app_state *st)
{
    snprintf(st->status, sizeof st->status, "数据来自 /proc 实时读取 · 点击刷新可重新采样");
}

void app_monitor_open(struct app_state *st) { monitor_refresh(st); }

void app_monitor_render(struct aether_win *w, int ox, int oy, int cw, int ch, struct app_state *st)
{
    (void)w;
    unsigned long tot = read_kv("/proc/meminfo", "MemTotal:");
    unsigned long av  = read_kv("/proc/meminfo", "MemAvailable:");
    double mem_used = tot > 0 ? (double)(tot - av) / (double)tot * 100.0 : 0;
    char kern[160] = "unknown", cpu[160] = "unknown", up[64] = "0", load[64] = "-";
    FILE *f = fopen("/proc/version", "r");
    if (f) { if (!fgets(kern, sizeof kern, f)) kern[0] = 0; fclose(f); }
    size_t kn = strlen(kern); while (kn && kern[kn - 1] < 32) kern[--kn] = 0;
    f = fopen("/proc/uptime", "r");
    if (f) { if (!fgets(up, sizeof up, f)) up[0] = 0; fclose(f); }
    f = fopen("/proc/loadavg", "r");
    if (f) { if (!fgets(load, sizeof load, f)) load[0] = 0; fclose(f); }
    f = fopen("/proc/cpuinfo", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f))
            if (!strncmp(line, "model name", 10)) {
                char *p = strchr(line, ':');
                if (p) { snprintf(cpu, sizeof cpu, "%s", p + 2); size_t n = strlen(cpu); while (n && cpu[n - 1] < 32) cpu[--n] = 0; }
                break;
            }
        fclose(f);
    }
    /* 进程数：真实遍历 /proc 下的 PID 目录 */
    int nproc = 0;
    DIR *d = opendir("/proc");
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            char c = de->d_name[0];
            if (c >= '1' && c <= '9') nproc++;
        }
        closedir(d);
    }

    int y = oy + 10;
    aether_ui_label(ox + 12, y, "系统监视器 · 实时 /proc 数据", C_TEXT); y += 24;
    char b[200];
    aether_ui_fit(b, sizeof b, kern, cw - 24);
    aether_ui_label(ox + 12, y, b, C_DIM); y += 20;
    aether_ui_fit(b, sizeof b, cpu, cw - 24);
    aether_ui_label(ox + 12, y, b, C_DIM); y += 26;

    aether_ui_label(ox + 12, y, "内存使用", C_TEXT);
    snprintf(b, sizeof b, "%.1f%%  (%lu / %lu KB)", mem_used, (tot - av), tot);
    aether_ui_label(ox + cw - 190, y, b, C_DIM);
    y += 20;
    aether_ui_bar(ox + 12, y, cw - 24, 14, mem_used, C_ACCENT); y += 24;

    char run[200], runfit[200];
    snprintf(run, sizeof run, "运行时间 %s s   |   负载 %s   |   进程数 %d", up, load, nproc);
    aether_ui_fit(runfit, sizeof runfit, run, cw - 24);
    aether_ui_label(ox + 12, y, runfit, C_TEXT); y += 28;

    aether_ui_label(ox + 12, y, "进程列表（/proc 实时枚举前 8 个 PID）", C_TEXT); y += 22;
    d = opendir("/proc");
    int shown = 0;
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) && shown < 8) {
            char c = de->d_name[0];
            if (c < '1' || c > '9') continue;
            char p[96], comm[64] = "?";
            snprintf(p, sizeof p, "/proc/%s/comm", de->d_name);
            FILE *cf = fopen(p, "r");
            if (cf) { if (fgets(comm, sizeof comm, cf)) { size_t n = strlen(comm); while (n && comm[n - 1] < 32) comm[--n] = 0; } fclose(cf); }
            snprintf(b, sizeof b, "  PID %-8s %s", de->d_name, comm);
            char bb[160]; aether_ui_fit(bb, sizeof bb, b, cw - 24);
            aether_ui_label(ox + 12, y, bb, shown % 2 ? C_DIM : C_TEXT);
            y += 18; shown++;
        }
        closedir(d);
    }
    aether_ui_button(ox + cw - 100, oy + 8, 88, 28, "刷新", C_BTN, C_BTN_TX);
    aether_ui_status(ox, oy, cw, ch, st);
}

void app_monitor_click(struct aether_win *w, int lx, int ly, int cw, int ch, struct app_state *st)
{
    (void)w; (void)ch;
    if (aether_ui_hit(lx, ly, cw - 100, 8, 88, 28)) monitor_refresh(st);
}

/* ==================== 系统设置 ====================
 * 阶段 3 起，设置按总纲 5.15 重写为「18 项固定一级结构 + 可交互二级页」，
 * 实现移至 aether-settings.c（本文件只保留统一派发，避免两套实现）。 */

/* ==================== 关于本机 ==================== */
void app_about_open(struct app_state *st)
{
    snprintf(st->status, sizeof st->status, "AetherOS 以太操作系统 · 手机版");
}

void app_about_render(struct aether_win *w, int ox, int oy, int cw, int ch, struct app_state *st)
{
    (void)w;
    aether_draw_logo(ox + 92, oy + 92, 150);
    aether_ui_label(ox + 190, oy + 34, "AetherOS 以太操作系统", C_TEXT);
    aether_ui_label(ox + 190, oy + 58, "手机版 · Phone Edition", C_DIM);
    aether_ui_label(ox + 190, oy + 82, "基于 Linux 6.6.70 内核构建", C_DIM);
    aether_ui_label(ox + 190, oy + 104, "用户态：BusyBox + AetherOS 自研图形栈", C_DIM);
    int y = oy + 172;
    aether_fill_rect(ox + 14, y, cw - 28, 1, C_BORDER);
    y += 12;
    aether_ui_label(ox + 14, y, "发行版模块清单统计（18 类 / 182 项）", C_TEXT); y += 24;
    int cnt[4] = { 0, 0, 0, 0 };
    for (int g = 0; g < AETHER_MOD_GROUP_COUNT; g++)
        for (int i = 0; i < aether_mod_groups[g].count; i++) {
            char s = aether_mod_groups[g].items[i].state;
            int k = s == 'A' ? 0 : s == 'B' ? 1 : s == 'C' ? 2 : 3;
            cnt[k]++;
        }
    char b[160];
    snprintf(b, sizeof b, "本版已实现 %d   上游已提供 %d   部分实现 %d   规划中 %d",
             cnt[0], cnt[1], cnt[2], cnt[3]);
    aether_ui_label(ox + 14, y, b, C_ACCENT); y += 26;
    aether_ui_label(ox + 14, y, "A 已实现   B 上游提供   C 部分实现   - 未实现", C_DIM); y += 26;
    aether_ui_label(ox + 14, y, "前提：内核底层机制由 Linux 内核原生提供，无需重复开发。", C_DIM); y += 22;
    aether_ui_label(ox + 14, y, "本清单只含发行版需自研 / 整合 / 用户态实现的模块。", C_DIM);
    aether_ui_status(ox, oy, cw, ch, st);
}

void app_about_click(struct aether_win *w, int lx, int ly, int cw, int ch, struct app_state *st)
{
    (void)w; (void)lx; (void)ly; (void)cw; (void)ch; (void)st;
}

/* ==================== 系统模块清单 ==================== */
void app_modules_open(struct app_state *st)
{
    st->mod_sel = 0;
    snprintf(st->status, sizeof st->status, "共 18 类 182 项 · 点击左侧类别查看逐项真实归属");
}

void app_modules_render(struct aether_win *w, int ox, int oy, int cw, int ch, struct app_state *st)
{
    int lw = 250;
    int y = oy + 8;
    aether_ui_label(ox + 10, y, "模块分类（18 类）", C_TEXT);
    aether_ui_label(ox + lw + 14, y, "逐项真实归属", C_TEXT);
    y += 22;
    aether_fill_rect(ox + lw, oy + 8, 1, ch - 32, C_BORDER);

    int row_h = 20;
    int rows = (ch - 32 - 24) / row_h;
    for (int g = 0; g < AETHER_MOD_GROUP_COUNT && g < rows; g++) {
        int yy = y + g * row_h;
        if (g == st->mod_sel) aether_fill_rect(ox + 6, yy, lw - 12, row_h, C_ACCENT);
        char t[80];
        aether_ui_fit(t, sizeof t, aether_mod_groups[g].name, lw - 24);
        char cnt[16];
        snprintf(cnt, sizeof cnt, "%d", aether_mod_groups[g].count);
        aether_ui_label(ox + 12, yy + 2, t, g == st->mod_sel ? pack_rgb(255, 255, 255) : C_DIM);
        aether_ui_label(ox + lw - 34, yy + 2, cnt, g == st->mod_sel ? pack_rgb(255, 255, 255) : C_DIM);
    }

    const struct aether_mod_group *G = &aether_mod_groups[st->mod_sel];
    int rx = ox + lw + 12;
    char hdr[160];
    snprintf(hdr, sizeof hdr, "%s  (%d 项)", G->name, G->count);
    char hh[160]; aether_ui_fit(hh, sizeof hh, hdr, cw - lw - 28);
    aether_ui_label(rx, y, hh, C_TEXT);
    y += 24;
    int maxr = (ch - 32 - 24 - 24) / row_h;
    for (int i = 0; i < G->count && i < maxr; i++) {
        int idx = w->scroll + i;
        if (idx >= G->count) break;
        int yy = y + i * row_h;
        const struct aether_mod_item *it = &G->items[idx];
        char mark[4] = { it->state, 0, 0, 0 };
        aether_ui_label(rx, yy + 2, mark, aether_ui_state_color(it->state));
        char t[160]; aether_ui_fit(t, sizeof t, it->name, cw - lw - 60);
        aether_ui_label(rx + 20, yy + 2, t, C_TEXT);
    }
    if (G->count > maxr) aether_ui_label(rx, y + maxr * row_h, "上下方向键滚动", C_DIM);
    aether_ui_status(ox, oy, cw, ch, st);
}

void app_modules_click(struct aether_win *w, int lx, int ly, int cw, int ch, struct app_state *st)
{
    (void)cw;
    int lw = 250, y = 30, row_h = 20;
    int rows = (ch - 32 - 24) / row_h;
    if (lx >= 6 && lx < lw - 6 && ly >= y && ly < y + rows * row_h) {
        int g = (ly - y) / row_h;
        if (g >= 0 && g < AETHER_MOD_GROUP_COUNT) {
            st->mod_sel = g; w->scroll = 0;
            snprintf(st->status, sizeof st->status, "%s · %d 项 · %s",
                     aether_mod_groups[g].name, aether_mod_groups[g].count,
                     "A 已实现 / B 上游提供 / C 部分实现 / - 未实现");
        }
    }
}

void app_modules_key(struct aether_win *w, unsigned code, struct app_state *st)
{
    int maxr = 14;
    if (code == K_DOWN) w->scroll++;
    else if (code == K_UP) w->scroll--;
    else if (code == K_LEFT && st->mod_sel > 0) { st->mod_sel--; w->scroll = 0; }
    else if (code == K_RIGHT && st->mod_sel < AETHER_MOD_GROUP_COUNT - 1) { st->mod_sel++; w->scroll = 0; }
    if (w->scroll < 0) w->scroll = 0;
    if (w->scroll > aether_mod_groups[st->mod_sel].count - maxr)
        w->scroll = aether_mod_groups[st->mod_sel].count - maxr;
    if (w->scroll < 0) w->scroll = 0;
}

/* ==================== 统一派发接口 ==================== */
void aether_app_open(struct aether_win *win)
{
    struct app_state *st = aether_app_state(win);
    if (!st) return;
    memset(st, 0, sizeof *st);
    win->scroll = 0;
    switch (win->kind) {
    case APP_FILES:    app_files_open(st); break;
    case APP_TERMINAL: app_term_open(st); break;
    case APP_EDITOR:   app_editor_open(st); break;
    case APP_CALC:     app_calc_open(st); break;
    case APP_MONITOR:  app_monitor_open(st); break;
    case APP_SETTINGS: app_settings_open(st); break;
    case APP_ABOUT:    app_about_open(st); break;
    case APP_MODULES:  app_modules_open(st); break;
    case APP_BROWSER:  app_browser_open(st); break;
    case APP_GALLERY:  app_gallery_open(st); break;
    case APP_VIDEO:    app_video_open(st); break;
    case APP_MUSIC:    app_music_open(st); break;
    case APP_CLOCK:    app_clock_open(st); break;
    case APP_CALENDAR: app_calendar_open(st); break;
    case APP_STORE:    app_store_open(st); break;
    case APP_NOTES:    app_notes_open(st); break;
    default: break;
    }
}

void aether_app_close(struct aether_win *win)
{
    struct app_state *st = aether_app_state(win);
    if (st) memset(st, 0, sizeof *st);
}

void aether_app_render(struct aether_win *win, int ox, int oy, int cw, int ch)
{
    struct app_state *st = aether_app_state(win);
    if (!st) return;
    aether_ui_palette();
    aether_render_set_clip(ox, oy, cw, ch);
    switch (win->kind) {
    case APP_FILES:    app_files_render(win, ox, oy, cw, ch, st); break;
    case APP_TERMINAL: app_term_render(win, ox, oy, cw, ch, st); break;
    case APP_EDITOR:   app_editor_render(win, ox, oy, cw, ch, st); break;
    case APP_CALC:     app_calc_render(win, ox, oy, cw, ch, st); break;
    case APP_MONITOR:  app_monitor_render(win, ox, oy, cw, ch, st); break;
    case APP_SETTINGS: app_settings_render(win, ox, oy, cw, ch, st); break;
    case APP_ABOUT:    app_about_render(win, ox, oy, cw, ch, st); break;
    case APP_MODULES:  app_modules_render(win, ox, oy, cw, ch, st); break;
    case APP_BROWSER:  app_browser_render(win, ox, oy, cw, ch, st); break;
    case APP_GALLERY:  app_gallery_render(win, ox, oy, cw, ch, st); break;
    case APP_VIDEO:    app_video_render(win, ox, oy, cw, ch, st); break;
    case APP_MUSIC:    app_music_render(win, ox, oy, cw, ch, st); break;
    case APP_CLOCK:    app_clock_render(win, ox, oy, cw, ch, st); break;
    case APP_CALENDAR: app_calendar_render(win, ox, oy, cw, ch, st); break;
    case APP_STORE:    app_store_render(win, ox, oy, cw, ch, st); break;
    case APP_NOTES:    app_notes_render(win, ox, oy, cw, ch, st); break;
    default: break;
    }
    aether_render_clear_clip();
}

void aether_app_click(struct aether_win *win, int lx, int ly, int cw, int ch)
{
    struct app_state *st = aether_app_state(win);
    if (!st) return;
    aether_ui_palette();
    switch (win->kind) {
    case APP_FILES:    app_files_click(win, lx, ly, cw, ch, st); break;
    case APP_TERMINAL: app_term_click(win, lx, ly, cw, ch, st); break;
    case APP_EDITOR:   app_editor_click(win, lx, ly, cw, ch, st); break;
    case APP_CALC:     app_calc_click(win, lx, ly, cw, ch, st); break;
    case APP_MONITOR:  app_monitor_click(win, lx, ly, cw, ch, st); break;
    case APP_SETTINGS: app_settings_click(win, lx, ly, cw, ch, st); break;
    case APP_ABOUT:    app_about_click(win, lx, ly, cw, ch, st); break;
    case APP_MODULES:  app_modules_click(win, lx, ly, cw, ch, st); break;
    case APP_BROWSER:  app_browser_click(win, lx, ly, cw, ch, st); break;
    case APP_GALLERY:  app_gallery_click(win, lx, ly, cw, ch, st); break;
    case APP_VIDEO:    app_video_click(win, lx, ly, cw, ch, st); break;
    case APP_MUSIC:    app_music_click(win, lx, ly, cw, ch, st); break;
    case APP_CLOCK:    app_clock_click(win, lx, ly, cw, ch, st); break;
    case APP_CALENDAR: app_calendar_click(win, lx, ly, cw, ch, st); break;
    case APP_STORE:    app_store_click(win, lx, ly, cw, ch, st); break;
    case APP_NOTES:    app_notes_click(win, lx, ly, cw, ch, st); break;
    default: break;
    }
}

void aether_app_key(struct aether_win *win, unsigned code, int shift)
{
    struct app_state *st = aether_app_state(win);
    if (!st) return;
    g_shift = shift;
    switch (win->kind) {
    case APP_FILES:    app_files_key(win, code, st); break;
    case APP_TERMINAL: app_term_key(win, code, st); break;
    case APP_EDITOR:   app_editor_key(win, code, st); break;
    case APP_CALC:     app_calc_key(win, code, st); break;
    case APP_MODULES:  app_modules_key(win, code, st); break;
    case APP_SETTINGS: app_settings_key(win, code, st); break;
    case APP_BROWSER:  app_browser_key(win, code, st); break;
    case APP_GALLERY:  app_gallery_key(win, code, st); break;
    case APP_VIDEO:    app_video_key(win, code, st); break;
    case APP_MUSIC:    app_music_key(win, code, st); break;
    case APP_CLOCK:    app_clock_key(win, code, st); break;
    case APP_CALENDAR: app_calendar_key(win, code, st); break;
    case APP_STORE:    app_store_key(win, code, st); break;
    case APP_NOTES:    app_notes_key(win, code, st); break;
    default: break;
    }
}

void aether_app_cmd(struct aether_win *win, const char *line)
{
    struct app_state *st = aether_app_state(win);
    if (!st) return;
    aether_ui_palette();
    switch (win->kind) {
    case APP_SETTINGS: app_settings_cmd(win, line, st); break;
    case APP_BROWSER:  app_browser_cmd(win, line, st); break;
    case APP_GALLERY:  app_gallery_cmd(win, line, st); break;
    case APP_VIDEO:    app_video_cmd(win, line, st); break;
    case APP_MUSIC:    app_music_cmd(win, line, st); break;
    case APP_CLOCK:    app_clock_cmd(win, line, st); break;
    case APP_CALENDAR: app_calendar_cmd(win, line, st); break;
    case APP_STORE:    app_store_cmd(win, line, st); break;
    case APP_NOTES:    app_notes_cmd(win, line, st); break;
    default: break;
    }
}
