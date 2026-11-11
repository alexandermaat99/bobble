// Now Playing screen: album art fills the round display (dimmed), a progress ring runs around the
// edge, with the track, controls, and the playing device's volume on top. The square: the cover as a tile at the
// top, the track under it, the times and a progress bar, and three round buttons.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "esp_timer.h"
#include "gfx.h"
#include "spotify.h"
#include "spotify_ui.h"
#include "spotify_logo.h"
#include "screen.h"
#include "tiles.h"

#define C_WHITE  rgb(255, 255, 255)
#define C_GREY   rgb(170, 170, 185)
#define C_DIM    rgb(90, 90, 105)
#define C_GREEN  rgb(30, 215, 96)          // Spotify green
#define C_PANEL  rgb(30, 30, 44)
#define C_PRESS  rgb(0, 110, 70)
#define C_NOTE   rgb(255, 150, 60)

#define RING_IN 220
#define RING_OUT 229

enum { ID_NONE, ID_PREV, ID_PLAY, ID_NEXT, ID_VDOWN, ID_VUP, ID_VOL };   // ID_VOL: the volume (slider, tile or knob)
typedef struct { int x0, y0, x1, y1, id; } btn_t;
#define CTRL_Y 442
#define BAR_Y 398
#define BAR_X0 36
#define BAR_X1 (W - 36)
#define C_BTN  rgb(64, 64, 70)
#define C_TRACK rgb(55, 55, 60)
static const btn_t btns[] = {
    {CX - 80 - 26, CTRL_Y - 26, CX - 80 + 26, CTRL_Y + 26, ID_PREV},
    {CX - 34, CTRL_Y - 34, CX + 34, CTRL_Y + 34, ID_PLAY},
    {CX + 80 - 26, CTRL_Y - 26, CX + 80 + 26, CTRL_Y + 26, ID_NEXT},
};
#define NBTN 3
// The Field look's: the times either side of the progress (a tape counter), which leaves room for the buttons to sit
// clear of both the progress and the bottom edge
#define F_BAR_Y  372                               // the progress row's middle
#define F_CTRL_Y 428
#define F_KNOB_X (W - 58)                          // the volume knob, in the corner right of the keys
#define F_KNOB_R 20
static const btn_t fbtns[] = {
    {CX - 84 - 24, F_CTRL_Y - 24, CX - 84 + 24, F_CTRL_Y + 24, ID_PREV},
    {CX - 30, F_CTRL_Y - 30, CX + 30, F_CTRL_Y + 30, ID_PLAY},
    {CX + 84 - 24, F_CTRL_Y - 24, CX + 84 + 24, F_CTRL_Y + 24, ID_NEXT},
};
// The Tiles look's: the controls in a row of tiles along the bottom, the times at its ends
#define T_ROW_Y0 392
#define T_ROW_Y1 466
static const btn_t tbtns[] = {
    {CX - 116, T_ROW_Y0 + 4, CX - 50, T_ROW_Y1 - 4, ID_PREV},
    {CX - 40, T_ROW_Y0, CX + 40, T_ROW_Y1, ID_PLAY},
    {CX + 50, T_ROW_Y0 + 4, CX + 116, T_ROW_Y1 - 4, ID_NEXT},
};

static spotify_state_t s;
static float progress;                 // 0..1 for the ring
static bool down;
static int press_id, press_x, press_y;
// The volume (Classic's slider, the Tiles tile, the Field knob), dragged up and down: the volume as the finger has it,
// until Spotify's catches up. A tap mutes it, and another brings it back.
static int drag_vol = -1, drag_from, drag_moved;   // drag_moved: the furthest the finger's gone up or down
static int64_t drag_until;             // (shown this long after letting go)
static int unmute_vol = -1;            // muted by a tap: the volume a second tap brings back
// Classic's volume: a slider like the control panel's, beside the cover
#define C_VOL_X0 (ART_X + ART_SZ + 18)
#define C_VOL_X1 (C_VOL_X0 + 54)
static int shown_volume(void) { return drag_vol >= 0 ? drag_vol : s.volume; }

// Spotify titles can contain non-ASCII; keep what the font can show
static void ascii(char *s)
{
    char *o = s;
    for (unsigned char *p = (unsigned char *)s; *p; p++) {
        if (*p == 0xE2 && p[1] == 0x80 && p[2]) {
            char r = p[2] == 0x98 || p[2] == 0x99 ? '\'' : p[2] == 0x9C || p[2] == 0x9D ? '"' : p[2] == 0x93 || p[2] == 0x94 ? '-' : 0;
            if (r) { *o++ = r; p += 2; continue; }
        }
        if (*p >= 0x20 && *p < 0x7F) *o++ = *p;
    }
    *o = 0;
}

// ---------------------------------------------------------------- background: the art
void spotify_ui_render_bg(uint16_t *buf, int y0, int y1)
{
    uint32_t ver;
    const uint16_t *art = s.active ? spotify_art(&ver) : NULL;
    if (art) memcpy(buf, art + y0 * W, (y1 - y0) * W * 2);
    else memset(buf, 0, (y1 - y0) * W * 2);
}

// ---------------------------------------------------------------- overlay
static bool pressed(int id) { return down && press_id == id; }


// s cut with ".." to fit max_w px in font f
static void fitw(char *out, size_t cap, const char *in, const font_t *f, int max_w)
{
    strlcpy(out, in, cap);
    int n = strlen(out);
    if (ftext_width(f, out) <= max_w) return;
    while (n > 0 && ftext_width_n(f, out, n) + ftext_width(f, "..") > max_w) n--;
    while (n > 0 && out[n - 1] == ' ') n--;
    strlcpy(out + n, "..", cap - n);
}

static void ftext_cx(uint16_t *buf, int y0, int h, int cx, int y, const font_t *f, const char *str, uint16_t col)
{
    draw_ftext(buf, y0, h, cx - ftext_width(f, str) / 2, y, f, str, col);
}

static void ftext_c(uint16_t *buf, int y0, int h, int y, const font_t *f, const char *str, uint16_t col)
{
    ftext_cx(buf, y0, h, CX, y, f, str, col);
}

static void fmt_time(char *out, size_t cap, int ms)
{
    int sec = ms / 1000;
    snprintf(out, cap, "%d:%02d", sec / 60, sec % 60);
}

// skip back / forward: a triangle with a bar at its tip
static void draw_skip(uint16_t *buf, int y0, int h, float cx, float cy, bool next, uint16_t c)
{
    static const float back[] = {19, 20, 9, 12, 19, 4}, back_bar[] = {5, 19, 5, 5};
    static const float fwd[] = {5, 4, 15, 12, 5, 20}, fwd_bar[] = {19, 5, 19, 19};
    aa_icon(buf, y0, h, cx, cy, 20, next ? fwd : back, 3, true, 2, c);
    aa_icon(buf, y0, h, cx, cy, 20, next ? fwd_bar : back_bar, 2, false, 2, c);
}

// Not connected, or nothing playing: the Spotify logo (main/spotify_logo.h, its own SVG rasterised) and what to do
#define LOGO_Y 168
#define LOGO_R (SPOTIFY_LOGO_SIZE / 2)
static void draw_logo(uint16_t *buf, int y0, int h)
{
    uint16_t g = SWAP16(C_GREEN);                       // the mask in Spotify green, over the black
    int gr = g >> 11, gg = (g >> 5) & 63, gb = g & 31, ox = CX - LOGO_R, oy = LOGO_Y - LOGO_R;
    for (int y = 0; y < SPOTIFY_LOGO_SIZE; y++) {
        if (oy + y < y0 || oy + y >= y0 + h) continue;
        uint16_t *row = buf + (oy + y - y0) * W + ox;
        for (int x = 0; x < SPOTIFY_LOGO_SIZE; x++) {
            int q = y * SPOTIFY_LOGO_SIZE + x, a = (spotify_logo[q / 2] >> (q & 1 ? 0 : 4)) & 15;
            if (!a) continue;
            uint16_t d = SWAP16(row[x]);
            int dr = d >> 11, dg = (d >> 5) & 63, db = d & 31;
            dr += (gr - dr) * a / 15; dg += (gg - dg) * a / 15; db += (gb - db) * a / 15;
            row[x] = SWAP16((uint16_t)((dr << 11) | (dg << 5) | db));
        }
    }
}

static void draw_square_idle(uint16_t *buf, int y0, int h)
{
    draw_logo(buf, y0, h);
    ftext_c(buf, y0, h, LOGO_Y + LOGO_R + 22, &font_body, "Spotify", C_WHITE);
    ftext_c(buf, y0, h, LOGO_Y + LOGO_R + 76, &font_label, s.configured ? "Nothing playing" : "Not connected", rgb(150, 150, 158));
    ftext_c(buf, y0, h, LOGO_Y + LOGO_R + 106, &font_small,
            s.configured ? "Play something on any device" : "Connect it from the Bobble menu on your Mac", rgb(100, 100, 108));
    if (s.note[0]) ftext_c(buf, y0, h, LOGO_Y + LOGO_R + 152, &font_small, s.note, C_NOTE);
}

static void tiles_speaker(uint16_t *buf, int y0, int h, float cx, float cy, int waves, uint16_t c);

static void draw_square(uint16_t *buf, int y0, int h)
{
    char line[96], l1[96];
    if (!spotify_art(&(uint32_t){0}))                   // cover still loading: a grey tile where it goes
        fill_rrect(buf, y0, h, ART_X, ART_Y, ART_X + ART_SZ, ART_Y + ART_SZ, 18, rgb(28, 28, 32));

    // the volume: a slider like the control panel's, lit from the bottom (drag it; a tap mutes)
    int vol = shown_volume();
    if (vol >= 0) {
        const uint16_t mod = pressed(ID_VOL) ? rgb(58, 58, 64) : rgb(36, 36, 40);
        aa_rrect(buf, y0, h, C_VOL_X0, ART_Y, C_VOL_X1, ART_Y + ART_SZ, 22, mod);
        float fy = ART_Y + ART_SZ - ART_SZ * (vol > 100 ? 100 : vol) / 100.0f, vy = ART_Y + ART_SZ - 30;
        if (ART_Y + ART_SZ - fy >= 2) {
            int ct, cb;
            gfx_get_clip(&ct, &cb);
            gfx_clip((int)fy > ct ? (int)fy : ct, cb);
            aa_rrect(buf, y0, h, C_VOL_X0, ART_Y, C_VOL_X1, ART_Y + ART_SZ, 22, rgb(228, 228, 234));
            gfx_clip(ct, cb);
        }
        tiles_speaker(buf, y0, h, (C_VOL_X0 + C_VOL_X1) / 2.0f - 4, vy, vol == 0 ? -1 : vol < 50 ? 1 : 2,
                      vy - 12 > fy ? rgb(36, 36, 40) : C_WHITE);
    }

    strlcpy(line, s.title, sizeof line);
    ascii(line);
    fitw(l1, sizeof l1, line, &font_label, W - 72);
    ftext_c(buf, y0, h, ART_Y + ART_SZ + 14, &font_label, l1, C_WHITE);
    strlcpy(line, s.artist, sizeof line);
    ascii(line);
    fitw(l1, sizeof l1, line, &font_small, W - 72);
    ftext_c(buf, y0, h, ART_Y + ART_SZ + 42, &font_small, l1, rgb(150, 150, 158));

    // times and the progress bar
    if (s.note[0]) ftext_c(buf, y0, h, BAR_Y - 26, &font_small, s.note, C_NOTE);
    if (s.duration_ms > 0) {
        if (!s.note[0]) {
            fmt_time(line, sizeof line, (int)(progress * s.duration_ms));
            draw_ftext(buf, y0, h, BAR_X0, BAR_Y - 26, &font_small, line, rgb(150, 150, 158));
            fmt_time(line, sizeof line, s.duration_ms);
            draw_ftext(buf, y0, h, BAR_X1 - ftext_width(&font_small, line), BAR_Y - 26, &font_small, line, rgb(150, 150, 158));
        }
        int px = BAR_X0 + (int)(progress * (BAR_X1 - BAR_X0));
        aa_rrect(buf, y0, h, BAR_X0, BAR_Y, BAR_X1, BAR_Y + 4, 2, C_TRACK);
        if (px > BAR_X0 + 3) aa_rrect(buf, y0, h, BAR_X0, BAR_Y, px, BAR_Y + 4, 2, C_WHITE);
    }

    // controls: grey discs either side of a white one
    for (int i = 0; i < 3; i += 2) {
        float cx = (btns[i].x0 + btns[i].x1) / 2.0f;
        aa_disc(buf, y0, h, cx, CTRL_Y, 21, pressed(btns[i].id) ? C_PRESS : C_BTN);
        if (s.episode) {                                // podcasts: 10 s back / forward
            ftext_cx(buf, y0, h, (int)cx, CTRL_Y - 10, &font_small, "10", C_WHITE);
        } else {
            draw_skip(buf, y0, h, cx, CTRL_Y, i == 2, C_WHITE);
        }
    }
    uint16_t ic = pressed(ID_PLAY) ? C_WHITE : rgb(10, 10, 14);
    aa_disc(buf, y0, h, CX, CTRL_Y, 29, pressed(ID_PLAY) ? C_PRESS : C_WHITE);
    if (s.playing) {                                    // pause: two bars
        static const float l[] = {9, 5, 9, 19}, r[] = {15, 5, 15, 19};
        aa_icon(buf, y0, h, CX, CTRL_Y, 26, l, 2, false, 2, ic);
        aa_icon(buf, y0, h, CX, CTRL_Y, 26, r, 2, false, 2, ic);
    } else {                                            // play (nudged right to look centred)
        static const float tri[] = {7, 4, 19, 12, 7, 20};
        aa_icon(buf, y0, h, CX + 1.5f, CTRL_Y, 26, tri, 3, true, 2, ic);
    }
}

// ---------------------------------------------------------------- the Field look (Settings > Look)
// Teenage Engineering-ish, like the calendar's: the cover where it always is, two tape reels either side of it (the
// left one empties as the song plays, the right one fills, and they step round once a second while it plays), the
// artist in orange capitals, the progress as a row of lit segments, and square buttons with an orange play.
#define F_ORANGE rgb(255, 102, 26)
#define F_GREY   rgb(118, 118, 126)
#define F_DIM    rgb(56, 56, 62)
#define F_PANEL  rgb(20, 20, 23)
#define REEL_Y   (ART_Y + ART_SZ / 2)
#define REEL_R   40
#define REEL_TAPE_S 32.0f                       // how fast the tape runs (px a second at the edge of the tape wound on)
#define REEL_US  50000                          // the reels turn 20 times a second, and only their boxes are redrawn

static float reel_a[2];                         // the reels' angles (radians), turned while it plays
static bool reels_moved;

static float reel_tape(float fill) { return 14 + (REEL_R - 18) * (fill < 0 ? 0 : fill > 1 ? 1 : fill); }   // its radius

static void upper(char *t) { for (; *t; t++) if (*t >= 'a' && *t <= 'z') *t -= 32; }

static void field_grid(uint16_t *buf, int y0, int h)   // the dot grid, round the cover
{
    for (int y = y0 - y0 % 24 + 12; y < y0 + h; y += 24) {
        if (y < y0) continue;
        for (int x = 12; x < W; x += 24) {
            if (x > ART_X - 4 && x < ART_X + ART_SZ + 4 && y > ART_Y - 4 && y < ART_Y + ART_SZ + 4) continue;
            buf[(y - y0) * W + x] = rgb(34, 34, 38);
        }
    }
}

// a reel: its rim, the tape wound on it (fill 0..1), and the hub with three notches at angle a (radians)
static void reel(uint16_t *buf, int y0, int h, float cx, float cy, float fill, float a)
{
    if (cy + REEL_R + 2 <= y0 || cy - REEL_R - 2 >= y0 + h) return;
    aa_disc(buf, y0, h, cx, cy, REEL_R, F_DIM);
    aa_disc(buf, y0, h, cx, cy, REEL_R - 2, 0);
    float tr = reel_tape(fill);
    aa_disc(buf, y0, h, cx, cy, tr, rgb(70, 46, 30));           // the tape: brown
    aa_disc(buf, y0, h, cx, cy, 12, rgb(235, 235, 240));         // the hub
    for (int k = 0; k < 3; k++) {
        float b = a + k * 2 * (float)M_PI / 3, p[4] = {cx + 4 * sinf(b), cy - 4 * cosf(b), cx + 10 * sinf(b), cy - 10 * cosf(b)};
        aa_path(buf, y0, h, p, 2, false, 3, 0);
    }
    aa_disc(buf, y0, h, cx, cy, 2.5f, 0);
}

static void field_reels(uint16_t *buf, int y0, int h, float p)
{
    reel(buf, y0, h, ART_X / 2.0f, REEL_Y, 1 - p, reel_a[0]);
    reel(buf, y0, h, W - ART_X / 2.0f, REEL_Y, p, reel_a[1] + 0.5f);
}

// Turn the reels while it plays, like a cassette's: the tape runs at one speed, so the full reel turns slowly and the
// nearly empty one fast
static void reels_turn(int64_t now)
{
    static int64_t at;
    if (!ui_field() || !s.active || !s.playing || s.duration_ms <= 0) { at = 0; return; }
    if (at && now - at < REEL_US) return;
    float dt = at ? fminf(0.25f, (now - at) / 1e6f) : 0;
    at = now;
    reel_a[0] += dt * REEL_TAPE_S / reel_tape(1 - progress);
    reel_a[1] += dt * REEL_TAPE_S / reel_tape(progress);
    for (int k = 0; k < 2; k++) if (reel_a[k] > 2 * (float)M_PI) reel_a[k] -= 2 * (float)M_PI;
    if (dt > 0) reels_moved = true;
}

int spotify_ui_take_reels(int r[2][4])
{
    bool m = reels_moved;
    reels_moved = false;
    if (!m) return 0;
    for (int k = 0; k < 2; k++) {
        int cx = k ? W - ART_X / 2 : ART_X / 2;
        r[k][0] = cx - REEL_R - 2; r[k][1] = REEL_Y - REEL_R - 2; r[k][2] = cx + REEL_R + 2; r[k][3] = REEL_Y + REEL_R + 2;
    }
    return 2;
}
bool spotify_ui_reels_dirty(void) { return reels_moved; }

// a square button: dark with a rim, or (main) orange; pressed: lighter
static void field_btn(uint16_t *buf, int y0, int h, const btn_t *b, bool main, bool on)
{
    uint16_t fill = main ? (on ? rgb(255, 150, 90) : F_ORANGE) : on ? rgb(50, 50, 56) : F_PANEL;
    aa_rrect(buf, y0, h, b->x0, b->y0, b->x1, b->y1, main ? 12 : 9, main ? fill : F_DIM);
    if (!main) aa_rrect(buf, y0, h, b->x0 + 2, b->y0 + 2, b->x1 - 2, b->y1 - 2, 7, fill);
}

// the volume as a knob: a ring of ticks lit orange as far as it's turned up, the number in the middle (drag it up and
// down, as a knob in music software)
static void field_knob(uint16_t *buf, int y0, int h, int vol, bool on)
{
    const float cx = F_KNOB_X, cy = F_CTRL_Y;
    if (vol < 0 || cy + F_KNOB_R + 12 <= y0 || cy - F_KNOB_R - 12 >= y0 + h) return;   // (a device without volume: none)
    const int n = 11;                                 // ticks over 270 degrees, the gap at the bottom
    int lit = (int)lroundf(vol / 100.0f * (n - 1)) + (vol > 0);
    for (int k = 0; k < n; k++) {
        float a = (-135 + 270.0f * k / (n - 1)) * (float)M_PI / 180;
        float p[4] = {cx + (F_KNOB_R + 4) * sinf(a), cy - (F_KNOB_R + 4) * cosf(a), cx + (F_KNOB_R + 9) * sinf(a), cy - (F_KNOB_R + 9) * cosf(a)};
        aa_path(buf, y0, h, p, 2, false, 2, k < lit ? F_ORANGE : F_DIM);
    }
    aa_disc(buf, y0, h, cx, cy, F_KNOB_R, on ? rgb(110, 110, 118) : F_DIM);
    aa_disc(buf, y0, h, cx, cy, F_KNOB_R - 2, on ? rgb(50, 50, 56) : F_PANEL);
    char t[12];
    snprintf(t, sizeof t, "%d", vol);
    ftext_cx(buf, y0, h, (int)cx, (int)cy - font_mono_s.line_h / 2, &font_mono_s, t, C_WHITE);
}

static void draw_field_idle(uint16_t *buf, int y0, int h)
{
    field_grid(buf, y0, h);
    field_reels(buf, y0, h, 0);
    ftext_c(buf, y0, h, 300, &font_mono_l, s.configured ? "NOTHING PLAYING" : "NOT CONNECTED", rgb(235, 235, 240));
    ftext_c(buf, y0, h, 344, &font_mono_s, s.configured ? "PLAY SOMETHING ON ANY DEVICE" : "CONNECT IT FROM THE BOBBLE APP ON YOUR MAC", F_GREY);
    if (s.note[0]) ftext_c(buf, y0, h, 380, &font_small, s.note, F_ORANGE);
}

static void draw_field(uint16_t *buf, int y0, int h)
{
    char line[96], l1[96];
    field_grid(buf, y0, h);
    if (!spotify_art(&(uint32_t){0}))                   // cover still loading: a dark tile where it goes
        fill_rrect(buf, y0, h, ART_X, ART_Y, ART_X + ART_SZ, ART_Y + ART_SZ, 18, F_PANEL);
    field_reels(buf, y0, h, s.duration_ms > 0 ? progress : 0);
    strlcpy(line, s.title, sizeof line);
    ascii(line);
    fitw(l1, sizeof l1, line, &font_label, W - 72);
    ftext_c(buf, y0, h, ART_Y + ART_SZ + 8, &font_label, l1, C_WHITE);
    strlcpy(line, s.artist, sizeof line);
    ascii(line);
    upper(line);
    fitw(l1, sizeof l1, line, &font_mono_s, W - 72);
    ftext_c(buf, y0, h, ART_Y + ART_SZ + 34, &font_mono_s, l1, F_ORANGE);
    // the progress: the time so far, segments lit as far as it's got (the one it's in: orange), the length; a note
    // (Spotify needs Premium...) takes the row instead
    int ty = F_BAR_Y - 10;                            // (the mono capitals' middle on the segments')
    if (s.note[0]) ftext_c(buf, y0, h, F_BAR_Y - font_small.line_h / 2, &font_small, s.note, F_ORANGE);
    else if (s.duration_ms > 0) {
        fmt_time(line, sizeof line, (int)(progress * s.duration_ms));
        draw_ftext(buf, y0, h, BAR_X0, ty, &font_mono_s, line, F_GREY);
        fmt_time(l1, sizeof l1, s.duration_ms);
        draw_ftext(buf, y0, h, BAR_X1 - ftext_width(&font_mono_s, l1), ty, &font_mono_s, l1, F_GREY);
        const int x0 = BAR_X0 + 60, x1 = BAR_X1 - 60, n = 36;   // (room for "1:02:33" either side)
        float sw = (x1 - x0) / (float)n;
        int lit = (int)(progress * n);
        for (int i = 0; i < n; i++) {
            float x = x0 + i * sw;
            aa_rrect(buf, y0, h, x, F_BAR_Y - 5, x + sw - 3, F_BAR_Y + 5, 1.2f, i < lit ? C_WHITE : i == lit ? F_ORANGE : rgb(40, 40, 44));
        }
    }
    // controls: square buttons, the middle one orange
    for (int i = 0; i < 3; i += 2) {
        field_btn(buf, y0, h, &fbtns[i], false, pressed(fbtns[i].id));
        float cx = (fbtns[i].x0 + fbtns[i].x1) / 2.0f;
        if (s.episode) ftext_cx(buf, y0, h, (int)cx, F_CTRL_Y - 10, &font_mono_s, "10", C_WHITE);
        else draw_skip(buf, y0, h, cx, F_CTRL_Y, i == 2, C_WHITE);
    }
    field_btn(buf, y0, h, &fbtns[1], true, pressed(ID_PLAY));
    field_knob(buf, y0, h, shown_volume(), pressed(ID_VOL));
    if (s.playing) {
        static const float l[] = {9, 5, 9, 19}, r[] = {15, 5, 15, 19};
        aa_icon(buf, y0, h, CX, F_CTRL_Y, 24, l, 2, false, 2.4f, 0);
        aa_icon(buf, y0, h, CX, F_CTRL_Y, 24, r, 2, false, 2.4f, 0);
    } else {
        static const float tri[] = {7, 4, 19, 12, 7, 20};
        aa_icon(buf, y0, h, CX + 1.5f, F_CTRL_Y, 24, tri, 3, true, 2.4f, 0);
    }
}

// ---------------------------------------------------------------- the Tiles look (Settings > Look)
// Like a phone's widgets: the cover with a tall tile either side of it (how far the song's got, filling up in orange;
// the volume, in blue), the song in a light tile under it, and a row of tiles: the time so far, the three keys (play
// in yellow) and the time left.
#define T_SIDE 86                                     // the side tiles' width

static int tiles_mid(const font_t *f, float cy) { return (int)lroundf(cy - f->ascent + 0.3f * f->line_h); }

// a tall tile filled from the bottom to frac (0..1) in c
static void tiles_level(uint16_t *buf, int y0, int h, float x0, float x1, float frac, uint16_t c)
{
    aa_rrect(buf, y0, h, x0, ART_Y, x1, ART_Y + ART_SZ, 26, T_CASE);
    float fy = ART_Y + ART_SZ - ART_SZ * (frac < 0 ? 0 : frac > 1 ? 1 : frac);
    if (ART_Y + ART_SZ - fy < 2) return;
    int ct, cb;
    gfx_get_clip(&ct, &cb);
    gfx_clip((int)fy > ct ? (int)fy : ct, cb);
    aa_rrect(buf, y0, h, x0, ART_Y, x1, ART_Y + ART_SZ, 26, c);
    gfx_clip(ct, cb);
}

// a speaker with waves (none when silent; -1: muted, crossed out)
static void tiles_speaker(uint16_t *buf, int y0, int h, float cx, float cy, int waves, uint16_t c)
{
    static const float cone[] = {3, 9, 7, 9, 12, 4.5f, 12, 19.5f, 7, 15, 3, 15}, slash[] = {2, 2, 22, 22};
    aa_icon(buf, y0, h, cx, cy, 24, cone, 6, true, 2, c);
    if (waves < 0) aa_icon(buf, y0, h, cx + 2, cy, 24, slash, 2, false, 2, c);
    for (int k = 1; k <= waves; k++) {
        float p[2 * 8], r = 4.5f + 4.5f * k;
        for (int i = 0; i < 8; i++) {
            float a = (-45 + 90 * i / 7.0f) * (float)M_PI / 180;
            p[2 * i] = cx + 1 + r * cosf(a);
            p[2 * i + 1] = cy + r * sinf(a);
        }
        aa_path(buf, y0, h, p, 8, false, 2, c);
    }
}

// a time in a tile
static void tiles_time(uint16_t *buf, int y0, int h, int x0, int x1, const char *t, const font_t *f, uint16_t bg)
{
    aa_rrect(buf, y0, h, x0, T_ROW_Y0, x1, T_ROW_Y1, 26, bg);
    ftext_cx(buf, y0, h, (x0 + x1) / 2, tiles_mid(f, (T_ROW_Y0 + T_ROW_Y1) / 2.0f), f, t, T_INK);
}

// the logo in a dark tile, what to do in a light one
static void draw_tiles_idle(uint16_t *buf, int y0, int h)
{
    aa_rrect(buf, y0, h, 14, ART_Y, W - 14, ART_Y + ART_SZ, 30, T_CASE);
    draw_logo(buf, y0, h);
    ftext_c(buf, y0, h, LOGO_Y + LOGO_R + 22, &font_body, "Spotify", C_WHITE);
    const int iy0 = ART_Y + ART_SZ + 10, iy1 = iy0 + 76;
    aa_rrect(buf, y0, h, 14, iy0, W - 14, iy1, 24, T_PAPER);
    draw_ftext(buf, y0, h, 32, iy0 + 11, &font_label, s.configured ? "Nothing playing" : "Not connected", T_INK);
    draw_ftext(buf, y0, h, 32, iy0 + 37, &font_small,
               s.configured ? "Play something on any device" : "Connect it from the Bobble menu on your Mac", T_INK2);
    if (s.note[0]) ftext_c(buf, y0, h, iy1 + 16, &font_small, s.note, T_RED);
}

static void draw_tiles(uint16_t *buf, int y0, int h)
{
    char line[96], l1[96];
    if (!spotify_art(&(uint32_t){0}))                   // cover still loading: a dark tile where it goes
        fill_rrect(buf, y0, h, ART_X, ART_Y, ART_X + ART_SZ, ART_Y + ART_SZ, 18, T_CASE);
    tiles_level(buf, y0, h, ART_X - 10 - T_SIDE, ART_X - 10, s.duration_ms > 0 ? progress : 0, T_ORANGE);
    // the volume: drag it up and down (its number at the top)
    int vol = shown_volume();
    float vx = ART_X + ART_SZ + 10 + T_SIDE / 2.0f, vy = ART_Y + ART_SZ - 32, vtop = ART_Y + ART_SZ * (1 - vol / 100.0f);
    tiles_level(buf, y0, h, ART_X + ART_SZ + 10, ART_X + ART_SZ + 10 + T_SIDE, vol >= 0 ? vol / 100.0f : 0,
                pressed(ID_VOL) ? tile_hot(T_SKY) : T_SKY);
    tiles_speaker(buf, y0, h, vx - 4, vy, vol == 0 ? -1 : vol < 50 ? 1 : 2, vol >= 0 && vy > vtop ? T_INK : C_WHITE);
    if (vol >= 0) {
        snprintf(line, sizeof line, "%d", vol);
        float ny = ART_Y + 26;
        ftext_cx(buf, y0, h, (int)vx, tiles_mid(&font_label, ny), &font_label, line, ny > vtop + 6 ? T_INK : C_WHITE);
    }
    static const float note[] = {9, 17, 9, 5, 18, 3, 18, 15};   // (a note at the foot of the song's tile)
    float nx = ART_X - 10 - T_SIDE / 2.0f, ny = vy;
    uint16_t nc = s.duration_ms > 0 && progress > 32.0f / ART_SZ ? T_INK : C_WHITE;
    aa_icon(buf, y0, h, nx, ny, 24, note, 4, false, 2, nc);
    aa_disc(buf, y0, h, nx - 5.5f, ny + 5, 2.8f, nc);
    aa_disc(buf, y0, h, nx + 3.5f, ny + 3, 2.8f, nc);

    // the song and who it's by, in a light tile (a note, Spotify needs Premium..., in place of who)
    const int iy0 = ART_Y + ART_SZ + 10, iy1 = T_ROW_Y0 - 10;
    aa_rrect(buf, y0, h, 14, iy0, W - 14, iy1, 24, T_PAPER);
    strlcpy(line, s.title, sizeof line);
    ascii(line);
    fitw(l1, sizeof l1, line, &font_label, W - 72);
    draw_ftext(buf, y0, h, 32, iy0 + 11, &font_label, l1, T_INK);
    strlcpy(line, s.note[0] ? s.note : s.artist, sizeof line);
    ascii(line);
    fitw(l1, sizeof l1, line, &font_small, W - 72);
    draw_ftext(buf, y0, h, 32, iy0 + 37, &font_small, l1, s.note[0] ? T_RED : T_INK2);

    // the row: times either end, the keys between
    if (s.duration_ms > 0) {
        // the bigger font if the song's longest time fits (so it doesn't change size as the seconds go by)
        l1[0] = '-';
        fmt_time(l1 + 1, sizeof l1 - 1, s.duration_ms);
        for (char *q = l1; *q; q++) if (*q >= '0' && *q <= '9') *q = '8';
        const font_t *tf = ftext_width(&font_body, l1) <= tbtns[0].x0 - 10 - 14 - 16 ? &font_body : &font_label;
        fmt_time(line, sizeof line, (int)(progress * s.duration_ms));
        tiles_time(buf, y0, h, 14, tbtns[0].x0 - 10, line, tf, T_ORANGE);
        int left = s.duration_ms - (int)(progress * s.duration_ms);
        line[0] = '-';
        fmt_time(line + 1, sizeof line - 1, left);
        tiles_time(buf, y0, h, tbtns[2].x1 + 10, W - 14, line, tf, T_RED);
    } else {
        tiles_time(buf, y0, h, 14, tbtns[0].x0 - 10, "", &font_body, T_CASE);
        tiles_time(buf, y0, h, tbtns[2].x1 + 10, W - 14, "", &font_body, T_CASE);
    }
    for (int i = 0; i < 3; i += 2) {
        const btn_t *b = &tbtns[i];
        aa_rrect(buf, y0, h, b->x0, b->y0, b->x1, b->y1, 22, pressed(b->id) ? tile_hot(T_CASE2) : T_CASE2);
        float cx = (b->x0 + b->x1) / 2.0f, cy = (b->y0 + b->y1) / 2.0f;
        if (s.episode) ftext_cx(buf, y0, h, (int)cx, tiles_mid(&font_label, cy), &font_label, "10", C_WHITE);
        else draw_skip(buf, y0, h, cx, cy, i == 2, C_WHITE);
    }
    const btn_t *pb = &tbtns[1];
    float py = (pb->y0 + pb->y1) / 2.0f;
    aa_rrect(buf, y0, h, pb->x0, pb->y0, pb->x1, pb->y1, 26, pressed(ID_PLAY) ? tile_hot(T_YELLOW) : T_YELLOW);
    if (s.playing) {
        static const float l[] = {9, 5, 9, 19}, r[] = {15, 5, 15, 19};
        aa_icon(buf, y0, h, CX, py, 30, l, 2, false, 2.2f, T_INK);
        aa_icon(buf, y0, h, CX, py, 30, r, 2, false, 2.2f, T_INK);
    } else {
        static const float tri[] = {7, 4, 19, 12, 7, 20};
        aa_icon(buf, y0, h, CX + 2, py, 30, tri, 3, true, 2.2f, T_INK);
    }
}

void spotify_ui_draw(uint16_t *buf, int y0, int h)
{
    if (!s.configured || !s.active) {
        if (ui_field()) draw_field_idle(buf, y0, h);
        else if (ui_tiles()) draw_tiles_idle(buf, y0, h);
        else draw_square_idle(buf, y0, h);
        return;
    }

    if (ui_field()) draw_field(buf, y0, h);
    else if (ui_tiles()) draw_tiles(buf, y0, h);
    else draw_square(buf, y0, h);
}

// ---------------------------------------------------------------- input
static int hit(int x, int y)
{
    const btn_t *b = btns;
    if (ui_field()) b = fbtns;
    else if (ui_tiles()) b = tbtns;
    if (ui_field() && s.volume >= 0 && hypotf(x - F_KNOB_X, y - F_CTRL_Y) < F_KNOB_R + 22) return ID_VOL;
    if (ui_tiles() && s.volume >= 0 && x >= ART_X + ART_SZ + 4 && x < ART_X + ART_SZ + 16 + T_SIDE && y >= ART_Y - 8 &&
        y < ART_Y + ART_SZ + 8) return ID_VOL;
    if (!ui_field() && !ui_tiles() && s.volume >= 0 && x >= C_VOL_X0 - 10 && x < C_VOL_X1 + 20 && y >= ART_Y - 8 &&
        y < ART_Y + ART_SZ + 8) return ID_VOL;
    for (int i = 0; i < NBTN; i++) {
        if (i >= 3 && s.volume < 0) break;
        if (x >= b[i].x0 - 6 && x < b[i].x1 + 6 && y >= b[i].y0 - 8 && y < b[i].y1 + 10) return b[i].id;
    }
    return ID_NONE;
}

void spotify_ui_cancel_touch(void)
{
    if (press_id == ID_VOL) drag_vol = -1;             // (a swipe took it: as it was)
    down = false;
    press_id = ID_NONE;
}

void spotify_ui_update(int tx, int ty, int64_t now_us)
{
    spotify_get(&s);
    if (s.duration_ms > 0) {
        int ms = s.progress_ms + (s.playing ? (int)((now_us - s.fetched_us) / 1000) : 0);
        progress = ms > s.duration_ms ? 1.0f : ms / (float)s.duration_ms;
    } else {
        progress = 0;
    }
    reels_turn(now_us);
    if (drag_vol >= 0 && press_id != ID_VOL && now_us > drag_until) drag_vol = -1;   // Spotify's own from here
    if (tx >= 0 && !down) {
        down = true;
        press_x = tx; press_y = ty;
        press_id = s.active ? hit(tx, ty) : ID_NONE;
        if (press_id == ID_VOL) { drag_vol = drag_from = shown_volume(); drag_moved = 0; }
    } else if (tx >= 0) {
        if (press_id == ID_VOL) {                      // up and down: the volume follows the finger
            if (abs(tx - press_x) > 24 && abs(tx - press_x) > abs(ty - press_y)) spotify_ui_cancel_touch();   // (a swipe)
            else {
                if (abs(ty - press_y) > drag_moved) drag_moved = abs(ty - press_y);
                int v = drag_from + (press_y - ty) * 100 / (ui_field() ? 120 : ART_SZ);   // (the knob: quicker, near the edge)
                drag_vol = v < 0 ? 0 : v > 100 ? 100 : v;
            }
        } else if (abs(tx - press_x) > 16 || abs(ty - press_y) > 16) press_id = ID_NONE;   // it's a swipe, not a tap
    } else if (down) {
        down = false;
        if (press_id == ID_VOL) {
            if (drag_moved <= 8) {                     // a tap: mute, and again to unmute
                if (drag_from > 0) { unmute_vol = drag_from; drag_vol = 0; }
                else drag_vol = unmute_vol > 0 ? unmute_vol : 30;
            }
            if (drag_vol != s.volume) {
                char a[8];
                snprintf(a, sizeof a, "%d", drag_vol);
                spotify_command_arg(SP_VOL_SET, a);
            }
            drag_until = now_us + 4000000;
        }
        switch (press_id) {
        case ID_PREV: spotify_command(s.episode ? SP_SEEK_BACK : SP_PREV); break;
        case ID_PLAY: spotify_command(SP_PLAY_PAUSE); break;
        case ID_NEXT: spotify_command(s.episode ? SP_SEEK_FWD : SP_NEXT); break;
        case ID_VDOWN: spotify_command(SP_VOL_DOWN); break;
        case ID_VUP: spotify_command(SP_VOL_UP); break;
        }
        press_id = ID_NONE;
    }
}

bool spotify_ui_dirty(void)
{
    static uint32_t seen_art;
    static int seen_sec = -1;
    static bool seen_playing;
    static int seen_vol;
    static char seen_title[96];
    uint32_t ver;
    spotify_art(&ver);
    int sec = (int)(progress * s.duration_ms / 1000);
    bool d = ver != seen_art || sec != seen_sec || s.playing != seen_playing || strcmp(seen_title, s.title) ||
             shown_volume() != seen_vol;
    seen_art = ver;
    seen_sec = sec;
    seen_playing = s.playing;
    seen_vol = shown_volume();
    strlcpy(seen_title, s.title, sizeof seen_title);
    return d;
}
