// The Status page: Bobble's eyes act out your status, big, on black, with a glow in the status's colour behind them and
// its name under them. Each status has its own little performance (all made of the eyes' own squint, lid, smile and
// look, plus a few dots or z's), and a row of icons along the bottom picks one.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "gfx.h"
#include "calendar.h"
#include "pet_ui.h"
#include "status_ui.h"

enum { ST_AVAILABLE, ST_BUSY, ST_FOCUS, ST_MEETING, ST_BRB, ST_LUNCH, ST_AWAY, ST_N };

typedef struct { const char *name, *line; int col[3]; } stat_t;
static const stat_t stats[ST_N] = {
    {"Available",     "Come say hi",       {90, 225, 130}},
    {"Busy",          "Heads down",        {255, 95, 80}},
    {"Focus",         "Do not disturb",    {170, 120, 255}},
    {"In a meeting",  "Back soon",         {100, 160, 255}},
    {"Be right back", "Back in a few",     {255, 190, 70}},
    {"Out to lunch",  "Yum",               {255, 150, 60}},
    {"Away",          "Zzz",               {150, 150, 165}},
};

#define EYE_D 150            // the eyes: size, how far each is from the middle, and height
#define EYE_GAP 92
#define EYE_Y 175
#define NAME_Y 292           // the status's name, and the line under it
#define CHIP_Y 418           // the row of icons: height, and the distance between them
#define CHIP_STEP 62

static int cur = ST_AVAILABLE, prev_st = ST_AVAILABLE;
static bool loaded, auto_meeting, was_in_meeting, was_down;
static int64_t chg_t, last_us, last_dirty, next_cal, t_down;
static float ecol[3] = {255, 255, 255};     // the eyes' colour (white with a tint of the status's), eased
static int t_x0, t_y0, t_lx, t_ly;
static ical_item_t items[24];

static float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static uint16_t rgbk(const int c[3], float k) { return rgb((int)(c[0] * k), (int)(c[1] * k), (int)(c[2] * k)); }

static void save(void)
{
    nvs_handle_t h;
    if (nvs_open("demo", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "status", (uint8_t)cur);
    nvs_commit(h);
    nvs_close(h);
}

static void load(void)
{
    nvs_handle_t h;
    uint8_t v;
    if (nvs_open("demo", NVS_READONLY, &h) != ESP_OK) return;
    if (nvs_get_u8(h, "status", &v) == ESP_OK && v < ST_N) cur = v;
    nvs_close(h);
}

// manual: you picked it (kept, and a meeting's end no longer changes it)
static void set_status(int s, bool manual, int64_t now)
{
    if (manual) { auto_meeting = false; }
    if (s == cur) return;
    cur = s;
    chg_t = now;
    if (manual) save();
}

static bool in_meeting_now(time_t t)
{
    int n = calendar_items(items, sizeof items / sizeof items[0]);
    for (int i = 0; i < n; i++)
        if (!items[i].all_day && !items[i].free && items[i].start <= t && t < items[i].end) return true;
    return false;
}

static void tap(int x, int y, int64_t now)
{
    for (int i = 0; i < ST_N; i++) {
        float dx = x - (CX + (i - 3) * CHIP_STEP), dy = y - CHIP_Y;
        if (dx * dx + dy * dy < 32 * 32) { set_status(i, true, now); chg_t = now; return; }
    }
    if (y < NAME_Y) chg_t = now;                      // poked the eyes: they bounce
}

void status_ui_cancel_touch(void) { was_down = false; }

void status_ui_update(int tx, int ty, int64_t now)
{
    if (!loaded) { loaded = true; load(); chg_t = now - 10000000; }
    float dt = last_us ? clampf((now - last_us) / 1e6f, 0, 0.2f) : 0.03f;
    last_us = now;

    time_t wall = time(NULL);
    if (now >= next_cal && wall > 1700000000) {        // a meeting starts: "in a meeting" till it ends (unless you're away or focusing)
        next_cal = now + 5000000;
        bool in = in_meeting_now(wall);
        if (in && !was_in_meeting && cur != ST_AWAY && cur != ST_FOCUS && cur != ST_MEETING) {
            prev_st = cur;
            auto_meeting = true;
            set_status(ST_MEETING, false, now);
        } else if (!in && was_in_meeting && auto_meeting) {
            auto_meeting = false;
            if (cur == ST_MEETING) set_status(prev_st, false, now);
        }
        was_in_meeting = in;
    }
    for (int c = 0; c < 3; c++) ecol[c] += (255 + (stats[cur].col[c] - 255) * 0.55f - ecol[c]) * (1 - expf(-dt * 6));

    bool down = tx >= 0;
    if (down && !was_down) { t_down = now; t_x0 = t_lx = tx; t_y0 = t_ly = ty; }
    if (down) { t_lx = tx; t_ly = ty; }
    if (!down && was_down && now - t_down < 500000 && abs(t_lx - t_x0) + abs(t_ly - t_y0) < 24) tap(t_lx, t_ly, now);
    was_down = down;
}

bool status_ui_dirty(void)
{
    int64_t now = esp_timer_get_time();
    if (now - last_dirty < 80000) return false;
    last_dirty = now;
    return true;
}

// A blink every `period` s, `len` s long, shifted by `off`: 0 .. 1 .. 0 (1 = shut)
static float blink_k(float t, float period, float len, float off)
{
    float ph = fmodf(t + off, period);
    return ph < len ? sinf((float)M_PI * ph / len) : 0;
}

// What each status's icon is, drawn in ink over a disc of colour bg
static void glyph(uint16_t *buf, int y0, int h, int id, float cx, float cy, uint16_t ink, uint16_t bg)
{
    switch (id) {
    case ST_AVAILABLE: {                              // a smile
        aa_disc(buf, y0, h, cx - 6.5f, cy - 4.5f, 2.3f, ink);
        aa_disc(buf, y0, h, cx + 6.5f, cy - 4.5f, 2.3f, ink);
        const float sm[8] = {cx - 8.5f, cy + 2.5f, cx - 4, cy + 7.5f, cx + 4, cy + 7.5f, cx + 8.5f, cy + 2.5f};
        aa_path(buf, y0, h, sm, 4, false, 2.4f, ink);
        break;
    }
    case ST_BUSY:                                     // a bar across ("no entry")
        aa_rrect(buf, y0, h, cx - 9, cy - 2.5f, cx + 9, cy + 2.5f, 2.5f, ink);
        break;
    case ST_FOCUS:                                    // a moon
        aa_disc(buf, y0, h, cx - 1, cy, 9.5f, ink);
        aa_disc(buf, y0, h, cx + 4.5f, cy - 3.5f, 8, bg);
        break;
    case ST_MEETING:                                  // a calendar page
        aa_rrect(buf, y0, h, cx - 9, cy - 8.5f, cx + 9, cy + 9, 3, ink);
        aa_rrect(buf, y0, h, cx - 7, cy - 3.5f, cx + 7, cy + 7, 1.5f, bg);
        break;
    case ST_BRB: {                                    // a clock
        aa_disc(buf, y0, h, cx, cy, 10, ink);
        aa_disc(buf, y0, h, cx, cy, 7.6f, bg);
        const float hd[6] = {cx, cy - 5, cx, cy, cx + 4, cy + 2.5f};
        aa_path(buf, y0, h, hd, 3, false, 2, ink);
        break;
    }
    case ST_LUNCH:                                    // a cookie with a bite out
        aa_disc(buf, y0, h, cx, cy, 10, ink);
        aa_disc(buf, y0, h, cx + 8, cy - 6.5f, 5, bg);
        aa_disc(buf, y0, h, cx - 3, cy + 1, 1.5f, bg);
        aa_disc(buf, y0, h, cx + 2, cy + 5, 1.5f, bg);
        break;
    default: {                                        // a Z
        const float z[8] = {cx - 6, cy - 6, cx + 6, cy - 6, cx - 6, cy + 6, cx + 6, cy + 6};
        aa_path(buf, y0, h, z, 4, false, 2.6f, ink);
    }
    }
}

void status_ui_draw(uint16_t *buf, int y0, int h)
{
    int64_t now = esp_timer_get_time();
    float t = now / 1e6f, age = chg_t ? clampf((now - chg_t) / 1e6f, 0, 10) : 10;
    const stat_t *S = &stats[cur];

    // a glow in its colour behind the eyes, breathing
    float br = 0.5f + 0.5f * sinf(t * (cur == ST_BUSY ? 3.2f : cur == ST_AWAY ? 0.9f : 1.6f));
    aa_disc(buf, y0, h, CX, EYE_Y, 215, rgbk(S->col, 0.05f + 0.03f * br));
    aa_disc(buf, y0, h, CX, EYE_Y, 165, rgbk(S->col, 0.09f + 0.04f * br));

    // the performance: where they look, bob, squint, lid, cross-eyed, smile, blink, and a wink of the right one
    float sc = 1 + 0.18f * sinf(age * 20) * expf(-age * 6);   // (a squash and bounce on every change)
    float lx = 0, ly = 0, bob = 0, hs = 1, ws = 1, lid = 0, cross = 0, happy = 0, blink = 0, wink = 0;
    switch (cur) {
    case ST_AVAILABLE: {                              // bright and friendly: bouncing, a big grin now and then, a wink
        bob = -fabsf(sinf(t * 2.2f)) * 10;
        float p = fmodf(t, 5);
        happy = 0.25f + 0.1f * sinf(t * 1.3f) + (p < 1.2f ? 0.65f * sinf((float)M_PI * p / 1.2f) : 0);
        lx = sinf(t * 0.7f) * 8;
        blink = blink_k(t, 4.3f, 0.16f, 0);
        wink = blink_k(t, 7, 0.4f, 3);
        break;
    }
    case ST_BUSY:                                     // narrowed, scanning a page side to side
        hs = 0.7f;
        lid = 0.12f;
        cross = 0.3f;
        lx = sinf(t * 1.8f) * 34;
        ly = sinf(t * 3.6f) * 2;
        blink = blink_k(t, 9, 0.14f, 0);
        break;
    case ST_FOCUS:                                    // heavy lids, breathing slowly, looking down
        lid = 0.42f + 0.06f * sinf(t * 1.2f);
        hs = 0.92f + 0.04f * sinf(t * 1.2f);
        ly = 6;
        blink = blink_k(t, 7, 0.3f, 2);
        break;
    case ST_MEETING: {                                // looking off to the side at the screen, a glance back now and then
        float p = fmodf(t, 6), back = p < 1.2f ? sinf((float)M_PI * p / 1.2f) : 0;
        lx = (22 + 6 * sinf(t * 0.9f)) * (1 - back);
        lid = 0.1f;
        blink = blink_k(t, 5.2f, 0.16f, 1);
        break;
    }
    case ST_BRB:                                      // looking around for the time, swaying
        lx = cosf(t * 1.5f) * 30;
        ly = sinf(t * 3) * 14;
        bob = sinf(t * 1.5f) * 6;
        happy = 0.1f;
        blink = blink_k(t, 4, 0.16f, 0.5f);
        break;
    case ST_LUNCH: {                                  // chomping, pleased
        float p = fmodf(t, 3.4f);
        happy = 0.55f;
        if (p < 1.3f) hs = 1 - 0.32f * fabsf(sinf(p * 10));
        break;
    }
    default:                                          // fast asleep: just a line, breathing
        hs = 0.07f + 0.02f * sinf(t * 1.2f);
        break;
    }
    int ec[3] = {(int)ecol[0], (int)ecol[1], (int)ecol[2]};
    for (int i = 0; i < 2; i++) {
        float side = i ? 1 : -1;
        float hh = EYE_D * hs * sc * (1 - 0.94f * blink) * (i ? 1 - 0.93f * wink : 1);
        pet_ui_draw_eye(buf, y0, h, CX + side * EYE_GAP + lx, EYE_Y + ly + bob, EYE_D * ws * sc, fmaxf(hh, 1), lid, -side * cross,
                        clampf(happy, 0, 1), ec);
    }

    // a few extras
    if (cur == ST_MEETING)                            // typing "..." beside the eyes
        for (int k = 0; k < 3; k++)
            aa_disc(buf, y0, h, CX + 120 + k * 18, EYE_Y - 108 - 7 * fmaxf(0, sinf(t * 7 - k * 0.8f)), 4.5f, rgbk(S->col, 1));
    if (cur == ST_LUNCH && fmodf(t, 3.4f) < 1.3f) {   // crumbs flying while it chomps
        float p = fmodf(t, 3.4f);
        for (int k = 0; k < 3; k++) {
            float u = fmodf(p * 1.2f + k * 0.33f, 1);
            aa_disc(buf, y0, h, CX + (k - 1) * 60 * u, EYE_Y + 100 - 70 * u + 90 * u * u, 3.8f * (1 - u) + 0.5f, rgbk(S->col, 1 - u * 0.5f));
        }
    }
    if (cur == ST_AWAY)                               // z's drifting up
        for (int k = 0; k < 3; k++) {
            float u = fmodf(t * 0.45f + k / 3.0f, 1), a = sinf((float)M_PI * u);
            draw_ftext(buf, y0, h, (int)(CX + 120 + u * 50), (int)(EYE_Y - 40 - u * 110), &font_label, "z", rgbk(S->col, a));
        }

    // its name, fading and rising in
    float la = clampf(age / 0.35f, 0, 1);
    int ny = NAME_Y + (int)((1 - la) * 16);
    draw_ftext(buf, y0, h, CX - ftext_width(&font_big, S->name) / 2, ny, &font_big, S->name, rgb((int)(250 * la), (int)(250 * la), (int)(250 * la)));
    draw_ftext(buf, y0, h, CX - ftext_width(&font_label, S->line) / 2, ny + 56, &font_label, S->line, rgbk(S->col, 0.9f * la));

    // the icons: tap one to switch
    for (int i = 0; i < ST_N; i++) {
        float cx = CX + (i - 3) * CHIP_STEP, cy = CHIP_Y;
        const int *c = stats[i].col;
        if (i == cur) {
            float r = 26 + 1.5f * sinf(t * 4);
            aa_disc(buf, y0, h, cx, cy, r + 3, rgb(235, 235, 240));
            aa_disc(buf, y0, h, cx, cy, r, rgbk(c, 0.95f));
            glyph(buf, y0, h, i, cx, cy, rgb(20, 20, 26), rgbk(c, 0.95f));
        } else {
            aa_disc(buf, y0, h, cx, cy, 22, rgbk(c, 0.22f));
            glyph(buf, y0, h, i, cx, cy, rgbk(c, 0.95f), rgbk(c, 0.22f));
        }
    }
}
