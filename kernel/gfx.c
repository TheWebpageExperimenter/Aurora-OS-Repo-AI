/*
 * Software 2D renderer: 32-bit XRGB back buffer + cached wallpaper, anti-aliased
 * rounded rects / circles / lines (integer math only, the kernel has no FPU/SSE),
 * alpha blending and Spleen bitmap fonts.
 */
#include "gfx.h"
#include "limine.h"
#include "pmm.h"
#include "font_spleen.h"

static struct limine_framebuffer *fb;
static uint32_t *back, *wall;
static int W, H;
static int cx0, cy0, cx1, cy1;           /* clip rect, x1/y1 exclusive */
static bool fast_fmt;                    /* screen is plain XRGB8888 */

int gfx_w(void) { return W; }
int gfx_h(void) { return H; }

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int iabs(int a) { return a < 0 ? -a : a; }

static inline uint32_t mix(uint32_t d, uint32_t s, uint32_t a) {
    if (a >= 255) return s;
    if (a == 0) return d;
    uint32_t ia = 255 - a;
    uint32_t r = (((d >> 16) & 255) * ia + ((s >> 16) & 255) * a + 127) / 255;
    uint32_t g = (((d >> 8) & 255) * ia + ((s >> 8) & 255) * a + 127) / 255;
    uint32_t b = ((d & 255) * ia + (s & 255) * a + 127) / 255;
    return (r << 16) | (g << 8) | b;
}

static inline void blend(int x, int y, uint32_t c, uint32_t a) {
    if (x < cx0 || y < cy0 || x >= cx1 || y >= cy1) return;
    uint32_t *p = &back[y * W + x];
    *p = mix(*p, c, a);
}

void gfx_pixel(int x, int y, uint32_t c, uint32_t a) { blend(x, y, c, a); }

/* ---------- clip ---------- */

void gfx_clip_reset(void) { cx0 = 0; cy0 = 0; cx1 = W; cy1 = H; }
void gfx_clip(int x, int y, int w, int h) {
    cx0 = imax(0, x); cy0 = imax(0, y);
    cx1 = imin(W, x + w); cy1 = imin(H, y + h);
    if (cx1 < cx0) cx1 = cx0;
    if (cy1 < cy0) cy1 = cy0;
}
void gfx_clip_get(int *x, int *y, int *w, int *h) {
    *x = cx0; *y = cy0; *w = cx1 - cx0; *h = cy1 - cy0;
}

/* ---------- presenting ---------- */

static inline uint32_t pack(uint32_t c) {
    return (((c >> 16) & 255) << fb->red_mask_shift)
         | (((c >> 8) & 255) << fb->green_mask_shift)
         | ((c & 255) << fb->blue_mask_shift);
}

static void blit_rect(int x, int y, int w, int h) {
    int x0 = imax(0, x), y0 = imax(0, y), x1 = imin(W, x + w), y1 = imin(H, y + h);
    if (x1 <= x0 || y1 <= y0) return;
    for (int j = y0; j < y1; j++) {
        uint32_t *src = back + j * W + x0;
        uint32_t *dst = (uint32_t *)((uint8_t *)fb->address + (uint64_t)j * fb->pitch) + x0;
        if (fast_fmt) memcpy(dst, src, (size_t)(x1 - x0) * 4);
        else for (int i = 0; i < x1 - x0; i++) dst[i] = pack(src[i]);
    }
}

void gfx_present(void) { blit_rect(0, 0, W, H); }
void gfx_restore(int x, int y, int w, int h) { blit_rect(x, y, w, h); }
void gfx_frame_begin(void) { memcpy(back, wall, (size_t)W * H * 4); gfx_clip_reset(); }

static const char *cursor_art[CURSOR_H] = {
    "X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ",
    "X....X      ", "X.....X     ", "X......X    ", "X.......X   ", "X........X  ",
    "X.....XXXXX ", "X..X..X     ", "X.X X..X    ", "XX  X..X    ", "X    X..X   ",
    "     X..X   ", "      X..X  ", "      X..X  ", "       XX   "
};

void gfx_draw_cursor(int x, int y) {
    uint32_t black = pack(RGB(10, 10, 14)), white = pack(RGB(255, 255, 255));
    for (int j = 0; j < CURSOR_H; j++) {
        if (y + j < 0 || y + j >= H) continue;
        uint32_t *row = (uint32_t *)((uint8_t *)fb->address + (uint64_t)(y + j) * fb->pitch);
        for (int i = 0; i < CURSOR_W; i++) {
            char ch = cursor_art[j][i];
            if (ch == ' ' || x + i < 0 || x + i >= W) continue;
            row[x + i] = ch == 'X' ? black : white;
        }
    }
}

/* ---------- filled shapes ---------- */

static void fill_span(int y, int xa, int xb, uint32_t c, uint32_t a) {
    if (y < cy0 || y >= cy1) return;
    xa = imax(xa, cx0); xb = imin(xb, cx1);
    uint32_t *p = back + y * W;
    if (a >= 255) { for (int x = xa; x < xb; x++) p[x] = c; }
    else if (a) { for (int x = xa; x < xb; x++) p[x] = mix(p[x], c, a); }
}

void gfx_rect(int x, int y, int w, int h, uint32_t c, uint32_t a) {
    for (int j = 0; j < h; j++) fill_span(y + j, x, x + w, c, a);
}

/* coverage (0..255) of pixel (i,j) inside a w*h rounded rect of radius r */
static int rrect_cov(int i, int j, int w, int h, int r) {
    if (i < 0 || j < 0 || i >= w || j >= h) return 0;
    int dx = 0, dy = 0;
    if (j < r) dy = r - j; else if (j >= h - r) dy = j - (h - r) + 1;
    if (i < r) dx = r - i; else if (i >= w - r) dx = i - (w - r) + 1;
    if (!dx || !dy) return 255;
    int ddx = 2 * dx - 1, ddy = 2 * dy - 1, R = 2 * r;
    int cv = 128 + ((R * R - (ddx * ddx + ddy * ddy)) * 255) / (4 * R);
    return cv < 0 ? 0 : cv > 255 ? 255 : cv;
}

static uint32_t lerp_col(uint32_t c0, uint32_t c1, int t) {      /* t 0..255 */
    return mix(c0, c1, (uint32_t)t);
}

static void rrect_core(int x, int y, int w, int h, int r, uint32_t c0, uint32_t c1,
                       uint32_t alpha, bool grad) {
    if (w <= 0 || h <= 0) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if (py < cy0 || py >= cy1) continue;
        bool edge_row = (j < r || j >= h - r);
        int ia = imax(0, cx0 - x), ib = imin(w, cx1 - x);
        for (int i = ia; i < ib; i++) {
            if (!edge_row && !grad) {            /* bulk fast path */
                int run_end = ib;
                fill_span(py, x + i, x + run_end, c0, alpha);
                break;
            }
            int cov = edge_row ? rrect_cov(i, j, w, h, r) : 255;
            if (i < r || i >= w - r) cov = rrect_cov(i, j, w, h, r);
            if (!cov) continue;
            uint32_t col = c0;
            if (grad) col = lerp_col(c0, c1, ((i * 255) / w + (j * 255) / h) / 2);
            blend(x + i, py, col, (uint32_t)cov * alpha / 255);
        }
    }
}

void gfx_rrect(int x, int y, int w, int h, int r, uint32_t c, uint32_t a) {
    rrect_core(x, y, w, h, r, c, c, a, false);
}
void gfx_rrect_grad(int x, int y, int w, int h, int r, uint32_t c0, uint32_t c1) {
    rrect_core(x, y, w, h, r, c0, c1, 255, true);
}

void gfx_rrect_stroke(int x, int y, int w, int h, int r, uint32_t c, uint32_t a) {
    if (w < 3 || h < 3) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if (py < cy0 || py >= cy1) continue;
        if (j <= r || j >= h - 1 - r) {
            for (int i = 0; i < w; i++) {
                int co = rrect_cov(i, j, w, h, r);
                int ci = rrect_cov(i - 1, j - 1, w - 2, h - 2, r > 0 ? r - 1 : 0);
                int cv = co - ci;
                if (cv > 0) blend(x + i, py, c, (uint32_t)cv * a / 255);
            }
        } else {
            blend(x, py, c, a);
            blend(x + w - 1, py, c, a);
        }
    }
}

void gfx_shadow(int x, int y, int w, int h, int r) {
    for (int k = 1; k <= 9; k++) {
        int d = 10 - k;
        uint32_t a = (uint32_t)(d * d * 26) / 100;       /* quadratic falloff */
        if (!a) continue;
        gfx_rrect_stroke(x - k, y - k + 5, w + 2 * k, h + 2 * k, r + k, RGB(0, 0, 0), a);
    }
}

/* ---------- sub-pixel circles / rings / lines ---------- */

static inline int cov_from(int64_t r2, int64_t d2, int64_t r16) {
    int64_t v = 128 + ((r2 - d2) * 255) / (2 * r16 * 16);
    return v < 0 ? 0 : v > 255 ? 255 : (int)v;
}

void gfx_circle16(int cx, int cy, int r, uint32_t c, uint32_t a) {
    if (r <= 0) return;
    int x0 = (cx - r) / 16 - 1, x1 = (cx + r) / 16 + 2;
    int y0 = (cy - r) / 16 - 1, y1 = (cy + r) / 16 + 2;
    int64_t r2 = (int64_t)r * r;
    for (int j = y0; j < y1; j++)
        for (int i = x0; i < x1; i++) {
            int64_t dx = i * 16 + 8 - cx, dy = j * 16 + 8 - cy;
            int cv = cov_from(r2, dx * dx + dy * dy, r);
            if (cv) blend(i, j, c, (uint32_t)cv * a / 255);
        }
}

void gfx_ring16(int cx, int cy, int r, int thick, uint32_t c, uint32_t a, bool gap_top) {
    int ro = r + thick / 2, ri = r - thick / 2;
    if (ri < 1) ri = 1;
    int x0 = (cx - ro) / 16 - 1, x1 = (cx + ro) / 16 + 2;
    int y0 = (cy - ro) / 16 - 1, y1 = (cy + ro) / 16 + 2;
    int64_t ro2 = (int64_t)ro * ro, ri2 = (int64_t)ri * ri;
    for (int j = y0; j < y1; j++)
        for (int i = x0; i < x1; i++) {
            int64_t dx = i * 16 + 8 - cx, dy = j * 16 + 8 - cy;
            int64_t d2 = dx * dx + dy * dy;
            if (gap_top && dy < 0 && iabs((int)dx) < thick) continue;
            int cv = cov_from(ro2, d2, ro) - cov_from(ri2, d2, ri);
            if (cv > 0) blend(i, j, c, (uint32_t)cv * a / 255);
        }
}

void gfx_line16(int ax, int ay, int bx, int by, int thick, uint32_t c, uint32_t a) {
    int64_t rr = thick / 2;
    if (rr < 1) rr = 1;
    int pad = (int)rr + 16;
    int x0 = (imin(ax, bx) - pad) / 16 - 1, x1 = (imax(ax, bx) + pad) / 16 + 2;
    int y0 = (imin(ay, by) - pad) / 16 - 1, y1 = (imax(ay, by) + pad) / 16 + 2;
    int64_t abx = bx - ax, aby = by - ay, den = abx * abx + aby * aby, r2 = rr * rr;
    for (int j = y0; j < y1; j++)
        for (int i = x0; i < x1; i++) {
            int64_t px = i * 16 + 8, py = j * 16 + 8;
            int64_t num = (px - ax) * abx + (py - ay) * aby, qx, qy;
            if (den == 0 || num <= 0) { qx = ax; qy = ay; }
            else if (num >= den)      { qx = bx; qy = by; }
            else { qx = ax + abx * num / den; qy = ay + aby * num / den; }
            int64_t dx = px - qx, dy = py - qy;
            int cv = cov_from(r2, dx * dx + dy * dy, rr);
            if (cv) blend(i, j, c, (uint32_t)cv * a / 255);
        }
}

/* ---------- text ---------- */

static const struct { int w, h, bpr; const uint8_t *data; } fonts[3] = {
    { 6, 12, 1, &spleen6x12[0][0] },
    { 8, 16, 1, &spleen8x16[0][0] },
    { 12, 24, 2, &spleen12x24[0][0] },
};

int gfx_font_h(int f) { return fonts[f].h; }
int gfx_font_w(int f) { return fonts[f].w; }
int gfx_text_w(const char *s, int f) { int n = 0; while (s[n]) n++; return n * fonts[f].w; }

static void glyph_a(int x, int y, char ch, uint32_t c, uint32_t a, int f) {
    if (ch < 32 || ch > 126) ch = '?';
    int fw = fonts[f].w, fh = fonts[f].h, bpr = fonts[f].bpr;
    if (x + fw <= cx0 || x >= cx1 || y + fh <= cy0 || y >= cy1) return;
    const uint8_t *g = fonts[f].data + (ch - 32) * fh * bpr;
    for (int j = 0; j < fh; j++) {
        uint32_t row = g[j * bpr];
        if (bpr == 2) row = (row << 8) | g[j * bpr + 1];
        int top = bpr * 8 - 1;
        for (int i = 0; i < fw; i++)
            if ((row >> (top - i)) & 1) blend(x + i, y + j, c, a);
    }
}

void gfx_glyph(int x, int y, char ch, uint32_t c, int f) { glyph_a(x, y, ch, c, 255, f); }

int gfx_text_a(int x, int y, const char *s, uint32_t c, uint32_t a, int f) {
    int x0 = x;
    for (; *s; s++) { glyph_a(x, y, *s, c, a, f); x += fonts[f].w; }
    return x - x0;
}
int gfx_text(int x, int y, const char *s, uint32_t c, int f) { return gfx_text_a(x, y, s, c, 255, f); }

/* ---------- wallpaper ---------- */

static uint32_t rng = 0x2545F491;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static int32_t dxt[4096];

static void blob(int cxp, int cyp, int rx, int ry, uint32_t col, int maxa) {
    int x0 = imax(0, cxp - rx), x1 = imin(W, cxp + rx);
    int y0 = imax(0, cyp - ry), y1 = imin(H, cyp + ry);
    for (int x = x0; x < x1; x++) {
        int64_t dx = x - cxp;
        dxt[x] = (int32_t)((dx * dx * 1024) / ((int64_t)rx * rx));
    }
    for (int y = y0; y < y1; y++) {
        int64_t dy = y - cyp;
        int dyt = (int)((dy * dy * 1024) / ((int64_t)ry * ry));
        for (int x = x0; x < x1; x++) {
            int d = dxt[x] + dyt;
            if (d >= 1024) continue;
            int t = 1024 - d;
            int f = (t * t) >> 10;
            uint32_t a = (uint32_t)(f * maxa) >> 10;
            wall[y * W + x] = mix(wall[y * W + x], col, a);
        }
    }
}

static void make_wallpaper(void) {
    for (int y = 0; y < H; y++) {
        uint32_t t = (uint32_t)(y * 255 / H);
        uint32_t c = mix(RGB(7, 10, 30), RGB(18, 62, 84), t);
        for (int x = 0; x < W; x++) wall[y * W + x] = c;
    }
    blob(W * 30 / 100, H * 30 / 100, W * 45 / 100, H * 20 / 100, RGB(60, 230, 160), 120);
    blob(W * 62 / 100, H * 22 / 100, W * 40 / 100, H * 17 / 100, RGB(110, 120, 255), 110);
    blob(W * 85 / 100, H * 38 / 100, W * 30 / 100, H * 24 / 100, RGB(190, 100, 240), 90);
    blob(W * 18 / 100, H * 85 / 100, W * 38 / 100, H * 20 / 100, RGB(40, 180, 200), 70);
    for (int i = 0; i < 220; i++) {                       /* stars */
        int x = (int)(rnd() % (uint32_t)W), y = (int)(rnd() % (uint32_t)(H * 6 / 10));
        uint32_t a = 70 + rnd() % 150;
        wall[y * W + x] = mix(wall[y * W + x], RGB(255, 255, 255), a);
    }
}

bool gfx_init(struct limine_framebuffer *f) {
    if (!f || f->bpp != 32 || f->width > 4096) return false;
    fb = f; W = (int)f->width; H = (int)f->height;
    uint64_t pages = ((uint64_t)W * H * 4 + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t pb = pmm_alloc_contiguous(pages), pw = pmm_alloc_contiguous(pages);
    if (!pb || !pw) return false;
    back = phys_to_virt(pb); wall = phys_to_virt(pw);
    fast_fmt = f->red_mask_shift == 16 && f->green_mask_shift == 8 && f->blue_mask_shift == 0;
    gfx_clip_reset();
    make_wallpaper();
    return true;
}
