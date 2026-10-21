// Library page (right of Now Playing). Two tabs: Playlists (Liked Songs + playlists) and Podcasts
// (shows you follow -> their latest episodes, resuming where you left off). A shuffle toggle and a device
// picker sit under the tabs.
#include <stdio.h>
#include <string.h>
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

#define LIST_TOP 154
#define LIST_BOT 410
#define ROW_H 62
#define TITLE_CHARS 16

enum { V_PLAYLISTS, V_SHOWS, V_EPISODES, V_DEVICES };
static int view, tab_view = V_PLAYLISTS;   // tab_view: where Back from devices returns
enum { ID_NONE, ID_SHUFFLE, ID_DEVICE, ID_BACK, ID_TAB_PL, ID_TAB_POD, ID_ROW = 100 };

typedef struct { int x0, y0, x1, y1; } box_t;
// header: a two-part tab switch, then Shuffle / device a clear gap below it
static const box_t tab_pl = {CX - 128, 40, CX, 88}, tab_pod = {CX, 40, CX + 128, 88},
                   shuffle_btn = {CX - 166, 100, CX - 8, 144}, device_btn = {CX + 8, 100, CX + 166, 144},
                   back_btn = {CX - 85, 92, CX + 85, 140};

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

static void fmt_min(char *out, size_t cap, int ms) { snprintf(out, cap, "%d min", (ms + 59999) / 60000); }


void library_ui_draw(uint16_t *buf, int y0, int h)
{
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
