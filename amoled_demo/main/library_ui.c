// Library page (right of Now Playing). Two tabs: Playlists (Liked Songs + playlists) and Podcasts
// (shows you follow -> their latest episodes, resuming where you left off). A shuffle toggle and a device
// picker sit under the tabs.
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <math.h>
#include "esp_attr.h"
#include "gfx.h"
#include "spotify.h"
#include "library_ui.h"

#define C_WHITE  rgb(255, 255, 255)
#define C_GREY   rgb(150, 150, 168)
#define C_DIM    rgb(80, 80, 96)
#define C_PANEL  rgb(28, 28, 42)
#define C_PRESS  rgb(0, 100, 60)
#define C_GREEN  rgb(30, 215, 96)

#if !SCREEN_ROUND
#define LIST_TOP list_top()         // (higher in the subpages, which have no buttons)
#define LIST_BOT H
#define ROW_H (view == V_EPISODES ? 78 : 64)   // (episodes: room for a two-line title)
#define PAD 24                      // the side margin
#else
#define LIST_TOP 154
#define LIST_BOT 410
#define ROW_H 62
#endif
#define TITLE_CHARS 16

enum { V_PLAYLISTS, V_SHOWS, V_EPISODES, V_DEVICES };
static int view, tab_view = V_PLAYLISTS;   // tab_view: where Back from devices returns
#if !SCREEN_ROUND
static int list_top(void) { return view == V_DEVICES || view == V_EPISODES ? 100 : 150; }
#endif
enum { ID_NONE, ID_SHUFFLE, ID_DEVICE, ID_BACK, ID_TAB_PL, ID_TAB_POD, ID_ROW = 100 };

typedef struct { int x0, y0, x1, y1; } box_t;
// header: a two-part tab switch, then Shuffle / device a clear gap below it
#if !SCREEN_ROUND                   // (the square: a segmented switch, and a back link top left in the subpages)
static const box_t tab_pl = {PAD, 48, CX, 84}, tab_pod = {CX, 48, W - PAD, 84},
                   shuffle_btn = {PAD, 96, CX - 6, 136}, device_btn = {CX + 6, 96, W - PAD, 136},
                   back_btn = {8, 46, 150, 88};
#else
static const box_t tab_pl = {CX - 128, 40, CX, 88}, tab_pod = {CX, 40, CX + 128, 88},
                   shuffle_btn = {CX - 166, 100, CX - 8, 144}, device_btn = {CX + 8, 100, CX + 166, 144},
                   back_btn = {CX - 85, 92, CX + 85, 140};
#endif

EXT_RAM_BSS_ATTR static sp_playlist_t pls[51];
EXT_RAM_BSS_ATTR static sp_show_t shows[30];
EXT_RAM_BSS_ATTR static sp_episode_t eps[20];
static sp_device_t devs[10];
static int n_pls = -1, n_shows = -1, n_eps = -1, n_devs;
static char eps_show[32], open_show[32], open_show_name[64];
static spotify_state_t s;
static uint32_t seen_version = 0xFFFFFFFF;

static bool down, dragging;
static int press_id, press_x, press_y, scroll, scroll_at_press;
static float fling, last_y;
static int64_t touched_us;          // a tapped row stays highlighted briefly
static int touched_row = -1;

static void fit(char *out, size_t cap, const char *in, int max_chars)
{
    char tmp[128];
    int o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o < (int)sizeof tmp - 1; p++) {
        if (*p == 0xE2 && p[1] == 0x80 && p[2]) {          // curly quotes / dashes -> ASCII
            char r = p[2] == 0x98 || p[2] == 0x99 ? '\'' : p[2] == 0x9C || p[2] == 0x9D ? '"' : p[2] == 0x93 || p[2] == 0x94 ? '-' : 0;
            if (r) { tmp[o++] = r; p += 2; continue; }
        }
        if (*p >= 0x20 && *p < 0x7F) tmp[o++] = *p;        // the font is ASCII only
    }
    tmp[o] = 0;
    if ((int)strlen(tmp) <= max_chars) { strlcpy(out, tmp, cap); return; }
    snprintf(out, cap, "%.*s..", max_chars - 2, tmp);
}

static int rows(void)
{
    switch (view) {
    case V_PLAYLISTS: return n_pls > 0 ? n_pls : 0;
    case V_SHOWS: return n_shows > 0 ? n_shows : 0;
    case V_EPISODES: return n_eps > 0 && !strcmp(eps_show, open_show) ? n_eps : 0;
    default: return n_devs;
    }
}

static int max_scroll(void)
{
    int m = rows() * ROW_H - (LIST_BOT - LIST_TOP);
    return m > 0 ? m : 0;
}

static void go(int v) { view = v; scroll = 0; fling = 0; }

bool library_ui_animating(void) { return fling != 0 || dragging; }

void library_ui_cancel_touch(void) { down = dragging = false; press_id = ID_NONE; fling = 0; }

void library_ui_enter(void)
{
    if (view == V_DEVICES) go(tab_view);
    down = false;
    fling = 0;
    spotify_command(SP_LOAD_LIBRARY);
}

// ---------------------------------------------------------------- drawing
static bool on(int x, int y, box_t b) { return x >= b.x0 - 4 && x < b.x1 + 4 && y >= b.y0 - 5 && y < b.y1 + 5; }

#if SCREEN_ROUND
static void pill(uint16_t *buf, int y0, int h, box_t b, const char *label, uint16_t bg, uint16_t fg)
{
    fill_rrect(buf, y0, h, b.x0, b.y0, b.x1, b.y1, 22, bg);
    draw_text(buf, y0, h, (b.x0 + b.x1) / 2 - text_width(label, 2) / 2, (b.y0 + b.y1) / 2 - 7, 2, label, fg);
}

static void tabs(uint16_t *buf, int y0, int h, bool podcasts, int pressed)
{
    fill_rrect(buf, y0, h, tab_pl.x0, tab_pl.y0, tab_pod.x1, tab_pod.y1, 24, C_PANEL);      // the switch
    const box_t *act = podcasts ? &tab_pod : &tab_pl;
    fill_rrect(buf, y0, h, act->x0 + 4, act->y0 + 4, act->x1 - 4, act->y1 - 4, 20, C_GREEN);
    if (pressed == ID_TAB_PL || pressed == ID_TAB_POD) {
        const box_t *p = pressed == ID_TAB_POD ? &tab_pod : &tab_pl;
        if (p != act) fill_rrect(buf, y0, h, p->x0 + 4, p->y0 + 4, p->x1 - 4, p->y1 - 4, 20, C_PRESS);
    }
    const char *lab[2] = {"Playlists", "Podcasts"};
    const box_t *bx[2] = {&tab_pl, &tab_pod};
    for (int i = 0; i < 2; i++) {
        bool on_ = (i == 1) == podcasts;
        int cx = (bx[i]->x0 + bx[i]->x1) / 2;
        draw_text(buf, y0, h, cx - text_width(lab[i], 2) / 2, (bx[i]->y0 + bx[i]->y1) / 2 - 7, 2, lab[i], on_ ? rgb(0, 30, 10) : C_GREY);
    }
}

#endif

static void fmt_min(char *out, size_t cap, int ms) { snprintf(out, cap, "%d min", (ms + 59999) / 60000); }


#if !SCREEN_ROUND
// ---------------------------------------------------------------- the square: in the style of the phone's music app
#define C_TEXT2  rgb(152, 152, 160)          // secondary text
#define C_FILL   rgb(38, 38, 42)             // buttons, the switch's track, artwork tiles
#define C_FILL_HI rgb(92, 92, 98)            // the switch's selected segment
#define C_ROW_HI rgb(44, 44, 48)             // a row under the finger
#define C_SEP    rgb(36, 36, 40)

// in cut with ".." to fit max_w px in f (ASCII only, like the font)
static void fitw(char *out, size_t cap, const char *in, const font_t *f, int max_w)
{
    fit(out, cap, in, 120);
    int n = strlen(out);
    if (ftext_width(f, out) <= max_w) return;
    while (n > 0 && ftext_width_n(f, out, n) + ftext_width(f, "..") > max_w) n--;
    while (n > 0 && out[n - 1] == ' ') n--;
    strlcpy(out + n, "..", cap - n);
}

// in on up to two lines of max_w px, broken between words, the second cut with ".." if need be
static void wrap2w(const char *in, const font_t *f, int max_w, char *l1, char *l2, size_t cap)
{
    char full[128];
    fit(full, sizeof full, in, 120);
    l2[0] = 0;
    if (ftext_width(f, full) <= max_w) { strlcpy(l1, full, cap); return; }
    int cut = 0;
    for (int i = 1; full[i]; i++)
        if (full[i] == ' ' && ftext_width_n(f, full, i) <= max_w) cut = i;
    if (!cut) { fitw(l1, cap, full, f, max_w); return; }
    snprintf(l1, cap, "%.*s", cut, full);
    fitw(l2, cap, full + cut + 1, f, max_w);
}

// "2022-08-10" -> "Aug 10, 2022"
static void nice_date(char *out, size_t cap, const char *d)
{
    static const char *mon[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int y, m, dd;
    if (sscanf(d, "%d-%d-%d", &y, &m, &dd) == 3 && m >= 1 && m <= 12) snprintf(out, cap, "%s %d, %d", mon[m - 1], dd, y);
    else snprintf(out, cap, "%.10s", d);
}

static void ftext_mid(uint16_t *buf, int y0, int h, int cx, int cy, const font_t *f, const char *t, uint16_t col)
{
    draw_ftext(buf, y0, h, cx - ftext_width(f, t) / 2, cy - f->line_h / 2, f, t, col);
}

static void circle_icon(uint16_t *buf, int y0, int h, float cx, float cy, float r, float w, uint16_t col)
{
    float p[48];
    for (int i = 0; i < 24; i++) {
        p[2 * i] = cx + r * cosf(i * (float)M_PI / 12);
        p[2 * i + 1] = cy + r * sinf(i * (float)M_PI / 12);
    }
    aa_path(buf, y0, h, p, 24, true, w, col);
}

static const float ic_chev_r[] = {9, 5, 16, 12, 9, 19}, ic_chev_l[] = {15, 5, 8, 12, 15, 19};

static void shuffle_icon(uint16_t *buf, int y0, int h, float cx, float cy, uint16_t c)
{
    static const float a[] = {2, 18, 6, 18, 14, 6, 21, 6}, ah[] = {17, 2, 21, 6, 17, 10};
    static const float b1[] = {2, 6, 6, 6, 8.7f, 10}, b2[] = {11.3f, 14, 14, 18, 21, 18}, bh[] = {17, 14, 21, 18, 17, 22};
    aa_icon(buf, y0, h, cx, cy, 20, a, 4, false, 2, c);
    aa_icon(buf, y0, h, cx, cy, 20, ah, 3, false, 2, c);
    aa_icon(buf, y0, h, cx, cy, 20, b1, 3, false, 2, c);
    aa_icon(buf, y0, h, cx, cy, 20, b2, 3, false, 2, c);
    aa_icon(buf, y0, h, cx, cy, 20, bh, 3, false, 2, c);
}

static void speaker_icon(uint16_t *buf, int y0, int h, float cx, float cy, float sz, uint16_t c)
{
    float k = sz / 24;
    aa_rrect(buf, y0, h, cx - 7 * k, cy - 10 * k, cx + 7 * k, cy + 10 * k, 2.5f * k, c);
    aa_rrect(buf, y0, h, cx - 7 * k + 1.8f, cy - 10 * k + 1.8f, cx + 7 * k - 1.8f, cy + 10 * k - 1.8f, 1.5f * k, C_FILL);
    circle_icon(buf, y0, h, cx, cy + 3 * k, 3.6f * k, 1.8f, c);
    aa_disc(buf, y0, h, cx, cy - 5.5f * k, 1.3f * k, c);
}

static void laptop_icon(uint16_t *buf, int y0, int h, float cx, float cy, float sz, uint16_t c)
{
    static const float screen[] = {5, 5, 19, 5, 19, 15, 5, 15}, base[] = {2, 19, 22, 19};
    aa_icon(buf, y0, h, cx, cy, sz, screen, 4, true, 2, c);
    aa_icon(buf, y0, h, cx, cy, sz, base, 2, false, 2, c);
}

static void phone_icon(uint16_t *buf, int y0, int h, float cx, float cy, float sz, uint16_t c)
{
    float k = sz / 24;
    aa_rrect(buf, y0, h, cx - 6 * k, cy - 10 * k, cx + 6 * k, cy + 10 * k, 2.5f * k, c);
    aa_rrect(buf, y0, h, cx - 6 * k + 1.8f, cy - 10 * k + 1.8f, cx + 6 * k - 1.8f, cy + 10 * k - 1.8f, 1.5f * k, C_FILL);
    aa_rrect(buf, y0, h, cx - 1.5f * k, cy + 6.5f * k, cx + 1.5f * k, cy + 6.5f * k + 1.6f, 0.8f, c);
}

static void note_icon(uint16_t *buf, int y0, int h, float cx, float cy, float sz, uint16_t c)
{
    static const float stem[] = {9, 17, 9, 5, 20, 3, 20, 15};
    float k = sz / 24;
    aa_icon(buf, y0, h, cx, cy, sz, stem, 4, false, 2, c);
    aa_disc(buf, y0, h, cx + (6.5f - 12) * k, cy + (17 - 12) * k, 2.8f * k, c);
    aa_disc(buf, y0, h, cx + (17.5f - 12) * k, cy + (15 - 12) * k, 2.8f * k, c);
}

static void heart_icon(uint16_t *buf, int y0, int h, float cx, float cy, float sz, uint16_t c)
{
    float p[64], k = sz / 34;
    for (int i = 0; i < 32; i++) {                    // the classic heart curve
        float t = i * 2 * (float)M_PI / 32, st = sinf(t);
        p[2 * i] = cx + 16 * st * st * st * k;
        p[2 * i + 1] = cy - (13 * cosf(t) - 5 * cosf(2 * t) - 2 * cosf(3 * t) - cosf(4 * t)) * k - 1;
    }
    aa_path(buf, y0, h, p, 32, true, 2, c);
}

// the row's artwork: its cover (art, once it's loaded), or else a rounded tile with a glyph
static void tile(uint16_t *buf, int y0, int h, int x, int y, int kind, const char *name, const uint16_t *art)
{
    const int S = SP_THUMB;
    float cx = x + S / 2.0f, cy = y + S / 2.0f;
    if (art) { aa_image(buf, y0, h, x, y, S, art, 8); return; }
    if (kind == 1) {                                  // Liked Songs: Spotify's purple tile with a heart
        aa_rrect(buf, y0, h, x, y, x + S, y + S, 8, rgb(92, 60, 210));
        heart_icon(buf, y0, h, cx, cy, 24, C_WHITE);
        return;
    }
    if (kind == 2) {                                  // a show: its initial on a quiet colour of its own
        static const uint8_t pal[6][3] = {{70, 84, 120}, {112, 72, 92}, {64, 104, 96}, {116, 92, 60}, {84, 74, 124}, {60, 96, 120}};
        unsigned hsh = 0;
        for (const char *q = name; *q; q++) hsh = hsh * 31 + (unsigned char)*q;
        const uint8_t *pc = pal[hsh % 6];
        aa_rrect(buf, y0, h, x, y, x + S, y + S, 8, rgb(pc[0], pc[1], pc[2]));
        char t[2] = {name[0] >= 'a' && name[0] <= 'z' ? name[0] - 32 : name[0], 0};
        if (t[0] < 0x21 || t[0] > 0x7E) t[0] = '#';
        ftext_mid(buf, y0, h, (int)cx, (int)cy, &font_body, t, C_WHITE);
        return;
    }
    aa_rrect(buf, y0, h, x, y, x + S, y + S, 8, C_FILL);
    if (kind == 3) {                                  // a device: by its type
        if (strcasestr(name, "computer")) laptop_icon(buf, y0, h, cx, cy, 24, C_TEXT2);
        else if (strcasestr(name, "phone")) phone_icon(buf, y0, h, cx, cy, 24, C_TEXT2);
        else speaker_icon(buf, y0, h, cx, cy, 24, C_TEXT2);
    }
    else note_icon(buf, y0, h, cx, cy, 22, C_TEXT2);
}

// a small green "now playing" mark: three bars
static void playing_mark(uint16_t *buf, int y0, int h, int x, int cy)
{
    static const int bh[3] = {10, 16, 7};
    for (int i = 0; i < 3; i++)
        aa_rrect(buf, y0, h, x + i * 6, cy + 8 - bh[i], x + i * 6 + 3.5f, cy + 8, 1.5f, C_GREEN);
}

static void button(uint16_t *buf, int y0, int h, box_t b, bool hot, uint16_t bg)
{
    aa_rrect(buf, y0, h, b.x0, b.y0, b.x1, b.y1, 12, hot ? C_ROW_HI : bg);
}

static void draw_square(uint16_t *buf, int y0, int h)
{
    char line[96], sub[64], t[16], l2[96];
    bool pressed_any = down && !dragging;
    if (!s.configured) {
        ftext_mid(buf, y0, h, CX, 214, &font_body, "Library", C_WHITE);
        ftext_mid(buf, y0, h, CX, 256, &font_label, "Connect Spotify in the Bobble app", C_TEXT2);
        return;
    }
    if (view == V_DEVICES || view == V_EPISODES) {     // a back link, and the page's title in the middle
        bool hot = pressed_any && press_id == ID_BACK;
        uint16_t c = hot ? rgb(20, 140, 64) : C_GREEN;
        const char *back = view == V_DEVICES ? "Library" : "Shows";
        aa_icon(buf, y0, h, 24, 66, 22, ic_chev_l, 3, false, 2.4f, c);
        draw_ftext(buf, y0, h, 36, 66 - font_label.line_h / 2, &font_label, back, c);
        fitw(line, sizeof line, view == V_DEVICES ? "Play on" : open_show_name, &font_label, W - 2 * 130);
        ftext_mid(buf, y0, h, CX, 66, &font_label, line, C_WHITE);
    } else {
        // segmented switch
        aa_rrect(buf, y0, h, tab_pl.x0, tab_pl.y0, tab_pod.x1, tab_pod.y1, 10, C_FILL);
        const box_t *act = view == V_SHOWS ? &tab_pod : &tab_pl;
        aa_rrect(buf, y0, h, act->x0 + 3, act->y0 + 3, act->x1 - 3, act->y1 - 3, 8, C_FILL_HI);
        const char *lab[2] = {"Playlists", "Podcasts"};
        const box_t *bx[2] = {&tab_pl, &tab_pod};
        for (int i = 0; i < 2; i++) {
            bool sel = bx[i] == act, hot = pressed_any && press_id == (i ? ID_TAB_POD : ID_TAB_PL);
            ftext_mid(buf, y0, h, (bx[i]->x0 + bx[i]->x1) / 2, (bx[i]->y0 + bx[i]->y1) / 2, &font_label, lab[i],
                      sel ? C_WHITE : hot ? rgb(210, 210, 216) : C_TEXT2);
        }
        // Shuffle: green when on, greyed for a podcast (it means nothing there)
        bool shuf_hot = pressed_any && press_id == ID_SHUFFLE;
        uint16_t sc = s.episode ? rgb(80, 80, 86) : s.shuffle ? C_GREEN : C_WHITE;
        button(buf, y0, h, shuffle_btn, shuf_hot, s.shuffle && !s.episode ? rgb(22, 52, 34) : C_FILL);
        int scx = (shuffle_btn.x0 + shuffle_btn.x1) / 2, sy = (shuffle_btn.y0 + shuffle_btn.y1) / 2;
        int sw = 20 + 8 + ftext_width(&font_label, "Shuffle");
        shuffle_icon(buf, y0, h, scx - sw / 2 + 10, sy, sc);
        draw_ftext(buf, y0, h, scx - sw / 2 + 28, sy - font_label.line_h / 2, &font_label, "Shuffle", sc);
        // the device it's playing on
        button(buf, y0, h, device_btn, pressed_any && press_id == ID_DEVICE, C_FILL);
        int dy = (device_btn.y0 + device_btn.y1) / 2;
        speaker_icon(buf, y0, h, device_btn.x0 + 22, dy, 20, C_GREEN);
        fitw(line, sizeof line, s.active && s.device[0] ? s.device : "Devices", &font_label, device_btn.x1 - device_btn.x0 - 40 - 28);
        draw_ftext(buf, y0, h, device_btn.x0 + 40, dy - font_label.line_h / 2, &font_label, line, C_WHITE);
        aa_icon(buf, y0, h, device_btn.x1 - 18, dy, 16, ic_chev_r, 3, false, 2, C_TEXT2);
    }

    int n = rows();
    bool loading = (view == V_PLAYLISTS && n_pls < 0) || (view == V_SHOWS && n_shows < 0) ||
                   (view == V_EPISODES && (n_eps < 0 || strcmp(eps_show, open_show)));
    if (loading) { ftext_mid(buf, y0, h, CX, 280, &font_label, "Loading...", C_TEXT2); return; }
    if (n == 0) {
        const char *msg = view == V_PLAYLISTS ? "No playlists" : view == V_SHOWS ? "No podcasts followed" :
                          view == V_EPISODES ? "No episodes" : "No devices found";
        ftext_mid(buf, y0, h, CX, 270, &font_label, msg, C_WHITE);
        if (view == V_DEVICES) ftext_mid(buf, y0, h, CX, 298, &font_small, "Open Spotify on a phone or computer", C_TEXT2);
        if (s.note[0]) ftext_mid(buf, y0, h, CX, 330, &font_small, s.note, rgb(255, 150, 60));
        return;
    }
    gfx_clip(LIST_TOP, LIST_BOT);
    for (int i = 0; i < n; i++) {
        int ry = LIST_TOP + i * ROW_H - scroll;
        if (ry + ROW_H <= y0 || ry >= y0 + h || ry + ROW_H <= LIST_TOP || ry >= LIST_BOT) continue;
        bool hot = (pressed_any && press_id == ID_ROW + i) || (touched_row == i), current = false, dim = false;
        int kind = 0, thumb = -1;
        const char *name = "";
        switch (view) {
        case V_PLAYLISTS:
            current = s.context_uri[0] && !strcmp(s.context_uri, pls[i].uri);
            name = pls[i].name;
            thumb = pls[i].thumb;
            if (pls[i].tracks >= 0) snprintf(sub, sizeof sub, "Playlist - %d songs", pls[i].tracks);
            else { strlcpy(sub, "Your saved songs", sizeof sub); kind = 1; }
            break;
        case V_SHOWS: {
            char uri[48];
            snprintf(uri, sizeof uri, "spotify:show:%.31s", shows[i].id);
            current = !strcmp(s.context_uri, uri);
            name = shows[i].name;
            thumb = shows[i].thumb;
            fit(sub, sizeof sub, shows[i].publisher, 60);
            kind = 2;
            break;
        }
        case V_EPISODES: {
            char day[24];
            name = eps[i].name;
            dim = eps[i].played;
            nice_date(day, sizeof day, eps[i].date);
            if (eps[i].played) snprintf(sub, sizeof sub, "%s - Played", day);
            else if (eps[i].resume_ms > 0) { fmt_min(t, sizeof t, eps[i].duration_ms - eps[i].resume_ms); snprintf(sub, sizeof sub, "%s - %s left", day, t); }
            else { fmt_min(t, sizeof t, eps[i].duration_ms); snprintf(sub, sizeof sub, "%s - %s", day, t); }
            break;
        }
        default:
            current = devs[i].active;
            name = devs[i].name;
            strlcpy(sub, devs[i].type, sizeof sub);
            kind = 3;
        }
        if (hot) fill_rect(buf, y0, h, 0, ry, W, ry + ROW_H, C_ROW_HI);
        bool tiled = view != V_EPISODES;
        int tx = tiled ? PAD + SP_THUMB + 14 : PAD, right = W - PAD - (current ? 30 : 0);
        uint16_t fg = current ? C_GREEN : dim ? rgb(100, 100, 108) : C_WHITE, fg2 = dim ? rgb(80, 80, 88) : C_TEXT2;
        if (tiled) tile(buf, y0, h, PAD, ry + (ROW_H - SP_THUMB) / 2, kind, kind == 3 ? devs[i].type : name, spotify_thumb(thumb));
        if (view == V_EPISODES) {                       // episode titles are long: two lines of small text
            wrap2w(name, &font_small, right - tx, line, l2, sizeof line);
            int tall = l2[0] ? 61 : 42, ty = ry + (ROW_H - tall) / 2;
            draw_ftext(buf, y0, h, tx, ty, &font_small, line, fg);
            if (l2[0]) draw_ftext(buf, y0, h, tx, ty + 19, &font_small, l2, fg);
            draw_ftext(buf, y0, h, tx, ty + tall - 19, &font_small, sub, fg2);
        } else {
            fitw(line, sizeof line, name, &font_label, right - tx);
            draw_ftext(buf, y0, h, tx, ry + 10, &font_label, line, fg);
            fitw(l2, sizeof l2, sub, &font_small, right - tx);
            draw_ftext(buf, y0, h, tx, ry + 35, &font_small, l2, fg2);
        }
        if (current && view == V_DEVICES) {             // the device in use: a check mark
            static const float chk[] = {5, 12.5f, 10, 17.5f, 19, 7};
            aa_icon(buf, y0, h, W - PAD - 10, ry + ROW_H / 2, 20, chk, 3, false, 2.4f, C_GREEN);
        } else if (current) {
            playing_mark(buf, y0, h, W - PAD - 16, ry + ROW_H / 2);
        }
        if (!hot && i + 1 < n) fill_rect(buf, y0, h, tx, ry + ROW_H - 1, W, ry + ROW_H, C_SEP);   // hairline
    }
    gfx_clip(0, H);
    int ms = max_scroll();
    if (ms > 0 && (down || fling != 0)) {              // scroll bar, only while it moves (as on a phone)
        int track = LIST_BOT - LIST_TOP - 16, total = ms + (LIST_BOT - LIST_TOP);
        int th = track * (LIST_BOT - LIST_TOP) / total, ty = LIST_TOP + 8 + (track - th) * scroll / ms;
        aa_rrect(buf, y0, h, W - 7, ty, W - 3, ty + th, 2, rgb(110, 110, 118));
    }
}
#endif

void library_ui_draw(uint16_t *buf, int y0, int h)
{
#if !SCREEN_ROUND
    draw_square(buf, y0, h);
#else
    char line[64], sub[48], t[16];
    bool pressed_any = down && !dragging;
    if (!s.configured) {
        text_c(buf, y0, h, 190, 3, "Library", C_GREEN);
        text_c(buf, y0, h, 236, 2, "Connect Spotify first", C_GREY);
        return;
    }
    if (view == V_DEVICES) {
        text_c(buf, y0, h, 52, 3, "Play on", C_WHITE);
        pill(buf, y0, h, back_btn, "Back", pressed_any && press_id == ID_BACK ? C_PRESS : C_PANEL, C_WHITE);
    } else if (view == V_EPISODES) {
        fit(line, sizeof line, open_show_name, 20);
        text_c(buf, y0, h, 56, 2, line, C_WHITE);
        pill(buf, y0, h, back_btn, "< Shows", pressed_any && press_id == ID_BACK ? C_PRESS : C_PANEL, C_WHITE);
    } else {
        tabs(buf, y0, h, view == V_SHOWS, pressed_any ? press_id : ID_NONE);
        if (s.episode)                                    // shuffle means nothing for a podcast episode
            pill(buf, y0, h, shuffle_btn, "Shuffle", rgb(20, 20, 30), C_DIM);
        else
            pill(buf, y0, h, shuffle_btn, "Shuffle",
                 pressed_any && press_id == ID_SHUFFLE ? C_PRESS : (s.shuffle ? C_GREEN : C_PANEL), s.shuffle ? rgb(0, 30, 10) : C_WHITE);
        fit(sub, sizeof sub, s.active && s.device[0] ? s.device : "Devices", 9);
        snprintf(line, sizeof line, "%s >", sub);
        pill(buf, y0, h, device_btn, line, pressed_any && press_id == ID_DEVICE ? C_PRESS : C_PANEL, C_WHITE);
    }

    int n = rows();
    bool loading = (view == V_PLAYLISTS && n_pls < 0) || (view == V_SHOWS && n_shows < 0) ||
                   (view == V_EPISODES && (n_eps < 0 || strcmp(eps_show, open_show)));
    if (loading) { text_c(buf, y0, h, 240, 2, "Loading...", C_GREY); return; }
    if (n == 0) {
        const char *msg = view == V_PLAYLISTS ? "No playlists" : view == V_SHOWS ? "No podcasts followed" :
                          view == V_EPISODES ? "No episodes" : "No devices found";
        text_c(buf, y0, h, 230, 2, msg, C_GREY);
        if (view == V_DEVICES) text_c(buf, y0, h, 256, 2, "Open Spotify on a device", C_DIM);
        if (s.note[0]) text_c(buf, y0, h, 290, 2, s.note, rgb(255, 150, 60));
        return;
    }
    gfx_clip(LIST_TOP, LIST_BOT);
    for (int i = 0; i < n; i++) {
        int ry = LIST_TOP + i * ROW_H - scroll;
        if (ry + ROW_H <= y0 || ry >= y0 + h || ry + ROW_H <= LIST_TOP || ry >= LIST_BOT) continue;
        bool hot = (pressed_any && press_id == ID_ROW + i) || (touched_row == i), current = false, dim = false;
        switch (view) {
        case V_PLAYLISTS:
            current = s.context_uri[0] && !strcmp(s.context_uri, pls[i].uri);
            fit(line, sizeof line, pls[i].name, TITLE_CHARS);
            if (pls[i].tracks >= 0) snprintf(sub, sizeof sub, "%d songs", pls[i].tracks);
            else strlcpy(sub, "Your saved songs", sizeof sub);
            break;
        case V_SHOWS: {
            char uri[48];
            snprintf(uri, sizeof uri, "spotify:show:%.31s", shows[i].id);
            current = !strcmp(s.context_uri, uri);
            fit(line, sizeof line, shows[i].name, TITLE_CHARS);
            fit(sub, sizeof sub, shows[i].publisher, 24);
            break;
        }
        case V_EPISODES:
            fit(line, sizeof line, eps[i].name, TITLE_CHARS);
            dim = eps[i].played;
            if (eps[i].played) snprintf(sub, sizeof sub, "%.10s  Played", eps[i].date);
            else if (eps[i].resume_ms > 0) { fmt_min(t, sizeof t, eps[i].duration_ms - eps[i].resume_ms); snprintf(sub, sizeof sub, "%.10s  %s left", eps[i].date, t); }
            else { fmt_min(t, sizeof t, eps[i].duration_ms); snprintf(sub, sizeof sub, "%.10s  %s", eps[i].date, t); }
            break;
        default:
            current = devs[i].active;
            fit(line, sizeof line, devs[i].name, TITLE_CHARS);
            strlcpy(sub, devs[i].type, sizeof sub);
        }
        if (current) strlcat(sub, view == V_DEVICES ? "  - active" : "  - playing", sizeof sub);
        if (hot || current) fill_rrect(buf, y0, h, CX - 162, ry + 3, CX + 162, ry + ROW_H - 3, 14, hot ? C_PRESS : C_PANEL);
        uint16_t fg = current ? C_GREEN : (dim ? C_DIM : C_WHITE);
        if (view == V_EPISODES) {                           // episode titles are long: two lines of medium text
            char l1[40], l2[40], full[128];
            fit(full, sizeof full, eps[i].name, 120);
            l2[0] = 0;
            if ((int)strlen(full) <= 24) strlcpy(l1, full, sizeof l1);   // fits on one line
            else {
                int cut = 24;
                while (cut > 12 && full[cut] != ' ') cut--;
                if (full[cut] != ' ') cut = 24;
                snprintf(l1, sizeof l1, "%.*s", cut, full);
                fit(l2, sizeof l2, full + cut + (full[cut] == ' '), 24);
            }
            draw_text(buf, y0, h, CX - 146, ry + 6, 2, l1, fg);
            draw_text(buf, y0, h, CX - 146, ry + 24, 2, l2, fg);
            draw_text(buf, y0, h, CX - 146, ry + 43, 2, sub, current ? C_GREEN : (dim ? C_DIM : C_GREY));
        } else {
            draw_text(buf, y0, h, CX - 146, ry + 10, 3, line, fg);
            draw_text(buf, y0, h, CX - 146, ry + 38, 2, sub, current ? C_GREEN : (dim ? C_DIM : C_GREY));
        }
    }
    gfx_clip(0, H);
    int ms = max_scroll();
    if (ms > 0) {                                         // scroll bar
        int track = LIST_BOT - LIST_TOP - 16, total = ms + (LIST_BOT - LIST_TOP);
        int th = track * (LIST_BOT - LIST_TOP) / total, ty = LIST_TOP + 8 + (track - th) * scroll / ms;
        fill_rrect(buf, y0, h, CX + 168, ty, CX + 173, ty + th, 2, C_DIM);
    }
#endif
}

// ---------------------------------------------------------------- input
static int hit(int x, int y)
{
    if (view == V_PLAYLISTS || view == V_SHOWS) {
        if (on(x, y, tab_pl) && x < CX) return ID_TAB_PL;
        if (on(x, y, tab_pod) && x >= CX) return ID_TAB_POD;
        if (on(x, y, shuffle_btn)) return s.episode ? ID_NONE : ID_SHUFFLE;
        if (on(x, y, device_btn)) return ID_DEVICE;
    } else if (on(x, y, back_btn)) {
        return ID_BACK;
    }
    if (y >= LIST_TOP && y < LIST_BOT) {
        int i = (y - LIST_TOP + scroll) / ROW_H;
        if (i < rows()) return ID_ROW + i;
    }
    return ID_NONE;
}

static void tap(int id, int64_t now_us)
{
    if (id == ID_TAB_PL) { go(V_PLAYLISTS); tab_view = view; return; }
    if (id == ID_TAB_POD) { go(V_SHOWS); tab_view = view; return; }
    if (id == ID_SHUFFLE) { spotify_command(SP_SHUFFLE); return; }
    if (id == ID_DEVICE) { go(V_DEVICES); spotify_command_arg(SP_LOAD_LIBRARY, "devices"); return; }
    if (id == ID_BACK) { go(view == V_EPISODES ? V_SHOWS : tab_view); return; }
    if (id < ID_ROW) return;
    int i = id - ID_ROW;
    touched_row = i;
    touched_us = now_us;
    char arg[96];
    switch (view) {
    case V_PLAYLISTS: spotify_command_arg(SP_PLAY_CONTEXT, pls[i].uri); break;
    case V_SHOWS:                                         // open the show's episodes
        strlcpy(open_show, shows[i].id, sizeof open_show);
        strlcpy(open_show_name, shows[i].name, sizeof open_show_name);
        spotify_command_arg(SP_LOAD_EPISODES, shows[i].id);
        go(V_EPISODES);
        touched_row = -1;
        break;
    case V_EPISODES:                                      // play, resuming unless it was finished
        snprintf(arg, sizeof arg, "%s %s %d", open_show, eps[i].id, eps[i].played ? 0 : eps[i].resume_ms);
        spotify_command_arg(SP_PLAY_EPISODE, arg);
        break;
    case V_DEVICES:
        spotify_command_arg(SP_TRANSFER, devs[i].id);
        go(tab_view);
        break;
    }
}

void library_ui_update(int tx, int ty, int64_t now_us)
{
    spotify_get(&s);
    uint32_t v = spotify_library_version();
    if (v != seen_version) {
        seen_version = v;
        n_pls = spotify_playlists(pls, 51);
        n_shows = spotify_shows(shows, 30);
        n_eps = spotify_episodes(eps, 20, eps_show, sizeof eps_show);
        if (v == 0) n_pls = n_shows = -1;                 // nothing fetched yet
        n_devs = spotify_devices(devs, 10);
        int ms = max_scroll();
        if (scroll > ms) scroll = ms;
    }
    if (touched_row >= 0 && now_us - touched_us > 400000) touched_row = -1;

    if (tx >= 0 && !down) {
        down = true;
        dragging = false;
        press_x = tx; press_y = ty;
        scroll_at_press = scroll;
        last_y = ty;
        fling = 0;
        press_id = hit(tx, ty);
    } else if (tx >= 0) {
        if (abs(ty - press_y) > 14 || abs(tx - press_x) > 14) {
            if (press_y >= LIST_TOP && press_y < LIST_BOT) dragging = true;
            press_id = ID_NONE;
        }
        if (dragging) {
            int ms = max_scroll();
            scroll = scroll_at_press - (ty - press_y);
            scroll = scroll < 0 ? 0 : (scroll > ms ? ms : scroll);
            fling = fling * 0.5f + (last_y - ty) * 0.5f;
            last_y = ty;
        }
    } else if (down) {
        down = false;
        if (!dragging) {
            tap(press_id, now_us);
            fling = 0;
        }
        dragging = false;
        press_id = ID_NONE;
    }
    if (!down && fling != 0) {                            // glide after a flick
        scroll += (int)lroundf(fling);
        fling *= 0.9f;
        int ms = max_scroll();
        if (scroll <= 0 || scroll >= ms) { scroll = scroll < 0 ? 0 : (scroll > ms ? ms : scroll); fling = 0; }
        if (fabsf(fling) < 0.5f) fling = 0;
    }
}

bool library_ui_dirty(void)
{
    static uint32_t seen;
    static int seen_view = -1;
    static bool seen_shuffle;
    static char seen_ctx[96], seen_dev[48];
    bool d = fling != 0 || down || touched_row >= 0 || seen != seen_version || seen_view != view ||
             seen_shuffle != s.shuffle || strcmp(seen_ctx, s.context_uri) || strcmp(seen_dev, s.device);
    seen = seen_version;
    seen_view = view;
    seen_shuffle = s.shuffle;
    strlcpy(seen_ctx, s.context_uri, sizeof seen_ctx);
    strlcpy(seen_dev, s.device, sizeof seen_dev);
    return d;
}
