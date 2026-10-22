/*
 * Aurora desktop shell (runs inside the kernel until user space exists).
 *  - Aurora boot splash, classic glass login, desktop with icons and clock
 *  - Taskbar docked bottom / left / right, medium icons, clear Aero styling and running-app indicators
 *  - Start menu with pinned apps and live search
 *  - Window manager: drag, minimise, close, focus/raise
 *  - Apps: AuroraFM, AuroraNotes, AuroraMonitor, AuroraSettings
 * Rendering is "redraw on change": every frame is composed into a back buffer,
 * and every visible control registers a hit-rectangle so input needs no extra layout code.
 */
#include "kernel.h"
#include "gfx.h"
#include "icons.h"
#include "input.h"
#include "timeutil.h"
#include "pmm.h"
#include "limine.h"

#define TITLE_H 32

#define C_TEXT   RGB(236, 240, 247)
#define C_DIM    RGB(142, 152, 172)
#define C_WIN    RGB(30, 33, 44)
#define C_TITLE  RGB(38, 42, 56)
#define C_CARD   RGB(40, 44, 58)
#define C_FIELD  RGB(22, 25, 36)
#define C_BORDER RGB(70, 76, 96)
#define C_DARKTX RGB(14, 18, 28)

enum { APP_FILES, APP_NOTES, APP_MONITOR, APP_SETTINGS, APP_COUNT };
enum { TB_BOTTOM, TB_LEFT, TB_RIGHT };
enum { SCREEN_BOOT, SCREEN_LOGIN, SCREEN_DESKTOP };
enum { LAYER_DESKTOP = -1, LAYER_OVERLAY = -2 };

enum {
    ACT_NONE = 0,
    ACT_WIN_BODY = 0x100, ACT_WIN_TITLE = 0x110, ACT_WIN_CLOSE = 0x120, ACT_WIN_MIN = 0x130,
    ACT_TB_START = 0x200, ACT_TB_SEARCH = 0x201, ACT_TB_BG = 0x202, ACT_TB_SHOWDESK = 0x203,
    ACT_TB_APP = 0x210,
    ACT_SM_APP = 0x300, ACT_SM_POWER = 0x310, ACT_SM_SEARCH = 0x311, ACT_SM_BODY = 0x312,
    ACT_DESK_BG = 0x400, ACT_DESK_ICON = 0x410,
    ACT_SET_PAGE = 0x500, ACT_SET_TBPOS = 0x510, ACT_SET_ICON = 0x520, ACT_SET_ACCENT = 0x530,
    ACT_SET_PINTB = 0x540, ACT_SET_PINST = 0x550,
    ACT_FILES_NAV = 0x600, ACT_FILES_ROW = 0x620,
    ACT_LOGIN_SIGNIN = 0x700
};

static const char *app_label[APP_COUNT] = { "Files", "Notes", "Monitor", "Settings" };
static const char *app_title[APP_COUNT] = { "AuroraFM", "AuroraNotes", "AuroraMonitor", "AuroraSettings" };
static const int   app_icon[APP_COUNT]  = { ICON_FILES, ICON_NOTES, ICON_MONITOR, ICON_SETTINGS };
static const int   app_w[APP_COUNT]     = { 660, 520, 560, 660 };
static const int   app_h[APP_COUNT]     = { 420, 360, 390, 470 };

static const uint32_t accents[5] = {
    RGB(90, 220, 190), RGB(150, 122, 255), RGB(84, 154, 255), RGB(255, 192, 92), RGB(255, 112, 152)
};
static const int icon_sizes[3] = { 24, 32, 40 };

static struct {
    int  tb_pos, icon, accent;
    bool pin_tb[APP_COUNT], pin_start[APP_COUNT];
} cfg = { TB_BOTTOM, 32, 0, { true, true, false, false }, { true, true, true, true } };
#define ACCENT (accents[cfg.accent])

/* ---------- state ---------- */

static int SW, SH;
static int shell_screen = SCREEN_BOOT;
static uint64_t screen_started;
struct win { bool open, min; int x, y, w, h; };
static struct win wins[APP_COUNT];
static int zorder[APP_COUNT], zcount;

static bool start_open;
static char search[32];
static int  search_len;
static int  desk_sel = -1;
static int  drag_app = -1, drag_dx, drag_dy;
static int  last_click_id = -1;
static uint64_t last_click_ms;
static int  settings_page = 1;
static int  files_nav = 1, files_sel = -1;
static char notes[2048];
static int  notes_len;

static struct rtc_time now;
static int fps_hist[60], fps_idx, frames;
static bool dirty = true, blink_on;
static int hover_id;

static int cur_layer, top_at_mouse;
static bool over_overlay;

static int tb_x, tb_y, tb_w, tb_h, tb_slot;
static int work_x, work_y, work_w, work_h;
struct tb_item { int id, app, icon, x, y; };
static struct tb_item tbi[16];
static int ntbi;

/* ---------- hit testing ---------- */

struct hit { int x, y, w, h, id; };
static struct hit hits[512];
static int nhits;

static bool in_rect(int px, int py, int x, int y, int w, int h) {
    return px >= x && py >= y && px < x + w && py < y + h;
}
static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

static void hit_add(int x, int y, int w, int h, int id) {
    int cx, cy, cw, ch;
    gfx_clip_get(&cx, &cy, &cw, &ch);
    int x0 = imax(x, cx), y0 = imax(y, cy), x1 = imin(x + w, cx + cw), y1 = imin(y + h, cy + ch);
    if (x1 <= x0 || y1 <= y0 || nhits >= 512) return;
    hits[nhits++] = (struct hit){ x0, y0, x1 - x0, y1 - y0, id };
}

static int hit_find(int px, int py) {
    for (int i = nhits - 1; i >= 0; i--)
        if (in_rect(px, py, hits[i].x, hits[i].y, hits[i].w, hits[i].h)) return hits[i].id;
    return ACT_NONE;
}

static bool hov(int x, int y, int w, int h) {
    if (!in_rect(mouse_x, mouse_y, x, y, w, h)) return false;
    int cx, cy, cw, ch;
    gfx_clip_get(&cx, &cy, &cw, &ch);
    if (!in_rect(mouse_x, mouse_y, cx, cy, cw, ch)) return false;
    if (cur_layer == LAYER_OVERLAY) return true;
    return !over_overlay && cur_layer == top_at_mouse;
}

/* registers a clickable area and reports hover */
static bool ui_btn(int x, int y, int w, int h, int id) {
    hit_add(x, y, w, h, id);
    return hov(x, y, w, h);
}

/* ---------- small widgets ---------- */

static void text_c(int cx, int y, const char *s, uint32_t c, int font) {
    gfx_text(cx - gfx_text_w(s, font) / 2, y, s, c, font);
}

static void seg_btn(int x, int y, int w, int h, const char *label, bool sel, int id) {
    bool hv = ui_btn(x, y, w, h, id);
    gfx_rrect(x, y, w, h, 6, sel ? ACCENT : (hv ? RGB(68, 75, 96) : RGB(50, 55, 72)), 255);
    text_c(x + w / 2, y + (h - 12) / 2, label, sel ? C_DARKTX : C_TEXT, FONT_S);
}

static void toggle(int x, int y, bool on, int id) {
    bool hv = ui_btn(x, y, 38, 20, id);
    gfx_rrect(x, y, 38, 20, 10, on ? ACCENT : (hv ? RGB(84, 92, 114) : RGB(62, 68, 88)), 255);
    gfx_circle16((x + (on ? 28 : 10)) * 16, (y + 10) * 16, 7 * 16, on ? C_DARKTX : RGB(235, 238, 245), 255);
}

static void fmt_uptime(char *buf, size_t n) {
    uint64_t s = uptime_ms() / 1000;
    ksnprintf(buf, n, "%02lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)((s / 60) % 60),
              (unsigned long)(s % 60));
}

/* ---------- window helpers ---------- */

static void z_remove(int a) {
    int j = 0;
    for (int i = 0; i < zcount; i++) if (zorder[i] != a) zorder[j++] = zorder[i];
    zcount = j;
}
static void z_push(int a) { z_remove(a); zorder[zcount++] = a; }

static int top_app(void) {
    for (int i = zcount - 1; i >= 0; i--) {
        struct win *w = &wins[zorder[i]];
        if (w->open && !w->min) return zorder[i];
    }
    return -1;
}

static int win_at(int px, int py) {
    for (int i = zcount - 1; i >= 0; i--) {
        struct win *w = &wins[zorder[i]];
        if (w->open && !w->min && in_rect(px, py, w->x, w->y, w->w, w->h)) return zorder[i];
    }
    return LAYER_DESKTOP;
}

static void clamp_win(struct win *w) {
    w->x = imax(work_x - w->w + 90, imin(w->x, work_x + work_w - 90));
    w->y = imax(work_y, imin(w->y, work_y + work_h - TITLE_H));
}

static void open_app(int a) {
    struct win *w = &wins[a];
    if (!w->open) {
        w->open = true;
        w->w = imin(app_w[a], work_w - 20);
        w->h = imin(app_h[a], work_h - 20);
        w->x = work_x + (work_w - w->w) / 2 - 50 + a * 30;
        w->y = work_y + (work_h - w->h) / 2 - 30 + a * 22;
        clamp_win(w);
    }
    w->min = false;
    z_push(a);
    start_open = false;
}

static void close_app(int a) { wins[a].open = false; wins[a].min = false; z_remove(a); }

/* ---------- taskbar layout ---------- */

static void layout_taskbar(void) {
    /* Large, touch-friendly targets inside a taller Aero glass taskbar. */
    tb_slot = cfg.icon + 24;
    int thick = tb_slot + 16, gap = 6;
    ntbi = 0;
    tbi[ntbi++] = (struct tb_item){ ACT_TB_START, -1, ICON_LOGO, 0, 0 };
    tbi[ntbi++] = (struct tb_item){ ACT_TB_SEARCH, -1, ICON_SEARCH, 0, 0 };
    for (int a = 0; a < APP_COUNT; a++)
        if (cfg.pin_tb[a] || wins[a].open)
            tbi[ntbi++] = (struct tb_item){ ACT_TB_APP + a, a, app_icon[a], 0, 0 };
    int total = ntbi * tb_slot + (ntbi - 1) * gap;

    switch (cfg.tb_pos) {
    case TB_LEFT:
        tb_x = 0; tb_y = 0; tb_w = thick; tb_h = SH;
        work_x = thick; work_y = 0; work_w = SW - thick; work_h = SH;
        break;
    case TB_RIGHT:
        tb_x = SW - thick; tb_y = 0; tb_w = thick; tb_h = SH;
        work_x = 0; work_y = 0; work_w = SW - thick; work_h = SH;
        break;
    default:
        tb_x = 0; tb_y = SH - thick; tb_w = SW; tb_h = thick;
        work_x = 0; work_y = 0; work_w = SW; work_h = SH - thick;
        break;
    }
    for (int i = 0; i < ntbi; i++) {
        if (cfg.tb_pos == TB_BOTTOM) {
            tbi[i].x = (SW - total) / 2 + i * (tb_slot + gap);
            tbi[i].y = tb_y + (thick - tb_slot) / 2;
        } else {
            tbi[i].x = tb_x + (thick - tb_slot) / 2;
            tbi[i].y = 12 + i * (tb_slot + gap);
        }
    }
}

static void start_geom(int *x, int *y, int *w, int *h) {
    *w = 420; *h = 500;
    if (*h > work_h - 20) *h = work_h - 20;
    if (cfg.tb_pos == TB_BOTTOM) { *x = (SW - *w) / 2; *y = tb_y - 10 - *h; }
    else if (cfg.tb_pos == TB_LEFT) { *x = tb_x + tb_w + 8; *y = 10; }
    else { *x = tb_x - 8 - *w; *y = 10; }
}

/* ======================================================================
 *  Applications
 * ====================================================================== */

/* ---- AuroraFM ---- */
struct fent { const char *name, *type, *size; bool dir; };
static const struct fent f_desktop[] = {
    { "This PC", "Shortcut", "", false }, { "Aurora Notes", "Shortcut", "", false },
    { "Settings", "Shortcut", "", false } };
static const struct fent f_docs[] = {
    { "Projects", "Folder", "", true }, { "Budget.xlsx", "Spreadsheet", "48 KB", false },
    { "Resume.docx", "Document", "112 KB", false }, { "notes.txt", "Text file", "2 KB", false },
    { "aurora-spec.md", "Markdown", "9 KB", false } };
static const struct fent f_down[] = {
    { "aurora.iso", "Disc image", "3.7 MB", false }, { "limine.zip", "Archive", "1.4 MB", false },
    { "wallpaper.png", "Image", "822 KB", false } };
static const struct fent f_pics[] = {
    { "Screenshots", "Folder", "", true }, { "aurora-boot.png", "Image", "16 KB", false },
    { "panic-test.png", "Image", "11 KB", false } };
#define NENT(a) ((int)(sizeof(a) / sizeof((a)[0])))
static const struct { const char *name; const struct fent *e; int n; } fnav[4] = {
    { "Desktop", f_desktop, NENT(f_desktop) }, { "Documents", f_docs, NENT(f_docs) },
    { "Downloads", f_down, NENT(f_down) },     { "Pictures", f_pics, NENT(f_pics) } };

static void app_files(int cx, int cy, int cw, int ch) {
    int ty = cy + 8;
    icon_draw(ICON_CHEV_L, cx + 12, ty + 4, 20, C_DIM);
    icon_draw(ICON_CHEV_R, cx + 40, ty + 4, 20, C_DIM);
    icon_draw(ICON_CHEV_UP, cx + 68, ty + 4, 20, C_DIM);
    int px = cx + 104, pw = cw - 104 - 14;
    gfx_rrect(px, ty, pw, 28, 14, C_FIELD, 255);
    char path[64];
    ksnprintf(path, sizeof path, "This PC  >  %s", fnav[files_nav].name);
    gfx_text(px + 16, ty + 8, path, C_TEXT, FONT_S);

    /* navigation pane */
    int nx = cx + 10, ny = cy + 52;
    gfx_text(nx + 6, ny + 2, "Quick access", C_DIM, FONT_S);
    for (int i = 0; i < 4; i++) {
        int y = ny + 22 + i * 30;
        bool hv = ui_btn(nx, y, 138, 28, ACT_FILES_NAV + i);
        if (i == files_nav) {
            gfx_rrect(nx, y, 138, 28, 6, ACCENT, 46);
            gfx_rrect(nx, y + 6, 3, 16, 1, ACCENT, 255);
        } else if (hv) gfx_rrect(nx, y, 138, 28, 6, RGB(255, 255, 255), 16);
        icon_draw(ICON_FILES, nx + 12, y + 6, 16, 0);
        gfx_text(nx + 36, y + 8, fnav[i].name, C_TEXT, FONT_S);
    }
    gfx_rect(cx + 156, cy + 52, 1, ch - 52 - 40, RGB(255, 255, 255), 18);

    /* file list */
    int lx = cx + 166, lw = cw - 166 - 14, hy = cy + 54;
    int col_t = lx + lw * 54 / 100, col_s = lx + lw * 82 / 100;
    gfx_text(lx + 34, hy, "Name", C_DIM, FONT_S);
    gfx_text(col_t, hy, "Type", C_DIM, FONT_S);
    gfx_text(col_s, hy, "Size", C_DIM, FONT_S);
    gfx_rect(lx, hy + 18, lw, 1, RGB(255, 255, 255), 18);
    const struct fent *e = fnav[files_nav].e;
    int n = fnav[files_nav].n;
    for (int i = 0; i < n; i++) {
        int ry = hy + 24 + i * 28;
        bool hv = ui_btn(lx, ry, lw, 26, ACT_FILES_ROW + i);
        if (i == files_sel) {
            gfx_rrect(lx, ry, lw, 26, 6, ACCENT, 40);
        } else if (hv) gfx_rrect(lx, ry, lw, 26, 6, RGB(255, 255, 255), 14);
        icon_draw(e[i].dir ? ICON_FILES : ICON_DOC, lx + 10, ry + 5, 16, 0);
        gfx_text(lx + 34, ry + 7, e[i].name, C_TEXT, FONT_S);
        gfx_text(col_t, ry + 7, e[i].type, C_DIM, FONT_S);
        gfx_text(col_s, ry + 7, e[i].size, C_DIM, FONT_S);
    }
    char st[96];
    ksnprintf(st, sizeof st, "%d items    Demo data - AuroraFS is not mounted yet", n);
    gfx_text(cx + 14, cy + ch - 30, st, C_DIM, FONT_S);
}

/* ---- AuroraNotes ---- */
static void app_notes(int cx, int cy, int cw, int ch) {
    int tx = cx + 16, ty = cy + 14, cols = (cw - 32) / 8;
    int col = 0, row = 0;
    for (int i = 0; i <= notes_len; i++) {
        if (i == notes_len) break;
        char c = notes[i];
        if (c == '\n') { col = 0; row++; continue; }
        if (col >= cols) { col = 0; row++; }
        gfx_glyph(tx + col * 8, ty + row * 18, c, C_TEXT, FONT_M);
        col++;
    }
    if (col >= cols) { col = 0; row++; }
    if (blink_on && top_app() == APP_NOTES)
        gfx_rect(tx + col * 8, ty + row * 18, 2, 16, ACCENT, 255);
    if (notes_len == 0)
        gfx_text(tx + 6, ty + 1, "Click here and start typing...", C_DIM, FONT_M);
    char st[64];
    ksnprintf(st, sizeof st, "%d characters    (not saved - AuroraFS pending)", notes_len);
    gfx_text(cx + 16, cy + ch - 30, st, C_DIM, FONT_S);
}

/* ---- AuroraMonitor ---- */
static void app_monitor(int cx, int cy, int cw, int ch) {
    int x0 = cx + 16, w = cw - 32;
    uint64_t total = pmm_total_bytes(), freeb = pmm_free_bytes(), used = total - freeb;
    char b[96];

    gfx_rrect(x0, cy + 14, w, 84, 8, C_CARD, 255);
    gfx_text(x0 + 16, cy + 24, "Memory", C_TEXT, FONT_M);
    ksnprintf(b, sizeof b, "%lu MiB used of %lu MiB  (%lu%%)", (unsigned long)(used >> 20),
              (unsigned long)(total >> 20), (unsigned long)(total ? used * 100 / total : 0));
    gfx_text(x0 + 16, cy + 46, b, C_DIM, FONT_S);
    gfx_rrect(x0 + 16, cy + 68, w - 32, 12, 6, C_FIELD, 255);
    int fw = total ? (int)((uint64_t)(w - 32) * used / total) : 0;
    if (fw > 0) gfx_rrect(x0 + 16, cy + 68, imax(fw, 12), 12, 6, ACCENT, 255);

    int hw = (w - 12) / 2;
    gfx_rrect(x0, cy + 110, hw, 74, 8, C_CARD, 255);
    gfx_text(x0 + 16, cy + 120, "Display", C_TEXT, FONT_M);
    ksnprintf(b, sizeof b, "%d x %d, 32-bit", SW, SH);
    gfx_text(x0 + 16, cy + 146, b, C_DIM, FONT_S);
    gfx_text(x0 + 16, cy + 162, "Software renderer", C_DIM, FONT_S);

    gfx_rrect(x0 + hw + 12, cy + 110, hw, 74, 8, C_CARD, 255);
    gfx_text(x0 + hw + 28, cy + 120, "Uptime", C_TEXT, FONT_M);
    char up[16]; fmt_uptime(up, sizeof up);
    gfx_text(x0 + hw + 28, cy + 146, up, C_DIM, FONT_S);
    ksnprintf(b, sizeof b, "CPU timestamp: %lu MHz", (unsigned long)tsc_mhz());
    gfx_text(x0 + hw + 28, cy + 162, b, C_DIM, FONT_S);

    int gy = cy + 196, gh = ch - 196 - 16;
    gfx_rrect(x0, gy, w, gh, 8, C_CARD, 255);
    gfx_text(x0 + 16, gy + 10, "UI frames per second (last 60 s)", C_TEXT, FONT_S);
    int gx = x0 + 16, gw = w - 32, top = gy + 32, bh = gh - 46;
    gfx_rrect(gx, top, gw, bh, 4, C_FIELD, 255);
    int mx = 10;
    for (int i = 0; i < 60; i++) mx = imax(mx, fps_hist[i]);
    int bw = gw / 60;
    for (int i = 0; i < 60; i++) {
        int v = fps_hist[(fps_idx + i) % 60];
        int hh = v * (bh - 4) / mx;
        if (hh > 0) gfx_rect(gx + 2 + i * bw, top + bh - 2 - hh, imax(bw - 1, 1), hh, ACCENT, 255);
    }
}

/* ---- AuroraSettings ---- */
static const char *set_pages[3] = { "System", "Personalization", "About" };

static void kv(int x, int y, const char *k, const char *v) {
    gfx_text(x, y, k, C_DIM, FONT_S);
    gfx_text(x + 130, y, v, C_TEXT, FONT_S);
}

static void app_settings(int cx, int cy, int cw, int ch) {
    (void)cw; (void)ch;
    for (int i = 0; i < 3; i++) {
        int y = cy + 14 + i * 34;
        bool hv = ui_btn(cx + 10, y, 168, 30, ACT_SET_PAGE + i);
        if (i == settings_page) {
            gfx_rrect(cx + 10, y, 168, 30, 6, ACCENT, 46);
            gfx_rrect(cx + 10, y + 7, 3, 16, 1, ACCENT, 255);
        } else if (hv) gfx_rrect(cx + 10, y, 168, 30, 6, RGB(255, 255, 255), 16);
        gfx_text(cx + 26, y + 9, set_pages[i], C_TEXT, FONT_S);
    }
    gfx_rect(cx + 188, cy + 12, 1, ch - 24, RGB(255, 255, 255), 18);

    int px = cx + 208, py = cy + 16;
    char b[96];
    gfx_text(px, py, set_pages[settings_page], C_TEXT, FONT_L);
    py += 40;

    if (settings_page == 0) {
        ksnprintf(b, sizeof b, "%d x %d  (32-bit colour)", SW, SH); kv(px, py, "Display", b); py += 26;
        ksnprintf(b, sizeof b, "%lu MiB usable, %lu MiB free", (unsigned long)(pmm_total_bytes() >> 20),
                  (unsigned long)(pmm_free_bytes() >> 20));
        kv(px, py, "Memory", b); py += 26;
        ksnprintf(b, sizeof b, "x86-64, timestamp %lu MHz", (unsigned long)tsc_mhz()); kv(px, py, "Processor", b); py += 26;
        fmt_uptime(b, sizeof b); kv(px, py, "Uptime", b); py += 26;
        kv(px, py, "Time zone", "UTC (hardware clock)"); py += 26;
        kv(px, py, "Input", "PS/2 keyboard and mouse");
    } else if (settings_page == 1) {
        gfx_text(px, py, "Taskbar position", C_DIM, FONT_S);
        static const char *pos[3] = { "Bottom", "Left", "Right" };
        for (int i = 0; i < 3; i++) seg_btn(px + i * 94, py + 20, 88, 28, pos[i], cfg.tb_pos == i, ACT_SET_TBPOS + i);
        py += 62;
        gfx_text(px, py, "Taskbar icon size", C_DIM, FONT_S);
        static const char *isz[3] = { "Small  24", "Medium  32", "Large  40" };
        for (int i = 0; i < 3; i++) seg_btn(px + i * 94, py + 20, 88, 28, isz[i], cfg.icon == icon_sizes[i], ACT_SET_ICON + i);
        py += 62;
        gfx_text(px, py, "Accent colour", C_DIM, FONT_S);
        for (int i = 0; i < 5; i++) {
            int sx = px + i * 40, sy = py + 20;
            bool hv = ui_btn(sx, sy, 32, 32, ACT_SET_ACCENT + i);
            gfx_circle16((sx + 16) * 16, (sy + 16) * 16, (hv ? 15 : 13) * 16, accents[i], 255);
            if (cfg.accent == i) gfx_ring16((sx + 16) * 16, (sy + 16) * 16, 15 * 16, 24, C_TEXT, 255, false);
        }
        py += 64;
        gfx_text(px, py, "Pinned apps", C_DIM, FONT_S);
        gfx_text(px + 190, py, "Taskbar", C_DIM, FONT_S);
        gfx_text(px + 270, py, "Start", C_DIM, FONT_S);
        for (int a = 0; a < APP_COUNT; a++) {
            int y = py + 20 + a * 28;
            icon_draw(app_icon[a], px, y, 16, 0);
            gfx_text(px + 26, y + 2, app_label[a], C_TEXT, FONT_S);
            toggle(px + 184, y - 2, cfg.pin_tb[a], ACT_SET_PINTB + a);
            toggle(px + 262, y - 2, cfg.pin_start[a], ACT_SET_PINST + a);
        }
    } else {
        icon_draw(ICON_LOGO, px, py, 40, 0);
        gfx_text(px + 56, py + 2, "Aurora OS", C_TEXT, FONT_M);
        gfx_text(px + 56, py + 22, "Milestone 4 - version 0.4", C_DIM, FONT_S);
        py += 66;
        gfx_text(px, py, "A from-scratch x86-64 operating system.", C_TEXT, FONT_S); py += 20;
        gfx_text(px, py, "Kernel, drivers and desktop shell written in C.", C_DIM, FONT_S); py += 20;
        gfx_text(px, py, "Licensed under the MIT License.", C_DIM, FONT_S); py += 34;
        gfx_text(px, py, "Bootloader: Limine (BSD-2-Clause)", C_DIM, FONT_S); py += 20;
        gfx_text(px, py, "Font: Spleen by Frederic Cambus (BSD-2-Clause)", C_DIM, FONT_S);
    }
}

/* ======================================================================
 *  Window chrome
 * ====================================================================== */

static void draw_window(int a) {
    struct win *w = &wins[a];
    bool focused = (top_app() == a);
    cur_layer = a;
    gfx_clip_reset();
    gfx_shadow(w->x, w->y, w->w, w->h, 8);
    gfx_rrect(w->x, w->y, w->w, w->h, 8, C_WIN, 255);
    gfx_rrect(w->x, w->y, w->w, TITLE_H + 8, 8, C_TITLE, 255);
    gfx_rect(w->x, w->y + TITLE_H, w->w, 8, C_WIN, 255);
    gfx_rect(w->x + 1, w->y + TITLE_H - 1, w->w - 2, 1, RGB(255, 255, 255), 14);
    gfx_rrect_stroke(w->x, w->y, w->w, w->h, 8, focused ? ACCENT : C_BORDER, focused ? 210 : 255);

    hit_add(w->x, w->y, w->w, w->h, ACT_WIN_BODY + a);
    hit_add(w->x, w->y, w->w - 84, TITLE_H, ACT_WIN_TITLE + a);

    icon_draw(app_icon[a], w->x + 12, w->y + 8, 16, 0);
    gfx_text(w->x + 36, w->y + (TITLE_H - 12) / 2, app_title[a], focused ? C_TEXT : C_DIM, FONT_S);

    int bx = w->x + w->w - 40, by = w->y + 4;
    bool hc = ui_btn(bx, by, 32, 24, ACT_WIN_CLOSE + a);
    if (hc) gfx_rrect(bx, by, 32, 24, 6, RGB(232, 60, 72), 255);
    icon_draw(ICON_CLOSE, bx + 8, by + 4, 16, C_TEXT);
    bool hm = ui_btn(bx - 36, by, 32, 24, ACT_WIN_MIN + a);
    if (hm) gfx_rrect(bx - 36, by, 32, 24, 6, RGB(255, 255, 255), 24);
    icon_draw(ICON_MIN, bx - 36 + 8, by + 4, 16, C_TEXT);

    int cx = w->x + 1, cy = w->y + TITLE_H, cw = w->w - 2, ch = w->h - TITLE_H - 1;
    gfx_clip(cx, cy, cw, ch);
    switch (a) {
    case APP_FILES:    app_files(cx, cy, cw, ch); break;
    case APP_NOTES:    app_notes(cx, cy, cw, ch); break;
    case APP_MONITOR:  app_monitor(cx, cy, cw, ch); break;
    case APP_SETTINGS: app_settings(cx, cy, cw, ch); break;
    }
    gfx_clip_reset();
}

/* ======================================================================
 *  Desktop, taskbar, start menu
 * ====================================================================== */

static const int desk_apps[3] = { APP_FILES, APP_NOTES, APP_SETTINGS };
static const char *desk_labels[3] = { "This PC", "Notes", "Settings" };

static void draw_desktop(void) {
    cur_layer = LAYER_DESKTOP;
    hit_add(0, 0, SW, SH, ACT_DESK_BG);
    for (int i = 0; i < 3; i++) {
        int x = work_x + 12, y = work_y + 12 + i * 84;
        bool sel = desk_sel == i, hv = ui_btn(x, y, 76, 76, ACT_DESK_ICON + i);
        if (sel || hv) gfx_rrect(x, y, 76, 76, 6, RGB(255, 255, 255), sel ? 44 : 20);
        if (sel) gfx_rrect_stroke(x, y, 76, 76, 6, RGB(255, 255, 255), 70);
        icon_draw(app_icon[desk_apps[i]], x + 22, y + 8, 32, C_TEXT);
        int tw = gfx_text_w(desk_labels[i], FONT_S);
        gfx_text_a(x + (76 - tw) / 2 + 1, y + 49, desk_labels[i], RGB(0, 0, 0), 170, FONT_S);
        gfx_text(x + (76 - tw) / 2, y + 48, desk_labels[i], RGB(255, 255, 255), FONT_S);
    }
}

static void draw_taskbar(void) {
    cur_layer = LAYER_OVERLAY;
    gfx_clip_reset();
    /* Crystal-clear Aero glass: cool cyan tint, transparent body and specular edge. */
    gfx_rect(tb_x, tb_y, tb_w, tb_h, RGB(22, 91, 129), 142);
    if (cfg.tb_pos == TB_BOTTOM) {
        gfx_rect(tb_x, tb_y, tb_w, tb_h / 2, RGB(164, 232, 246), 48);
        gfx_rect(tb_x, tb_y, tb_w, 1, RGB(238, 253, 255), 190);
        gfx_rect(tb_x, tb_y + tb_h - 2, tb_w, 2, RGB(7, 49, 83), 115);
    } else if (cfg.tb_pos == TB_LEFT) {
        gfx_rect(tb_x, tb_y, tb_w, tb_h, RGB(64, 166, 203), 36);
        gfx_rect(tb_x + tb_w - 2, tb_y, 2, tb_h, RGB(232, 252, 255), 142);
        gfx_rect(tb_x, tb_y, 1, tb_h, RGB(255, 255, 255), 70);
    } else {
        gfx_rect(tb_x, tb_y, tb_w, tb_h, RGB(64, 166, 203), 36);
        gfx_rect(tb_x, tb_y, 2, tb_h, RGB(232, 252, 255), 142);
        gfx_rect(tb_x + tb_w - 1, tb_y, 1, tb_h, RGB(255, 255, 255), 70);
    }
    hit_add(tb_x, tb_y, tb_w, tb_h, ACT_TB_BG);

    int top = top_app();
    for (int i = 0; i < ntbi; i++) {
        struct tb_item *it = &tbi[i];
        bool hv = ui_btn(it->x, it->y, tb_slot, tb_slot, it->id);
        bool active = (it->id == ACT_TB_START && start_open) || (it->app >= 0 && it->app == top);
        if (active) {
            gfx_rrect(it->x, it->y, tb_slot, tb_slot, 10, RGB(221, 250, 255), 78);
            gfx_rrect_stroke(it->x, it->y, tb_slot, tb_slot, 10, RGB(255, 255, 255), 110);
        } else if (hv) {
            gfx_rrect(it->x, it->y, tb_slot, tb_slot, 10, RGB(231, 252, 255), 55);
            gfx_rrect_stroke(it->x, it->y, tb_slot, tb_slot, 10, RGB(255, 255, 255), 75);
        }
        icon_draw(it->icon, it->x + (tb_slot - cfg.icon) / 2, it->y + (tb_slot - cfg.icon) / 2,
                  cfg.icon, active ? RGB(12, 66, 91) : C_TEXT);
        if (it->app >= 0 && wins[it->app].open) {            /* running indicator */
            int len = active ? 16 : 6;
            uint32_t col = active ? RGB(199, 252, 255) : RGB(182, 238, 249);
            if (cfg.tb_pos == TB_BOTTOM)
                gfx_rrect(it->x + (tb_slot - len) / 2, it->y + tb_slot, len, 3, 1, col, 255);
            else if (cfg.tb_pos == TB_LEFT)
                gfx_rrect(tb_x + 1, it->y + (tb_slot - len) / 2, 3, len, 1, col, 255);
            else
                gfx_rrect(tb_x + tb_w - 4, it->y + (tb_slot - len) / 2, 3, len, 1, col, 255);
        }
    }

    /* clock / date tray + "show desktop" strip */
    char t[16], d[16];
    ksnprintf(t, sizeof t, "%02d:%02d", now.hour, now.minute);
    if (cfg.tb_pos == TB_BOTTOM) {
        ksnprintf(d, sizeof d, "%04d-%02d-%02d", now.year, now.month, now.day);
        /* A wider two-line tray: a prominent 24 px clock and readable date,
           both vertically centered in the taller taskbar without clipping. */
        const int tray_w = 150, tray_h = 50;
        int x = SW - 12 - tray_w;
        int y = tb_y + (tb_h - tray_h) / 2;
        bool hv = ui_btn(x, y, tray_w, tray_h, ACT_TB_BG);
        if (hv) gfx_rrect(x, y, tray_w, tray_h, 8, RGB(255, 255, 255), 20);
        gfx_text(x + (tray_w - gfx_text_w(t, FONT_L)) / 2, y + 1, t, C_TEXT, FONT_L);
        gfx_text(x + (tray_w - gfx_text_w(d, FONT_M)) / 2, y + 27, d, RGB(211, 239, 248), FONT_M);
        bool sd = ui_btn(SW - 6, tb_y, 6, tb_h, ACT_TB_SHOWDESK);
        gfx_rect(SW - 6, tb_y + 6, 1, tb_h - 12, RGB(255, 255, 255), sd ? 90 : 36);
    } else {
        ksnprintf(d, sizeof d, "%02d-%02d", now.month, now.day);
        int y = SH - 48;
        text_c(tb_x + tb_w / 2, y, t, C_TEXT, FONT_M);
        text_c(tb_x + tb_w / 2, y + 18, d, RGB(211, 239, 248), FONT_S);
        bool sd = ui_btn(tb_x, SH - 6, tb_w, 6, ACT_TB_SHOWDESK);
        gfx_rect(tb_x + 6, SH - 5, tb_w - 12, 1, RGB(255, 255, 255), sd ? 90 : 36);
    }
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static bool contains_ci(const char *hay, const char *needle) {
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (needle[j] && hay[i + j] && lower(hay[i + j]) == lower(needle[j])) j++;
        if (!needle[j]) return true;
    }
    return !needle[0];
}

static int search_matches(int *out) {
    int n = 0;
    for (int a = 0; a < APP_COUNT; a++)
        if (contains_ci(app_label[a], search) || contains_ci(app_title[a], search)) out[n++] = a;
    return n;
}

static void draw_start_menu(void) {
    cur_layer = LAYER_OVERLAY;
    gfx_clip_reset();
    int x, y, w, h;
    start_geom(&x, &y, &w, &h);
    gfx_shadow(x, y, w, h, 10);
    gfx_rrect(x, y, w, h, 10, RGB(33, 36, 50), 255);
    gfx_rrect_stroke(x, y, w, h, 10, RGB(255, 255, 255), 34);
    hit_add(x, y, w, h, ACT_SM_BODY);

    /* search box */
    bool sh = ui_btn(x + 20, y + 20, w - 40, 36, ACT_SM_SEARCH);
    gfx_rrect(x + 20, y + 20, w - 40, 36, 18, C_FIELD, 255);
    gfx_rrect_stroke(x + 20, y + 20, w - 40, 36, 18, ACCENT, sh || search_len ? 220 : 90);
    icon_draw(ICON_SEARCH, x + 34, y + 29, 18, C_DIM);
    if (search_len == 0) gfx_text(x + 62, y + 32, "Search apps", C_DIM, FONT_S);
    else {
        int tw = gfx_text(x + 62, y + 32, search, C_TEXT, FONT_S);
        if (blink_on) gfx_rect(x + 62 + tw + 1, y + 30, 1, 16, ACCENT, 255);
    }
    if (search_len == 0 && blink_on) gfx_rect(x + 61, y + 30, 1, 16, ACCENT, 255);

    int body_bottom = y + h - 60;
    if (search_len == 0) {
        gfx_text(x + 28, y + 76, "Pinned", C_TEXT, FONT_M);
        int shown = 0;
        for (int a = 0; a < APP_COUNT; a++) {
            if (!cfg.pin_start[a]) continue;
            int tx = x + 20 + (shown % 4) * 95, ty = y + 106 + (shown / 4) * 92;
            bool hv = ui_btn(tx, ty, 90, 84, ACT_SM_APP + a);
            if (hv) gfx_rrect(tx, ty, 90, 84, 8, RGB(255, 255, 255), 20);
            icon_draw(app_icon[a], tx + 33, ty + 12, 24, C_TEXT);
            text_c(tx + 45, ty + 52, app_label[a], C_TEXT, FONT_S);
            shown++;
        }
        if (!shown) gfx_text(x + 28, y + 110, "Nothing pinned. Use Settings > Personalization.", C_DIM, FONT_S);
        char info[64];
        ksnprintf(info, sizeof info, "Memory free: %lu MiB", (unsigned long)(pmm_free_bytes() >> 20));
        gfx_text(x + 28, body_bottom - 22, info, C_DIM, FONT_S);
    } else {
        int res[APP_COUNT], n = search_matches(res);
        gfx_text(x + 28, y + 76, "Best match", C_TEXT, FONT_M);
        for (int i = 0; i < n; i++) {
            int ry = y + 104 + i * 46;
            bool hv = ui_btn(x + 20, ry, w - 40, 42, ACT_SM_APP + res[i]);
            if (i == 0) gfx_rrect(x + 20, ry, w - 40, 42, 8, ACCENT, 40);
            else if (hv) gfx_rrect(x + 20, ry, w - 40, 42, 8, RGB(255, 255, 255), 20);
            icon_draw(app_icon[res[i]], x + 34, ry + 9, 24, C_TEXT);
            gfx_text(x + 72, ry + 7, app_label[res[i]], C_TEXT, FONT_S);
            gfx_text(x + 72, ry + 22, app_title[res[i]], C_DIM, FONT_S);
        }
        if (!n) gfx_text(x + 28, y + 108, "No apps match your search.", C_DIM, FONT_S);
    }

    /* user bar */
    gfx_rrect(x, y + h - 56, w, 56, 10, RGB(26, 28, 40), 255);
    gfx_rect(x, y + h - 56, w, 12, RGB(26, 28, 40), 255);
    gfx_rect(x + 1, y + h - 56, w - 2, 1, RGB(255, 255, 255), 18);
    gfx_circle16((x + 44) * 16, (y + h - 28) * 16, 16 * 16, ACCENT, 255);
    text_c(x + 44, y + h - 34, "A", C_DARKTX, FONT_M);
    gfx_text(x + 70, y + h - 34, "Aurora User", C_TEXT, FONT_S);
    int px = x + w - 56, py = y + h - 44;
    bool ph = ui_btn(px, py, 32, 32, ACT_SM_POWER);
    if (ph) gfx_rrect(px, py, 32, 32, 8, RGB(255, 255, 255), 24);
    icon_draw(ICON_POWER, px + 6, py + 6, 20, C_TEXT);
}

/* ======================================================================
 *  Boot splash and classic 2010s-inspired login screen
 * ====================================================================== */

static void draw_boot_screen(uint64_t elapsed) {
    gfx_clip_reset();
    gfx_frame_begin();
    gfx_rect(0, 0, SW, SH, RGB(3, 17, 43), 188);
    /* Soft glassy bands suggest an aurora without needing image assets. */
    gfx_rrect(SW / 8, SH / 5, SW * 3 / 4, 2, 1, RGB(132, 236, 248), 55);
    gfx_rrect(SW / 5, SH * 4 / 5, SW * 3 / 5, 1, 0, RGB(112, 229, 223), 48);

    int cx = SW / 2, cy = SH / 2;
    gfx_circle16(cx * 16, (cy - 60) * 16, 45 * 16, RGB(48, 206, 225), 38);
    gfx_circle16(cx * 16, (cy - 60) * 16, 35 * 16, RGB(29, 107, 154), 210);
    gfx_ring16(cx * 16, (cy - 60) * 16, 38 * 16, 2 * 16, RGB(202, 250, 255), 190, false);
    icon_draw(ICON_LOGO, cx - 23, cy - 83, 46, RGB(235, 253, 255));
    text_c(cx, cy - 4, "AURORA OS", RGB(241, 252, 255), FONT_L);
    text_c(cx, cy + 25, "A new horizon for your desktop", RGB(164, 211, 228), FONT_S);

    int bw = imin(300, SW - 80), bx = (SW - bw) / 2, by = cy + 83;
    gfx_rrect(bx, by, bw, 7, 4, RGB(8, 45, 76), 220);
    int progress = (int)(elapsed * 100 / 1700);
    if (progress > 94) progress = 94;
    if (progress < 3) progress = 3;
    int fill = (bw - 4) * progress / 100;
    gfx_rrect(bx + 2, by + 2, fill, 3, 2, RGB(121, 235, 245), 255);
    text_c(cx, by + 22, "Starting Aurora...", RGB(151, 197, 216), FONT_S);
}

static void draw_login_screen(void) {
    cur_layer = LAYER_OVERLAY;
    gfx_clip_reset();
    gfx_frame_begin();
    gfx_rect(0, 0, SW, SH, RGB(5, 26, 59), 146);
    gfx_rect(0, 0, SW, SH / 3, RGB(65, 159, 211), 28);
    gfx_rrect(SW / 10, SH / 7, SW * 4 / 5, 2, 1, RGB(218, 249, 255), 38);

    int pw = imin(480, SW - 40);
    int ph = imin(470, SH - 48);
    if (pw < 260) pw = SW - 16;
    if (ph < 330) ph = SH - 24;
    int px = (SW - pw) / 2, py = (SH - ph) / 2;
    int cx = SW / 2;

    /* Dark blue glass and silver-blue bevel evoke Windows 7 / early-2010s login. */
    gfx_shadow(px, py, pw, ph, 18);
    gfx_rrect(px, py, pw, ph, 18, RGB(13, 40, 72), 196);
    gfx_rrect(px + 2, py + 2, pw - 4, ph - 4, 16, RGB(70, 145, 185), 46);
    gfx_rrect_stroke(px, py, pw, ph, 18, RGB(207, 243, 255), 105);
    gfx_rrect(px + 9, py + 8, pw - 18, 4, 2, RGB(215, 250, 255), 122);

    int ay = py + 76;
    gfx_circle16(cx * 16, ay * 16, 43 * 16, RGB(170, 232, 246), 35);
    gfx_circle16(cx * 16, ay * 16, 35 * 16, RGB(171, 220, 238), 255);
    gfx_circle16(cx * 16, ay * 16, 31 * 16, RGB(37, 91, 130), 255);
    gfx_ring16(cx * 16, ay * 16, 35 * 16, 2 * 16, RGB(237, 251, 255), 220, false);
    text_c(cx, ay - 13, "A", RGB(247, 254, 255), FONT_L);
    text_c(cx, ay + 48, "Aurora User", RGB(248, 252, 255), FONT_L);
    text_c(cx, ay + 78, "Welcome back", RGB(179, 218, 237), FONT_S);

    int fw = imin(300, pw - 44), fx = cx - fw / 2, fy = py + 235;
    gfx_rrect(fx, fy, fw, 42, 5, RGB(7, 26, 49), 220);
    gfx_rrect_stroke(fx, fy, fw, 42, 5, RGB(162, 220, 241), 175);
    gfx_text(fx + 15, fy + 14, "Password", RGB(142, 179, 199), FONT_S);
    gfx_rrect(fx + fw - 39, fy + 5, 31, 32, 4, RGB(89, 170, 205), 220);
    icon_draw(ICON_CHEV_R, fx + fw - 32, fy + 11, 19, RGB(250, 254, 255));

    int bw = 132, bh = 38, bx = cx - bw / 2, by = fy + 54;
    bool hov_signin = ui_btn(bx, by, bw, bh, ACT_LOGIN_SIGNIN);
    gfx_rrect_grad(bx, by, bw, bh, 6,
                   hov_signin ? RGB(203, 250, 255) : RGB(154, 225, 242),
                   hov_signin ? RGB(104, 205, 234) : RGB(66, 161, 207));
    gfx_rrect_stroke(bx, by, bw, bh, 6, RGB(239, 254, 255), 180);
    text_c(cx, by + 11, "Sign in", RGB(8, 43, 66), FONT_M);
    text_c(cx, by + 106, "Press Enter or click Sign in", RGB(181, 218, 233), FONT_S);

    gfx_text(22, SH - 30, "AURORA OS", RGB(226, 247, 255), FONT_S);
    int rightw = gfx_text_w("PERSONAL SESSION", FONT_S);
    gfx_text(SW - rightw - 22, SH - 30, "PERSONAL SESSION", RGB(157, 202, 224), FONT_S);
}

/* ======================================================================
 *  Compose a frame
 * ====================================================================== */

static void compose(void) {
    nhits = 0;
    if (shell_screen == SCREEN_BOOT) {
        draw_boot_screen(uptime_ms() - screen_started);
        return;
    }
    if (shell_screen == SCREEN_LOGIN) {
        draw_login_screen();
        return;
    }
    layout_taskbar();
    int sx, sy, sw, sh;
    start_geom(&sx, &sy, &sw, &sh);
    over_overlay = in_rect(mouse_x, mouse_y, tb_x, tb_y, tb_w, tb_h) ||
                   (start_open && in_rect(mouse_x, mouse_y, sx, sy, sw, sh));
    top_at_mouse = win_at(mouse_x, mouse_y);

    gfx_frame_begin();
    draw_desktop();
    for (int i = 0; i < zcount; i++) {
        struct win *w = &wins[zorder[i]];
        if (w->open && !w->min) draw_window(zorder[i]);
    }
    draw_taskbar();
    if (start_open) draw_start_menu();
    frames++;
}

/* ======================================================================
 *  Actions and input
 * ====================================================================== */

static void close_start(void) { start_open = false; }

static void toggle_start(bool search_focus) {
    if (start_open && !search_focus) { start_open = false; return; }
    start_open = true; search_len = 0; search[0] = 0;
}

static void shutdown(void) {
    outw(0x604, 0x2000);       /* QEMU */
    outw(0xB004, 0x2000);      /* Bochs / old QEMU */
    outw(0x4004, 0x3400);      /* VirtualBox */
    gfx_clip_reset();
    gfx_frame_begin();
    gfx_rect(0, 0, SW, SH, RGB(0, 0, 0), 255);
    text_c(SW / 2, SH / 2 - 8, "It is now safe to turn off your computer.", C_TEXT, FONT_M);
    gfx_present();
    halt_forever();
}

static void raise_win(int a) { wins[a].min = false; z_push(a); }

static void do_action(int id, int mx, int my) {
    if (id == ACT_NONE) return;
    int a;
    if (id >= ACT_WIN_BODY && id < ACT_WIN_BODY + APP_COUNT) raise_win(id - ACT_WIN_BODY);
    else if (id >= ACT_WIN_TITLE && id < ACT_WIN_TITLE + APP_COUNT) {
        a = id - ACT_WIN_TITLE; raise_win(a);
        drag_app = a; drag_dx = mx - wins[a].x; drag_dy = my - wins[a].y;
    }
    else if (id >= ACT_WIN_CLOSE && id < ACT_WIN_CLOSE + APP_COUNT) close_app(id - ACT_WIN_CLOSE);
    else if (id >= ACT_WIN_MIN && id < ACT_WIN_MIN + APP_COUNT) wins[id - ACT_WIN_MIN].min = true;
    else if (id == ACT_TB_START) toggle_start(false);
    else if (id == ACT_TB_SEARCH) toggle_start(true);
    else if (id >= ACT_TB_APP && id < ACT_TB_APP + APP_COUNT) {
        a = id - ACT_TB_APP;
        if (!wins[a].open) open_app(a);
        else if (wins[a].min) { raise_win(a); start_open = false; }
        else if (top_app() == a) wins[a].min = true;
        else raise_win(a);
    }
    else if (id == ACT_TB_SHOWDESK) { for (a = 0; a < APP_COUNT; a++) wins[a].min = true; }
    else if (id >= ACT_SM_APP && id < ACT_SM_APP + APP_COUNT) open_app(id - ACT_SM_APP);
    else if (id == ACT_SM_POWER) shutdown();
    else if (id == ACT_DESK_BG) desk_sel = -1;
    else if (id >= ACT_DESK_ICON && id < ACT_DESK_ICON + 3) {
        int i = id - ACT_DESK_ICON; uint64_t t = uptime_ms();
        if (last_click_id == id && t - last_click_ms < 500) { open_app(desk_apps[i]); last_click_id = -1; }
        else { desk_sel = i; last_click_id = id; last_click_ms = t; }
    }
    else if (id >= ACT_SET_PAGE && id < ACT_SET_PAGE + 3) settings_page = id - ACT_SET_PAGE;
    else if (id >= ACT_SET_TBPOS && id < ACT_SET_TBPOS + 3) {
        cfg.tb_pos = id - ACT_SET_TBPOS; layout_taskbar();
        for (a = 0; a < APP_COUNT; a++) clamp_win(&wins[a]);
    }
    else if (id >= ACT_SET_ICON && id < ACT_SET_ICON + 3) cfg.icon = icon_sizes[id - ACT_SET_ICON];
    else if (id >= ACT_SET_ACCENT && id < ACT_SET_ACCENT + 5) cfg.accent = id - ACT_SET_ACCENT;
    else if (id >= ACT_SET_PINTB && id < ACT_SET_PINTB + APP_COUNT) cfg.pin_tb[id - ACT_SET_PINTB] ^= 1;
    else if (id >= ACT_SET_PINST && id < ACT_SET_PINST + APP_COUNT) cfg.pin_start[id - ACT_SET_PINST] ^= 1;
    else if (id >= ACT_FILES_NAV && id < ACT_FILES_NAV + 4) { files_nav = id - ACT_FILES_NAV; files_sel = -1; }
    else if (id >= ACT_FILES_ROW && id < ACT_FILES_ROW + 16) files_sel = id - ACT_FILES_ROW;
}

static void on_mouse_down(int mx, int my) {
    int id = hit_find(mx, my);
    if (shell_screen == SCREEN_LOGIN) {
        if (id == ACT_LOGIN_SIGNIN) {
            shell_screen = SCREEN_DESKTOP;
            dirty = true;
        }
        return;
    }
    if (shell_screen != SCREEN_DESKTOP) return;
    bool in_start = (id >= ACT_SM_APP && id <= ACT_SM_BODY) || id == ACT_TB_START || id == ACT_TB_SEARCH;
    if (start_open && !in_start) close_start();
    do_action(id, mx, my);
    dirty = true;
}

static void handle_mouse(void) {
    static int px = -1, py = -1;
    static uint8_t pb;
    int mx = mouse_x, my = mouse_y;
    uint8_t b = mouse_btn;
    if (mx != px || my != py) {
        if (drag_app >= 0 && (b & 1)) {
            struct win *w = &wins[drag_app];
            w->x = mx - drag_dx; w->y = my - drag_dy;
            clamp_win(w);
            dirty = true;
        } else {
            int id = hit_find(mx, my);
            if (id != hover_id) { hover_id = id; dirty = true; }
        }
    }
    if ((b & 1) && !(pb & 1)) on_mouse_down(mx, my);
    if (!(b & 1) && (pb & 1)) drag_app = -1;
    px = mx; py = my; pb = b;
}

static void handle_key(int k) {
    if (shell_screen == SCREEN_BOOT) return;
    if (shell_screen == SCREEN_LOGIN) {
        if (k == '\n' || k == ' ') {
            shell_screen = SCREEN_DESKTOP;
            dirty = true;
        }
        return;
    }
    if (k == KEY_SUPER) { toggle_start(false); return; }
    if (start_open) {
        if (k == 27) close_start();
        else if (k == '\b') { if (search_len) search[--search_len] = 0; }
        else if (k == '\n') {
            int res[APP_COUNT], n = search_matches(res);
            if (search_len && n) open_app(res[0]);
        }
        else if (k >= 32 && k < 127 && search_len < 30) { search[search_len++] = (char)k; search[search_len] = 0; }
        return;
    }
    if (top_app() == APP_NOTES) {
        if (k == '\b') { if (notes_len) notes_len--; }
        else if ((k == '\n' || (k >= 32 && k < 127)) && notes_len < (int)sizeof(notes) - 1) notes[notes_len++] = (char)k;
    }
}

void gui_run(struct limine_framebuffer *f) {
    if (!gfx_init(f)) return;
    SW = gfx_w(); SH = gfx_h();
    ps2_init(SW, SH);
    rtc_read(&now);
    notes_len = 0;
    shell_screen = SCREEN_BOOT;
    screen_started = uptime_ms();

    int cur_x = -1000, cur_y = -1000;
    uint64_t last_sec = uptime_ms() / 1000, last_blink = uptime_ms() / 500;
    uint64_t last_boot_frame = uptime_ms() / 50;

    for (;;) {
        bool got = ps2_poll();
        handle_mouse();
        int k;
        while ((k = ps2_getkey())) { handle_key(k); dirty = true; }

        uint64_t ms = uptime_ms();
        if (shell_screen == SCREEN_BOOT) {
            if (ms - screen_started >= 1800) {
                shell_screen = SCREEN_LOGIN;
                screen_started = ms;
                dirty = true;
            } else if (ms / 50 != last_boot_frame) {
                last_boot_frame = ms / 50;
                dirty = true;
            }
        }
        if (ms / 500 != last_blink) {
            last_blink = ms / 500; blink_on = last_blink & 1;
            if (start_open || top_app() == APP_NOTES) dirty = true;
        }
        if (ms / 1000 != last_sec) {
            last_sec = ms / 1000;
            int old_min = now.minute;
            rtc_read(&now);
            if (now.minute != old_min) dirty = true;
            fps_hist[fps_idx] = frames; fps_idx = (fps_idx + 1) % 60; frames = 0;
            if (wins[APP_MONITOR].open && !wins[APP_MONITOR].min) dirty = true;
            if (wins[APP_SETTINGS].open && !wins[APP_SETTINGS].min && settings_page == 0) dirty = true;
        }

        if (dirty) {
            dirty = false;
            compose();
            gfx_present();
            if (shell_screen != SCREEN_BOOT) gfx_draw_cursor(mouse_x, mouse_y);
            cur_x = mouse_x; cur_y = mouse_y;
        } else if (mouse_x != cur_x || mouse_y != cur_y) {
            gfx_restore(cur_x, cur_y, CURSOR_W, CURSOR_H);
            if (shell_screen != SCREEN_BOOT) gfx_draw_cursor(mouse_x, mouse_y);
            cur_x = mouse_x; cur_y = mouse_y;
        }
        if (!got) __asm__ volatile ("pause");
    }
}
