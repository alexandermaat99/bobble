// Strip-based drawing primitives and a 5x7 font (declared in gfx.h)
#include <string.h>
#include <math.h>
#include "gfx.h"

// classic 5x7 font, printable ASCII 0x20..0x7E, one byte per column, LSB at the top
static const uint8_t font5x7[95][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},{0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},{0x36,0x49,0x56,0x20,0x50},{0x00,0x08,0x07,0x03,0x00},
    {0x00,0x1C,0x22,0x41,0x00},{0x00,0x41,0x22,0x1C,0x00},{0x2A,0x1C,0x7F,0x1C,0x2A},{0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},{0x20,0x10,0x08,0x04,0x02},
    {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},{0x00,0x56,0x36,0x00,0x00},
    {0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},{0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3E},{0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},{0x3E,0x41,0x49,0x49,0x7A},
    {0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40},{0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},
    {0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},
    {0x63,0x14,0x08,0x14,0x63},{0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},{0x00,0x7F,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20},{0x00,0x41,0x41,0x7F,0x00},{0x04,0x02,0x01,0x02,0x04},{0x40,0x40,0x40,0x40,0x40},
    {0x00,0x01,0x02,0x04,0x00},{0x20,0x54,0x54,0x54,0x78},{0x7F,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},
    {0x38,0x44,0x44,0x48,0x7F},{0x38,0x54,0x54,0x54,0x18},{0x08,0x7E,0x09,0x01,0x02},{0x0C,0x52,0x52,0x52,0x3E},
    {0x7F,0x08,0x04,0x04,0x78},{0x00,0x44,0x7D,0x40,0x00},{0x20,0x40,0x44,0x3D,0x00},{0x7F,0x10,0x28,0x44,0x00},
    {0x00,0x41,0x7F,0x40,0x00},{0x7C,0x04,0x18,0x04,0x78},{0x7C,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},
    {0x7C,0x14,0x14,0x14,0x08},{0x08,0x14,0x14,0x18,0x7C},{0x7C,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20},{0x3C,0x40,0x40,0x20,0x7C},{0x1C,0x20,0x40,0x20,0x1C},{0x3C,0x40,0x30,0x40,0x3C},
    {0x44,0x28,0x10,0x28,0x44},{0x0C,0x50,0x50,0x50,0x3C},{0x44,0x64,0x54,0x4C,0x44},{0x00,0x08,0x36,0x41,0x00},
    {0x00,0x00,0x7F,0x00,0x00},{0x00,0x41,0x36,0x08,0x00},{0x08,0x04,0x08,0x10,0x08},
};

static int clip_top = 0, clip_bot = H;
static int base_top = -100000, base_bot = 100000;

void gfx_clip(int top, int bottom) { clip_top = top; clip_bot = bottom; }
void gfx_get_clip(int *top, int *bottom) { *top = clip_top; *bottom = clip_bot; }
void gfx_base_clip(int top, int bottom) { base_top = top; base_bot = bottom; }

int text_width(const char *s, int sc) { return (int)strlen(s) * 6 * sc - sc; }

// Visible rows of the strip [y0, y0+h) intersected with the clip
static inline int max3(int a, int b, int c) { int m = a > b ? a : b; return m > c ? m : c; }
static inline int min3(int a, int b, int c) { int m = a < b ? a : b; return m < c ? m : c; }
static inline int row_lo(int y0) { return max3(y0, clip_top, base_top); }
static inline int row_hi(int y0, int h) { return min3(y0 + h, clip_bot, base_bot); }

void draw_text(uint16_t *buf, int y0, int h, int x, int y, int sc, const char *s, uint16_t col)
{
    int lo = row_lo(y0), hi = row_hi(y0, h);
    if (y + 7 * sc <= lo || y >= hi) return;
    for (; *s; s++, x += 6 * sc) {
        unsigned char c = *s;
        if (c < 0x20 || c > 0x7E) c = '?';
        const uint8_t *g = font5x7[c - 0x20];
        for (int cx = 0; cx < 5; cx++)
            for (int cy = 0; cy < 7; cy++) {
                if (!(g[cx] & (1 << cy))) continue;
                for (int dy = 0; dy < sc; dy++) {
                    int py = y + cy * sc + dy;
                    if (py < lo || py >= hi) continue;
                    for (int dx = 0; dx < sc; dx++) {
                        int px = x + cx * sc + dx;
                        if (px >= 0 && px < W) buf[(py - y0) * W + px] = col;
                    }
                }
            }
    }
}

static const glyph_t *glyph(const font_t *f, unsigned char c) { return &f->g[(c < 0x20 || c > 0x7E ? '?' : c) - 0x20]; }

int ftext_width_n(const font_t *f, const char *s, int n)
{
    int w4 = 0;
    for (int i = 0; i < n && s[i]; i++) w4 += glyph(f, s[i])->adv4;
    return (w4 + 2) / 4;
}

int ftext_width(const font_t *f, const char *s) { return ftext_width_n(f, s, 1 << 30); }

// ---------------------------------------------------------------- anti-aliased shapes
static inline void blend_px(uint16_t *px, uint16_t col, float a)
{
    if (a <= 0) return;
    if (a >= 1) { *px = col; return; }
    uint16_t c = SWAP16(col), d = SWAP16(*px);
    int k = (int)(a * 256), dr = d >> 11, dg = (d >> 5) & 63, db = d & 31;
    dr += ((c >> 11) - dr) * k >> 8; dg += (((c >> 5) & 63) - dg) * k >> 8; db += ((c & 31) - db) * k >> 8;
    *px = SWAP16((uint16_t)((dr << 11) | (dg << 5) | db));
}

// shades the box [x0, x1) x [ya, yb) by coverage 0.5 - sdf(px, py) (sdf: signed distance in px, < 0 inside)
#define AA_FILL(X0, YA, X1, YB, SDF) do {                                                     \
    int lo = row_lo(y0), hi = row_hi(y0, h), ya_ = (int)floorf(YA), yb_ = (int)ceilf(YB);      \
    int xa_ = (int)floorf(X0), xb_ = (int)ceilf(X1);                                           \
    if (ya_ < lo) ya_ = lo;                                                                    \
    if (yb_ > hi) yb_ = hi;                                                                    \
    if (xa_ < 0) xa_ = 0;                                                                      \
    if (xb_ > W) xb_ = W;                                                                      \
    for (int iy = ya_; iy < yb_; iy++) {                                                       \
        uint16_t *row = buf + (iy - y0) * W;                                                   \
        float py = iy + 0.5f;                                                                  \
        for (int ix = xa_; ix < xb_; ix++) {                                                   \
            float px = ix + 0.5f;                                                              \
            blend_px(row + ix, col, 0.5f - (SDF));                                             \
        }                                                                                      \
    }                                                                                          \
} while (0)

void aa_disc(uint16_t *buf, int y0, int h, float cx, float cy, float r, uint16_t col)
{
    AA_FILL(cx - r - 1, cy - r - 1, cx + r + 1, cy + r + 1, sqrtf((px - cx) * (px - cx) + (py - cy) * (py - cy)) - r);
}

static inline float rrect_sdf(float px, float py, float x0, float ya, float x1, float yb, float r)
{
    float hx = (x1 - x0) / 2 - r, hy = (yb - ya) / 2 - r;
    float qx = fabsf(px - (x0 + x1) / 2) - hx, qy = fabsf(py - (ya + yb) / 2) - hy;
    float ox = fmaxf(qx, 0), oy = fmaxf(qy, 0);
    return sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0) - r;
}

void aa_rrect(uint16_t *buf, int y0, int h, float x0, float ya, float x1, float yb, float r, uint16_t col)
{
    AA_FILL(x0 - 1, ya - 1, x1 + 1, yb + 1, rrect_sdf(px, py, x0, ya, x1, yb, r));
}

static inline float seg_dist(float px, float py, float ax, float ay, float bx, float by)
{
    float dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
    float t = l2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / l2 : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    float ex = px - ax - t * dx, ey = py - ay - t * dy;
    return sqrtf(ex * ex + ey * ey);
}

static float path_sdf(float px, float py, const float *p, int n, bool closed, float w)
{
    float d = 1e9f;
    for (int i = 0; i + 1 < n + (closed ? 1 : 0); i++) {
        int j = (i + 1) % n;
        d = fminf(d, seg_dist(px, py, p[2 * i], p[2 * i + 1], p[2 * j], p[2 * j + 1]));
    }
    return d - w / 2;
}

void aa_path(uint16_t *buf, int y0, int h, const float *pts, int n, bool closed, float w, uint16_t col)
{
    float x0 = 1e9f, ya = 1e9f, x1 = -1e9f, yb = -1e9f;
    for (int i = 0; i < n; i++) {
        x0 = fminf(x0, pts[2 * i]); x1 = fmaxf(x1, pts[2 * i]);
        ya = fminf(ya, pts[2 * i + 1]); yb = fmaxf(yb, pts[2 * i + 1]);
    }
    float m = w / 2 + 1;
    AA_FILL(x0 - m, ya - m, x1 + m, yb + m, path_sdf(px, py, pts, n, closed, w));
}

void aa_icon(uint16_t *buf, int y0, int h, float cx, float cy, float size, const float *g, int n, bool closed, float w,
             uint16_t col)
{
    float k = size / 24, p[64];
    if (n > 32) n = 32;
    for (int i = 0; i < n; i++) {
        p[2 * i] = cx + (g[2 * i] - 12) * k;
        p[2 * i + 1] = cy + (g[2 * i + 1] - 12) * k;
    }
    aa_path(buf, y0, h, p, n, closed, w, col);
}

// Each glyph's coverage blends the colour over what's already in the strip (the bubble, or black)
void draw_ftext_n(uint16_t *buf, int y0, int h, int x, int y, const font_t *f, const char *s, int n, uint16_t col)
{
    int lo = row_lo(y0), hi = row_hi(y0, h);
    if (y + f->line_h <= lo || y >= hi) return;
    uint16_t c = SWAP16(col);
    int cr = c >> 11, cg = (c >> 5) & 63, cb = c & 31, x4 = x * 4;
    for (int i = 0; i < n && s[i]; i++) {
        const glyph_t *g = glyph(f, s[i]);
        int gx = (x4 + 2) / 4 + g->x;
        for (int r = 0; r < g->h; r++) {
            int py = y + g->y + r;
            if (py < lo || py >= hi) continue;
            uint16_t *row = buf + (py - y0) * W;
            for (int q = 0; q < g->w; q++) {
                int px = gx + q, k = r * g->w + q;
                if (px < 0 || px >= W) continue;
                int a = (f->bits[g->off + k / 2] >> (k & 1 ? 0 : 4)) & 15;
                if (!a) continue;
                if (a == 15) { row[px] = col; continue; }
                uint16_t d = SWAP16(row[px]);
                int dr = d >> 11, dg = (d >> 5) & 63, db = d & 31;
                dr += (cr - dr) * a / 15; dg += (cg - dg) * a / 15; db += (cb - db) * a / 15;
                row[px] = SWAP16((uint16_t)((dr << 11) | (dg << 5) | db));
            }
        }
        x4 += g->adv4;
    }
}

void draw_ftext(uint16_t *buf, int y0, int h, int x, int y, const font_t *f, const char *s, uint16_t col)
{
    draw_ftext_n(buf, y0, h, x, y, f, s, 1 << 30, col);
}

void text_c(uint16_t *buf, int y0, int h, int y, int sc, const char *s, uint16_t col)
{
    draw_text(buf, y0, h, CX - text_width(s, sc) / 2, y, sc, s, col);
}

void fill_rect(uint16_t *buf, int y0, int h, int x0, int ya, int x1, int yb, uint16_t col)
{
    int lo = row_lo(y0), hi = row_hi(y0, h);
    if (ya < lo) ya = lo;
    if (yb > hi) yb = hi;
    if (x0 < 0) x0 = 0;
    if (x1 > W) x1 = W;
    for (int y = ya; y < yb; y++)
        for (int x = x0; x < x1; x++) buf[(y - y0) * W + x] = col;
}

void fill_rrect(uint16_t *buf, int y0, int h, int x0, int ya, int x1, int yb, int r, uint16_t col)
{
    int lo = row_lo(y0), hi = row_hi(y0, h);
    for (int y = ya > lo ? ya : lo; y < yb && y < hi; y++) {
        int d = y - ya < yb - 1 - y ? y - ya : yb - 1 - y;    // distance to the nearest horizontal edge
        int inset = 0;
        if (d < r) inset = r - (int)sqrtf((float)(r * r - (r - d) * (r - d)));
        int a = x0 + inset < 0 ? 0 : x0 + inset, b = x1 - inset > W ? W : x1 - inset;
        for (int x = a; x < b; x++) buf[(y - y0) * W + x] = col;
    }
}

void fill_circle(uint16_t *buf, int y0, int h, int cx, int cy, int r, uint16_t col)
{
    int lo = row_lo(y0), hi = row_hi(y0, h);
    for (int y = (cy - r > lo ? cy - r : lo); y <= cy + r && y < hi; y++)
        for (int x = cx - r; x <= cx + r; x++)
            if (x >= 0 && x < W && (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r) buf[(y - y0) * W + x] = col;
}

void fill_tri(uint16_t *buf, int y0, int h, int ax, int ay, int bx, int by, int cx, int cy, uint16_t col)
{
    int lo = row_lo(y0), hi = row_hi(y0, h);
    int miny = ay < by ? (ay < cy ? ay : cy) : (by < cy ? by : cy), maxy = ay > by ? (ay > cy ? ay : cy) : (by > cy ? by : cy);
    int minx = ax < bx ? (ax < cx ? ax : cx) : (bx < cx ? bx : cx), maxx = ax > bx ? (ax > cx ? ax : cx) : (bx > cx ? bx : cx);
    if (miny < lo) miny = lo;
    if (maxy >= hi) maxy = hi - 1;
    if (minx < 0) minx = 0;
    if (maxx >= W) maxx = W - 1;
    for (int y = miny; y <= maxy; y++)
        for (int x = minx; x <= maxx; x++) {
            int e0 = (bx - ax) * (y - ay) - (by - ay) * (x - ax);
            int e1 = (cx - bx) * (y - by) - (cy - by) * (x - bx);
            int e2 = (ax - cx) * (y - cy) - (ay - cy) * (x - cx);
            if ((e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0)) buf[(y - y0) * W + x] = col;
        }
}
