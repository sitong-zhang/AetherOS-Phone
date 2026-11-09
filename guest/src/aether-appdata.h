/*
 * aether-appdata.h - 内置应用共享状态与 UI helper 声明
 * 拆分到独立头，使 apps / apps2 / ui 三个编译单元共用同一份状态。
 */
#ifndef AETHER_APPDATA_H
#define AETHER_APPDATA_H

#include "aether-desktop.h"

#define MAX_TERM_LINES 20
#define MAX_ED_LINES   20
#define MAX_ED_COLS    76
#define MAX_ENTRIES    64

struct file_entry {
    char name[96];
    int  isdir;
    unsigned long size;
};

struct app_state {
    /* 文件管理器 */
    char path[256];
    struct file_entry ent[MAX_ENTRIES];
    int nent;
    /* 终端 */
    char lines[MAX_TERM_LINES][100];
    int nlines;
    char input[100];
    int inlen;
    /* 编辑器 */
    char ed[MAX_ED_LINES][MAX_ED_COLS + 1];
    int ed_n, ed_row, ed_col;
    /* 计算器 */
    char disp[32];
    double acc;
    char op;
    int fresh;
    /* 模块清单 */
    int mod_sel;
    /* 通用 */
    char status[256];

    /* ---- 阶段 3 新增：浏览器 ---- */
    char url[256];
    char addr[256];  int addr_len;  int addr_edit;
    char hist[8][256]; int hist_n, hist_pos;
    char page[64][120]; int page_n;

    /* ---- 阶段 3 新增：图片查看器 / 视频 / 音乐（共享真实媒体库） ---- */
    char media[48][160]; int media_n, media_sel;
    int  zoom;                       /* 图片缩放档 0 适应 / 1 原图 / 2 放大 */
    int  playing, loop, volume;
    long play_start_ms, play_offset_ms;  /* 真实单调时钟驱动的播放进度 */
    int  wav_rate, wav_bits, wav_ch, wav_ms;
    int  vid_frames, vid_fps;

    /* ---- 阶段 3 新增：时钟（闹钟 / 计时器 / 秒表） ---- */
    int  clock_tab;                  /* 0 闹钟 1 计时器 2 秒表 */
    char alarms[8][16]; int alarm_on[8]; int alarm_n; int alarm_ring;
    int  timer_set_s, timer_left_s, timer_run; long timer_end_ms;
    int  sw_run; long sw_start_ms, sw_acc_ms; long sw_laps[8]; int sw_lap_n;
    char numbuf[24]; int numlen;

    /* ---- 阶段 3 新增：日历 ---- */
    int  cal_y, cal_m, cal_sel;
    char events[16][96]; int event_n;
    char ev_input[96]; int ev_len;

    /* ---- 阶段 3 新增：设置 18 项一级 + 二级页 ---- */
    int  set_page;                   /* -1 一级；0..17 二级页 */
    int  set_confirm;                /* 危险操作二次确认（0 无 / >0 待确认项） */
    char set_query[64]; int set_query_len;

    /* ---- 阶段 5：应用商店（总纲 5.16 / 模块十） ---- */
    int  store_view;                 /* 0 首页 1 详情 2 搜索结果 */
    int  store_sel;                  /* 详情/选中目录索引 */
    int  store_scroll;               /* 列表滚动 */
    int  store_hscroll;              /* 推荐位横向滚动 */
    int  store_install;              /* 0 空闲 1 下载 2 校验 3 安装 4 完成/已装 */
    int  store_prog;                 /* 0..100 */
    int  store_target;               /* enum app_kind 安装目标 */
    int  store_perm;                 /* 首开权限授予弹窗 0/1 */
    long store_t0;                   /* 安装计时起点（单调） */
    char store_query[48]; int store_qlen;

    /* ---- 阶段 5：笔记（文件真实存储） ---- */
    int  notes_sel;                  /* 选中笔记序号 */
    int  notes_edit;                 /* 1 编辑中 */
    int  notes_focus;                /* 0 标题 1 正文 */
    char notes_title[40]; int notes_tlen;
    char notes_body[1024]; int notes_blen;
    int  notes_scroll;
};

extern struct app_state aether_app_states[MAX_WINDOWS];
struct app_state *aether_app_state(struct aether_win *win);

/* ---- 主题调色板（ui.c 定义） ---- */
extern unsigned C_BG, C_PANEL, C_TEXT, C_DIM, C_ACCENT, C_BORDER, C_BTN, C_BTN_TX;
extern unsigned C_OK, C_WARN, C_BAD;
void aether_ui_palette(void);
/* 每帧由外壳注入最后触点与按压状态，应用内所有按钮自动获得按压反馈 */
void aether_ui_set_touch(int x, int y, int down);
unsigned aether_ui_state_color(char s);

/* ---- 绘图 helper（ui.c 定义） ---- */
int  aether_ui_hit(int lx, int ly, int x, int y, int w, int h);
void aether_ui_fit(char *dst, size_t cap, const char *src, int maxpx);
void aether_ui_label(int x, int y, const char *s, unsigned c);
void aether_ui_button(int x, int y, int w, int h, const char *s, unsigned bg, unsigned fg);
void aether_ui_bar(int x, int y, int w, int h, double pct, unsigned c);
void aether_ui_status(int ox, int oy, int cw, int ch, struct app_state *st);
/* 阶段 5：应用通用顶部栏（标题 + 返回），商店/笔记共用 */
int app_top_bar(struct aether_win *w, int ox, int oy, int cw, int ch, const char *title);

/* ---- 各应用实现（apps.c / apps2.c） ---- */
void app_files_open(struct app_state *st);
void app_files_render(struct aether_win *w, int ox, int oy, int cw, int ch, struct app_state *st);
void app_files_click(struct aether_win *w, int lx, int ly, int cw, int ch, struct app_state *st);
void app_files_key(struct aether_win *w, unsigned code, struct app_state *st);

void app_term_open(struct app_state *st);
void app_term_render(struct aether_win *w, int ox, int oy, int cw, int ch, struct app_state *st);
void app_term_click(struct aether_win *w, int lx, int ly, int cw, int ch, struct app_state *st);
void app_term_key(struct aether_win *w, unsigned code, struct app_state *st);

void app_editor_open(struct app_state *st);
void app_editor_render(struct aether_win *w, int ox, int oy, int cw, int ch, struct app_state *st);
void app_editor_click(struct aether_win *w, int lx, int ly, int cw, int ch, struct app_state *st);
void app_editor_key(struct aether_win *w, unsigned code, struct app_state *st);

void app_calc_open(struct app_state *st);
void app_calc_render(struct aether_win *w, int ox, int oy, int cw, int ch, struct app_state *st);
void app_calc_click(struct aether_win *w, int lx, int ly, int cw, int ch, struct app_state *st);
void app_calc_key(struct aether_win *w, unsigned code, struct app_state *st);

void app_monitor_open(struct app_state *st);
void app_monitor_render(struct aether_win *w, int ox, int oy, int cw, int ch, struct app_state *st);
void app_monitor_click(struct aether_win *w, int lx, int ly, int cw, int ch, struct app_state *st);

void app_settings_open(struct app_state *st);
void app_settings_render(struct aether_win *w, int ox, int oy, int cw, int ch, struct app_state *st);
void app_settings_click(struct aether_win *w, int lx, int ly, int cw, int ch, struct app_state *st);

void app_about_open(struct app_state *st);
void app_about_render(struct aether_win *w, int ox, int oy, int cw, int ch, struct app_state *st);
void app_about_click(struct aether_win *w, int lx, int ly, int cw, int ch, struct app_state *st);

void app_modules_open(struct app_state *st);
void app_modules_render(struct aether_win *w, int ox, int oy, int cw, int ch, struct app_state *st);
void app_modules_click(struct aether_win *w, int lx, int ly, int cw, int ch, struct app_state *st);
void app_modules_key(struct aether_win *w, unsigned code, struct app_state *st);

/* ---- 设置（18 项一级 + 可交互二级页） ---- */
void app_settings_key(struct aether_win *w, unsigned code, struct app_state *st);
void app_settings_cmd(struct aether_win *w, const char *line, struct app_state *st);

/* ---- 阶段 3：内置应用（第四章十二最低配置补齐） ---- */
#define APPDECL(name) \
    void app_##name##_open(struct app_state *st); \
    void app_##name##_render(struct aether_win *w, int ox, int oy, int cw, int ch, struct app_state *st); \
    void app_##name##_click(struct aether_win *w, int lx, int ly, int cw, int ch, struct app_state *st); \
    void app_##name##_key(struct aether_win *w, unsigned code, struct app_state *st); \
    void app_##name##_cmd(struct aether_win *w, const char *line, struct app_state *st)

APPDECL(browser);
APPDECL(gallery);
APPDECL(video);
APPDECL(music);
APPDECL(clock);
APPDECL(calendar);
APPDECL(store);
APPDECL(notes);

#endif /* AETHER_APPDATA_H */
