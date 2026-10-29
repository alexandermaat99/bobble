#pragma once
// Strip-based drawing helpers shared by the screens. Every function draws into one strip
// buffer covering rows [y0, y0+h) and clips to it (and to the vertical clip set by gfx_clip).
#include <stdint.h>
#include <stdbool.h>

#ifdef BOARD_SQUARE                 // the 4" 480x480 square LCD (the "86 box" board)
#define W 480
#define H 480
#define SCREEN_ROUND 0
#else                               // the 1.75" 466x466 round AMOLED
#define W 466
#define H 466
#define SCREEN_ROUND 1
#endif
#define CX (W / 2)
#define CY (H / 2)

#define SWAP16(c) ((uint16_t)(((c) >> 8) | ((c) << 8)))
static inline uint16_t rgb(int r, int g, int b)
{
    r = r < 0 ? 0 : (r > 255 ? 255 : r);
    g = g < 0 ? 0 : (g > 255 ? 255 : g);
    b = b < 0 ? 0 : (b > 255 ? 255 : b);
    uint16_t c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    return SWAP16(c);   // panel takes big-endian RGB565
}

void gfx_clip(int top, int bottom);   // restrict drawing to rows [top, bottom); gfx_clip(0, H) resets
void gfx_get_clip(int *top, int *bottom);   // the current gfx_clip, to put back after a temporary one
void gfx_base_clip(int top, int bottom);   // outer clip that gfx_clip can't widen (used to draw a sliding card)
int text_width(const char *s, int sc);
void draw_text(uint16_t *buf, int y0, int h, int x, int y, int sc, const char *s, uint16_t col);
void text_c(uint16_t *buf, int y0, int h, int y, int sc, const char *s, uint16_t col);   // centred
void fill_rect(uint16_t *buf, int y0, int h, int x0, int ya, int x1, int yb, uint16_t col);
void fill_rrect(uint16_t *buf, int y0, int h, int x0, int ya, int x1, int yb, int r, uint16_t col);
void fill_circle(uint16_t *buf, int y0, int h, int cx, int cy, int r, uint16_t col);
void fill_tri(uint16_t *buf, int y0, int h, int ax, int ay, int bx, int by, int cx, int cy, uint16_t col);
// Anti-aliased shapes, blended over the strip (float coordinates: pixel centres are at +0.5)
void aa_disc(uint16_t *buf, int y0, int h, float cx, float cy, float r, uint16_t col);
void aa_rrect(uint16_t *buf, int y0, int h, float x0, float ya, float x1, float yb, float r, uint16_t col);
// an n x n picture (px, in the panel's byte order) at x, y with its corners rounded to r
void aa_image(uint16_t *buf, int y0, int h, int x, int y, int n, const uint16_t *px, float r);
// a line through n points (x, y pairs) w px wide, with round ends and joins; closed: back to the first point too
void aa_path(uint16_t *buf, int y0, int h, const float *pts, int n, bool closed, float w, uint16_t col);
// a line icon on a 24 px grid (n points, as for aa_path; at most 32), drawn size px across, centred on cx, cy
void aa_icon(uint16_t *buf, int y0, int h, float cx, float cy, float size, const float *g, int n, bool closed, float w,
             uint16_t col);

// Anti-aliased proportional text (Inter, generated into font_ui.c by tools/make_font.py). y is the top of the line.
typedef struct { uint32_t off; uint8_t w, h; int8_t x, y; uint16_t adv4; } glyph_t;   // adv4: advance in 1/4 px
typedef struct { const glyph_t *g; const uint8_t *bits; uint8_t line_h, ascent; } font_t;
extern const font_t font_big, font_body, font_label, font_small;
extern const font_t font_clock;
extern const font_t font_mono_s, font_mono, font_mono_l;   // (the Field look: Space Mono Bold, 13 / 17 / 24 px, font_mono.c)   // (digits and ":" only: the square's big clock, in font_clock.c)
int ftext_width(const font_t *f, const char *s);
int ftext_width_n(const font_t *f, const char *s, int n);   // just the first n characters
void draw_ftext(uint16_t *buf, int y0, int h, int x, int y, const font_t *f, const char *s, uint16_t col);
void draw_ftext_n(uint16_t *buf, int y0, int h, int x, int y, const font_t *f, const char *s, int n, uint16_t col);
