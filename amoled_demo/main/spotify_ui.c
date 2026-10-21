// Now Playing screen: album art fills the round display (dimmed), a progress ring runs around the
// edge, with the track, controls, and the playing device's volume on top.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "esp_timer.h"
#include "gfx.h"
#include "spotify.h"
#include "spotify_ui.h"

#define C_WHITE  rgb(255, 255, 255)
#define C_GREY   rgb(170, 170, 185)
#define C_DIM    rgb(90, 90, 105)
#define C_GREEN  rgb(30, 215, 96)          // Spotify green
#define C_PANEL  rgb(30, 30, 44)
#define C_PRESS  rgb(0, 110, 70)
#define C_NOTE   rgb(255, 150, 60)

#define RING_IN 220
#define RING_OUT 229

enum { ID_NONE, ID_PREV, ID_PLAY, ID_NEXT, ID_VDOWN, ID_VUP };
typedef struct { int x0, y0, x1, y1, id; } btn_t;
// volume up top, track name in the middle (where swipes between pages cross), playback controls below
#define CTRL_Y 332
#define VOL_Y 140
static const btn_t btns[] = {
    {CX - 150, CTRL_Y - 35, CX - 70, CTRL_Y + 35, ID_PREV},
    {CX - 50, CTRL_Y - 45, CX + 50, CTRL_Y + 45, ID_PLAY},
    {CX + 70, CTRL_Y - 35, CX + 150, CTRL_Y + 35, ID_NEXT},
    {CX - 156, VOL_Y - 25, CX - 92, VOL_Y + 25, ID_VDOWN},
    {CX + 92, VOL_Y - 25, CX + 156, VOL_Y + 25, ID_VUP},
};
#define NBTN 5

static spotify_state_t s;
static float progress;                 // 0..1 for the ring
static bool down;
static int press_id, press_x, press_y;

// ---------------------------------------------------------------- helpers
static void fit(char *out, size_t cap, const char *in, int max_chars)
{
    if ((int)strlen(in) <= max_chars) { strlcpy(out, in, cap); return; }
    snprintf(out, cap, "%.*s..", max_chars - 2, in);
}

static void wrap2(const char *str, int n, char *l1, char *l2, size_t cap)
{
    l2[0] = 0;
    if ((int)strlen(str) <= n) { strlcpy(l1, str, cap); return; }
    int cut = n;
    while (cut > n / 2 && str[cut] != ' ') cut--;
    if (str[cut] != ' ') cut = n;
    snprintf(l1, cap, "%.*s", cut, str);
    const char *rest = str + cut;
    while (*rest == ' ') rest++;
    fit(l2, cap, rest, n);
}

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

// ---------------------------------------------------------------- background: art + progress ring
void spotify_ui_render_bg(uint16_t *buf, int y0, int y1)
{
    uint32_t ver;
    const uint16_t *art = s.active ? spotify_art(&ver) : NULL;
    if (art) memcpy(buf, art + y0 * W, (y1 - y0) * W * 2);
    else memset(buf, 0, (y1 - y0) * W * 2);
    if (!s.active || s.duration_ms <= 0) return;

    uint16_t played = C_GREEN, rest = rgb(55, 55, 68);
    float limit = progress * 2 * (float)M_PI;
    for (int y = y0; y < y1; y++) {
        float dy = y - CY + 0.5f;
        if (fabsf(dy) > RING_OUT) continue;
        float outer = sqrtf(RING_OUT * RING_OUT - dy * dy);
        float inner = fabsf(dy) < RING_IN ? sqrtf(RING_IN * RING_IN - dy * dy) : 0;
        uint16_t *o = buf + (y - y0) * W;
        for (int side = 0; side < 2; side++) {         // left and right arc of this row
            int xa = side ? (int)(CX + inner) : (int)(CX - outer), xb = side ? (int)(CX + outer) : (int)(CX - inner);
            if (xa < 0) xa = 0;
            if (xb >= W) xb = W - 1;
            for (int x = xa; x <= xb; x++) {
                float a = atan2f(x - CX + 0.5f, -dy);   // clockwise from 12 o'clock
                if (a < 0) a += 2 * (float)M_PI;
                o[x] = a <= limit ? played : rest;
            }
        }
    }
}

// ---------------------------------------------------------------- overlay
static bool pressed(int id) { return down && press_id == id; }

// Podcast seek button: a circular arrow with "10" inside (counter-clockwise for back)
static void draw_seek(uint16_t *buf, int y0, int h, int cx, int cy, bool fwd, uint16_t c)
{
    const int r0 = 21, r1 = 25;
    for (int y = cy - r1; y <= cy + r1; y++) {
        if (y < y0 || y >= y0 + h) continue;
        for (int x = cx - r1; x <= cx + r1; x++) {
            int dx = x - cx, dy = y - cy, d2 = dx * dx + dy * dy;
            if (d2 < r0 * r0 || d2 > r1 * r1) continue;
            float a = atan2f(dx, -dy) * 180 / (float)M_PI;   // 0 at the top, clockwise
            if (fwd ? (a > -10 && a < 40) : (a < 10 && a > -40)) continue;   // gap at the top for the arrowhead
            fill_rect(buf, y0, h, x, y, x + 1, y + 1, c);
        }
    }
    int ax = cx + (fwd ? 10 : -10);                   // arrowhead at the end of the arc, pointing around
    fill_tri(buf, y0, h, ax, cy - r1 - 7, ax, cy - r0 + 7, ax + (fwd ? 12 : -12), cy - (r0 + r1) / 2, c);
    draw_text(buf, y0, h, cx - text_width("10", 2) / 2, cy - 7, 2, "10", c);
}

static void draw_prev_next(uint16_t *buf, int y0, int h, int cx, int cy, bool next, uint16_t c)
{
    int d = next ? 1 : -1;
    fill_tri(buf, y0, h, cx - 18 * d, cy - 16, cx - 18 * d, cy + 16, cx, cy, c);
    fill_tri(buf, y0, h, cx, cy - 16, cx, cy + 16, cx + 18 * d, cy, c);
    fill_rect(buf, y0, h, next ? cx + 18 : cx - 22, cy - 16, next ? cx + 22 : cx - 18, cy + 16, c);
}

void spotify_ui_draw(uint16_t *buf, int y0, int h)
{
    char line[64], l1[48], l2[48];
    if (!s.configured || !s.active) {
        text_c(buf, y0, h, 150, 3, "Spotify", C_GREEN);
        if (!s.configured) {
            text_c(buf, y0, h, 200, 2, "To connect, on your Mac run", C_GREY);
            text_c(buf, y0, h, 226, 2, "./spotify_auth.py", C_WHITE);
        } else {
            text_c(buf, y0, h, 200, 2, "Nothing playing", C_GREY);
            text_c(buf, y0, h, 226, 2, "Start music on any device", C_DIM);
        }
        if (s.note[0]) text_c(buf, y0, h, 280, 2, s.note, C_NOTE);
        return;
    }

    // track
    strlcpy(line, s.title, sizeof line);
    ascii(line);
    wrap2(line, 16, l1, l2, sizeof l1);
    text_c(buf, y0, h, l2[0] ? 196 : 212, 3, l1, C_WHITE);
    if (l2[0]) text_c(buf, y0, h, 222, 3, l2, C_WHITE);
    strlcpy(line, s.artist, sizeof line);
    ascii(line);
    fit(l1, sizeof l1, line, 24);
    text_c(buf, y0, h, 256, 2, l1, C_GREY);

    // controls
    for (int i = 0; i < 3; i++)
        if (pressed(btns[i].id)) fill_circle(buf, y0, h, (btns[i].x0 + btns[i].x1) / 2, CTRL_Y, 38, C_PRESS);
    if (s.episode) {                                    // podcasts: jump 10 s back / forward instead of tracks
        draw_seek(buf, y0, h, CX - 110, CTRL_Y, false, C_WHITE);
        draw_seek(buf, y0, h, CX + 110, CTRL_Y, true, C_WHITE);
    } else {
        draw_prev_next(buf, y0, h, CX - 110, CTRL_Y, false, C_WHITE);
        draw_prev_next(buf, y0, h, CX + 110, CTRL_Y, true, C_WHITE);
    }
    fill_circle(buf, y0, h, CX, CTRL_Y, 42, pressed(ID_PLAY) ? C_PRESS : C_WHITE);
    uint16_t ic = pressed(ID_PLAY) ? C_WHITE : rgb(10, 10, 14);
    if (s.playing) {                                    // pause bars
        fill_rrect(buf, y0, h, CX - 15, CTRL_Y - 19, CX - 5, CTRL_Y + 19, 2, ic);
        fill_rrect(buf, y0, h, CX + 5, CTRL_Y - 19, CX + 15, CTRL_Y + 19, 2, ic);
    } else {                                            // play triangle (nudged right to look centred)
        fill_tri(buf, y0, h, CX - 11, CTRL_Y - 21, CX - 11, CTRL_Y + 21, CX + 20, CTRL_Y, ic);
    }

    // device + volume
    strlcpy(line, s.device, sizeof line);
    ascii(line);
    fit(l1, sizeof l1, line, 13);
    text_c(buf, y0, h, VOL_Y - 19, 2, l1, C_WHITE);
    if (s.volume >= 0) {
        snprintf(line, sizeof line, "%d%%", s.volume);
        text_c(buf, y0, h, VOL_Y + 5, 2, line, C_GREY);
        for (int i = 3; i < 5; i++) {
            const btn_t *b = &btns[i];
            fill_rrect(buf, y0, h, b->x0, b->y0, b->x1, b->y1, 14, pressed(b->id) ? C_PRESS : C_PANEL);
            int cx = (b->x0 + b->x1) / 2, cy = (b->y0 + b->y1) / 2;
            fill_rect(buf, y0, h, cx - 11, cy - 2, cx + 11, cy + 2, C_WHITE);
            if (b->id == ID_VUP) fill_rect(buf, y0, h, cx - 2, cy - 11, cx + 2, cy + 11, C_WHITE);
        }
    }
    if (s.note[0]) text_c(buf, y0, h, 394, 2, s.note, C_NOTE);
}

// ---------------------------------------------------------------- input
static int hit(int x, int y)
{
    for (int i = 0; i < NBTN; i++) {
        if (i >= 3 && s.volume < 0) break;
        if (x >= btns[i].x0 - 6 && x < btns[i].x1 + 6 && y >= btns[i].y0 - 8 && y < btns[i].y1 + 10) return btns[i].id;
    }
    return ID_NONE;
}

void spotify_ui_cancel_touch(void) { down = false; press_id = ID_NONE; }

void spotify_ui_update(int tx, int ty, int64_t now_us)
{
    spotify_get(&s);
    if (s.duration_ms > 0) {
        int ms = s.progress_ms + (s.playing ? (int)((now_us - s.fetched_us) / 1000) : 0);
        progress = ms > s.duration_ms ? 1.0f : ms / (float)s.duration_ms;
    } else {
        progress = 0;
    }
    if (tx >= 0 && !down) {
        down = true;
        press_x = tx; press_y = ty;
        press_id = s.active ? hit(tx, ty) : ID_NONE;
    } else if (tx >= 0) {
        if (abs(tx - press_x) > 16 || abs(ty - press_y) > 16) press_id = ID_NONE;   // it's a swipe, not a tap
    } else if (down) {
        down = false;
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
    static char seen_title[96];
    uint32_t ver;
    spotify_art(&ver);
    int sec = (int)(progress * s.duration_ms / 1000);
    bool d = ver != seen_art || sec != seen_sec || s.playing != seen_playing || strcmp(seen_title, s.title);
    seen_art = ver;
    seen_sec = sec;
    seen_playing = s.playing;
    strlcpy(seen_title, s.title, sizeof seen_title);
    return d;
}
