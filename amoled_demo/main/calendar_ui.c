// Calendar screen. Face: a 12-hour dial whose outer ring shows the next 12 hours of meetings as
// coloured arcs (positioned like a clock), with the current/next meeting in the middle.
// Agenda: scrollable list of today and tomorrow.
// The square: one page instead, in the style of the phone's lock screen and calendar: the clock and date on the
// left, the meeting that's on or next in a ring on the right, today's timeline (8 AM to 6 PM, stretched to take in anything earlier or later), and the
// agenda scrolling below.
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "gfx.h"
#include "calendar.h"
#include "calendar_ui.h"
#include "reminders.h"
#include "battery.h"

#define C_WHITE  rgb(255, 255, 255)
#define C_GREY   rgb(140, 140, 160)
#define C_DIM    rgb(80, 80, 100)
#define C_PANEL  rgb(26, 26, 40)
#define C_PRESS  rgb(0, 110, 96)
#define C_ACCENT rgb(0, 230, 200)
#define C_RED    rgb(255, 95, 95)
#define C_GREEN  rgb(60, 230, 120)

#define RING_IN 198
#define RING_OUT 222
#define TICK_IN 184
#define TICK_OUT 191

enum { V_FACE, V_AGENDA };
static int view;

static uint8_t *ang_lut, *rad_lut;   // per pixel: clockwise angle from 12 o'clock (0..255), distance from centre
static uint16_t ring_col[256];
static uint16_t palette[6];

#define MAX_ITEMS 96
EXT_RAM_BSS_ATTR static ical_item_t items[MAX_ITEMS];
static int n_items;
static time_t now_t;
static int64_t last_copy;

// touch
static bool down, dragging;
static int press_x, press_y, scroll, scroll_at_press;
static float fling, last_drag_y;          // agenda keeps gliding after a flick
static int press_id;
static bool follow_now;                   // (the square) keep the agenda at what's next until it's scrolled by hand
enum { ID_NONE, ID_AGENDA, ID_BACK, ID_BG };

static const struct { int x0, y0, x1, y1; } agenda_btn = {CX - 100, 306, CX + 100, 364}, back_btn = {CX - 100, 384, CX + 100, 442};

void cal_ui_init(void)
{
    if (SCREEN_ROUND) {                              // (only the round board has the dial: 460 KB of PSRAM)
        ang_lut = heap_caps_malloc(W * H, MALLOC_CAP_SPIRAM);
        rad_lut = heap_caps_malloc(W * H, MALLOC_CAP_SPIRAM);
    }
    for (int y = 0; y < H && ang_lut && rad_lut; y++)
        for (int x = 0; x < W; x++) {
            float dx = x - CX + 0.5f, dy = y - CY + 0.5f;
            float a = atan2f(dx, -dy);                 // 0 at 12 o'clock, clockwise
            if (a < 0) a += 2 * (float)M_PI;
            ang_lut[y * W + x] = (uint8_t)((int)(a / (2 * (float)M_PI) * 256) & 255);
            float r = sqrtf(dx * dx + dy * dy);
            rad_lut[y * W + x] = r > 255 ? 255 : (uint8_t)r;
        }
    palette[0] = rgb(0, 210, 190);
    palette[1] = rgb(255, 150, 60);
    palette[2] = rgb(170, 110, 255);
    palette[3] = rgb(255, 90, 150);
    palette[4] = rgb(70, 150, 255);
    palette[5] = rgb(240, 210, 60);
#if !SCREEN_ROUND                          // the square: the phone's own calendar colours (dark mode)
    palette[0] = rgb(10, 132, 255);        // blue
    palette[1] = rgb(255, 159, 10);        // orange
    palette[2] = rgb(191, 90, 242);        // purple
    palette[3] = rgb(48, 209, 88);         // green
    palette[4] = rgb(255, 55, 95);         // pink
    palette[5] = rgb(255, 214, 10);        // yellow
#endif
}

// ---------------------------------------------------------------- helpers
// One colour per calendar when there are several; otherwise alternate so back-to-back meetings differ
static int color_of(int i) { return calendar_count() > 1 ? palette[items[i].cal % 6] : palette[i % 6]; }

static void fmt_time(char *out, size_t cap, time_t t, bool compact)
{
    struct tm tm;
    localtime_r(&t, &tm);
    int h = tm.tm_hour % 12 ? tm.tm_hour % 12 : 12;
    if (compact) snprintf(out, cap, "%d:%02d%c", h, tm.tm_min, tm.tm_hour < 12 ? 'a' : 'p');
    else snprintf(out, cap, "%d:%02d %s", h, tm.tm_min, tm.tm_hour < 12 ? "AM" : "PM");
}

static void fmt_duration(char *out, size_t cap, const char *prefix, time_t secs)
{
    int m = (int)((secs + 59) / 60);
    if (m < 60) snprintf(out, cap, "%s%d min", prefix, m);
    else if (m < 24 * 60) snprintf(out, cap, "%s%d h %02d min", prefix, m / 60, m % 60);
    else snprintf(out, cap, "%s%d days", prefix, m / (24 * 60));
}

static bool same_day(time_t a, time_t b)
{
    struct tm x, y;
    localtime_r(&a, &x);
    localtime_r(&b, &y);
    return x.tm_yday == y.tm_yday && x.tm_year == y.tm_year;
}

static void fit(char *out, size_t cap, const char *in, int max_chars)
{
    if ((int)strlen(in) <= max_chars) { strlcpy(out, in, cap); return; }
    snprintf(out, cap, "%.*s..", max_chars - 2, in);
}

// Word-wrap into two lines of at most n chars; the second gets ".." if it still doesn't fit
static void wrap2(const char *s, int n, char *l1, char *l2, size_t cap)
{
    l2[0] = 0;
    if ((int)strlen(s) <= n) { strlcpy(l1, s, cap); return; }
    int cut = n;
    while (cut > n / 2 && s[cut] != ' ') cut--;
    if (s[cut] != ' ') cut = n;
    snprintf(l1, cap, "%.*s", cut, s);
    const char *rest = s + cut;
    while (*rest == ' ') rest++;
    fit(l2, cap, rest, n);
}

// Timed item happening at `t` (latest start wins when meetings overlap); -1 if none
static int item_at(time_t t)
{
    int best = -1;
    for (int i = 0; i < n_items; i++)
        if (!items[i].all_day && items[i].start <= t && t < items[i].end && (best < 0 || items[i].start >= items[best].start))
            best = i;
    return best;
}

static int next_item(time_t t)
{
    for (int i = 0; i < n_items; i++)
        if (!items[i].all_day && items[i].start > t) return i;   // items are sorted by start
    return -1;
}

static int clock_minutes(time_t t)
{
    struct tm tm;
    localtime_r(&t, &tm);
    return (tm.tm_hour % 12) * 60 + tm.tm_min;
}

// ---------------------------------------------------------------- per-frame state
// Meeting heat: 0 when the next busy meeting is 30+ min away, rising to 1 as it starts; -1 while you're in one
#define HEAT_LEAD_S (30 * 60)
static float meeting_heat(void)
{
    time_t next = 0;
    for (int i = 0; i < n_items; i++) {
        const ical_item_t *it = &items[i];
        if (it->all_day || it->free) continue;
        if (it->start <= now_t && now_t < it->end) return -1;
        if (it->start > now_t && (!next || it->start < next)) next = it->start;
    }
    if (!next || next - now_t >= HEAT_LEAD_S) return 0;
    float u = 1 - (float)(next - now_t) / HEAT_LEAD_S;
    return u * u;                                            // gentle at first, quicker near the start
}

static uint16_t toward(uint16_t be, int r2, int g2, int b2, float t)
{
    uint16_t c = SWAP16(be);
    int r = (c >> 11) << 3, g = ((c >> 5) & 63) << 2, b = (c & 31) << 3;
    return rgb(r + (r2 - r) * t, g + (g2 - g) * t, b + (b2 - b) * t);
}

static void rebuild_ring(void)
{
    int now_cm = clock_minutes(now_t);
    int cur = item_at(now_t);
    float heat = meeting_heat();
    if (heat < 0) {                                          // in a meeting: solid red
        for (int a = 0; a < 256; a++) ring_col[a] = rgb(220, 30, 40);
        return;
    }
    for (int a = 0; a < 256; a++) {
        int cm = (a * 720 + 360) / 256;                      // clock minute in the middle of this slice
        int delta = (cm - now_cm + 720) % 720;
        int i = item_at(now_t + delta * 60);
        if (i < 0) ring_col[a] = rgb(28, 28, 42);
        else {
            uint16_t c = SWAP16(color_of(i));
            int k = i == cur ? 256 : 190;                    // the meeting you're in is brightest
            ring_col[a] = rgb(((c >> 11) << 3) * k >> 8, (((c >> 5) & 63) << 2) * k >> 8, ((c & 31) << 3) * k >> 8);
        }
        if (heat > 0) ring_col[a] = toward(ring_col[a], 220, 30, 40, heat * 0.85f);   // warming up before a meeting
    }
}

static int agenda_rows(int *types, int *idx, int max);

// Apple Reminders (from the Mac), refreshed alongside the calendar items
#define MAX_REMS 64
EXT_RAM_BSS_ATTR static reminder_t rems[MAX_REMS];
static int n_rems;
static char ticked_id[96];               // just tapped: shows checked for a moment before it goes
static int64_t ticked_us;

#define TITLE_CHARS 16                   // large text per line in an agenda row
static int row_height(int type, int idx)
{
#if !SCREEN_ROUND
    (void)idx;
    return type == 0 ? 46 : type == 2 ? 44 : 64;   // day heading, "nothing scheduled", an event or reminder (a card)
#endif
    if (type == 0) return 50;            // day heading
    if (type == 2) return 44;            // "nothing scheduled"
    char l1[40], l2[40];
    wrap2(type == 3 ? rems[idx].title : items[idx].title, TITLE_CHARS, l1, l2, sizeof l1);
    return l2[0] ? 92 : 68;              // time line + one or two title lines
}

// Where the agenda starts: today's next meeting (or the one going on) at the top, with the day's heading above it if
// it's the day's first; nothing more today, the top of tomorrow, its heading first
static void scroll_to_now(void)
{
    int types[MAX_ITEMS * 2], idx[MAX_ITEMS * 2], n = agenda_rows(types, idx, MAX_ITEMS * 2), y = 0;
    int day = -1, day_y = 0, tomorrow_y = -1, target = -1;
    bool first = false;                               // (no rows yet under this day's heading)
    for (int r = 0; r < n && target < 0; r++) {
        if (types[r] == 0) {
            day = idx[r];
            day_y = y;
            first = true;
            if (day == 1) tomorrow_y = y;
        } else {
            if (day == 0 && types[r] == 1 && !items[idx[r]].all_day && items[idx[r]].end > now_t)
                target = first ? day_y : y - (SCREEN_ROUND ? 0 : 18);   // (the square: clear of the fade at the list's top)
            first = false;
        }
        y += row_height(types[r], idx[r]);
    }
    scroll = target >= 0 ? target : tomorrow_y >= 0 ? tomorrow_y : 0;
    if (scroll < 0) scroll = 0;
}

bool cal_ui_animating(void) { return fling != 0; }
bool cal_ui_is_face(void) { return view == V_FACE; }
void cal_ui_cancel_touch(void) { down = dragging = false; press_id = ID_NONE; fling = 0; }

void cal_ui_enter(void)
{
    view = V_FACE;
    down = false;
#if !SCREEN_ROUND
    fling = 0;
    follow_now = true;                     // (the square's agenda is always there: start it at what's next)
#endif
    last_copy = 0;                         // (no forced download here: it would land mid-animation)
}

// ---------------------------------------------------------------- agenda layout
#if !SCREEN_ROUND
#define LIST_TOP 242
#define LIST_BOT H
#else
#define LIST_TOP 68
#define LIST_BOT 376
#endif
#define AGENDA_DAYS 7

// row types: 0 = day header (idx = 0 today / 1 tomorrow), 1 = event, 2 = nothing scheduled, 3 = reminder
static int agenda_rows(int *types, int *idx, int max)
{
    int n = 0;
    time_t day0 = calendar_window_start();
    for (int d = 0; d < AGENDA_DAYS; d++) {
        struct tm tm;
        localtime_r(&day0, &tm);
        tm.tm_mday += d;
        tm.tm_isdst = -1;
        time_t ds = mktime(&tm);
        tm.tm_mday += 1;
        tm.tm_isdst = -1;
        time_t de = mktime(&tm);
        int head = n;
        if (n < max) { types[n] = 0; idx[n++] = d; }
        int before = n;
        for (int i = 0; i < n_items && n < max; i++)
            if (items[i].start < de && items[i].end > ds && (items[i].all_day || items[i].start >= ds || d == 0)) {
                types[n] = 1;
                idx[n++] = i;
            }
        for (int i = 0; i < n_rems && n < max; i++)          // reminders due that day (overdue ones go on today)
            if ((rems[i].due >= ds || d == 0) && rems[i].due < de) {
                types[n] = 3;
                idx[n++] = i;
            }
        if (n == before) {
            if (d < 2 && n < max) { types[n] = 2; idx[n++] = d; }       // "nothing scheduled"
            else if (d >= 2) n = head;                                    // skip empty later days
        }
    }
    return n;
}

// Agenda layout (rows and their heights), worked out once per data refresh rather than per strip
static int ag_types[MAX_ITEMS * 2], ag_idx[MAX_ITEMS * 2], ag_h[MAX_ITEMS * 2];   // (every frame: internal RAM)
static int ag_n, ag_total;
static uint32_t ag_ver = 0xFFFFFFFF;
static uint32_t data_version;              // bumped whenever items/reminders are re-read

static void compute_agenda(void)
{
    if (ag_ver == data_version) return;
    ag_ver = data_version;
    ag_n = agenda_rows(ag_types, ag_idx, MAX_ITEMS * 2);
    ag_total = 0;
    for (int r = 0; r < ag_n; r++) ag_total += ag_h[r] = row_height(ag_types[r], ag_idx[r]);
}

static int agenda_height(void)
{
    compute_agenda();
    return ag_total;
}

static int max_scroll(void)
{
    int m = agenda_height() - (LIST_BOT - LIST_TOP);
    return m > 0 ? m : 0;
}

// ---------------------------------------------------------------- drawing
void cal_ui_render_bg(uint16_t *buf, int y0, int y1)
{
    if (!SCREEN_ROUND || view != V_FACE || !ang_lut) {   // (the square has no dial: plain black)
        memset(buf, 0, (y1 - y0) * W * 2);
        return;
    }
    uint16_t tick = rgb(90, 90, 110);
    for (int y = y0; y < y1; y++) {
        uint16_t *o = buf + (y - y0) * W;
        const uint8_t *al = ang_lut + y * W, *rl = rad_lut + y * W;
        for (int x = 0; x < W; x++) {
            int r = rl[x];
            uint16_t c = 0;
            if (r >= RING_IN && r <= RING_OUT) c = ring_col[al[x]];
            else if (r >= TICK_IN && r <= TICK_OUT && ((al[x] * 12 + 6) & 255) < 14) c = tick;   // hour marks
            o[x] = c;
        }
    }
}

static bool pressed(int id) { return down && !dragging && press_id == id; }

static void draw_button(uint16_t *buf, int y0, int h, int x0, int ya, int x1, int yb, const char *label, bool on)
{
    fill_rrect(buf, y0, h, x0, ya, x1, yb, 18, on ? C_PRESS : C_PANEL);
    draw_text(buf, y0, h, (x0 + x1) / 2 - text_width(label, 3) / 2, (ya + yb) / 2 - 10, 3, label, C_WHITE);
}

static int reminders_due_today(void)
{
    struct tm tm;
    localtime_r(&now_t, &tm);
    tm.tm_hour = 23; tm.tm_min = 59; tm.tm_sec = 59; tm.tm_isdst = -1;
    time_t end = mktime(&tm);
    int n = 0;
    for (int i = 0; i < n_rems; i++) n += rems[i].due <= end;
    return n;
}

// The screen is drawn in ~18 strips, and the face's text needs time-zone conversions for every event:
// work it all out once when the time or the data changes, and let each strip just draw it.
static struct {
    time_t now;
    uint32_t ver;
    char date[32], time[24], err1[40], err2[40], allday[64], label[48], t1[40], t2[40], countdown[48], more[64];
    uint16_t label_col;
    bool has_show;
} face;

static void compute_face(void)
{
    if (face.now == now_t && face.ver == data_version) return;
    face.now = now_t;
    face.ver = data_version;
    char line[64];
    struct tm tm;
    localtime_r(&now_t, &tm);
    strftime(face.date, sizeof face.date, "%a %b %e", &tm);
    fmt_time(face.time, sizeof face.time, now_t, false);
    wrap2(calendar_error(), 26, face.err1, face.err2, sizeof face.err1);

    line[0] = 0;                           // all-day items for today, joined on one line
    for (int i = 0; i < n_items; i++)
        if (items[i].all_day && items[i].start <= now_t && now_t < items[i].end) {
            if (line[0]) strlcat(line, ", ", sizeof line);
            strlcat(line, items[i].title, sizeof line);
        }
    fit(face.allday, sizeof face.allday, line, 22);

    int cur = item_at(now_t), nxt = next_item(now_t);
    int show = cur >= 0 ? cur : nxt;
    face.has_show = show >= 0;
    face.more[0] = 0;
    if (show < 0) return;
    ical_item_t *it = &items[show];
    char when[24];
    fmt_time(when, sizeof when, it->start, false);
    if (cur >= 0) snprintf(face.label, sizeof face.label, "NOW");
    else if (same_day(it->start, now_t)) snprintf(face.label, sizeof face.label, "NEXT  %s", when);
    else if (same_day(it->start - 86400, now_t)) snprintf(face.label, sizeof face.label, "TOMORROW  %s", when);
    else {
        struct tm st;
        localtime_r(&it->start, &st);
        char wd[8];
        strftime(wd, sizeof wd, "%a", &st);
        for (char *c = wd; *c; c++) *c = toupper((unsigned char)*c);
        snprintf(face.label, sizeof face.label, "%s  %s", wd, when);
    }
    face.label_col = color_of(show);
    wrap2(it->title, 18, face.t1, face.t2, sizeof face.t1);
    if (cur >= 0) fmt_duration(face.countdown, sizeof face.countdown, "ends in ", it->end - now_t);
    else fmt_duration(face.countdown, sizeof face.countdown, "in ", it->start - now_t);

    int more = 0;
    for (int i = 0; i < n_items; i++)
        if (!items[i].all_day && i != show && items[i].start > now_t && same_day(items[i].start, now_t)) more++;
    int due = reminders_due_today();
    if (more && due) snprintf(face.more, sizeof face.more, "+%d more, %d reminder%s", more, due, due > 1 ? "s" : "");
    else if (more) snprintf(face.more, sizeof face.more, "+%d more today", more);
    else if (due) snprintf(face.more, sizeof face.more, "%d reminder%s today", due, due > 1 ? "s" : "");
}

static void draw_face(uint16_t *buf, int y0, int h)
{
    compute_face();
    float a = clock_minutes(now_t) / 720.0f * 2 * (float)M_PI;   // "now" marker on the ring
    int mx = CX + (int)(210 * sinf(a)), my = CY - (int)(210 * cosf(a));
    fill_circle(buf, y0, h, mx, my, 13, 0);
    fill_circle(buf, y0, h, mx, my, 9, C_WHITE);

    text_c(buf, y0, h, 90, 2, face.date, C_GREY);
    text_c(buf, y0, h, 114, 3, face.time, C_WHITE);
    if (!calendar_loaded()) {
        text_c(buf, y0, h, 200, 2, face.err1, C_GREY);
        text_c(buf, y0, h, 224, 2, face.err2, C_GREY);
        return;
    }
    if (face.allday[0]) text_c(buf, y0, h, 148, 2, face.allday, C_GREY);
    if (!face.has_show) {
        text_c(buf, y0, h, 200, 2, "Nothing coming up", C_GREY);
        text_c(buf, y0, h, 224, 2, "this week", C_GREY);
    } else {
        text_c(buf, y0, h, 176, 2, face.label, face.label_col);
        text_c(buf, y0, h, face.t2[0] ? 202 : 214, 3, face.t1, C_WHITE);
        if (face.t2[0]) text_c(buf, y0, h, 228, 3, face.t2, C_WHITE);
        text_c(buf, y0, h, 264, 2, face.countdown, C_GREY);
        if (face.more[0]) text_c(buf, y0, h, 286, 2, face.more, C_DIM);
    }
    draw_button(buf, y0, h, agenda_btn.x0, agenda_btn.y0, agenda_btn.x1, agenda_btn.y1, "Agenda", pressed(ID_AGENDA));
    if (calendar_error()[0]) text_c(buf, y0, h, 374, 2, "Offline - cached", C_RED);
}

static void draw_agenda(uint16_t *buf, int y0, int h)
{
    char line[48], t1[16], t2[16], l1[40], l2[40];
    text_c(buf, y0, h, 40, 3, "Agenda", C_WHITE);
    compute_agenda();
    int *types = ag_types, *idx = ag_idx, n = ag_n;

    gfx_clip(LIST_TOP, LIST_BOT);
    int y = LIST_TOP - scroll;
    for (int r = 0; r < n; r++) {
        int rh = ag_h[r];
        if (y + rh > y0 && y < y0 + h && y + rh > LIST_TOP && y < LIST_BOT) {
            if (types[r] == 0) {                          // "Today  Tue Oct 13"
                time_t d = calendar_window_start() + idx[r] * 86400 + 3600;   // +1 h: safe across DST
                struct tm tm;
                localtime_r(&d, &tm);
                char wd[12];
                strftime(line, sizeof line, idx[r] < 2 ? "%a %b %e" : "%b %e", &tm);
                strftime(wd, sizeof wd, "%A", &tm);   // later days go by name: "Monday Oct 19"
                const char *day = idx[r] == 0 ? "Today" : idx[r] == 1 ? "Tomorrow" : wd;
                draw_text(buf, y0, h, CX - 150, y + 14, 3, day, C_ACCENT);
                draw_text(buf, y0, h, CX - 150 + text_width(day, 3) + 14, y + 20, 2, line, C_GREY);
                fill_rect(buf, y0, h, CX - 150, y + 44, CX + 150, y + 46, C_DIM);
            } else if (types[r] == 2) {
                draw_text(buf, y0, h, CX - 140, y + 14, 2, "Nothing scheduled", C_DIM);
            } else if (types[r] == 3) {                   // reminder: checkbox, when, title
                reminder_t *rm = &rems[idx[r]];
                uint16_t lc = rgb(rm->r, rm->g, rm->b);
                bool ticked = !strcmp(ticked_id, rm->id), overdue = rm->due < calendar_window_start() ||
                              (rm->has_time && rm->due < now_t);
                int cy = y + rh / 2;
                fill_circle(buf, y0, h, CX - 146, cy, 13, lc);
                if (!ticked) fill_circle(buf, y0, h, CX - 146, cy, 10, 0);          // empty ring until tapped
                if (rm->has_time) fmt_time(t1, sizeof t1, rm->due, true);
                snprintf(line, sizeof line, "%s%s%s", overdue ? "Overdue  " : "", rm->has_time ? t1 : "", rm->has_time ? "" : rm->list);
                draw_text(buf, y0, h, CX - 124, y + 10, 2, line, overdue ? C_RED : C_GREY);
                wrap2(rm->title, TITLE_CHARS, l1, l2, sizeof l1);
                draw_text(buf, y0, h, CX - 124, y + 32, 3, l1, ticked ? C_DIM : C_WHITE);
                if (l2[0]) draw_text(buf, y0, h, CX - 124, y + 56, 3, l2, ticked ? C_DIM : C_WHITE);
            } else {                                      // time on top, title below in large text
                ical_item_t *it = &items[idx[r]];
                bool past = it->end <= now_t, now = !it->all_day && it->start <= now_t && now_t < it->end;
                if (now) fill_rrect(buf, y0, h, CX - 160, y + 3, CX + 160, y + rh - 3, 12, C_PANEL);
                fill_rrect(buf, y0, h, CX - 154, y + 10, CX - 148, y + rh - 10, 3, past ? C_DIM : color_of(idx[r]));
                if (it->all_day) strlcpy(line, "All day", sizeof line);
                else {
                    fmt_time(t1, sizeof t1, it->start, true);
                    fmt_time(t2, sizeof t2, it->end, true);
                    snprintf(line, sizeof line, "%s - %s%s", t1, t2, now ? "  now" : "");
                }
                draw_text(buf, y0, h, CX - 136, y + 10, 2, line, past ? C_DIM : (now ? C_ACCENT : C_GREY));
                wrap2(it->title, TITLE_CHARS, l1, l2, sizeof l1);
                draw_text(buf, y0, h, CX - 136, y + 32, 3, l1, past ? C_DIM : C_WHITE);
                if (l2[0]) draw_text(buf, y0, h, CX - 136, y + 56, 3, l2, past ? C_DIM : C_WHITE);
            }
        }
        y += rh;
    }
    gfx_clip(0, H);
    int ms = max_scroll();
    if (ms > 0) {                                         // scroll bar
        int track = LIST_BOT - LIST_TOP - 16, total = ms + (LIST_BOT - LIST_TOP);
        int th = track * (LIST_BOT - LIST_TOP) / total, ty = LIST_TOP + 8 + (track - th) * scroll / ms;
        fill_rrect(buf, y0, h, CX + 166, ty, CX + 171, ty + th, 2, C_DIM);
    }
    draw_button(buf, y0, h, back_btn.x0, back_btn.y0, back_btn.x1, back_btn.y1, "Back", pressed(ID_BACK));
}

#if !SCREEN_ROUND
// ---------------------------------------------------------------- the square
#define C_TEXT2  rgb(152, 152, 160)
#define C_SEP    rgb(36, 36, 40)
#define C_NOW    rgb(255, 69, 58)                 // the phone calendar's red "now" line
#define TL_X0 24                                  // the timeline: today's working hours, or more if the day needs it
#define TL_X1 (W - 24)
#define TL_Y 206
#define TL_FROM 8                                 // the usual span: 8 AM ...
#define TL_TO 18                                  // ... to 6 PM
static time_t tl_day0, tl_day1;                   // the span shown, on the hour (set by draw_timeline)

static void fitw(char *out, size_t cap, const char *in, const font_t *f, int max_w)
{
    strlcpy(out, in, cap);
    int n = strlen(out);
    if (ftext_width(f, out) <= max_w) return;
    while (n > 0 && ftext_width_n(f, out, n) + ftext_width(f, "..") > max_w) n--;
    while (n > 0 && out[n - 1] == ' ') n--;
    strlcpy(out + n, "..", cap - n);
}

static void ftext_cx(uint16_t *buf, int y0, int h, int cx, int y, const font_t *f, const char *t, uint16_t col)
{
    draw_ftext(buf, y0, h, cx - ftext_width(f, t) / 2, y, f, t, col);
}

// in on up to two lines of max_w px, broken between words, the second cut with ".." if need be
static void wrap2w_cal(const char *in, const font_t *f, int max_w, char *l1, char *l2, size_t cap)
{
    l2[0] = 0;
    if (ftext_width(f, in) <= max_w) { strlcpy(l1, in, cap); return; }
    int cut = 0;
    for (int i = 1; in[i]; i++)
        if (in[i] == ' ' && ftext_width_n(f, in, i) <= max_w) cut = i;
    if (!cut) { fitw(l1, cap, in, f, max_w); return; }
    snprintf(l1, cap, "%.*s", cut, in);
    fitw(l2, cap, in + cut + 1, f, max_w);
}

static float tl_x(time_t t) { return TL_X0 + (float)(t - tl_day0) * (TL_X1 - TL_X0) / (float)(tl_day1 - tl_day0); }

// c mixed toward (r, g, b) by t (0..1)
static uint16_t mixc(uint16_t c, int r, int g, int b, float t)
{
    uint16_t s = SWAP16(c);
    int cr = (s >> 11) << 3, cg = ((s >> 5) & 63) << 2, cb = (s & 31) << 3;
    return rgb(cr + (int)((r - cr) * t), cg + (int)((g - cg) * t), cb + (int)((b - cb) * t));
}

// a small clock face (the card's time line)
static void clock_icon(uint16_t *buf, int y0, int h, float cx, float cy, uint16_t c, uint16_t bg)
{
    static const float hands[] = {12, 7, 12, 12, 15.5f, 14};
    aa_disc(buf, y0, h, cx, cy, 6.5f, c);
    aa_disc(buf, y0, h, cx, cy, 5.1f, bg);
    aa_icon(buf, y0, h, cx, cy, 13, hands, 3, false, 1.5f, c);
}

// a calendar page in a disc (the all-day badge)
static void allday_icon(uint16_t *buf, int y0, int h, float cx, float cy, uint16_t c)
{
    aa_disc(buf, y0, h, cx, cy, 12, c);
    aa_rrect(buf, y0, h, cx - 6.5f, cy - 5.5f, cx + 6.5f, cy + 6.5f, 2, rgb(255, 255, 255));
    aa_rrect(buf, y0, h, cx - 5, cy - 2, cx + 5, cy + 5, 1, c);
    aa_rrect(buf, y0, h, cx - 3.6f, cy - 8, cx - 2.2f, cy - 4, 0.7f, rgb(255, 255, 255));
    aa_rrect(buf, y0, h, cx + 2.2f, cy - 8, cx + 3.6f, cy - 4, 0.7f, rgb(255, 255, 255));
}

// halfway to black: the part of the timeline that's already gone
static uint16_t dimmed(uint16_t c)
{
    uint16_t s = SWAP16(c);
    return rgb(((s >> 11) << 3) * 2 / 5, (((s >> 5) & 63) << 2) * 2 / 5, ((s & 31) << 3) * 2 / 5);
}

static void draw_timeline(uint16_t *buf, int y0, int h)
{
    if (TL_Y + 30 <= y0 || TL_Y - 14 >= y0 + h) return;
    // today's midnights, then the span: 8 AM to 6 PM, widened to the hour around any meeting today and around now
    struct tm tm;
    localtime_r(&now_t, &tm);
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    tm.tm_isdst = -1;
    time_t mid0 = mktime(&tm);
    tm.tm_mday += 1;
    tm.tm_isdst = -1;
    time_t mid1 = mktime(&tm);
    int h0 = TL_FROM, h1 = TL_TO;
    for (int i = 0; i <= n_items; i++) {
        time_t a, b;
        if (i == n_items) a = b = now_t;
        else if (items[i].all_day || items[i].end <= mid0 || items[i].start >= mid1) continue;
        else { a = items[i].start; b = items[i].end; }
        struct tm ta, tb;
        localtime_r(&a, &ta);
        localtime_r(&b, &tb);
        int ha = a <= mid0 ? 0 : ta.tm_hour, hb = b >= mid1 ? 24 : tb.tm_hour + (tb.tm_min || tb.tm_sec ? 1 : 0);
        if (ha < h0) h0 = ha;
        if (hb > h1) h1 = hb;
    }
    struct tm th;
    localtime_r(&mid0, &th);
    th.tm_hour = h0;
    th.tm_isdst = -1;
    tl_day0 = mktime(&th);
    localtime_r(&mid0, &th);
    th.tm_hour = h1;
    th.tm_isdst = -1;
    tl_day1 = h1 >= 24 ? mid1 : mktime(&th);
    float xn = tl_x(now_t);
    aa_rrect(buf, y0, h, TL_X0, TL_Y - 2, TL_X1, TL_Y + 2, 2, rgb(48, 48, 54));
    // hour marks: a dot every hour, a tick and label every few (about five labels in all); the ends always say
    // when the line starts and stops
    int span = h1 - h0, step = span <= 8 ? 2 : span <= 12 ? 3 : span <= 16 ? 4 : 6;
    int nw = ftext_width(&font_small, "Now") / 2 + 8;
    for (int k = h0; k <= h1; k++) {
        struct tm t2;
        localtime_r(&mid0, &t2);
        t2.tm_hour = k;
        t2.tm_isdst = -1;
        time_t t = k >= 24 ? mid1 : mktime(&t2);
        float x = tl_x(t);
        bool end = k == h0 || k == h1;
        if (!end && k % step) { aa_disc(buf, y0, h, x, TL_Y + 9, 1.2f, rgb(70, 70, 78)); continue; }
        char l[12];
        int hh = k % 24;
        if (hh == 12) strlcpy(l, "Noon", sizeof l);
        else snprintf(l, sizeof l, "%d %s", hh % 12 ? hh % 12 : 12, hh < 12 ? "AM" : "PM");
        if (k == 24) strlcpy(l, "11:59 PM", sizeof l);
        int lw = ftext_width(&font_small, l), lx = k == h0 ? TL_X0 : k == h1 ? TL_X1 - lw : (int)x - lw / 2;
        if (!end && (lx < TL_X0 + 52 || lx + lw > TL_X1 - 52)) {   // (too close to an end's label: just a dot)
            aa_disc(buf, y0, h, x, TL_Y + 9, 1.2f, rgb(70, 70, 78));
            continue;
        }
        aa_rrect(buf, y0, h, x - 0.8f, TL_Y + 5, x + 0.8f, TL_Y + 11, 0.8f, rgb(90, 90, 98));
        if (lx + lw < xn - nw || lx > xn + nw) draw_ftext(buf, y0, h, lx, TL_Y + 13, &font_small, l, C_TEXT2);   // (not under "Now")
    }
    // meetings: capsules on the line in their calendar's colour (free time: a thin one); gone already: dimmed
    for (int i = 0; i < n_items; i++) {
        ical_item_t *it = &items[i];
        if (it->all_day || it->end <= tl_day0 || it->start >= tl_day1) continue;
        float xa = fmaxf(TL_X0, tl_x(it->start)), xb = fminf(TL_X1, tl_x(it->end));
        if (xb - xa < 6) { float m = (xa + xb) / 2; xa = m - 3; xb = m + 3; }
        float hh = it->free ? 2.5f : 6.5f;
        uint16_t c = color_of(i);
        aa_rrect(buf, y0, h, xa, TL_Y - hh, xb, TL_Y + hh, hh, c);
        if (xa < xn) aa_rrect(buf, y0, h, xa, TL_Y - hh, fminf(xn, xb), TL_Y + hh, hh, dimmed(c));
    }
    // now
    aa_rrect(buf, y0, h, xn - 1.2f, TL_Y - 13, xn + 1.2f, TL_Y + 13, 1.2f, C_NOW);
    aa_disc(buf, y0, h, xn, TL_Y - 13, 3.5f, C_NOW);
    ftext_cx(buf, y0, h, (int)xn, TL_Y + 13, &font_small, "Now", C_NOW);
}

// "in 16h 26m" / "in 25m" / "in 3 days"
static void fmt_short(char *out, size_t cap, const char *prefix, time_t secs)
{
    int m = (int)((secs + 59) / 60);
    if (m < 60) snprintf(out, cap, "%s%dm", prefix, m);
    else if (m < 24 * 60) snprintf(out, cap, "%s%dh %02dm", prefix, m / 60, m % 60);
    else snprintf(out, cap, "%s%d days", prefix, m / (24 * 60));
}

// The ring on the right: what's on or next, with its countdown. Its arc fills in the meeting's colour over the hour
// before it starts, and shows how much of it has gone while it's on.
#define RING_CX 330
#define RING_CY 104
#define RING_R 84
static void draw_ring(uint16_t *buf, int y0, int h)
{
    if (RING_CY + RING_R + 4 <= y0 || RING_CY - RING_R - 4 >= y0 + h) return;
    aa_disc(buf, y0, h, RING_CX, RING_CY, RING_R + 1, rgb(150, 150, 158));
    aa_disc(buf, y0, h, RING_CX, RING_CY, RING_R - 1, 0);
    const int tw = 2 * RING_R - 20;                       // the words' width inside it (across its middle)
    char t[80], l2[80], when[32];
    if (!calendar_loaded()) {
        wrap2w_cal(calendar_count() ? calendar_error() : "Add a calendar in the Bobble app", &font_small, tw, t, l2, sizeof t);
        ftext_cx(buf, y0, h, RING_CX, RING_CY - (l2[0] ? 19 : 10), &font_small, t, C_TEXT2);
        if (l2[0]) ftext_cx(buf, y0, h, RING_CX, RING_CY, &font_small, l2, C_TEXT2);
        return;
    }
    int cur = item_at(now_t), nxt = next_item(now_t), show = cur >= 0 ? cur : nxt;
    if (show < 0) { ftext_cx(buf, y0, h, RING_CX, RING_CY - 10, &font_small, "Nothing coming up", C_TEXT2); return; }
    ical_item_t *it = &items[show];
    uint16_t c = color_of(show);
    float frac = cur >= 0 ? (float)(now_t - it->start) / fmaxf(1, (float)(it->end - it->start))
                          : 1 - (float)(it->start - now_t) / 3600;
    if (frac > 0) {                                       // the arc, clockwise from the top
        if (frac > 1) frac = 1;
        int n = 2 + (int)(frac * 40);
        float pts[2 * 42];
        for (int i = 0; i < n; i++) {
            float a = frac * 2 * (float)M_PI * i / (n - 1);
            pts[2 * i] = RING_CX + RING_R * sinf(a);
            pts[2 * i + 1] = RING_CY - RING_R * cosf(a);
        }
        aa_path(buf, y0, h, pts, n, false, 5, c);
    }
    if (cur >= 0) fmt_short(when, sizeof when, "ends in ", it->end - now_t);
    else fmt_short(when, sizeof when, "in ", it->start - now_t);
    wrap2w_cal(it->title, &font_label, tw - 14, t, l2, sizeof t);
    int lines = l2[0] ? 2 : 1, top = RING_CY - (lines * 24 + 22) / 2;
    int w1 = ftext_width(&font_label, t), x1 = RING_CX - (w1 + 16) / 2;
    aa_disc(buf, y0, h, x1 + 5, top + 12, 5, c);          // the dot, before the first line
    draw_ftext(buf, y0, h, x1 + 16, top, &font_label, t, C_WHITE);
    if (l2[0]) ftext_cx(buf, y0, h, RING_CX, top + 24, &font_label, l2, C_WHITE);
    ftext_cx(buf, y0, h, RING_CX, top + lines * 24 + 3, &font_small, when, C_TEXT2);
}

static void draw_list(uint16_t *buf, int y0, int h)
{
    char line[64], t1[16], t2[16], l1[96];
    compute_agenda();
    gfx_clip(LIST_TOP, LIST_BOT);
    int y = LIST_TOP - scroll;
    for (int r = 0; r < ag_n; r++) {
        int rh = ag_h[r], type = ag_types[r], ix = ag_idx[r];
        if (y + rh > y0 && y < y0 + h && y + rh > LIST_TOP && y < LIST_BOT) {
            if (type == 0) {                              // the day, bold: "Today" / "Tomorrow" / "Monday", then the date
                time_t d = calendar_window_start() + ix * 86400 + 3600;   // +1 h: safe across DST
                struct tm tm;
                localtime_r(&d, &tm);
                char wd[16], date[32];
                strftime(wd, sizeof wd, "%A", &tm);
                strftime(date, sizeof date, ix < 2 ? "%A, %B %e" : "%B %e", &tm);
                char *p = strstr(date, "  ");             // (%e pads single days)
                if (p) memmove(p + 1, p + 2, strlen(p + 2) + 1);
                const char *day = ix == 0 ? "Today" : ix == 1 ? "Tomorrow" : wd;
                draw_ftext(buf, y0, h, 20, y + 16, &font_label, day, C_WHITE);
                draw_ftext(buf, y0, h, 20 + ftext_width(&font_label, day) + 8, y + 20, &font_small, date, C_TEXT2);
            } else if (type == 2) {
                draw_ftext(buf, y0, h, 20, y + 10, &font_small, "Nothing scheduled", rgb(100, 100, 108));
            } else {
                // a card: tinted with the calendar's colour, the words in a light shade of it
                const int cx0 = 14, cx1 = W - 14, cy0 = y + 4, cy1 = y + rh - 4;
                float mid = (cy0 + cy1) / 2.0f;
                if (type == 3) {                          // reminder: a ring in its list's colour, filled when ticked
                    reminder_t *rm = &rems[ix];
                    uint16_t lc = rgb(rm->r, rm->g, rm->b), card = rgb(28, 28, 31);
                    bool ticked = !strcmp(ticked_id, rm->id), overdue = rm->due < calendar_window_start() ||
                                  (rm->has_time && rm->due < now_t);
                    aa_rrect(buf, y0, h, cx0, cy0, cx1, cy1, 16, card);
                    aa_disc(buf, y0, h, cx0 + 24, mid, 10, lc);
                    if (!ticked) aa_disc(buf, y0, h, cx0 + 24, mid, 8.2f, card);
                    else {
                        static const float chk[] = {7, 12.5f, 10.5f, 16, 17, 8.5f};
                        aa_icon(buf, y0, h, cx0 + 24, mid, 18, chk, 3, false, 2.2f, card);
                    }
                    fitw(l1, sizeof l1, rm->title, &font_label, cx1 - cx0 - 60);
                    draw_ftext(buf, y0, h, cx0 + 46, cy0 + 5, &font_label, l1, ticked ? rgb(100, 100, 108) : C_WHITE);
                    if (rm->has_time) fmt_time(t1, sizeof t1, rm->due, false);
                    snprintf(line, sizeof line, "%s%s", overdue ? "Overdue - " : "", rm->has_time ? t1 : rm->list);
                    draw_ftext(buf, y0, h, cx0 + 46, cy0 + 30, &font_small, line, overdue ? C_NOW : C_TEXT2);
                } else {
                    ical_item_t *it = &items[ix];
                    bool past = it->end <= now_t, now = !it->all_day && it->start <= now_t && now_t < it->end;
                    uint16_t c = color_of(ix);
                    uint16_t card = mixc(c, 20, 20, 22, past ? 0.9f : now ? 0.7f : 0.8f);
                    uint16_t fg = past ? rgb(110, 110, 118) : mixc(c, 255, 255, 255, 0.55f);
                    uint16_t fg2 = past ? rgb(90, 90, 98) : mixc(c, 255, 255, 255, 0.25f);
                    aa_rrect(buf, y0, h, cx0, cy0, cx1, cy1, 16, card);
                    int right = cx1 - 14;
                    if (now) {                            // a red "Now" tag on the right
                        int tw = ftext_width(&font_small, "Now");
                        aa_rrect(buf, y0, h, right - tw - 16, mid - 11, right, mid + 11, 11, C_NOW);
                        draw_ftext(buf, y0, h, right - tw - 8, (int)mid - 10, &font_small, "Now", C_WHITE);
                        right -= tw + 26;
                    }
                    if (it->all_day) {                    // badge, title, "all-day" on the right
                        allday_icon(buf, y0, h, cx0 + 24, mid, past ? rgb(90, 90, 98) : c);
                        int aw = ftext_width(&font_small, "all-day");
                        draw_ftext(buf, y0, h, right - aw, (int)mid - 10, &font_small, "all-day", fg2);
                        fitw(l1, sizeof l1, it->title, &font_label, right - aw - 12 - (cx0 + 46));
                        draw_ftext(buf, y0, h, cx0 + 46, (int)mid - 12, &font_label, l1, fg);
                    } else {                              // bar, title, a clock and the times
                        aa_rrect(buf, y0, h, cx0 + 12, cy0 + 10, cx0 + 16, cy1 - 10, 2, past ? rgb(80, 80, 88) : c);
                        fitw(l1, sizeof l1, it->title, &font_label, right - (cx0 + 28));
                        draw_ftext(buf, y0, h, cx0 + 28, cy0 + 5, &font_label, l1, fg);
                        fmt_time(t1, sizeof t1, it->start, false);
                        fmt_time(t2, sizeof t2, it->end, false);
                        snprintf(line, sizeof line, "%s - %s", t1, t2);
                        clock_icon(buf, y0, h, cx0 + 35, cy0 + 39.5f, fg2, card);
                        draw_ftext(buf, y0, h, cx0 + 47, cy0 + 30, &font_small, line, fg2);
                    }
                }
            }
        }
        y += rh;
    }
    gfx_clip(0, H);
    for (int yy = LIST_TOP; yy < LIST_TOP + 24; yy++) {   // the list fades in under the timeline
        if (yy < y0 || yy >= y0 + h || scroll <= 0) continue;
        int k = (yy - LIST_TOP) * (yy - LIST_TOP) * 256 / (24 * 24);
        uint16_t *row = buf + (yy - y0) * W;
        for (int x = 0; x < W; x++) {
            if (!row[x]) continue;
            uint16_t c = SWAP16(row[x]);
            int r = (c >> 11) * k >> 8, g = ((c >> 5) & 63) * k >> 8, b = (c & 31) * k >> 8;
            row[x] = SWAP16((uint16_t)(r << 11 | g << 5 | b));
        }
    }
    int ms = max_scroll();
    if (ms > 0 && (dragging || fling != 0)) {           // scroll bar, only while it moves
        int track = LIST_BOT - LIST_TOP - 16, total = ms + (LIST_BOT - LIST_TOP);
        int th = track * (LIST_BOT - LIST_TOP) / total, ty = LIST_TOP + 8 + (track - th) * scroll / ms;
        aa_rrect(buf, y0, h, W - 7, ty, W - 3, ty + th, 2, rgb(110, 110, 118));
    }
}

static void draw_square(uint16_t *buf, int y0, int h)
{
    compute_face();
    char date[40], t[12];
    struct tm tm;
    localtime_r(&now_t, &tm);
    if (y0 < 180) {                                       // the clock and the date, on the left
        snprintf(t, sizeof t, "%d:%02d", tm.tm_hour % 12 ? tm.tm_hour % 12 : 12, tm.tm_min);
        ftext_cx(buf, y0, h, 128, 42, &font_clock, t, C_WHITE);
        strftime(date, sizeof date, "%a, %b %e", &tm);
        char *p = strstr(date, "  ");                     // (%e pads single days)
        if (p) memmove(p + 1, p + 2, strlen(p + 2) + 1);
        ftext_cx(buf, y0, h, 128, 124, &font_label, date, C_WHITE);
    }
    draw_ring(buf, y0, h);
    if (calendar_loaded()) {
        draw_timeline(buf, y0, h);
        draw_list(buf, y0, h);
    }
}
#endif

void cal_ui_draw(uint16_t *buf, int y0, int h)
{
#if !SCREEN_ROUND
    draw_square(buf, y0, h);
    return;
#endif
    if (view == V_FACE) draw_face(buf, y0, h);
    else draw_agenda(buf, y0, h);
}

// ---------------------------------------------------------------- input
static int hit_test(int x, int y)
{
    if (!SCREEN_ROUND) return ID_NONE;                  // (the square: no buttons; the list scrolls)
    if (view == V_FACE) {
        if (x >= agenda_btn.x0 - 8 && x < agenda_btn.x1 + 8 && y >= agenda_btn.y0 - 10 && y < agenda_btn.y1 + 24 && calendar_loaded()) return ID_AGENDA;
        return ID_BG;
    }
    if (x >= back_btn.x0 - 8 && x < back_btn.x1 + 8 && y >= back_btn.y0 - 10 && y < back_btn.y1 + 24) return ID_BACK;
    return ID_NONE;
}

bool cal_ui_update(int tx, int ty, int64_t now_us)
{
    now_t = time(NULL);
    if (now_us - last_copy > 500000) {               // pick up fresh data twice a second
        n_items = calendar_items(items, MAX_ITEMS);
        data_version++;
        if (!ticked_id[0] || now_us - ticked_us > 700000) {   // let a tick show briefly before it disappears
            if (ticked_id[0]) reminders_complete(ticked_id);
            ticked_id[0] = 0;
            n_rems = reminders_get(rems, MAX_REMS);
            data_version++;
        }
        rebuild_ring();
        last_copy = now_us;
        if (!SCREEN_ROUND && follow_now && !down) {
            scroll_to_now();
            int ms = max_scroll();
            if (scroll > ms) scroll = ms;
        }
    }

    bool next_screen = false;
    if (tx >= 0 && !down) {
        down = true;
        dragging = false;
        press_x = tx; press_y = ty;
        scroll_at_press = scroll;
        last_drag_y = ty;
        fling = 0;
        press_id = hit_test(tx, ty);
    } else if (tx >= 0) {
        if (abs(ty - press_y) > 14 || abs(tx - press_x) > 14) {
            if (view == V_AGENDA || (!SCREEN_ROUND && press_y >= LIST_TOP)) dragging = true, follow_now = false;
            else press_id = ID_NONE;
        }
        if (dragging) {
            scroll = scroll_at_press - (ty - press_y);
            int ms = max_scroll();
            scroll = scroll < 0 ? 0 : (scroll > ms ? ms : scroll);
            fling = fling * 0.5f + (last_drag_y - ty) * 0.5f;   // px per frame, for the glide after letting go
            last_drag_y = ty;
        }
    } else if (down) {
        down = false;
        if (!dragging) {
            if (press_id == ID_AGENDA) { view = V_AGENDA; scroll_to_now(); int ms = max_scroll(); if (scroll > ms) scroll = ms; }
            else if (press_id == ID_BACK) view = V_FACE;
            else if ((view == V_AGENDA || !SCREEN_ROUND) && press_y >= LIST_TOP && press_y < LIST_BOT && !ticked_id[0]) {
                int types[MAX_ITEMS * 2], idx[MAX_ITEMS * 2], n = agenda_rows(types, idx, MAX_ITEMS * 2), y = LIST_TOP - scroll;
                for (int r = 0; r < n; r++) {
                    int rh = row_height(types[r], idx[r]);
                    if (press_y >= y && press_y < y + rh && types[r] == 3) {
                        strlcpy(ticked_id, rems[idx[r]].id, sizeof ticked_id);
                        ticked_us = now_us;
                        last_copy = 0;                 // redraw checked now; it leaves on the next refresh
                    }
                    y += rh;
                }
            }
            else if (press_id == ID_BG) next_screen = true;   // plain tap on the face (unused for now)
        }
        if (!dragging) fling = 0;
        dragging = false;
        press_id = ID_NONE;
    }
    if (!down && fling != 0) {                         // glide, slowing down
        scroll += (int)lroundf(fling);
        fling *= 0.9f;
        int ms = max_scroll();
        if (scroll <= 0 || scroll >= ms) { scroll = scroll < 0 ? 0 : (scroll > ms ? ms : scroll); fling = 0; }
        if (fabsf(fling) < 0.5f) fling = 0;
    }
    return next_screen;
}
