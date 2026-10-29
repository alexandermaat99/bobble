// Pet page: two round white robot eyes on black. They blink and glance around by themselves, follow your
// finger, squint happily when tapped or stroked (tap an eye to make it wink), bob along while music
// plays, look tired on a low battery, get drowsy and nod off just before the screen goes to sleep, perk up
// before a meeting (a ring around the edge counts down the last 3 minutes, then a burst when it starts), look
// focused during one and relieved after, have a "yum" when plugged in, glance towards the calendar when a
// reminder is due, roll downhill when the board lies flat and is tipped, swing around when the screen
// spins to follow the board (a few quick spins in a row make them dizzy), and go dizzy when shaken.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include "esp_random.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "gfx.h"
#include "pet_ui.h"
#include "spotify.h"
#include "battery.h"
#include "calendar.h"
#include "reminders.h"
#include "agent.h"
#include "icons_ui.h"
#include "sprites_ui.h"

#define EYE_GAP 88          // eye centres sit this far either side of the middle
#define EYE_D   124         // eye diameter
#define EYE_Y   (CY - 4)

// One eye (an ellipse, normally a circle): centre, size, top lid (0 open .. 1 shut), lid slope (+ = inner corner lower: cross),
// happy (0 .. 1: cut away from below into an upturned arc)
typedef struct { float x, y, w, h, lid, tilt, happy; } eye_t;

static eye_t cur[2];                 // eased towards the mood's shape
static eye_t shown[2], drawn[2];     // what the next frame shows (cur + bounces, blinks), what the last one did
static float look_x, look_y;         // where the eyes are looking (px offset), eased fast like a glance
static float col[3] = {255, 255, 255}, col_drawn[3];
static int64_t last_us, next_blink, blink_t0 = -1, blink_len = 160000, next_glance;
static int64_t happy_until, surprise_until, dizzy_until, hop_t0 = -1, wink_t0 = -1, last_touch, last_pickup;
static int wink_eye;
static float glance_x, glance_y;
static float up_prev[3], shake;
static float ang_prev = 1000, spin_rate, spin_total;   // the screen turning: degrees/s, recent degrees
static int64_t spin_until;
static bool was_down, petting, was_charging, dirty = true;
static int64_t t_down;
static int t_x0, t_y0, t_lx, t_ly;
static float t_path;
static int zzz_step = -1, zzz_drawn = -2;
static bool asleep;
static char caption[220], caption_drawn[220];
static char cap_label[48], cap_label_drawn[48];   // over the agent's bubble: which project it's about
static char mt_caption[48];          // the meeting part of the caption (check_calendar can run between frames)
// what the caption is: plain text (a meeting), or the agent's speech bubble (busy / needs you / done)
enum { CAP_PLAIN, CAP_WORK, CAP_WAIT, CAP_DONE, CAP_STOP, CAP_HELLO, CAP_MEET, CAP_MEETQ };
// (hello: the first-run welcome; meet: a meeting announced; meetq: a quiet note while one's on)
static int welcome;                           // first-run stage (see pet_ui_set_welcome)
static bool wants_wifi;
static int cap_kind, cap_kind_drawn;
static float compact, compact_v;      // 0 .. 1: the eyes shrink down under the bubble while it shows (a spring), and its speed
// The bubble's motion. pop (0 .. ~1.1) grows it out of its tail on a lively spring once the eyes have made room,
// and draws it back in (no bounce) when it goes. When the message changes while it's up, the old words fade out,
// the box springs to its new size, the colours ease across, and the new words fade in.
static float pop, pop_v;
static float box_w, box_h, box_wv, box_hv;    // the box's size as shown, springing towards the laid-out one
static float bcol[6];                         // background, text colour as shown (rgb, eased)
static bool swap_pending;                     // new words waiting for the old ones to fade out
static int64_t done_t0 = -1;                  // when the bubble turned into "done" (the badge pops from then)
static int64_t tool_t0 = -1;                  // when the top icon last changed (Claude <-> an integration): it pops in from then
static int64_t hop2_at;                       // a second hop queued (done: a happy double hop)
// Several chats at once: a dot each under the eyes, coloured by state, the one shown bigger (tap the bubble for the next)
#define DOTS_N (AGENT_MAX + 1)                // the chats, and the calendar while a meeting's near or on
typedef struct { int n, sel, pulse, pulse2; int8_t bob[DOTS_N]; agent_state_t st[DOTS_N]; int slot[DOTS_N]; char ag[DOTS_N][12]; uint8_t fade[DOTS_N]; } dotsf_t;
static int64_t meet_pin_until;                // the calendar's logo was tapped: its meeting bubble shows till then
#define ICON_EDGE 14                           // the top icon comes no closer than this to the top of the glass
#define DOTS_R 216                             // the logos' arc down the left edge: radius, and radians between them
#define DOTS_STEP 0.17f
#if !SCREEN_ROUND
#define DOTS_X 28                              // square: a straight column down the left edge instead, this far in
#define EDGE_R 24                              // ...and the glow and the meeting ring follow its rounded corners
// How far (px) a point (dx, dy from the middle) is inside a rounded square of half-width `half`, corner radius rr
// (negative: outside)
static float sq_inside(float dx, float dy, float half, float rr)
{
    float qx = fabsf(dx) - (half - rr), qy = fabsf(dy) - (half - rr);
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0, m = qx > qy ? qx : qy;
    return rr - sqrtf(ox * ox + oy * oy) - (m < 0 ? m : 0);
}
#endif
// Each chat's colour (by its slot): the eyes take on the shown chat's, its dot wears it. Soft tints, so they still
// read as bright eyes on black.
static const int chat_col[AGENT_MAX][3] = {
    {255, 176, 150}, {150, 205, 255}, {150, 240, 200}, {205, 180, 255}, {255, 228, 150}, {255, 170, 205},
};
// Where else the shown chat's colour goes (the eyes stay white): 0 just its dot, 1 a ring round the bubble,
// 2 the chat's title over it
#ifndef CHAT_ACCENT
#define CHAT_ACCENT 2
#endif
// How far the eyes take on the shown chat's colour, in %: 0 always white .. 100 its full colour. A faint tint (the user's
// pick, 2026-10-21, after full colour felt too much); white whenever no chat is up (a meeting, nothing going on).
#ifndef EYE_COLOR
#define EYE_COLOR 30
#endif
static int shown_slot;                        // the shown chat's slot (its colour)
static char pair_shown[8], pair_drawn[8];     // pairing with the Mac: the code on screen ("" none; "ok" just paired)
// A faint ring of colour just inside the edge of the glass, in the shown chat's own colour (the same as its title), so
// switching chats always reads at a glance. Eased between colours (halo), quantised for redraws (halo_q).
static float halo[3], halo_col[3];
static int halo_q[3], halo_drawn[3];
static bool bub_closed;                       // a long "done" message has been read to the end: closed (a tap brings it back)
static dotsf_t dots, dots_drawn;
static int64_t swap_t0, swap_in_t0 = -10000000;
// The square board's crowd (a character per chat, or Bobble's own when there are none: see the end of the file; it's
// all the square shows, never the eyes); nothing on the round one. awake: something's being said (keeps the screen on).
static bool crowd_on, crowd_drawn_last, crowd_awake;
#if !SCREEN_ROUND
static void crowd_update(int64_t now, float dt);
static bool crowd_tap(int x, int y, int64_t now);
static void crowd_draw(uint16_t *buf, int y0, int h);
#else
static void crowd_update(int64_t now, float dt) {}
static bool crowd_tap(int x, int y, int64_t now) { return false; }
static void crowd_draw(uint16_t *buf, int y0, int h) {}
#endif
// What the bubble looks like this frame: worked out by pet_ui_update, drawn by pet_ui_draw, compared to the
// frame before to find what to redraw
typedef struct {
    bool on;
    int x0, y0, x1, y1, r;                    // the box (scaled about the tail)
    int tail[6];                              // the tail's corners
    int bg[3], fg[3];                         // colours, already faded by the bubble's opacity
    int ta;                                   // the words' (and label row's) opacity, 0 .. 64
    int badge_x, badge_y, badge_r;            // done: the check-mark badge on the corner (badge_r 0: none)
    int slot;                                 // whose bubble (its colour, for CHAT_ACCENT)
    int icon_y, icon_k;                       // the icon over it all (Claude's mark, or the integration in use): centre, size 0..~70
    int scroll;
} bubf_t;
static bubf_t bf, bf_drawn;
// The label row, bubble and shrunk eyes move as one group, centred up and down on the glass: grp_dy is how far
// it sits from where the bubble's laid out (eased when a message with more or fewer lines comes in)
static float grp_dy, grp_target;
#define EYE_DROP 130                  // how far the eyes move down to make room, before centring (~24 px clear of the tail)
// The bubble's text wrapped into lines (pointers into bl_text, redone when it changes). More lines than fit
// scroll through (bubble_scroll): scroll_t0 is when this text appeared.
#define BUBBLE_MAX_LINES 16
static const char *bl_line[BUBBLE_MAX_LINES];
static int bl_len[BUBBLE_MAX_LINES], bl_n, bl_tw;
static char bl_text[220], bl_label[48], bl_model[24], bl_agent[12], bl_tool[24], bl_provider[16];   // what it was laid out for (kept while it fades out)
static int bl_kind;
static int64_t scroll_t0;
static int bub_x0, bub_y0, bub_x1, bub_y1;   // the bubble's box as laid out (before riding with the eyes)
static int64_t next_cal;
EXT_RAM_BSS_ATTR static ical_item_t items[40];
// the next meeting: counting down (last 3 min), just started (first 2 min), going on; relief once it's over
#define MT_LEAD 180
#define MT_HOLD 120
static time_t mt_start, mt_end, mt_over;      // mt_over: end of the meeting that was on at the last look
static int64_t relief_until, yum_t0 = -1, nudge_t0 = -1, next_nudge, next_rem, last_hop_s = -1;
static bool reminder_due;
static int ring_q = -1, ring_drawn = -1;      // countdown ring state (quantised), as shown and as drawn
EXT_RAM_BSS_ATTR static reminder_t rems[30];

static float frand(float a, float b) { return a + (b - a) * (esp_random() / 4294967296.0f); }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static float ease(float v, float target, float k, float dt) { return v + (target - v) * (1 - expf(-k * dt)); }

static void start_blink(int64_t now, int64_t len)
{
    if (blink_t0 >= 0 && now - blink_t0 < blink_len) return;
    blink_t0 = now;
    blink_len = len;
}

// The next meeting starting within 10 minutes, or the one that's on (looked up every few seconds), and the caption for
// it: "Standup in 6 min", then a "2:41" countdown for the last 3 minutes, then "Standup now". Meetings starting at the
// same time are announced together ("Standup & Roadmap start in 5 minutes", "Standup +1  2:41"); one starting while
// another's on gets its own announcements.
static char mt_title[20];
static char mt_names[2][44];                  // the first two starting together, for the bubble
static int mt_more;                           // how many others start with it (0: it's on its own)

// "Standup", "Standup & Roadmap", "Standup & 2 more"
static void meeting_names(char *out, size_t len)
{
    if (!mt_more) snprintf(out, len, "%s", mt_names[0]);
    else if (mt_more == 1) snprintf(out, len, "%s & %s", mt_names[0], mt_names[1]);
    else snprintf(out, len, "%s & %d more", mt_names[0], mt_more);
}

static void check_calendar(int64_t now)
{
    time_t t = time(NULL);
    if (now >= next_cal && t > 1700000000) {          // (clock set)
        next_cal = now + 5000000;
        if (mt_end && t < mt_end && mt_start <= t) mt_over = mt_end;   // remember the one on, for the relief after
        mt_start = mt_end = 0;
        mt_more = 0;
        int n = calendar_items(items, sizeof items / sizeof items[0]), pick = -1;
        bool pick_up = false;
        for (int i = 0; i < n; i++) {                 // the earliest still to start (or just started); else the one on
            const ical_item_t *it = &items[i];
            if (it->all_day || it->free || it->end <= t || it->start - t > 600) continue;
            bool up = it->start > t - MT_HOLD;
            if (pick >= 0 && (pick_up > up || (pick_up == up && it->start >= mt_start))) continue;
            pick = i;
            pick_up = up;
            mt_start = it->start;
        }
        if (pick >= 0) {
            mt_end = items[pick].end;
            snprintf(mt_title, sizeof mt_title, "%s", items[pick].title);
            snprintf(mt_names[0], sizeof mt_names[0], "%s", items[pick].title);
            for (int i = 0; i < n; i++) {             // any others starting with it (within a minute)
                const ical_item_t *it = &items[i];
                if (i == pick || it->all_day || it->free || it->end <= t || labs((long)(it->start - mt_start)) > 60) continue;
                if (++mt_more == 1) snprintf(mt_names[1], sizeof mt_names[1], "%s", it->title);
                if (it->end > mt_end) mt_end = it->end;   // (on until the last of them ends)
            }
        }
        if (mt_over && t >= mt_over && t - mt_over < 120 && !(mt_start && mt_start <= t)) {   // just finished
            relief_until = now + 3000000;
            mt_over = 0;
        }
        if (mt_over && t - mt_over >= 120) mt_over = 0;
    }
    mt_caption[0] = 0;
    if (!mt_start || t < 1700000000) return;
    long to = (long)(mt_start - t);
    char who[32];
    if (mt_more) snprintf(who, sizeof who, "%.14s +%d", mt_title, mt_more);
    else snprintf(who, sizeof who, "%s", mt_title);
    if (to > MT_LEAD) snprintf(mt_caption, sizeof mt_caption, "%s in %ld min", who, (to + 59) / 60);
    else if (to > 0) snprintf(mt_caption, sizeof mt_caption, "%s  %ld:%02ld", who, to / 60, to % 60);
    else if (-to < MT_HOLD) snprintf(mt_caption, sizeof mt_caption, "%s now", who);
}

// Meetings in the bubble: "starts in 10 minutes" and "in 5 minutes" (30 s each), "starting now" (its first 2 min), and
// a quiet "In <title> until 3:30" while it's on. Which of those it is now, and the words for it.
enum { MP_NONE, MP_SOON, MP_START, MP_ON };
#define MT_SAY_US 30                                   // s an "in 10/5 minutes" stays up
static int meeting_phase(char *text, size_t len)
{
    time_t t = time(NULL);
    if (!mt_start || t < 1700000000) return MP_NONE;
    long to = (long)(mt_start - t);
    char who[100];
    meeting_names(who, sizeof who);
    if ((to <= 600 && to > 600 - MT_SAY_US) || (to <= 300 && to > 300 - MT_SAY_US)) {
        if (text) snprintf(text, len, "%s start%s in %ld minutes", who, mt_more ? "" : "s", (to + 59) / 60);
        return MP_SOON;
    }
    if (to <= 0 && -to < MT_HOLD) {
        if (text) snprintf(text, len, "%s %s starting now", who, mt_more ? "are" : "is");
        return MP_START;
    }
    if (to <= 0 && t < mt_end) {
        if (text) {
            struct tm e;
            localtime_r(&mt_end, &e);
            snprintf(text, len, "In %s until %d:%02d", who, e.tm_hour % 12 ? e.tm_hour % 12 : 12, e.tm_min);
        }
        return MP_ON;
    }
    return MP_NONE;
}

// A moment the screen should come on for (and stay on through): each announcement, then the last 3 minutes to the
// first 2 of the meeting. Each is a different value, so the screen wakes once for each.
time_t pet_ui_meeting_window(void)
{
    check_calendar(esp_timer_get_time());
    time_t t = time(NULL);
    if (meeting_phase(NULL, 0) == MP_SOON) return mt_start - (mt_start - t > 400 ? 600 : 300);
    return mt_start && t >= mt_start - MT_LEAD && t < mt_start + MT_HOLD ? mt_start : 0;
}

// A reminder whose time has come today (or one for today without a time)
static void check_reminders(int64_t now)
{
    if (now < next_rem) return;
    next_rem = now + 20000000;
    reminder_due = false;
    time_t t = time(NULL);
    int n = reminders_get(rems, sizeof rems / sizeof rems[0]);
    for (int i = 0; i < n && !reminder_due; i++)
        reminder_due = rems[i].due && rems[i].due <= t && t - rems[i].due < (rems[i].has_time ? 12 : 24) * 3600;
}

// Where logo i of n sits: on an arc down the left edge of the glass (square: a column), the first at the top
static void dot_pos(int i, int n, int *x, int *y)
{
#if !SCREEN_ROUND
    *x = DOTS_X;
    *y = (int)lroundf(CY + (i - (n - 1) / 2.0f) * DOTS_R * DOTS_STEP);   // (spaced as on the arc)
    return;
#endif
    float th = (float)M_PI - (i - (n - 1) / 2.0f) * DOTS_STEP;
    *x = (int)lroundf(CX + DOTS_R * cosf(th));
    *y = (int)lroundf(CY + DOTS_R * sinf(th));
}

static void tap(int x, int y, int64_t now)
{
    if (pair_shown[0]) {                              // the pairing card's up: a tap anywhere puts it away
        if (strcmp(pair_shown, "ok")) agent_pair_cancel();
        return;
    }
    int near = -1;                                    // on (or by) a chat's dot: that chat  (even the only one: a finished chat goes back to just its logo, and this brings its bubble back)
    float best = 24 * 24;
    for (int i = 0; i < dots.n; i++) {
        int dx, dy;
        dot_pos(i, dots.n, &dx, &dy);
        dx = x - dx; dy = y - dy;
        if (dx * dx + dy * dy < best) { best = dx * dx + dy * dy; near = i; }
    }
    if (near >= 0 && !strcmp(dots.ag[near], "calendar")) { meet_pin_until = now + 30000000; bub_closed = false; return; }   // the meeting
    if (near >= 0) { meet_pin_until = 0; agent_select_slot(dots.slot[near]); bub_closed = false; scroll_t0 = now; return; }
    if (bub_closed && cap_kind != CAP_PLAIN) {        // read to the end and closed: a tap brings it back, from the top
        scroll_t0 = now;
        bub_closed = false;
        return;
    }
    if (bf.on && cap_kind == CAP_HELLO && welcome == 1 && x >= bf.x0 - 10 && x <= bf.x1 + 10 && y >= bf.y0 - 10 && y <= bf.y1 + 16) {
        wants_wifi = true;                            // the welcome: on to Wi-Fi setup
        return;
    }
    if (bf.on && x >= bf.x0 - 10 && x <= bf.x1 + 10 && y >= bf.y0 - 44 && y <= bf.y1 + 16) {
        // on the bubble: the next one along the left (the chats, then the meeting, then round again)
        int chats = dots.n, cal = -1;
        for (int i = 0; i < dots.n; i++) if (!strcmp(dots.ag[i], "calendar")) { cal = i; chats--; }
        bool on_meeting = cap_kind == CAP_MEET || cap_kind == CAP_MEETQ;
        if (on_meeting && chats) { meet_pin_until = 0; agent_select_slot(dots.slot[0]); }   // meeting -> first chat
        else if (cal >= 0 && (chats <= 1 || dots.sel == chats - 1)) meet_pin_until = now + 30000000;   // last chat -> meeting
        else if (chats > 1) agent_select_next();
        bub_closed = false;
        return;
    }
    for (int i = 0; i < 2; i++)
        if (fabsf(x - shown[i].x) < shown[i].w / 2 + 12 && fabsf(y - shown[i].y) < shown[i].h / 2 + 12) {
            wink_eye = i;                             // poked an eye: it winks
            wink_t0 = now;
            happy_until = now + 1000000;
            return;
        }
    hop_t0 = now;                                     // anywhere else: a happy little hop
    happy_until = now + 1600000;
}

// ---- the agent's speech bubble: over the (shrunk) eyes, tail pointing down at them, the project's name above.
// Text wraps at spaces; up to 4 lines show, and a longer message rolls up through them a line at a time.
#define BUBBLE_BOTTOM 280                     // (sits just over the shrunk eyes, however many lines)
#define BUBBLE_TEXT_W 320
#define BUBBLE_LINES 4
#define BUBBLE_PX 22
#define BUBBLE_PY 15

static const font_t *bl_font = &font_body;   // big type for a short message, the usual size otherwise

// Wrap bl_text into bl_line/bl_len in font f
static void bubble_wrap(const font_t *f)
{
    const char *p = bl_text;
    bl_n = bl_tw = 0;
    while (*p == ' ') p++;
    while (*p && bl_n < BUBBLE_MAX_LINES) {
        int fit = 0, brk = 0;                         // characters that fit, and the last space among them
        while (p[fit] && ftext_width_n(f, p, fit + 1) <= BUBBLE_TEXT_W) if (p[++fit] == ' ') brk = fit;
        if (!p[fit]) brk = fit;                       // the rest fits
        else if (!brk) brk = fit ? fit : 1;           // one long word: break it
        int w = ftext_width_n(f, p, brk);
        if (w > bl_tw) bl_tw = w;
        bl_line[bl_n] = p;
        bl_len[bl_n++] = brk;
        p += brk;
        while (*p == ' ') p++;
    }
}

static void bubble_layout(void)
{
    strcpy(bl_text, caption);
    bl_font = &font_big;                              // a short message ("Editing main.c") in big type, if 2 lines hold it
    bubble_wrap(bl_font);
    if (bl_n > 2) bubble_wrap(bl_font = &font_body);
    const font_t *f = bl_font;
    int vis = bl_n < BUBBLE_LINES ? bl_n : BUBBLE_LINES;
    bub_x0 = CX - bl_tw / 2 - BUBBLE_PX;
    bub_x1 = CX + (bl_tw + 1) / 2 + BUBBLE_PX;
    bub_y1 = BUBBLE_BOTTOM;
    bub_y0 = bub_y1 - vis * f->line_h - 2 * BUBBLE_PY;
    int top = bub_y0 - 38, bottom = EYE_Y + EYE_DROP + (int)(EYE_D * 0.66f / 2);   // title row .. shrunk eyes
    int icon_top = top - 15 - ICON_SIZE;              // (the icon floats up near the edge on its own: see bubble_frame)
    // centred a little high (the solid eyes weigh the bottom down; the glass narrows at the top), with the icon kept
    // clear of the edge and the eyes clear of the dots along the bottom
    grp_target = CY - 26 - (top + bottom) / 2.0f;
    if (icon_top + grp_target < ICON_EDGE) grp_target = ICON_EDGE - icon_top;
    if (bottom + grp_target > CY + 200) grp_target = CY + 200 - bottom;   // (eyes clear of the bottom of the glass)
}

static float smooth(float x) { x = clampf(x, 0, 1); return x * x * (3 - 2 * x); }

// How far a long message has rolled up: read the start, then a line at a time (glide, pause), and stay at the end
#define SCROLL_FIRST 2.5f                     // s on the first lines, then per line: a glide and a pause
#define SCROLL_GLIDE 0.7f
#define SCROLL_PAUSE 1.4f
#define CLOSE_AFTER_US 5000000                 // a long "done" message closes this long after reaching its end

// When a long message has rolled to its end (esp_timer us), or -1 if it fits
static int64_t scroll_end(void)
{
    int extra = bl_n - BUBBLE_LINES;
    if (extra <= 0) return -1;
    return scroll_t0 + (int64_t)((SCROLL_FIRST + extra * SCROLL_GLIDE + (extra - 1) * SCROLL_PAUSE) * 1e6f);
}

static float bubble_scroll(int64_t now)
{
    int extra = bl_n - BUBBLE_LINES;
    if (extra <= 0) return 0;
    const float first = SCROLL_FIRST, glide = SCROLL_GLIDE, pause = SCROLL_PAUSE;
    float lh = bl_font->line_h, t = (now - scroll_t0) / 1e6f;
    if (t < first) return 0;
    t -= first;
    for (int k = 0; k < extra; k++) {
        if (t < glide) return (k + smooth(t / glide)) * lh;
        t -= glide;
        if (k < extra - 1) {
            if (t < pause) return (k + 1) * lh;
            t -= pause;
        }
    }
    return extra * lh;
}

// How far the bubble sits from its laid-out place: centred with the eyes, riding just above them as they shrink
static int bubble_rise(void) { return (int)lroundf(grp_dy - (EYE_DROP + grp_dy) * (1 - compact)); }

// This frame's bubble: the box at its springing size, scaled by pop about the tail (so it grows out of it), faded
// in over the first part of the pop; the words fade in over the last part, and out and back in around a swap
static void bubble_frame(int64_t now)
{
    memset(&bf, 0, sizeof bf);
    if (pop < 0.01f || !bl_n) return;
    float y1 = BUBBLE_BOTTOM + bubble_rise(), y0 = y1 - box_h, x0 = CX - box_w / 2, x1 = CX + box_w / 2;
    float ax = CX - 31, ay = y1 + 14, sc = 0.35f + 0.65f * pop;   // (the tail's tip: where it grows from)
#define SX(x) ((int)lroundf(ax + ((x) - ax) * sc))
#define SY(y) ((int)lroundf(ay + ((y) - ay) * sc))
    bf.on = true;
    bf.slot = shown_slot;
    bf.x0 = SX(x0); bf.x1 = SX(x1); bf.y0 = SY(y0); bf.y1 = SY(y1);
    bf.r = (int)lroundf(fminf(24 * sc, (bf.y1 - bf.y0) / 2.0f));
    int tl[6] = {SX(CX - 44), SY(y1 - 2), SX(CX - 18), SY(y1 - 2), SX(CX - 38), SY(y1 + 14)};
    memcpy(bf.tail, tl, sizeof tl);
#undef SX
#undef SY
    float sa = clampf(pop / 0.3f, 0, 1);
    for (int c = 0; c < 3; c++) { bf.bg[c] = (int)(bcol[c] * sa); bf.fg[c] = (int)(bcol[3 + c] * sa); }
    float swap = swap_pending ? 1 - smooth((now - swap_t0) / 120000.0f) : smooth((now - swap_in_t0) / 200000.0f);
    float fit = clampf(1 - fmaxf(fabsf(box_h - (bub_y1 - bub_y0)), fabsf(box_w - (bub_x1 - bub_x0))) / 14, 0, 1);   // (box nearly its new size)
    bf.ta = (int)lroundf(64 * smooth((pop - 0.75f) / 0.25f) * swap * fit);
    bf.scroll = bf.ta ? (int)lroundf(bubble_scroll(now)) : 0;
    if (bl_kind == CAP_DONE && done_t0 >= 0 && bf.ta) {   // the check-mark badge: pops onto the top right corner
        float t = (now - done_t0) / 1e6f - 0.15f;
        float k = t <= 0 ? 0 : 1 - expf(-t * 9) * cosf(t * 15);   // (a springy overshoot, settling at 1)
        bf.badge_r = (int)lroundf(16 * k * bf.ta / 64);
        bf.badge_x = bf.x1 - 8;
        bf.badge_y = bf.y0 + 6;
    }
    if ((bl_tool[0] || bl_provider[0] || bl_agent[0]) && bf.ta) {   // the icon over the title: springs in when it changes
        float t = tool_t0 >= 0 ? (now - tool_t0) / 1e6f - 0.05f : 1;
        float k = t <= 0 ? 0 : 1 - expf(-t * 9) * cosf(t * 15);
        bf.icon_k = (int)lroundf(64 * k * bf.ta / 64);
        // up near the top of the glass, or higher still over a tall bubble (15 px over the title, as the title is
        // over the bubble)
        bf.icon_y = (int)fminf(ICON_EDGE + ICON_SIZE / 2 + 8, bf.y0 - 38 - 15 - ICON_SIZE / 2);
    }
}

void pet_ui_wake(void)
{
    int64_t now = esp_timer_get_time();
    surprise_until = now + 700000;
    next_blink = now + 800000;
    dirty = true;
}

void pet_ui_cancel_touch(void)
{
    was_down = false;
    petting = false;
}

void pet_ui_update(int tx, int ty, int64_t now, const float up[3], float ang, int64_t sleep_in)
{
    float dt = last_us ? (now - last_us) / 1e6f : 0.03f;
    if (dt > 0.2f) dt = 0.2f;                         // back after a while: just jump ahead
    last_us = now;
    float t = now / 1e6f;
    if (!next_blink) { next_blink = now + 2000000; next_glance = now + 1500000; last_touch = now; }

    // ---- the board moving: a shake makes it dizzy, picking it up surprises it
    float s = fabsf(up[0] - up_prev[0]) + fabsf(up[1] - up_prev[1]) + fabsf(up[2] - up_prev[2]);
    memcpy(up_prev, up, sizeof up_prev);
    shake = shake * 0.85f + s;
    if (shake > 1.6f && now > dizzy_until) { dizzy_until = now + 2500000; shake = 0; }
    if (s > 0.15f && now > dizzy_until && now - last_pickup > 8000000) {
        last_pickup = now;
        surprise_until = now + 700000;
    }

    // ---- the screen spinning to follow the board: the eyes swing behind the turn; lots of turning makes them dizzy
    float da = ang_prev > 999 ? 0 : ang - ang_prev;
    while (da > 180) da -= 360;
    while (da < -180) da += 360;
    ang_prev = ang;
    spin_rate = ease(spin_rate, da / dt, 20, dt);
    spin_total = spin_total * expf(-dt / 2.5f) + fabsf(da);
    if (spin_total > 400 && now > dizzy_until) { dizzy_until = now + 2500000; spin_total = 0; }
    if (fabsf(spin_rate) > 60) spin_until = now + 300000;
    else if (spin_until && now >= spin_until) { spin_until = 0; start_blink(now, 200000); }   // stopped: blink, look around

    // ---- touch: follow the finger, tap for a hop or a wink, stroke to pet
    bool down = tx >= 0;
    if (down && !was_down) { t_down = now; t_x0 = t_lx = tx; t_y0 = t_ly = ty; t_path = 0; }
    if (down) {
        t_path += abs(tx - t_lx) + abs(ty - t_ly);
        t_lx = tx;
        t_ly = ty;
        last_touch = now;
        if (t_path > 160 && now - t_down > 500000) petting = true;
    }
    if (!down && was_down) {
        if (petting) happy_until = now + 1200000;
        else if (now - t_down < 400000 && abs(t_lx - t_x0) + abs(t_ly - t_y0) < 24) {
            if (!crowd_tap(t_lx, t_ly, now)) tap(t_lx, t_ly, now);
        }
        petting = false;
    }
    was_down = down;

    // ---- what's going on around it
    check_calendar(now);
    check_reminders(now);
    time_t wall = time(NULL);
    long to = mt_start ? (long)(mt_start - wall) : 100000;   // seconds to the meeting (< 0: it's on)
    bool counting = to > 0 && to <= MT_LEAD, starting = to <= 0 && -to < 3, in_meeting = mt_start && to <= 0;
    // counting down: nudges (a hop and wide eyes) every 20 s, every 8 s in the last minute, every second at the end
    if (counting) {
        int every = to > 60 ? 20 : to > 10 ? 8 : 1;
        if (to % every == 0 && to != last_hop_s) { last_hop_s = to; hop_t0 = now; if (to > 10) surprise_until = now + 600000; }
    }
    if (starting && last_hop_s != 0) { last_hop_s = 0; hop_t0 = now; surprise_until = now + 1500000; }   // it's time!
    if (!counting && !starting) last_hop_s = -1;
    // a reminder due: every 40 s or so, a double look towards the calendar (the page to the right)
    if (reminder_due && !counting && !in_meeting && now >= next_nudge) {
        nudge_t0 = now;
        next_nudge = now + (int64_t)(frand(35, 50) * 1e6f);
    }
    int64_t nd = nudge_t0 >= 0 ? now - nudge_t0 : -1;
    bool nudging = nd >= 0 && nd < 1700000 && !(nd >= 550000 && nd < 850000);
    // a coding agent on the Mac: busy (focused, eyes scanning a page), needs you (wide eyes on you, a hop every
    // few seconds), just finished (a happy hop)
    agent_status_t ag;
    agent_get(&ag);
    if (!agent_pairing(pair_shown)) strcpy(pair_shown, agent_just_paired() ? "ok" : "");
    static uint32_t ag_attention;
    static int64_t next_ag_hop;
    bool ag_work = ag.state == AG_WORKING, ag_wait = ag.state == AG_WAITING && !counting && !starting;
    if (ag.attention != ag_attention) {
        ag_attention = ag.attention;
        int64_t settle = cap_kind != CAP_PLAIN && compact < 0.5f ? 550000 : 0;   // (making room first: hop once it's done)
        if (ag.state == AG_DONE) { hop_t0 = now + settle; hop2_at = hop_t0 + 380000; happy_until = now + settle + 2500000; }
        if (ag_wait) { hop_t0 = now + settle; surprise_until = now + settle + 900000; next_ag_hop = now + 6000000; }
    }
    if (ag_wait && now >= next_ag_hop) { hop_t0 = now; next_ag_hop = now + 6000000; }
    if (hop2_at && now >= hop2_at) { hop_t0 = now; hop2_at = 0; }
    caption[0] = 0;
    cap_kind = CAP_PLAIN;
    char mtext[160];
    int mphase = meeting_phase(mtext, sizeof mtext);
    static int last_mphase;                           // (an announcement just came up: the eyes perk up and hop)
    if ((mphase == MP_SOON || mphase == MP_START) && mphase != last_mphase) { hop_t0 = now; surprise_until = now + 900000; }
    last_mphase = mphase;
    // who gets the bubble: a chat waiting for you, then a meeting's announcement, then the chats, then (quieter) the
    // meeting's countdown and its "until" note
    if (ag_wait) { snprintf(caption, sizeof caption, "%s", ag.detail[0] ? ag.detail : "Needs you"); cap_kind = CAP_WAIT; }
    else if (mphase == MP_SOON || mphase == MP_START) { strcpy(caption, mtext); cap_kind = CAP_MEET; }
    else if (now < meet_pin_until && mt_start) {      // (its logo was tapped) where things stand with it
        time_t tw = time(NULL);
        long to = (long)(mt_start - tw);
        char who[100];
        meeting_names(who, sizeof who);
        if (to > 0) snprintf(caption, sizeof caption, "%s start%s in %ld minute%s", who, mt_more ? "" : "s", (to + 59) / 60, (to + 59) / 60 == 1 ? "" : "s");
        else strcpy(caption, mtext[0] ? mtext : mt_title);
        cap_kind = CAP_MEET;
    }
    else if (ag_work) { snprintf(caption, sizeof caption, "%s", ag.detail); cap_kind = CAP_WORK; }
    else if (ag.state == AG_DONE) { snprintf(caption, sizeof caption, "%s", ag.detail[0] ? ag.detail : "Done"); cap_kind = CAP_DONE; }
    else if (ag.state == AG_STOPPED) { snprintf(caption, sizeof caption, "%s", ag.detail[0] ? ag.detail : "Stopped"); cap_kind = CAP_STOP; }
    else if (mphase == MP_ON) { strcpy(caption, mtext); cap_kind = CAP_MEETQ; }
    else if (mt_caption[0]) strcpy(caption, mt_caption);
    else if (welcome == 1) { strcpy(caption, "Hi! Let's get me online. Tap here to set up Wi-Fi."); cap_kind = CAP_HELLO; }
    else if (welcome == 2) { strcpy(caption, "Almost there! Get the Bobble app on your Mac, then choose Pair."); cap_kind = CAP_HELLO; }
    snprintf(cap_label, sizeof cap_label, "%s", cap_kind == CAP_PLAIN ? "" : ag.title[0] ? ag.title : ag.project);   // which chat
    bool meet = cap_kind == CAP_MEET || cap_kind == CAP_MEETQ;
    const char *c_model = ag.model, *c_agent = ag.agent, *c_tool = ag.tool, *c_prov = ag.provider;
    if (meet) {                                       // a meeting: its start time over the bubble, a calendar on top
        struct tm st;
        localtime_r(&mt_start, &st);
        if (mt_more) snprintf(cap_label, sizeof cap_label, "%d meetings at %d:%02d", mt_more + 1, st.tm_hour % 12 ? st.tm_hour % 12 : 12, st.tm_min);
        else snprintf(cap_label, sizeof cap_label, "Meeting at %d:%02d", st.tm_hour % 12 ? st.tm_hour % 12 : 12, st.tm_min);
        c_model = c_agent = c_prov = "";
        c_tool = "meeting";
    }
    bool news = cap_kind != CAP_PLAIN && (strcmp(caption, bl_text) || cap_kind != bl_kind || strcmp(cap_label, bl_label) ||
                                          strcmp(c_model, bl_model) || strcmp(c_agent, bl_agent) ||
                                          strcmp(c_tool, bl_tool) || strcmp(c_prov, bl_provider));
    if (news && pop > 0.05f && bl_n && !swap_pending) { swap_pending = true; swap_t0 = now; }   // showing: fade out first
    if (news && (pop <= 0.05f || !bl_n || (swap_pending && now - swap_t0 >= 120000))) {
        if (strcmp(caption, bl_text)) scroll_t0 = now;   // new words: read from the top
        bubble_layout();
        if (cap_kind == CAP_DONE && bl_kind != CAP_DONE) {   // finished: the badge pops on, and a bump if it was already up
            done_t0 = now;
            if (pop > 0.5f) pop_v += 4;
        }
        bl_kind = cap_kind;
        strcpy(bl_label, cap_label);
        strcpy(bl_model, c_model);
        strcpy(bl_agent, c_agent);
        if (strcmp(c_tool, bl_tool) || strcmp(c_prov, bl_provider) || pop <= 0.05f) tool_t0 = now;   // (a new icon, or the bubble coming up)
        strcpy(bl_provider, c_prov);
        strcpy(bl_tool, c_tool);
        if (swap_pending) swap_in_t0 = now;
        swap_pending = false;
    }
    if (!news) swap_pending = false;                 // (changed back before the swap)
    // the ring: 3 minutes draining around the edge, flashing as the meeting starts
    ring_q = counting ? (int)(to * 2) : starting ? 10000 + (int)((now / 1000) % 1000 / 167) : -1;
    spotify_state_t sp;
    spotify_get(&sp);
    int pct = battery_percent();
    bool charging = battery_charging();
    if (charging && !was_charging) { happy_until = now + 2600000; yum_t0 = now; }   // plugged in: look down, yum
    was_charging = charging;
    bool low = pct >= 0 && pct <= 15 && !charging;
    bool dizzy = now < dizzy_until;
    // the screen is about to go off: eyelids droop for 3 s, then the eyes sleep for the last 5 s
    bool drowsy = sleep_in < 8000000 && !dizzy;
    asleep = sleep_in < 5000000 && !dizzy;

    // ---- mood -> eye shape
    float lid = 0, cross = 0, happy = 0, hs = 1, ws = 1, tc[3] = {255, 255, 255};
    if (dizzy) hs = 0.82f;
    else if (asleep) { hs = 0.06f; tc[0] = tc[1] = 150; tc[2] = 160; }
    else if (drowsy) lid = 0.25f + 0.35f * clampf(1 - (sleep_in - 5000000) / 3000000.0f, 0, 1);
    else if (petting || now < happy_until) happy = 1;
    else if (now < spin_until) { hs = 0.86f; ws = 1.1f; }   // whoa
    else if (now < surprise_until) { hs = 1.15f; ws = 0.9f; }
    else if (ag_wait) { hs = 1.12f; ws = 0.95f; }     // the agent needs you: all eyes
    else if (now < relief_until) happy = 0.8f;        // meeting over: phew
    else if (counting || (mt_caption[0] && !in_meeting)) hs = 1.08f;   // meeting soon: wide awake
    else if (in_meeting) { hs = 0.8f; lid = cap_kind != CAP_PLAIN ? 0 : 0.18f; }   // (no lids under a bubble: looks cut off)  // in a meeting: focused
    else if (ag_work) hs = 0.88f;                     // the agent's busy: concentrating with it (no lids: under the bubble
                                                      // a flat top looks cut off by it)
    else if (ag.state == AG_DONE) happy = 0.5f;       // finished: pleased
    else if (low) lid = 0.25f;                        // tired
    else if (sp.playing) happy = 0.45f;
    shown_slot = ag.slot;
    bool bob = sp.playing && !dizzy && !drowsy;       // keeps bopping along through a meeting caption or a low battery

    // ---- where to look: dizzy spin > downhill (lying flat) > glancing around (not at the finger: a tap made them
    // jerk towards it)
    float lx = glance_x, ly = glance_y;
    if (yum_t0 >= 0 && now - yum_t0 < 2600000 && !dizzy) {   // plugged in: down at the charger
        lx = 0;
        ly = 48;
    } else if (now < relief_until && !dizzy) {        // meeting over: eyes up, phew
        lx = 0;
        ly = -42;
    } else if ((counting || starting || ag_wait) && !dizzy) {   // meeting coming, or the agent needs you: look right at you
        lx = 0;
        ly = 0;
    } else if (nudging && !dizzy) {                   // look over there (the calendar)
        lx = 80;
        ly = -6;
    } else if (dizzy) {
        lx = 30 * cosf(t * 9.4f);
        ly = 20 * sinf(t * 9.4f);
    } else if (now < spin_until) {                    // lag behind the turn
        lx = clampf(-spin_rate * 0.15f, -80, 80);
        ly = 12;
    } else {
        float r = ang * (float)M_PI / 180, co = cosf(r), si = sinf(r);
        float gx = -(co * up[0] + si * up[1]), gy = -(-si * up[0] + co * up[1]);   // gravity across the page
        float g = sqrtf(gx * gx + gy * gy);
        if (fabsf(up[2]) > 0.85f && g > 0.06f) {      // lying flat and tipped (past a slightly unlevel desk)
            float k = (g - 0.06f) / g;
            lx = clampf(gx * k * 320, -80, 80);
            ly = clampf(gy * k * 240, -50, 50);
        } else if (now >= next_glance && !asleep) {
            bool far = frand(0, 1) < 0.4f;
            glance_x = far ? frand(-70, 70) : frand(-20, 20);
            glance_y = far ? frand(-40, 35) : frand(-12, 12);
            if (in_meeting) glance_y = frand(5, 30);      // in a meeting: eyes on the notes
            else if (ag_work) {                           // the agent's busy: reading the bubble over them, unhurried
                glance_x = frand(-22, 22);
                glance_y = frand(-8, -2);
                far = false;
            }
            else if (cap_kind != CAP_PLAIN) {             // any other bubble up: small, calm looks (it's what to read)
                glance_x = frand(-25, 25);
                glance_y = frand(-6, 10);
                far = false;
            }
            else if (mt_caption[0]) glance_y = frand(-40, -15);   // keeps peeking up at the meeting
            if (far && frand(0, 1) < 0.1f) start_blink(now, 160000);
            next_glance = now + (int64_t)((cap_kind != CAP_PLAIN && !in_meeting ? frand(1.2f, 2.8f) : frand(1.2f, 4.5f)) * 1e6f);
            lx = glance_x;
            ly = glance_y;
        }
        if (asleep) lx = ly = 0;
    }
    if (cap_kind != CAP_PLAIN && !(down && !petting)) ly = clampf(ly, -12, 30);   // under the bubble: don't look up into its tail
    float lk = cap_kind != CAP_PLAIN && !(down && !petting) ? 9 : 28;   // (under a bubble: glide, don't dart)
    look_x = ease(look_x, lx, lk, dt);
    look_y = ease(look_y, ly, lk, dt);

    // ---- blinking
    if (now >= next_blink && !asleep) {
        bool sleepy = drowsy || low;
        start_blink(now, sleepy ? 340000 : 160000);
        next_blink = now + (int64_t)(frand(sleepy ? 3 : 5, sleepy ? 7 : 11) * 1e6f);
        if (frand(0, 1) < 0.08f) next_blink = now + 320000;   // sometimes a double blink
    }

    // ---- ease the shapes, then add the quick stuff (bob, hop, blink, wink) on top
    // making room for the bubble: a critically damped spring, so the eyes ease off, glide and settle without a jolt.
    // The bubble pops out of its tail once they're most of the way, and the eyes only grow back once it's nearly gone.
    int64_t se = scroll_end();
    // (only once it's finished: while it works or waits for you, the bubble stays up at the end, showing it's going)
    bub_closed = cap_kind == CAP_DONE && se > 0 && now > se + CLOSE_AFTER_US;
    bool want = cap_kind != CAP_PLAIN && !bub_closed;
    float target = want || pop > 0.25f, pop_target = want && compact > 0.55f;
    float bw = bub_x1 - bub_x0, bh = bub_y1 - bub_y0;
    for (float left = dt; left > 0; left -= 0.004f) {
        float st = fminf(left, 0.004f), w = 9;
        compact_v += (w * w * (target - compact) - 2 * w * compact_v) * st;
        compact += compact_v * st;
        float pw = pop_target ? 15 : 22, pz = pop_target ? 0.6f : 1;   // in: lively (a little overshoot); out: quick, no bounce
        pop_v += (pw * pw * (pop_target - pop) - 2 * pz * pw * pop_v) * st;
        pop += pop_v * st;
        if (pop < 0) pop = pop_v = 0;
        float bwk = 16;                              // the box resizing to a new message
        box_wv += (bwk * bwk * (bw - box_w) - 2 * bwk * box_wv) * st;
        box_w += box_wv * st;
        box_hv += (bwk * bwk * (bh - box_h) - 2 * bwk * box_hv) * st;
        box_h += box_hv * st;
    }
    static const float palette[8][6] = {{0}, {40, 40, 50, 228, 228, 236}, {244, 244, 247, 22, 22, 28}, {208, 240, 218, 16, 72, 40},
                                        {34, 34, 40, 118, 118, 128}, {244, 244, 247, 22, 22, 28},
                                        {244, 244, 247, 22, 22, 28}, {40, 40, 50, 190, 190, 200}};   // (meet: white; meetq: quiet)   // (plain, working, waiting, done, stopped: greyed out, hello)
    if (pop < 0.02f) {                               // hidden: no morphing from whatever it was last time
        box_w = bw; box_h = bh; box_wv = box_hv = 0;
        memcpy(bcol, palette[bl_kind], sizeof bcol);
    } else for (int c = 0; c < 6; c++) bcol[c] = ease(bcol[c], palette[bl_kind][c], 10, dt);
    if (compact < 0.05f) grp_dy = grp_target;           // (appearing: start centred)
    else grp_dy = ease(grp_dy, grp_target, 8, dt);
    bubble_frame(now);
    {
        // (it fades in once the bubble's fully up, and out once it's gone: never during the pop, which stays smooth)
        // shows for 5 s when a bubble comes up, or when it switches to another chat or model; then fades
        static int64_t halo_t0 = -10000000;
        static int halo_slot = -1;
        static char halo_prov[16];
        static bool was_up;
        bool up = pop > 0.9f && bl_kind != CAP_HELLO;
        if (up && (!was_up || shown_slot != halo_slot || strcmp(bl_provider, halo_prov))) halo_t0 = now;
        was_up = up;
        halo_slot = shown_slot;
        strcpy(halo_prov, bl_provider);
        // Its strength follows a smooth curve in time (1.2 s in, 1.2 s out) rather than easing per frame: every frame
        // of the fade redraws the whole glass (~0.1 s turned), so the steps need to be small and even
        static float level;                           // 0 .. 1
        static int64_t last;
        float step = last ? (now - last) / 1.2e6f : 0;
        last = now;
        bool on = up && now - halo_t0 < 5000000;
        level = clampf(level + (on ? step : -step), 0, 1);
        float k = smooth(level);
        float tgt[3];                                 // the chat's own colour (as its title): a switch always shows
        static const float meet_col[3] = {120, 170, 255};
        for (int c = 0; c < 3; c++) tgt[c] = bl_kind == CAP_MEET || bl_kind == CAP_MEETQ ? meet_col[c] : chat_col[shown_slot][c];
        for (int c = 0; c < 3; c++) {
            if (k > 0.01f) halo_col[c] = ease(halo_col[c], tgt[c], 6, dt);   // (colour changes glide too)
            else halo_col[c] = tgt[c];
            halo[c] = halo_col[c] * k;
            halo_q[c] = (int)(halo[c] / 2);
        }
    }
    float cs = 1 - 0.34f * compact;                   // eye size while the bubble shows
    for (int i = 0; i < 2; i++) {
        float side = i ? 1 : -1, persp = look_x / 80;  // the eye on the side being looked at grows a little
        float k = 1 + 0.1f * side * persp;
        if (dizzy) k *= 1 + 0.15f * sinf(t * 9.4f + i * (float)M_PI);
        eye_t g = {CX + side * EYE_GAP * (1 - 0.25f * compact), EYE_Y + (EYE_DROP + grp_dy) * compact, EYE_D * ws, EYE_D * hs * k,
                   lid, -side * cross, happy};
        float kk = 14;
        cur[i].w = ease(cur[i].w ? cur[i].w : g.w, g.w, kk, dt);
        cur[i].h = ease(cur[i].h ? cur[i].h : g.h, g.h, kk, dt);
        cur[i].lid = ease(cur[i].lid, g.lid, kk, dt);
        cur[i].tilt = ease(cur[i].tilt, g.tilt, kk, dt);
        cur[i].happy = ease(cur[i].happy, g.happy, kk, dt);
        cur[i].x = g.x;
        cur[i].y = g.y;

        shown[i] = cur[i];
        shown[i].w *= cs;                             // (scaled here, not eased: size and position ride the same spring)
        shown[i].h *= cs;
        shown[i].x += look_x * cs;
        shown[i].y += look_y * cs;
        if (bob) shown[i].y -= fabsf(sinf(t * (float)M_PI * 2)) * 9;
        if (hop_t0 >= 0 && now >= hop_t0 && now - hop_t0 < 350000) shown[i].y -= (starting ? 40 : 24) * cs * sinf((float)M_PI * (now - hop_t0) / 350000.0f);
        if (yum_t0 >= 0 && now - yum_t0 > 500000 && now - yum_t0 < 1700000)   // two chomps
            shown[i].h *= 1 - 0.3f * fabsf(sinf((float)M_PI * 2 * (now - yum_t0 - 500000) / 1200000.0f));
        if (now < relief_until)                       // a slow sigh: sink, then settle back
            shown[i].y += 10 * sinf((float)M_PI * clampf(1 - (relief_until - now) / 3000000.0f, 0, 1));
        if (blink_t0 >= 0 && now - blink_t0 < blink_len)
            shown[i].h *= 1 - 0.94f * sinf((float)M_PI * (now - blink_t0) / blink_len);
        if (wink_t0 >= 0 && i == wink_eye && now - wink_t0 < 450000)
            shown[i].h *= 1 - 0.93f * sinf((float)M_PI * (now - wink_t0) / 450000.0f);
    }
    if (EYE_COLOR && !asleep && cap_kind >= CAP_WORK && cap_kind <= CAP_STOP)   // (a chat's bubble is what's up)
        for (int c = 0; c < 3; c++) tc[c] = 255 + (chat_col[shown_slot][c] - 255) * EYE_COLOR / 100.0f;
    for (int c = 0; c < 3; c++) col[c] = ease(col[c], tc[c], 6, dt);
    memset(&dots, 0, sizeof dots);
    time_t wall_now = time(NULL);
    bool cal_dot = mt_start && wall_now > 1700000000 && wall_now >= mt_start - 600 && wall_now < mt_end;
    if (ag.n >= 1 || cal_dot) {                       // down the left edge of the glass (even just one)
        dots.n = ag.n;
        dots.sel = ag.dot_shown;
        memcpy(dots.st, ag.dots, sizeof dots.st);
        memcpy(dots.slot, ag.dot_slot, sizeof dots.slot);
        memcpy(dots.ag, ag.dot_agent, sizeof dots.ag);
        memcpy(dots.fade, ag.dot_fade, sizeof dots.fade);
        for (int i = 0; i < dots.n; i++) {            // just finished: bobs for 5 s, smaller and smaller
            if (!ag.dot_done[i]) continue;
            float t = (ag.dot_done[i] - 1) / 10.0f;
            dots.bob[i] = (int8_t)lroundf(-fabsf(sinf(now / 1e6f * (float)M_PI / 0.5f)) * 7 * (1 - t / 5));
        }
        for (int i = 0; i < dots.n; i++) {            // active: their colour breathes (16 steps a cycle: cheap to redraw)
            if (dots.st[i] == AG_WORKING) dots.pulse = (int)((now / 150000) % 16);    // working: ~2.4 s
            if (dots.st[i] == AG_WAITING) dots.pulse2 = (int)((now / 75000) % 16);    // needs you: ~1.2 s, quicker
        }
        if (cal_dot) {                                // the calendar: breathes while it's announcing
            int c = dots.n++;
            strcpy(dots.ag[c], "calendar");
            bool says = mphase == MP_SOON || mphase == MP_START;
            dots.st[c] = says ? AG_WAITING : AG_IDLE;
            if (says) dots.pulse2 = (int)((now / 75000) % 16);
            if (cap_kind == CAP_MEET || cap_kind == CAP_MEETQ) dots.sel = c;
        }
    }

    crowd_update(now, dt);

    // ---- anything visibly different from the last frame?
    zzz_step = asleep ? (int)(t * 12) : -1;
    float diff = 0;
    for (int i = 0; i < 2; i++) {
        diff = fmaxf(diff, fabsf(shown[i].x - drawn[i].x) + fabsf(shown[i].y - drawn[i].y));
        diff = fmaxf(diff, fabsf(shown[i].w - drawn[i].w) + fabsf(shown[i].h - drawn[i].h));
        diff = fmaxf(diff, (fabsf(shown[i].lid - drawn[i].lid) + fabsf(shown[i].tilt - drawn[i].tilt) +
                            fabsf(shown[i].happy - drawn[i].happy)) * 100);
    }
    for (int c = 0; c < 3; c++) diff = fmaxf(diff, fabsf(col[c] - col_drawn[c]) / 4);
    if (diff > 0.3f || zzz_step != zzz_drawn || ring_q != ring_drawn || strcmp(caption, caption_drawn) || cap_kind != cap_kind_drawn ||
        strcmp(cap_label, cap_label_drawn) || memcmp(&bf, &bf_drawn, sizeof bf) ||
        memcmp(&dots, &dots_drawn, sizeof dots) || strcmp(pair_shown, pair_drawn) ||
        memcmp(halo_q, halo_drawn, sizeof halo_q)) dirty = true;
}

bool pet_ui_bubble_open(void) { return crowd_on ? crowd_awake : (cap_kind != CAP_PLAIN && cap_kind != CAP_HELLO && cap_kind != CAP_MEETQ && !bub_closed); }   // (the welcome lets it sleep)

void pet_ui_set_welcome(int stage) { welcome = stage; }

bool pet_ui_wants_wifi(void)
{
    bool w = wants_wifi;
    wants_wifi = false;
    return w;
}

bool pet_ui_dirty(void)
{
    bool d = dirty;
    dirty = false;
    return d;
}

bool pet_ui_take_rect(int *x0, int *y0, int *x1, int *y1)
{
    int r[2][4], n = pet_ui_take_rects(r);
    if (x0 && n) {
        *x0 = r[0][0]; *y0 = r[0][1]; *x1 = r[0][2]; *y1 = r[0][3];
        for (int i = 1; i < n; i++) {
            if (r[i][0] < *x0) *x0 = r[i][0];
            if (r[i][1] < *y0) *y0 = r[i][1];
            if (r[i][2] > *x1) *x1 = r[i][2];
            if (r[i][3] > *y1) *y1 = r[i][3];
        }
    }
    return n > 0;
}

int pet_ui_take_rects(int r[2][4])
{
    // just the eyes and the agent's bubble changed: the box around where they were and are now. The meeting's plain
    // caption, the ring or a colour change need the whole page
    const char *plain = cap_kind == CAP_PLAIN ? caption : "", *plain_drawn = cap_kind_drawn == CAP_PLAIN ? caption_drawn : "";
    bool part = !strcmp(plain, plain_drawn) && ring_q == ring_drawn && !strcmp(pair_shown, pair_drawn) && !pair_shown[0] &&
                !memcmp(halo_q, halo_drawn, sizeof halo_q);   // (the halo goes all the way round: the whole page)
    memcpy(halo_drawn, halo_q, sizeof halo_q);
    strcpy(pair_drawn, pair_shown);
    float a = W, b = H, c = 0, d = 0;
    bool eyes = memcmp(shown, drawn, sizeof drawn) || memcmp(col, col_drawn, sizeof col) || zzz_step >= 0 || zzz_drawn >= 0;
    for (int i = 0; i < 2 && eyes; i++)
        for (int k = 0; k < 2; k++) {
            const eye_t *e = k ? &drawn[i] : &shown[i];
            a = fminf(a, e->x - e->w / 2);
            c = fmaxf(c, e->x + e->w / 2);
            b = fminf(b, e->y - fmaxf(e->h / 2, 1));
            d = fmaxf(d, e->y + fmaxf(e->h / 2, 1));
        }
    if (zzz_step >= 0 || zzz_drawn >= 0) {            // the z's drift up beside the right eye
        c = fmaxf(c, CX + EYE_GAP + 40 + 50 + 20);
        b = fminf(b, EYE_Y - 60 - 100 - 2);
    }
    if (memcmp(&bf, &bf_drawn, sizeof bf))            // the bubble moved, grew, faded or rolled: where it was and is now
        for (int k = 0; k < 2; k++) {
            const bubf_t *q = k ? &bf_drawn : &bf;
            if (!q->on) continue;
            a = fminf(a, fminf(q->x0, CX - 150)); c = fmaxf(c, fmaxf(q->x1, CX + 150));   // (the label row)
            b = fminf(b, q->y0 - 40); d = fmaxf(d, fmaxf(q->y1, q->tail[5]) + 2);
            if (q->badge_r) { c = fmaxf(c, q->badge_x + 24); b = fminf(b, q->badge_y - 24); }   // (the badge pokes out)
            if (q->icon_k) b = fminf(b, q->icon_y - ICON_SIZE / 2 - 6);   // (the icon over the title, at its biggest)
        }
    bf_drawn = bf;
    int n = 0;
    if (c > a) { r[n][0] = (int)a - 3; r[n][1] = (int)b - 3; r[n][2] = (int)c + 3; r[n][3] = (int)d + 3; n++; }
    if (memcmp(&dots, &dots_drawn, sizeof dots)) {   // the logos changed: just the bottom of the glass, on its own
#if SCREEN_ROUND
        r[n][0] = 0; r[n][1] = CY - 160; r[n][2] = CX - DOTS_R + 44; r[n][3] = CY + 160; n++;   // (the left edge)
#else
        r[n][0] = 0; r[n][1] = CY - 160; r[n][2] = DOTS_X + 44; r[n][3] = CY + 160; n++;
#endif
    }
    dots_drawn = dots;
    if (!n) { r[0][0] = CX - 1; r[0][1] = CY - 1; r[0][2] = CX + 1; r[0][3] = CY + 1; n = 1; }   // (nothing to speak of)
    memcpy(drawn, shown, sizeof drawn);
    memcpy(col_drawn, col, sizeof col_drawn);
    zzz_drawn = zzz_step;
    strcpy(caption_drawn, caption);
    cap_kind_drawn = cap_kind;
    strcpy(cap_label_drawn, cap_label);
    ring_drawn = ring_q;
    bool crowd = crowd_on || crowd_drawn_last;        // (the crowd, or just gone: the whole page)
    crowd_drawn_last = crowd_on;
    return part && !crowd && drawn[0].w > 0 ? n : 0;
}

// One eye, anti-aliased on black. Only the pixels along edges need a square root: each row's solid middle
// is filled straight away, which keeps a frame cheap enough for smooth animation.
static void draw_eye(uint16_t *buf, int y0, int h, const eye_t *e)
{
    float rx = e->w / 2, ry = fmaxf(e->h / 2, 1);
    float sy = rx / ry, ds = fminf(1, ry / rx);       // rows stretched so the ellipse is a circle of radius rx
    float m = 0.5f / ds;                              // half a screen pixel, in stretched units
    int ya = (int)floorf(e->y - ry - 1), yb = (int)ceilf(e->y + ry + 1);
    if (ya < y0) ya = y0;
    if (yb > y0 + h) yb = y0 + h;
    float lid_y = e->y - ry + e->lid * 2 * ry, lid_n = 1 / sqrtf(1 + e->tilt * e->tilt);
    float R = e->w * 0.75f, ccy = e->y + ry + R - e->happy * ry * 1.5f;
    bool lidded = e->lid > 0.002f || fabsf(e->tilt) > 0.002f, smiling = e->happy > 0.002f;
    uint16_t solid = rgb((int)col[0], (int)col[1], (int)col[2]);
    for (int y = ya; y < yb; y++) {
        float py = (y + 0.5f - e->y) * sy;
        float ho2 = (rx + m) * (rx + m) - py * py;
        if (ho2 <= 0) continue;
        float ho = sqrtf(ho2), hi2 = (rx - m) * (rx - m) - py * py, hi = hi2 > 0 ? sqrtf(hi2) : 0;
        float dyc = y + 0.5f - ccy, hc_o = 0, hc_i = 0;   // the smile's cut-out on this row
        if (smiling && fabsf(dyc) < R + 0.5f) {
            float o2 = (R + 0.5f) * (R + 0.5f) - dyc * dyc, i2 = (R - 0.5f) * (R - 0.5f) - dyc * dyc;
            hc_o = o2 > 0 ? sqrtf(o2) : 0;
            hc_i = i2 > 0 ? sqrtf(i2) : 0;
        }
        int xa = (int)floorf(e->x - ho), xb = (int)ceilf(e->x + ho);
        if (xa < 0) xa = 0;
        if (xb > W) xb = W;
        uint16_t *row = buf + (y - y0) * W;
        for (int x = xa; x < xb; x++) {
            float px = x + 0.5f - e->x, ax = fabsf(px), a = 1;
            if (ax < hc_i) continue;                  // inside the smile's cut-out
            if (ax >= hi) a = clampf(0.5f - (sqrtf(px * px + py * py) - rx) * ds, 0, 1);
            if (ax < hc_o) a = fminf(a, clampf(0.5f - (R - sqrtf(px * px + dyc * dyc)), 0, 1));
            if (lidded) a = fminf(a, clampf(0.5f - (lid_y + e->tilt * px - (y + 0.5f)) * lid_n, 0, 1));
            if (a <= 0) continue;
            row[x] = a >= 1 ? solid : rgb((int)(col[0] * a), (int)(col[1] * a), (int)(col[2] * a));
        }
    }
}

// The countdown ring just inside the glass's edge: a faint track, and the time left from 12 o'clock clockwise
#define RING_R 219
#define RING_T 5
#if !SCREEN_ROUND
// Square: the same track round the rounded square, and the time left measured along it (so it drains at an even
// pace down the straight sides and round the corners alike)
#define RING_IN (CX - RING_R)                   // (as far in from the edge as the round one)
static float sq_along(float dx, float dy, float a, float rr)   // 0 .. 1 round the track, clockwise from the top middle
{
    float q = rr * (float)M_PI / 2, s;
    if (fabsf(dx) <= a) s = dy < 0 ? (dx >= 0 ? dx : 8 * a + 4 * q + dx) : 3 * a + 2 * q + (a - dx);   // top, bottom
    else if (fabsf(dy) <= a) s = dx > 0 ? a + q + (dy + a) : 5 * a + 3 * q + (a - dy);             // right, left
    else if (dx > 0 && dy < 0) s = a + rr * atan2f(dx - a, -(dy + a));                          // the corners
    else if (dx > 0) s = 3 * a + q + rr * atan2f(dy - a, dx - a);
    else if (dy > 0) s = 5 * a + 2 * q + rr * atan2f(-(dx + a), dy - a);
    else s = 7 * a + 3 * q + rr * atan2f(-(dy + a), -(dx + a));
    return s / (8 * a + 4 * q);
}
static void draw_ring(uint16_t *buf, int y0, int h, float frac, int bright)
{
    float half = CX - RING_IN, rr = EDGE_R - RING_IN > 8 ? EDGE_R - RING_IN : 8, band = RING_IN + RING_T;
    for (int y = y0; y < y0 + h; y++) {
        float dy = y + 0.5f - CY;
        bool edge_row = y < band + 2 || y >= H - band - 2;
        uint16_t *row = buf + (y - y0) * W;
        for (int x = 0; x < W; x++) {
            if (!edge_row && x >= band + 2 && x < W - band - 2) { x = W - band - 3; continue; }   // (just the edge)
            float dx = x + 0.5f - CX, d = sq_inside(dx, dy, half, rr);   // (from the track's middle line)
            float a = clampf(RING_T / 2.0f - fabsf(d) + 0.5f, 0, 1);
            if (a <= 0) continue;
            int v = (int)((sq_along(dx, dy, half - rr, rr) <= frac ? bright : 38) * a);
            row[x] = rgb(v, v, v);
        }
    }
}
#else
static void draw_ring(uint16_t *buf, int y0, int h, float frac, int bright)
{
    float ro = RING_R + RING_T / 2.0f, ri = RING_R - RING_T / 2.0f, lim = frac * 2 * (float)M_PI;
    int ya = CY - (int)ro - 1, yb = CY + (int)ro + 2;
    if (ya < y0) ya = y0;
    if (yb > y0 + h) yb = y0 + h;
    for (int y = ya; y < yb; y++) {
        float dy = y + 0.5f - CY, o2 = (ro + 1) * (ro + 1) - dy * dy, i2 = (ri - 1) * (ri - 1) - dy * dy;
        if (o2 <= 0) continue;
        int xo = (int)ceilf(sqrtf(o2)), xi = i2 > 0 ? (int)floorf(sqrtf(i2)) : 0;
        uint16_t *row = buf + (y - y0) * W;
        for (int x = CX - xo; x < CX + xo; x++) {
            if (x > CX - xi && x < CX + xi - 1) { x = CX + xi - 1; continue; }   // skip the middle
            if (x < 0 || x >= W) continue;
            float dx = x + 0.5f - CX, d = sqrtf(dx * dx + dy * dy);
            float a = clampf(fminf(ro - d, d - ri) + 0.5f, 0, 1);
            if (a <= 0) continue;
            float ang = atan2f(dx, -dy);
            if (ang < 0) ang += 2 * (float)M_PI;
            int v = (int)((ang <= lim ? bright : 38) * a);
            row[x] = rgb(v, v, v);
        }
    }
}
#endif

// Colour v (0..255 per channel) faded towards black by a (the screen behind the bubble is black)
static uint16_t dim(const int v[3], float a) { return rgb((int)(v[0] * a), (int)(v[1] * a), (int)(v[2] * a)); }

// Claude's mark, the real one (Simple Icons' "claude" path, rasterised by Core Graphics at 8x8 samples a pixel): 0 .. 16
// coverage for pixels cx-R-1 .. cx+R+1, stamped in Claude's colour.
#define SPARK_R 21                              // (as big as the integration logos)
#define SPARK_N (2 * SPARK_R + 3)
static const uint8_t spark_cov[SPARK_N * SPARK_N] = {
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,1,13,15,13,3,0,0,0,0,0,0,0,0,0,1,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,11,16,16,16,13,0,0,0,0,0,0,0,0,3,14,16,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,1,16,16,16,16,16,4,0,0,0,0,0,0,0,10,16,16,15,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,14,16,16,16,16,11,0,0,0,0,0,0,0,14,16,16,14,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,8,16,16,16,16,16,2,0,0,0,0,0,0,16,16,16,12,0,0,0,0,0,0,0,3,3,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,1,14,16,16,16,16,9,0,0,0,0,0,2,16,16,16,9,0,0,0,0,0,1,11,16,16,7,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,5,16,16,16,16,15,1,0,0,0,0,3,16,16,16,7,0,0,0,0,0,11,16,16,16,15,2,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,12,16,16,16,16,8,0,0,0,0,4,16,16,16,3,0,0,0,0,10,16,16,16,16,16,2,0,0,0,0,0,0,
    0,0,0,0,1,13,14,10,1,0,0,0,0,3,16,16,16,16,15,1,0,0,0,6,16,16,16,1,0,0,0,9,16,16,16,16,16,13,0,0,0,0,0,0,0,
    0,0,0,0,10,16,16,16,14,3,0,0,0,0,9,16,16,16,16,8,0,0,0,7,16,16,13,0,0,0,6,16,16,16,16,16,16,4,0,0,0,0,0,0,0,
    0,0,0,0,8,16,16,16,16,15,6,0,0,0,1,15,16,16,16,15,1,0,0,9,16,16,10,0,0,3,15,16,16,16,16,16,8,0,0,0,0,0,0,0,0,
    0,0,0,0,3,15,16,16,16,16,16,10,1,0,0,7,16,16,16,16,9,0,0,11,16,16,7,0,1,14,16,16,16,16,16,11,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,3,13,16,16,16,16,16,13,2,0,0,14,16,16,16,15,1,0,12,16,16,4,0,12,16,16,16,16,16,14,1,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,9,16,16,16,16,16,15,6,0,5,16,16,16,16,9,0,14,16,16,1,8,16,16,16,16,16,15,3,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,5,15,16,16,16,16,16,10,1,12,16,16,16,15,1,15,16,14,4,16,16,16,16,16,16,6,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,1,10,16,16,16,16,16,14,7,16,16,16,16,6,16,16,15,15,16,16,16,16,16,11,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,5,14,16,16,16,16,16,16,16,16,16,14,16,16,16,16,16,16,16,16,14,1,0,0,0,0,0,0,0,0,0,1,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,1,10,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,6,0,0,0,0,2,5,8,10,13,16,15,7,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,5,15,16,16,16,16,16,16,16,16,16,16,16,16,16,16,12,1,4,8,11,15,16,16,16,16,16,16,16,12,0,
    0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,1,11,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,15,5,0,
    0,12,16,15,14,13,12,11,10,9,8,7,6,5,3,1,0,6,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,14,10,7,3,0,0,0,
    0,11,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,14,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,15,11,7,4,1,0,0,0,0,0,0,0,
    0,1,8,10,11,12,12,13,14,15,15,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,14,10,6,3,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,2,2,4,16,16,16,16,16,16,16,16,16,16,16,15,14,12,11,10,10,10,10,9,7,6,5,3,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,6,14,16,16,16,16,16,16,16,16,16,16,16,15,16,16,16,16,16,16,16,16,16,16,16,16,11,1,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,13,16,16,16,16,16,16,16,16,16,16,16,16,16,10,3,7,11,14,16,16,16,16,16,16,16,16,16,11,0,
    0,0,0,0,0,0,0,0,0,0,0,0,2,11,16,16,16,16,11,9,16,16,16,16,16,16,16,16,16,16,10,0,0,0,2,6,10,14,16,16,16,16,16,13,0,
    0,0,0,0,0,0,0,0,0,0,1,9,16,16,16,16,15,6,5,16,16,15,13,16,16,16,16,16,16,16,16,10,0,0,0,0,0,0,1,5,9,12,7,0,0,
    0,0,0,0,0,0,0,0,0,7,15,16,16,16,16,10,1,2,15,16,16,5,14,16,15,16,16,16,16,16,16,16,12,1,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,2,12,16,16,16,16,14,5,0,1,13,16,16,10,1,16,16,7,13,16,16,16,8,14,16,16,13,1,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,6,15,16,16,16,16,10,1,0,0,10,16,16,13,1,3,16,16,5,4,16,16,16,14,2,12,16,16,14,2,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,5,16,16,16,16,14,4,0,0,0,6,16,16,15,3,0,6,16,16,4,0,10,16,16,16,10,0,10,16,16,14,3,0,0,0,0,0,0,0,0,
    0,0,0,0,0,7,16,16,16,9,1,0,0,0,3,15,16,16,6,0,0,9,16,16,2,0,1,15,16,16,16,5,0,8,16,16,15,3,0,0,0,0,0,0,0,
    0,0,0,0,0,0,7,6,3,0,0,0,0,1,14,16,16,10,0,0,0,12,16,16,1,0,0,6,16,16,16,14,1,0,6,15,16,15,4,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,12,16,16,14,1,0,0,0,15,16,15,0,0,0,0,11,16,16,16,10,0,0,3,13,16,16,3,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,8,16,16,16,3,0,0,0,2,16,16,14,0,0,0,0,2,15,16,16,16,4,0,0,1,10,16,6,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,5,16,16,16,6,0,0,0,0,6,16,16,12,0,0,0,0,0,6,16,16,16,14,1,0,0,0,3,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,2,15,16,16,7,0,0,0,0,0,9,16,16,11,0,0,0,0,0,0,11,16,16,16,3,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,7,16,16,8,0,0,0,0,0,0,12,16,16,9,0,0,0,0,0,0,1,14,16,16,5,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,2,10,8,0,0,0,0,0,0,0,15,16,16,8,0,0,0,0,0,0,0,4,10,9,1,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,16,16,16,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,15,16,16,5,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,5,14,9,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};

// Colour v at coverage a over what's there already (on black, the same as fading it; on a bubble, no dark fringe)
static void over(uint16_t *px, const int v[3], float a)
{
    uint16_t d = SWAP16(*px);
    float r = (d >> 11) * 255 / 31.0f, g = ((d >> 5) & 63) * 255 / 63.0f, b = (d & 31) * 255 / 31.0f;
    *px = rgb((int)(r + (v[0] - r) * a), (int)(g + (v[1] - g) * a), (int)(b + (v[2] - b) * a));
}

static void draw_spark(uint16_t *buf, int y0, int h, int cx, int cy, const int v[3], float alpha, float scale)
{
    int R = (int)ceilf((SPARK_R + 1) * scale);
    for (int y = cy - R; y <= cy + R; y++) {
        if (y < y0 || y >= y0 + h) continue;
        for (int x = cx - R; x <= cx + R; x++) {
            int i = (int)lroundf((x - cx) / scale) + SPARK_R + 1, j = (int)lroundf((y - cy) / scale) + SPARK_R + 1;
            if (x < 0 || x >= W || i < 0 || j < 0 || i >= SPARK_N || j >= SPARK_N) continue;
            int n = spark_cov[j * SPARK_N + i];
            if (!n) continue;
            over(&buf[(y - y0) * W + x], v, alpha * n / 16);
        }
    }
}

void pet_ui_spark(uint16_t *buf, int y0, int h, int cx, int cy, int r, int g, int b, float scale)
{
    draw_spark(buf, y0, h, cx, cy, (int[3]){r, g, b}, 1, scale);
}

// Over the bubble (under the icon): which chat (its title, or the project) and the model, e.g.
// "Access to other chats  Opus 5.5". A long title is cut to fit the glass up there.
static void draw_label_row(uint16_t *buf, int y0, int h, int y, float alpha)
{
    static const int title_col[3] = {205, 205, 215}, model_col[3] = {140, 140, 152};
    const font_t *f = &font_label;
    char t[52];
    snprintf(t, sizeof t, "%s", bl_label);
    int mw = bl_model[0] ? ftext_width(f, bl_model) : 0, room = (SCREEN_ROUND ? 300 : 380) - (mw ? mw + 12 : 0);   // (square: no curve to clear)
    if (ftext_width(f, t) > room && mw) {             // the chat's name matters more: the model makes way
        room += mw + 12;
        mw = 0;
    }
    if (ftext_width(f, t) > room) {
        int n = strlen(t), dots_w = ftext_width(f, "...");
        while (n > 0 && ftext_width_n(f, t, n) + dots_w > room) n--;
        while (n > 0 && t[n - 1] == ' ') n--;
        strcpy(t + n, "...");
    }
    int tw = t[0] ? ftext_width(f, t) : 0;
    int total = tw + (tw && mw ? 12 : 0) + mw, x = CX - total / 2;
    static const int meet_col[3] = {140, 185, 255};  // (a meeting: the calm blue of its halo)
    const int *tc = bl_kind == CAP_MEET || bl_kind == CAP_MEETQ ? meet_col : CHAT_ACCENT == 2 ? chat_col[bf.slot] : title_col;
    if (tw) { draw_ftext(buf, y0, h, x, y, f, t, dim(tc, alpha)); x += tw + 12; }
    if (mw) draw_ftext(buf, y0, h, x, y, f, bl_model, dim(model_col, alpha));
}

// The chats' dots, on an arc just inside the bottom edge, each in its chat's colour: solid while it works,
// breathing when it needs you, a ring when it's done. The shown one is bigger.
static void disc(uint16_t *buf, int y0, int h, float cx, float cy, float r, float ring, const int c[3], float k)
{
    for (int y = (int)(cy - r - 1); y <= (int)(cy + r + 1); y++) {
        if (y < y0 || y >= y0 + h) continue;
        for (int x = (int)(cx - r - 1); x <= (int)(cx + r + 1); x++) {
            if (x < 0 || x >= W) continue;
            float d = sqrtf((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy));
            float a = clampf(r + 0.5f - d, 0, 1);
            if (ring > 0) a = fminf(a, clampf(d - (r - ring) + 0.5f, 0, 1));   // hollow: just the rim
            if (a <= 0) continue;
            a *= k;
            uint16_t *p = buf + (y - y0) * W + x;
            uint16_t o = SWAP16(*p);                   // (over black, or the edge of the one beside)
            int r0 = (o >> 11) << 3, g0 = ((o >> 5) & 63) << 2, b0 = (o & 31) << 3;
            *p = rgb(r0 + (int)((c[0] - r0) * a), g0 + (int)((c[1] - g0) * a), b0 + (int)((c[2] - b0) * a));
        }
    }
}

static void draw_icon(uint16_t *buf, int y0, int h, const icon_t *ic, int cx, int cy, int n, const int col[3]);
static void draw_calendar(uint16_t *buf, int y0, int h, int cx, int cy, float k, float b, bool date);

// The chats down the left edge: each one's app (Conductor, Cursor, Claude Code's spark, Codex's >_) in the chat's own
// colour (as its title and halo), the shown one bigger; and the calendar while a meeting's near or on. In use, one
// pulses in strength (on black, that's how see-through it is); stale ones grey and darken, until the session's closed.
static void draw_dots(uint16_t *buf, int y0, int h)
{
    for (int i = 0; i < dots.n; i++) {
        int x, y;
        dot_pos(i, dots.n, &x, &y);
        y += dots.bob[i];
        float sc = i == dots.sel ? 0.7f : 0.52f, k = 1;
        if (dots.st[i] == AG_WORKING || dots.st[i] == AG_WAITING) {   // in use: fades in and out (35% .. 100%)
            int ph = dots.st[i] == AG_WAITING ? dots.pulse2 : dots.pulse;
            k = 0.35f + 0.65f * (0.5f + 0.5f * cosf(ph / 16.0f * 2 * (float)M_PI));
        }
        const char *a = dots.ag[i];
        if (!strcmp(a, "calendar")) { draw_calendar(buf, y0, h, x, y, sc * 0.85f, k, false); continue; }
        const icon_t *ic = NULL;
        for (const icon_t *q = icons_ui; q->key; q++) if (!strcmp(q->key, a)) { ic = q; break; }
        int base[3];                                  // in its chat's colour, like its title and halo
        memcpy(base, chat_col[dots.slot[i]], sizeof base);
        float f = dots.fade[i] / 15.0f, lum = (base[0] * 0.3f + base[1] * 0.59f + base[2] * 0.11f) * 0.75f;
        int c[3];                                     // stale: towards a dim grey
        for (int q = 0; q < 3; q++) c[q] = (int)((base[q] + (lum - base[q]) * f) * (1 - 0.6f * f) * k);
        if (!strcmp(a, "claude")) { draw_spark(buf, y0, h, x, y, c, 1, sc); continue; }
        if (!strcmp(a, "codex")) {
            const font_t *ft = &font_label;
            draw_ftext(buf, y0, h, x - ftext_width(ft, ">_") / 2, y - ft->line_h / 2, ft, ">_", rgb(c[0], c[1], c[2]));
            continue;
        }
        if (ic) draw_icon(buf, y0, h, ic, x, y, (int)lroundf(ICON_SIZE * sc), c);
        else disc(buf, y0, h, x, y, 12 * sc, 0, c, 1);
    }
    for (int i = 0; i < dots.n; i++) {                // finished: a green dot by its logo until the chat starts again
        if (dots.st[i] != AG_DONE) continue;
        int x, y;
        dot_pos(i, dots.n, &x, &y);
        float sc = i == dots.sel ? 0.7f : 0.52f, off = ICON_SIZE * sc * 0.5f;
        static const int black[3] = {0, 0, 0}, green[3] = {52, 199, 89};
        disc(buf, y0, h, x + off, y + dots.bob[i] - off, 6.5f, 0, black, 1);   // (a black rim keeps it off the logo)
        disc(buf, y0, h, x + off, y + dots.bob[i] - off, 4.5f, 0, green, 1);
    }
}

// The "done" badge: a green disc with a white tick, anti-aliased (coverage by distance to the edge and the tick's
// two strokes)
static float seg_dist(float px, float py, float ax, float ay, float bx, float by)
{
    float vx = bx - ax, vy = by - ay, t = clampf(((px - ax) * vx + (py - ay) * vy) / (vx * vx + vy * vy), 0, 1);
    float dx = px - ax - t * vx, dy = py - ay - t * vy;
    return sqrtf(dx * dx + dy * dy);
}

static void draw_badge(uint16_t *buf, int y0, int h, int cx, int cy, int r)
{
    float k = r / 16.0f, w = 2.0f * k;                 // tick: (-6,0) -> (-2,5) -> (7,-5) at full size, ~4 px thick
    for (int y = cy - r - 1; y <= cy + r + 1; y++) {
        if (y < y0 || y >= y0 + h) continue;
        for (int x = cx - r - 1; x <= cx + r + 1; x++) {
            if (x < 0 || x >= W) continue;
            float px = x + 0.5f - cx, py = y + 0.5f - cy;
            float disc = clampf(r + 0.5f - sqrtf(px * px + py * py), 0, 1);
            if (disc <= 0) continue;
            float d = fminf(seg_dist(px, py, -6 * k, 0, -2 * k, 5 * k), seg_dist(px, py, -2 * k, 5 * k, 7 * k, -5 * k));
            float tick = clampf(w + 0.5f - d, 0, 1);
            int cr = 52 + (int)((255 - 52) * tick), cg = 199 + (int)((255 - 199) * tick), cb = 89 + (int)((255 - 89) * tick);
            uint16_t *pp = buf + (y - y0) * W + x;
            if (disc >= 1) { *pp = rgb(cr, cg, cb); continue; }
            uint16_t d16 = SWAP16(*pp);                // the edge: blend over what's there (bubble or black)
            int br = (d16 >> 11) << 3, bg = ((d16 >> 5) & 63) << 2, bb = (d16 & 31) << 3;
            *pp = rgb(br + (int)((cr - br) * disc), bg + (int)((cg - bg) * disc), bb + (int)((cb - bb) * disc));
        }
    }
}

// A little calendar page (red top, today's date), size k (1: 42 px wide), at brightness b (on black)
static void draw_calendar(uint16_t *buf, int y0, int h, int cx, int cy, float k, float b, bool date)
{
    int w = (int)lroundf(42 * k), hh = (int)lroundf(40 * k), r = (int)lroundf(6 * k);
    if (w < 4) return;
    uint16_t white = rgb((int)(240 * b), (int)(240 * b), (int)(245 * b)), red = rgb((int)(235 * b), (int)(80 * b), (int)(70 * b));
    fill_rrect(buf, y0, h, cx - w / 2, cy - hh / 2, cx + w / 2, cy + hh / 2, r, white);
    fill_rrect(buf, y0, h, cx - w / 2, cy - hh / 2, cx + w / 2, cy - hh / 2 + hh / 3, r, red);
    fill_rect(buf, y0, h, cx - w / 2, cy - hh / 2 + hh / 6, cx + w / 2, cy - hh / 2 + hh / 3, red);
    if (!date) return;
    time_t t = time(NULL);
    struct tm d;
    localtime_r(&t, &d);
    char day[4];
    snprintf(day, sizeof day, "%d", d.tm_mday);
    const font_t *f = &font_label;
    int c = (int)(30 * b);
    draw_ftext(buf, y0, h, cx - ftext_width(f, day) / 2, cy - hh / 2 + hh / 3 + (hh * 2 / 3 - f->line_h) / 2, f, day, rgb(c, c, c + 6));
}

// The icon over the title: the integration in use (its logo), or else who made the model doing the work (Claude's
// spark, OpenAI's >_, Gemini, Cursor). (The app it's in is the mark along the bottom.) Its initial if there's no logo.
// k is its size (springing in: 0 .. a little over 1).
static void draw_top_icon(uint16_t *buf, int y0, int h, int cx, int cy, float k)
{
    static const int claude_col[3] = {217, 119, 87}, other_col[3] = {120, 120, 135};
    const char *key = bl_tool[0] ? bl_tool : bl_provider[0] ? bl_provider : bl_agent;
    if (!strcmp(key, "meeting")) { draw_calendar(buf, y0, h, cx, cy, k, 1, k > 0.8f); return; }
    if (!strcmp(key, "claude")) {
        draw_spark(buf, y0, h, cx, cy, claude_col, 1, k);
        return;
    }
    if (!strcmp(key, "openai") || !strcmp(key, "codex")) {   // (no brand logo to borrow: a terminal prompt)
        const font_t *f = &font_big;
        if (k > 0.8f) draw_ftext(buf, y0, h, cx - ftext_width(f, ">_") / 2, cy - f->line_h / 2, f, ">_", rgb(235, 235, 240));
        return;
    }
    const icon_t *ic = NULL;
    for (const icon_t *i = icons_ui; i->key; i++) if (!strcmp(i->key, key)) { ic = i; break; }
    if (ic) { draw_icon(buf, y0, h, ic, cx, cy, (int)lroundf(ICON_SIZE * k), (int[3]){ic->r, ic->g, ic->b}); return; }
    if (!bl_tool[0]) { disc(buf, y0, h, cx, cy, 10 * k, 0, other_col, 1); return; }   // an unknown maker: a plain dot
    char l[2] = {(char)(key[0] >= 'a' && key[0] <= 'z' ? key[0] - 32 : key[0]), 0};   // an integration without a logo
    const font_t *f = &font_body;
    if (k > 0.8f) draw_ftext(buf, y0, h, cx - ftext_width(f, l) / 2, cy - f->line_h / 2, f, l, rgb(200, 200, 210));
}

// A logo n px across, centred on cx, cy, in colour col (over what's there)
static void draw_icon(uint16_t *buf, int y0, int h, const icon_t *ic, int cx, int cy, int n, const int col[3])
{
    int ox = cx - n / 2, oy = cy - n / 2;
    for (int y = 0; y < n; y++) {
        int py = oy + y;
        if (py < y0 || py >= y0 + h) continue;
        for (int x = 0; x < n; x++) {
            int sx = x * ICON_SIZE / n, sy = y * ICON_SIZE / n, q = sy * ICON_SIZE + sx;
            int a = (ic->mask[q / 2] >> (q & 1 ? 0 : 4)) & 15;
            if (!a || ox + x < 0 || ox + x >= W) continue;
            over(&buf[(py - y0) * W + ox + x], col, a / 15.0f);
        }
    }
}

// Draw the bubble as bubble_frame worked it out. The rolling text stays inside the box's text area, and a line
// fades out (or in) as it crosses the top or bottom.
static void draw_bubble(uint16_t *buf, int y0, int h)
{
    const font_t *f = bl_font;
    float ta = bf.ta / 64.0f;
    if (ta > 0) draw_label_row(buf, y0, h, bf.y0 - 38, ta);   // (comes and goes with the words)
    uint16_t bgc = rgb(bf.bg[0], bf.bg[1], bf.bg[2]);
    if (CHAT_ACCENT == 1) {                          // a ring in the chat's colour
        float sa = bf.bg[0] + bf.bg[1] + bf.bg[2] ? 1 : 0, k = (bf.ta / 64.0f) * sa;
        const int *c = chat_col[bf.slot];
        fill_rrect(buf, y0, h, bf.x0 - 3, bf.y0 - 3, bf.x1 + 3, bf.y1 + 3, bf.r + 3, rgb((int)(c[0] * k), (int)(c[1] * k), (int)(c[2] * k)));
    }
    fill_rrect(buf, y0, h, bf.x0, bf.y0, bf.x1, bf.y1, bf.r, bgc);
    fill_tri(buf, y0, h, bf.tail[0], bf.tail[1], bf.tail[2], bf.tail[3], bf.tail[4], bf.tail[5], bgc);
    if (ta <= 0) return;
    int top, bot, ty0 = bf.y0 + BUBBLE_PY, ty1 = bf.y1 - BUBBLE_PY, tx = (bf.x0 + bf.x1) / 2 - bl_tw / 2;
    gfx_get_clip(&top, &bot);
    gfx_clip(top > ty0 ? top : ty0, bot < ty1 ? bot : ty1);
    for (int i = 0; i < bl_n; i++) {
        int y = ty0 + i * f->line_h - bf.scroll;
        float a = fminf(1, fminf((float)(y + f->line_h - ty0), (float)(ty1 - y)) / f->line_h);
        if (a <= 0) continue;
        a = a * a * ta;                               // (gone well before it's cut in half)
        draw_ftext_n(buf, y0, h, tx, y, f, bl_line[i], bl_len[i],
                     rgb(bf.bg[0] + (int)((bf.fg[0] - bf.bg[0]) * a), bf.bg[1] + (int)((bf.fg[1] - bf.bg[1]) * a),
                         bf.bg[2] + (int)((bf.fg[2] - bf.bg[2]) * a)));
    }
    gfx_clip(top, bot);
    if (bf.badge_r > 1) draw_badge(buf, y0, h, bf.badge_x, bf.badge_y, bf.badge_r);
    if (bf.icon_k > 2) draw_top_icon(buf, y0, h, CX, bf.icon_y, bf.icon_k / 64.0f);
}

// Pairing with the Mac: a card over everything with the code to type into Agent Bridge, then "Paired" for a moment
static void draw_pairing(uint16_t *buf, int y0, int h)
{
    fill_rrect(buf, y0, h, CX - 170, CY - 92, CX + 170, CY + 122, 28, rgb(30, 30, 38));
    const font_t *s = &font_label, *b = &font_big;
    if (!strcmp(pair_shown, "ok")) {
        draw_badge(buf, y0, h, CX, CY - 22, 26);
        draw_ftext(buf, y0, h, CX - ftext_width(b, "Paired") / 2, CY + 14, b, "Paired", rgb(235, 235, 240));
        return;
    }
    char code[10];                                    // "482 913": easier to read off
    snprintf(code, sizeof code, "%.3s %.3s", pair_shown, pair_shown + 3);
    char t1[48];                                      // (its name: the one to pick in the app, when there are several)
    snprintf(t1, sizeof t1, "Pair %s with your Mac", agent_device_name());
    const char *t2 = "Type this code in the Bobble app";
    const char *t3 = "Not you? Tap to close";
    if (ftext_width(s, t1) > 320) snprintf(t1, sizeof t1, "%s", agent_device_name());
    draw_ftext(buf, y0, h, CX - ftext_width(s, t1) / 2, CY - 72, s, t1, rgb(160, 160, 172));
    draw_ftext(buf, y0, h, CX - ftext_width(b, code) / 2, CY - 34, b, code, rgb(255, 255, 255));
    draw_ftext(buf, y0, h, CX - ftext_width(s, t2) / 2, CY + 48, s, t2, rgb(120, 120, 132));
    draw_ftext(buf, y0, h, CX - ftext_width(s, t3) / 2, CY + 82, s, t3, rgb(90, 90, 100));
}

// The halo: brightest at the edge of the glass, fading out over ~55 px towards the middle, faint
#define HALO_IN 178
#define HALO_OUT 233
#if !SCREEN_ROUND
static void draw_halo(uint16_t *buf, int y0, int h) {}   // square: none (a glow round a square edge looked wrong)
#else
static void draw_halo(uint16_t *buf, int y0, int h)
{
    if (!halo_q[0] && !halo_q[1] && !halo_q[2]) return;
    float peak = 0.42f;
    for (int y = y0; y < y0 + h; y++) {
        float dy = y + 0.5f - CY, o2 = HALO_OUT * HALO_OUT - dy * dy, i2 = HALO_IN * HALO_IN - dy * dy;
        if (o2 <= 0) continue;
        int xo = (int)ceilf(sqrtf(o2)), xi = i2 > 0 ? (int)floorf(sqrtf(i2)) : 0;
        uint16_t *row = buf + (y - y0) * W;
        for (int x = CX - xo; x < CX + xo; x++) {
            if (x > CX - xi && x < CX + xi - 1) { x = CX + xi - 1; continue; }   // (just the band near the edge)
            if (x < 0 || x >= W) continue;
            float dx = x + 0.5f - CX, d = sqrtf(dx * dx + dy * dy);
            float a = clampf((d - HALO_IN) / (HALO_OUT - HALO_IN), 0, 1);
            a = a * a * peak;
            if (a <= 0.004f) continue;
            row[x] = rgb((int)(halo[0] * a), (int)(halo[1] * a), (int)(halo[2] * a));   // (on black)
        }
    }
}
#endif

void pet_ui_draw(uint16_t *buf, int y0, int h)
{
    if (crowd_on) {                                   // the square's crowd instead of the one pet
        if (ring_q >= 10000) draw_ring(buf, y0, h, 1, (ring_q - 10000) % 2 ? 90 : 255);
        else if (ring_q >= 0) draw_ring(buf, y0, h, ring_q / (2.0f * MT_LEAD), ring_q <= 60 ? 255 : 170);
        crowd_draw(buf, y0, h);
        if (pair_shown[0]) draw_pairing(buf, y0, h);
        return;
    }
    draw_halo(buf, y0, h);
    if (ring_q >= 10000) {                            // starting: the full ring flashes
        int ph = ring_q - 10000;
        draw_ring(buf, y0, h, 1, ph % 2 ? 90 : 255);
    } else if (ring_q >= 0) {
        draw_ring(buf, y0, h, ring_q / (2.0f * MT_LEAD), ring_q <= 60 ? 255 : 170);
    }
    if (bf.on) draw_bubble(buf, y0, h);
    for (int i = 0; i < 2; i++) draw_eye(buf, y0, h, &shown[i]);
    if (dots.n) draw_dots(buf, y0, h);
    if (pair_shown[0]) draw_pairing(buf, y0, h);
    if (zzz_step >= 0) {                              // fast asleep: z's drifting up
        float p = zzz_step / 12.0f / 2.4f;
        for (int k = 0; k < 3; k++) {
            float q = p + k / 3.0f;
            q -= floorf(q);
            float a = sinf((float)M_PI * q);
            int sc = q > 0.5f ? 3 : 2;
            draw_text(buf, y0, h, CX + EYE_GAP + 40 + (int)(q * 50), EYE_Y - 60 - (int)(q * 100), sc, "z",
                      rgb((int)(col[0] * a), (int)(col[1] * a), (int)(col[2] * a)));
        }
    }
    if (caption[0] && cap_kind == CAP_PLAIN) text_c(buf, y0, h, 372, 2, caption, rgb(150, 150, 170));
}

// ---------------------------------------------------------------- the square's crowd
// Bobble Square never shows the eyes: a pixel character per chat along the bottom (sprites_ui: the knight, wizard,
// robot..., by the chat's slot), acting out its state (marching and tapping away while it works, hopping and waving
// when it needs you, cheering when it's done, standing greyed when stopped) under its own bubble (its logo and title,
// then its words), up to CROWD_MAX side by side, the most pressing ones. A chat finished or stopped for a minute loses
// its bubble and walks off to wait in a line along the left edge, smaller and dimmer, till it's busy again or closes
// (characters always walk to a new place, never slide). With no chats, or a meeting to announce, Bobble's own character (the host) is there too, saying
// what the eyes would have (the welcome, a meeting); a tap on it during the welcome goes on to Wi-Fi setup.
#if !SCREEN_ROUND
#define CROWD_MAX 3
#define CROWD_PX 5                            // screen pixels to a sprite pixel
#define CROWD_FEET (H - 14)                   // where the characters stand
#define CROWD_HEAD (CROWD_FEET - SPRITE_H * CROWD_PX)
#define CROWD_TOP 46                          // the bubbles reach no higher than this (clear of the status corner)
#define CROWD_PAD 12
#define CROWD_LINES 10
#define CROWD_POOL (AGENT_MAX + 2)            // characters at once (every chat and the host, and one leaving)
#define REST_AFTER_US 60000000                // finished or stopped this long: its bubble goes, it walks off to wait
#define REST_MAX 4                            // the waiting line along the left edge: this many, this far apart, this
#define REST_STEP 44                          // size (as a share of the others')
#define REST_SCALE 0.75f
#define WALK_PX_S 150                         // walking speed
#define HOST_SLOT AGENT_MAX                   // the host's place in crowd_chat (after the chats' slots)
#define HOST_WHO 2                            // ...and its character (the robot, like Bobble's robot eyes)

typedef struct {
    int slot;                                 // the chat's (-1: free)
    bool live;                                // wanted this frame (else shrinking away)
    float x, cw, k, kv;                       // centre and column width (eased), size (a spring, 0 .. 1)
    int64_t next_blink, blink_t0, hop_t0, next_hop;
    agent_state_t state;
    float phase;                              // (so they don't all march in step)
    pose_t pose;
    float dy;                                 // hopping: px up
    float sc;                                 // its size (1, or REST_SCALE waiting at the side), eased
    bool resting, walking;
} mascot_t;
static mascot_t crowd[CROWD_POOL];   // (these: every frame, so internal RAM: PSRAM's shared with the scan-out)
static agent_chat_t crowd_chat[AGENT_MAX + 1];   // by slot (and the host's)
static int crowd_more;                        // chats up that don't fit
static int16_t crowd_q[CROWD_POOL][12];    // what the last frame showed, roughly (anything changed: redraw)
static uint32_t crowd_sum;
// The choices over a waiting chat's bubble ("Yes" / "No", a question's answers), as last drawn: where to tap
#define PILL_GAP 8
static struct { int slot, n, r[AGENT_OPTS][4]; } pills[CROWD_POOL];
static int ask_n;                             // chats waiting on a choice (the first has the screen; "1 of 3")
// How the choices look: 1 inside the bubble, over the question, as pills in the chat's colour; 2 a numbered list over
// the bubble, like a terminal's
#ifndef ASK_STYLE
#define ASK_STYLE 1
#endif

static const int urgency[] = {[AG_IDLE] = 0, [AG_STOPPED] = 1, [AG_DONE] = 2, [AG_WORKING] = 3, [AG_WAITING] = 4};
#define PEEK_US 30000000                      // a resting one tapped: back up front with its last bubble this long
static int peek_slot = -1;
static int64_t peek_until;
static int crowd_rank(const agent_chat_t *c)      // which matter most (a meeting announced, or one you tapped, most of all; resting least)
{
    if (c->slot == HOST_SLOT) return c->state == AG_WAITING ? 5 : 0;
    if (c->slot == peek_slot) return 5;
    return c->bubble ? urgency[c->state] : 0;
}
static const float crowd_pal[5][6] = {{0}, {40, 40, 50, 228, 228, 236}, {244, 244, 247, 22, 22, 28},
                                      {208, 240, 218, 16, 72, 40}, {34, 34, 40, 118, 118, 128}};   // (by state, as the big bubble)

static mascot_t *crowd_find(int slot, float x, float cw, int64_t now)
{
    for (int i = 0; i < CROWD_POOL; i++) if (crowd[i].slot == slot) return &crowd[i];
    for (int i = 0; i < CROWD_POOL; i++) {
        mascot_t *m = &crowd[i];
        if (m->slot >= 0) continue;
        memset(m, 0, sizeof *m);
        m->slot = slot;
        m->x = x;
        m->cw = cw;
        m->state = AG_IDLE;
        m->blink_t0 = m->hop_t0 = -1;
        m->next_blink = now + (int64_t)(frand(1, 4) * 1e6f);
        m->phase = frand(0, 6.28f);
        return m;
    }
    return NULL;
}

static void crowd_update(int64_t now, float dt)
{
    static bool init;
    if (!init) { init = true; for (int i = 0; i < CROWD_POOL; i++) crowd[i].slot = -1; }
    static agent_chat_t all[AGENT_MAX + 1];           // (3 KB: off the main task's stack)
    agent_chat_t *pick[AGENT_MAX + 1], *rest[AGENT_MAX];
    int n = agent_get_chats(all), m = 0, r = 0;
    // a chat waiting on a choice takes the whole screen, the one that asked first; the rest step aside till it's
    // answered, then the next one asking goes
    agent_chat_t *ask = NULL;
    ask_n = 0;
    for (int i = 0; i < n; i++)
        if (all[i].state == AG_WAITING && all[i].nopt) {
            ask_n++;
            if (!ask || all[i].since < ask->since) ask = &all[i];
        }
    for (int i = 0; i < n; i++) {                     // the busy ones up front; the quiet ones (a minute done) off to the side
        agent_chat_t *c = &all[i];
        bool quiet = (c->state == AG_DONE || c->state == AG_STOPPED) && now - c->since > REST_AFTER_US;
        if (c->slot == peek_slot && now < peek_until) { quiet = false; c->bubble = true; }   // (tapped: its last words again)
        if (ask) quiet = c != ask;
        if (quiet) { c->bubble = false; rest[r++] = c; }
        else pick[m++] = c;
    }
    // the host: what the eyes would be saying (the welcome, a meeting), when no chat's up front or there's a meeting
    const char *say = cap_kind == CAP_MEET || cap_kind == CAP_MEETQ || cap_kind == CAP_HELLO || (cap_kind == CAP_PLAIN && caption[0])
                      ? caption : mt_caption;
    if (!ask && (!m || say[0])) {
        agent_chat_t *hc = &all[n];
        memset(hc, 0, sizeof *hc);
        hc->slot = HOST_SLOT;
        hc->bubble = say[0] != 0;
        hc->state = cap_kind == CAP_MEET || cap_kind == CAP_HELLO ? AG_WAITING : AG_IDLE;   // (announcing: waving)
        strlcpy(hc->detail, say, sizeof hc->detail);
        pick[m++] = hc;
    }
    for (int i = 1; i < m; i++)                       // the most pressing first (needs you, working, ...; newest among equals)
        for (int j = i; j > 0; j--) {
            agent_chat_t *a = pick[j - 1], *b = pick[j];
            int ra = crowd_rank(a), rb = crowd_rank(b);
            if (rb > ra || (rb == ra && b->since > a->since)) { pick[j - 1] = b; pick[j] = a; }
            else break;
        }
    crowd_more = (m > CROWD_MAX ? m - CROWD_MAX : 0) + (r > REST_MAX ? r - REST_MAX : 0);
    if (m > CROWD_MAX) m = CROWD_MAX;
    if (r > REST_MAX) r = REST_MAX;
    for (int i = 1; i < m; i++)                       // ...then left to right by slot, so each keeps its place
        for (int j = i; j > 0 && pick[j]->slot < pick[j - 1]->slot; j--) { agent_chat_t *t = pick[j]; pick[j] = pick[j - 1]; pick[j - 1] = t; }
    for (int i = 1; i < r; i++)
        for (int j = i; j > 0 && rest[j]->slot < rest[j - 1]->slot; j--) { agent_chat_t *t = rest[j]; rest[j] = rest[j - 1]; rest[j - 1] = t; }

    // where each one goes: the waiting line along the left, the rest of the width shared by those up front
    float lane = r ? 26 + (r - 1) * REST_STEP + 34 : 0, cw = (W - lane) / m;
    for (int i = 0; i < CROWD_POOL; i++) crowd[i].live = false;
    uint32_t sum = crowd_more;
    crowd_awake = false;
    for (int i = 0; i < m + r; i++) {
        bool resting = i >= m;
        agent_chat_t *c = resting ? rest[i - m] : pick[i];
        float x = resting ? 26 + (i - m) * REST_STEP : lane + cw * (i + 0.5f);
        mascot_t *ms = crowd_find(c->slot, x, cw, now);
        if (!ms) continue;
        crowd_chat[c->slot] = *c;
        ms->live = true;
        ms->resting = resting;
        if (ms->k < 0.05f) { ms->x = x; ms->sc = resting ? REST_SCALE : 1; }   // (just arrived: there already)
        float d = x - ms->x, step = WALK_PX_S * dt;   // walking there, at a steady pace
        ms->walking = fabsf(d) > 1;
        ms->x = fabsf(d) <= step ? x : ms->x + (d > 0 ? step : -step);
        ms->cw = ms->k < 0.05f ? cw : ease(ms->cw, cw, 8, dt);
        ms->sc = ease(ms->sc, resting ? REST_SCALE : 1, 4, dt);
        if (c->state != ms->state) {                  // a change of heart: a hop for news (needs you, done)
            if (ms->state != AG_IDLE && (c->state == AG_WAITING || c->state == AG_DONE)) ms->hop_t0 = now;
            ms->next_hop = now + 4000000;
            ms->state = c->state;
        }
        if (c->bubble && (c->slot != HOST_SLOT || cap_kind == CAP_MEET)) crowd_awake = true;   // (as the eyes' bubble)
        sum = sum * 31 + c->bubble;
        for (const char *p = c->detail; *p; p++) sum = sum * 31 + (uint8_t)*p;
        for (const char *p = c->title; *p; p++) sum = sum * 31 + (uint8_t)*p;
        sum = sum * 31 + c->state;
        sum = sum * 31 + c->nopt * 8 + c->picked + 1;
        for (int o = 0; o < c->nopt; o++) for (const char *p = c->opt[o]; *p; p++) sum = sum * 31 + (uint8_t)*p;
    }

    sum = sum * 31 + ask_n;
    bool any = false, changed = sum != crowd_sum;
    crowd_sum = sum;
    float t = now / 1e6f;
    for (int i = 0; i < CROWD_POOL; i++) {
        mascot_t *ms = &crowd[i];
        if (ms->slot < 0) continue;
        float w = 14, target = ms->live ? 1 : 0;      // growing in / shrinking away: a lively spring in, quick out
        float st = dt / 4;
        for (int k = 0; k < 4; k++) { ms->kv += (w * w * (target - ms->k) - 1.3f * w * ms->kv) * st; ms->k += ms->kv * st; }
        if (!ms->live && ms->k < 0.02f) { ms->slot = -1; changed = true; continue; }
        any = true;
        pose_t p = {ARM_DOWN, ARM_DOWN, 0, false};
        float dy = 0, since = (now - crowd_chat[ms->slot].since) / 1e6f;
        int beat = (int)floorf(t * 3 + ms->phase);    // (a third of a second each)
        if (ms->walking) {                            // on the way: striding, arms swinging
            int stride = (int)floorf(t * 7);
            p.step = (stride & 1) + 1;
            p.arm_l = stride & 1 ? ARM_OUT : ARM_DOWN;
            p.arm_r = stride & 1 ? ARM_DOWN : ARM_OUT;
            dy = stride & 1 ? 2 : 0;
        } else switch (ms->resting ? AG_IDLE : ms->state) {
        case AG_WORKING:                              // marching in place, one hand tapping away
            p.step = (beat & 1) + 1;
            p.arm_r = beat % 4 < 2 ? ARM_OUT : ARM_DOWN;
            break;
        case AG_WAITING:                              // waving at you, a hop now and then
            p.arm_r = beat & 1 ? ARM_UP : ARM_OUT;
            if (now >= ms->next_hop) { ms->hop_t0 = now; ms->next_hop = now + 4000000; }
            break;
        case AG_DONE:                                 // cheering, for a few seconds; then just standing happily
            if (since < 4) { p.arm_l = p.arm_r = ARM_UP; dy = fabsf(sinf(t * 7)) * 6; }
            break;
        default: break;                               // stopped, waiting at the side, the host with nothing to say: still
        }
        if (now >= ms->next_blink) { ms->blink_t0 = now; ms->next_blink = now + (int64_t)(frand(2, 6) * 1e6f); }
        p.blink = ms->blink_t0 >= 0 && now - ms->blink_t0 < 140000;
        if (ms->hop_t0 >= 0 && now - ms->hop_t0 < 350000) dy += 18 * sinf((float)M_PI * (now - ms->hop_t0) / 350000.0f);
        ms->pose = p;
        ms->dy = dy;
        int16_t q[12] = {(int16_t)ms->slot, (int16_t)lroundf(ms->x), (int16_t)lroundf(ms->cw), (int16_t)lroundf(ms->k * 100),
                         (int16_t)lroundf(dy), p.arm_l, p.arm_r, p.step, p.blink, (int16_t)ms->state, ms->resting,
                         (int16_t)lroundf(ms->sc * 100)};
        if (memcmp(q, crowd_q[i], sizeof q)) { memcpy(crowd_q[i], q, sizeof q); changed = true; }
    }
    if (any != crowd_on) changed = true;
    crowd_on = any;
    if (changed) dirty = true;
}

static bool crowd_tap(int x, int y, int64_t now)
{
    if (!crowd_on) return false;
    for (int i = 0; i < CROWD_POOL; i++) {         // a character waiting at the side: back up front with its last bubble
        const mascot_t *ms = &crowd[i];            // (or, tapped again while it's there, off it goes)
        if (ms->slot < 0 || ms->slot == HOST_SLOT || !ms->live) continue;
        float half = SPRITE_W * CROWD_PX * ms->sc / 2 + 8;
        if (fabsf(x - ms->x) > half || y < CROWD_FEET - SPRITE_H * CROWD_PX * ms->sc - 12) continue;
        bool peeked = ms->slot == peek_slot && now < peek_until;
        if (!ms->resting && !peeked) continue;
        peek_slot = peeked ? -1 : ms->slot;
        peek_until = peeked ? 0 : now + PEEK_US;
        dirty = true;
        return true;
    }
    for (int i = 0; i < CROWD_POOL; i++) {         // a choice over a bubble: the answer, sent on to the Mac
        if (crowd[i].slot < 0 || crowd[i].slot != pills[i].slot) continue;
        for (int o = 0; o < pills[i].n; o++) {
            const int *r = pills[i].r[o];
            if (x >= r[0] - PILL_GAP / 2 && x <= r[2] + PILL_GAP / 2 && y >= r[1] - PILL_GAP / 2 && y <= r[3] + PILL_GAP / 2) {
                agent_answer(crowd[i].slot, o);
                return true;
            }
        }
    }
    for (int i = 0; i < CROWD_POOL; i++) {         // (a chat's character: nothing yet)
        mascot_t *ms = &crowd[i];
        if (ms->slot != HOST_SLOT || !ms->live || fabsf(x - ms->x) > ms->cw / 2) continue;
        if (cap_kind == CAP_HELLO && welcome == 1) wants_wifi = true;   // the welcome: on to Wi-Fi setup
    }
    return true;
}

// Wrap s to width tw in font f: up to max lines (the last cut with "..." if there's more); how many
static int crowd_wrap(const char *s, const font_t *f, int tw, const char **line, int *len, int max, bool *cut)
{
    int n = 0;
    *cut = false;
    while (*s == ' ') s++;
    while (*s && n < max) {
        int fit = 0, brk = 0;
        while (s[fit] && ftext_width_n(f, s, fit + 1) <= tw) if (s[++fit] == ' ') brk = fit;
        if (!s[fit]) brk = fit;
        else if (!brk) brk = fit ? fit : 1;
        line[n] = s;
        len[n++] = brk;
        s += brk;
        while (*s == ' ') s++;
    }
    if (*s && n) {                                    // more than fits: the last line ends "..."
        *cut = true;
        int dw = ftext_width(f, "...");
        while (len[n - 1] > 0 && ftext_width_n(f, line[n - 1], len[n - 1]) + dw > tw) len[n - 1]--;
        while (len[n - 1] > 0 && line[n - 1][len[n - 1] - 1] == ' ') len[n - 1]--;
    }
    return n;
}

static void crowd_logo(uint16_t *buf, int y0, int h, const char *agent, int x, int y, const int c[3])
{
    if (!strcmp(agent, "claude")) { draw_spark(buf, y0, h, x, y, c, 1, 0.5f); return; }
    if (!strcmp(agent, "codex")) {
        const font_t *ft = &font_label;
        draw_ftext(buf, y0, h, x - ftext_width(ft, ">_") / 2, y - ft->line_h / 2, ft, ">_", rgb(c[0], c[1], c[2]));
        return;
    }
    for (const icon_t *q = icons_ui; q->key; q++)
        if (!strcmp(q->key, agent)) { draw_icon(buf, y0, h, q, x, y, 22, c); return; }
    disc(buf, y0, h, x, y, 7, 0, c, 1);
}

static void crowd_draw(uint16_t *buf, int y0, int h)
{
    const font_t *f = &font_label;
    for (int pass = 0; pass < 2 * CROWD_POOL; pass++) {   // the waiting line first (behind), then the ones up front
        int i = pass % CROWD_POOL;
        const mascot_t *ms = &crowd[i];
        if (pass < CROWD_POOL) pills[i].n = 0;
        if (ms->slot < 0 || ms->k < 0.05f || ms->resting != (pass < CROWD_POOL)) continue;
        const agent_chat_t *c = &crowd_chat[ms->slot];
        if (ms->resting) {                            // waiting at the side: just itself, smaller and dimmer
            float k = clampf(ms->k, 0, 1);
            sprite_draw(buf, y0, h, ms->slot, &ms->pose, ms->x, CROWD_FEET - ms->dy, CROWD_PX * k * ms->sc, 0.6f * fminf(1, k * 1.5f));
            continue;
        }
        bool host = ms->slot == HOST_SLOT, resting = !host && !c->bubble;
        static const int host_col[3] = {205, 205, 215};
        static const float quiet[6] = {40, 40, 50, 190, 190, 200};
        const int *cc = host ? host_col : chat_col[ms->slot];
        float k = clampf(ms->k, 0, 1), a = k * k;     // (grows in from black)
        int st = c->state >= AG_WORKING && c->state <= AG_STOPPED ? c->state : AG_WORKING;
        const float *pal = host ? (c->state == AG_WAITING ? crowd_pal[AG_WAITING] : quiet) : resting ? crowd_pal[AG_STOPPED] : crowd_pal[st];
        bool light = pal[0] > 128;
        float dimk = st == AG_STOPPED && !host ? 0.5f : resting ? 0.6f : 1;
        if (host && !c->bubble) {                     // the host with nothing to say: just itself
            sprite_draw(buf, y0, h, HOST_WHO, &ms->pose, ms->x, CROWD_FEET - ms->dy, CROWD_PX * k * ms->sc, fminf(1, k * 1.5f));
            continue;
        }

        // the bubble: logo and title, then the words, over its character with the tail pointing down at it
        // (waiting on a choice, it has the screen: the whole width, over the ones that stepped aside)
        bool asking = !host && !resting && c->state == AG_WAITING && c->nopt;
        int x0 = asking ? 8 : (int)lroundf(ms->x - ms->cw / 2 + 8), x1 = asking ? W - 8 : (int)lroundf(ms->x + ms->cw / 2 - 8);
        int tw = x1 - x0 - 2 * CROWD_PAD;
        int y1 = CROWD_HEAD - 18;
        const font_t *tf = &font_small;               // the title: small, wrapped beside the logo, all of it (titles are
        const char *tline[4];                         // under 48 characters: four narrow lines hold any)
        int tlen[4];
        bool tcut;
        int tn = host ? 0 : crowd_wrap(c->title[0] ? c->title : c->project, tf, tw - 30, tline, tlen, 4, &tcut);
        int title_h = host ? 0 : (tn ? tn : 1) * tf->line_h + (resting ? 0 : 8);   // (the host: no title; resting: only one)
        // waiting on a choice: one each, stacked, in the bubble over the question (ASK_STYLE 1: pills) or a numbered
        // list over the bubble (2); they take their room from its words
        int np = !host && !resting && c->state == AG_WAITING ? c->nopt : 0, ph = f->line_h + 12;
        int inner = ASK_STYLE == 1 && np ? np * (ph + PILL_GAP) + 4 : 0;               // (in the bubble)
        int outer = ASK_STYLE == 2 && np ? np * ph + 14 : 0;                             // (over it)
        int room = (y1 - CROWD_TOP - 2 * CROWD_PAD - title_h - inner - outer) / f->line_h;
        if (room < 1) room = 1;
        const char *line[CROWD_LINES];
        int len[CROWD_LINES];
        bool cut = false;
        int nl = resting ? 0 : crowd_wrap(c->detail, f, tw, line, len, room < CROWD_LINES ? room : CROWD_LINES, &cut);
        int yb0 = y1 - 2 * CROWD_PAD - title_h - nl * f->line_h - inner;
        uint16_t bg = rgb((int)(pal[0] * a), (int)(pal[1] * a), (int)(pal[2] * a));
        fill_rrect(buf, y0, h, x0, yb0, x1, y1, 16, bg);
        int tx = (int)lroundf(ms->x);
        fill_tri(buf, y0, h, tx - 9, y1 - 1, tx + 9, y1 - 1, tx, y1 + 12, bg);
        int lc[3];                                    // the logo and title in the chat's colour (darker on a light bubble)
        for (int q = 0; q < 3; q++) lc[q] = (int)(cc[q] * (light ? 0.5f : 1) * a);
        int ty = yb0 + CROWD_PAD + inner;
        if (!host) crowd_logo(buf, y0, h, c->agent, x0 + CROWD_PAD + 11, ty + tf->line_h / 2, lc);
        uint16_t tc = rgb(lc[0], lc[1], lc[2]);
        for (int l = 0; l < tn; l++) {
            int ly = ty + l * tf->line_h;
            draw_ftext_n(buf, y0, h, x0 + CROWD_PAD + 30, ly, tf, tline[l], tlen[l], tc);
            if (tcut && l == tn - 1) draw_ftext(buf, y0, h, x0 + CROWD_PAD + 30 + ftext_width_n(tf, tline[l], tlen[l]), ly, tf, "...", tc);
        }
        uint16_t fg = rgb((int)(pal[3] * a), (int)(pal[4] * a), (int)(pal[5] * a));
        pills[i].slot = ms->slot;
        pills[i].n = np;
        for (int o = 0; o < np; o++) {                // first choice at the top
            bool picked = c->picked == o, other = c->picked >= 0 && !picked;
            int px0, px1, pt, pb, tcol[3];
            char lab[40];
#if ASK_STYLE == 1
            px0 = x0 + CROWD_PAD;
            px1 = x1 - CROWD_PAD;
            pt = yb0 + CROWD_PAD + o * (ph + PILL_GAP);
            pb = pt + ph;
            float mix = other ? 0.7f : 0;             // tapped: the others fade into the bubble
            int pc[3];
            for (int q = 0; q < 3; q++) { pc[q] = (int)(lc[q] + (pal[q] * a - lc[q]) * mix); tcol[q] = (int)((255 + (pal[q] - 255) * mix) * a); }
            fill_rrect(buf, y0, h, px0, pt, px1, pb, ph / 2, rgb(pc[0], pc[1], pc[2]));
            strlcpy(lab, c->opt[o], sizeof lab);
#else
            static const int green[3] = {80, 210, 110};
            px0 = 12;                                 // (the whole width, like a terminal's)
            px1 = W - 12;
            pt = yb0 - 14 - (np - o) * ph;
            pb = pt + ph;
            float ga = a * (other ? 0.35f : 1);
            uint16_t gl = rgb((int)(green[0] * ga * 0.6f), (int)(green[1] * ga * 0.6f), (int)(green[2] * ga * 0.6f));
            if (picked) fill_rrect(buf, y0, h, px0, pt, px1, pb, 0, rgb((int)(green[0] * a), (int)(green[1] * a), (int)(green[2] * a)));
            fill_rrect(buf, y0, h, px0, pb - 1, px1, pb, 0, gl);
            if (o == 0) fill_rrect(buf, y0, h, px0, pt, px1, pt + 1, 0, gl);
            for (int q = 0; q < 3; q++) tcol[q] = picked ? (int)(12 * a) : (int)(green[q] * ga);
            snprintf(lab, sizeof lab, "%d. %.31s", o + 1, c->opt[o]);
#endif
            int *r = pills[i].r[o];
            r[0] = px0; r[1] = pt; r[2] = px1; r[3] = pb;
            int lw = px1 - px0 - 2 * CROWD_PAD;       // (cut to fit, with "...")
            if (ftext_width(f, lab) > lw) {
                int k = (int)strlen(lab);
                while (k > 0 && ftext_width_n(f, lab, k) + ftext_width(f, "...") > lw) k--;
                strcpy(lab + k, "...");
            }
            int lx = ASK_STYLE == 1 ? (px0 + px1) / 2 - ftext_width(f, lab) / 2 : px0 + 8;
            draw_ftext(buf, y0, h, lx, pt + (ph - f->line_h) / 2, f, lab, rgb(tcol[0], tcol[1], tcol[2]));
        }
        for (int l = 0; l < nl; l++) {
            int ly = ty + title_h + l * f->line_h;
            draw_ftext_n(buf, y0, h, x0 + CROWD_PAD, ly, f, line[l], len[l], fg);
            if (cut && l == nl - 1) draw_ftext(buf, y0, h, x0 + CROWD_PAD + ftext_width_n(f, line[l], len[l]), ly, f, "...", fg);
        }

        // its character, growing in from its feet; greyed when stopped
        sprite_draw(buf, y0, h, host ? HOST_WHO : ms->slot, &ms->pose, ms->x, CROWD_FEET - ms->dy, CROWD_PX * k * ms->sc, dimk * fminf(1, k * 1.5f));
    }
    if (ask_n > 1) {                                  // others waiting on a choice too, after this one: top middle (the clock
        char t[16];                                   // has the left corner)
        snprintf(t, sizeof t, "1 of %d", ask_n);
        draw_ftext(buf, y0, h, CX - ftext_width(&font_small, t) / 2, 17, &font_small, t, rgb(150, 150, 162));
    }
    if (crowd_more) {                                 // more chats than room: how many more, bottom right
        char t[8];
        snprintf(t, sizeof t, "+%d", crowd_more);
        draw_ftext(buf, y0, h, W - 14 - ftext_width(f, t), H - 34, f, t, rgb(120, 120, 132));
    }
}
#endif
