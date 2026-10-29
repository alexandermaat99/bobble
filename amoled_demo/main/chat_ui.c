// The chat page: talking with Claude through the square board's speaker and microphones (chat.c does the talking).
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "esp_attr.h"
#include "gfx.h"
#include "chat.h"
#include "pet_ui.h"
#include "chat_ui.h"

#define C_WHITE  rgb(255, 255, 255)
#define C_GREY   rgb(150, 150, 160)
#define C_DIM    rgb(90, 90, 100)
#define C_BTN    rgb(44, 44, 50)
#define C_BTN_DN rgb(78, 78, 86)
#define C_RED    rgb(255, 69, 58)
#define C_PANEL  rgb(30, 30, 34)
#define C_LINE   rgb(54, 54, 60)
#define C_BLUE   rgb(10, 132, 255)
static const int claude[3] = {217, 119, 87};          // Claude's orange
#define C_CLAUDE rgb(217, 119, 87)

#define BTN_Y 392
#define MIC_R 48
#define SIDE_R 32
#define SIDE_DX 124
#define HOLD_US 500000                                 // holding the speaker this long opens the menu

enum { B_NONE, B_MIC, B_SPEAKER, B_CLOSE, B_MENU_ROW = 100 };

EXT_RAM_BSS_ATTR static chat_status_t cs;          // (PSRAM: internal RAM is short)
static uint32_t seen_version = 0xFFFFFFFF;
static bool dirty = true;
static int pressed = B_NONE;                           // what the finger went down on
static int64_t pressed_at;
static bool held;                                       // the speaker was held: the menu opened, the tap doesn't count
static bool menu;
static int menu_press = -1;
static float t_anim;                                    // seconds, for the thinking dots
static float level;                                     // the halo round the mic (eased)

// What's shown, wrapped to the glass once whenever it changes (draw just copies it out, strip by strip)
#define LINE_CAP 72
#define HEARD_LINES 2
#define SAID_LINES 3                                    // (40 px each: the rest of a long reply rolls up)
EXT_RAM_BSS_ATTR static char heard_l[HEARD_LINES][LINE_CAP], said_l[SAID_LINES][LINE_CAP];
static int n_heard, n_said;

// ---------------------------------------------------------------- text

// ASCII only (the fonts have nothing else): curly quotes and dashes become plain ones, anything else goes
static void plain(char *out, size_t cap, const char *in)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 1 < cap; p++) {
        if (*p == 0xE2 && p[1] == 0x80 && p[2]) {
            char r = p[2] == 0x98 || p[2] == 0x99 ? '\'' : p[2] == 0x9C || p[2] == 0x9D ? '"' : p[2] == 0x93 || p[2] == 0x94 ? '-' : 0;
            p += 2;
            if (r) out[o++] = r;
            continue;
        }
        if (*p == '\n' || *p == '\t') out[o++] = ' ';
        else if (*p >= 0x20 && *p < 0x7F) out[o++] = *p;
    }
    out[o] = 0;
}

// Word-wrap into lines max_w wide; keeps the last max lines (a long reply rolls up as it's spoken). How many.
static int wrap(const font_t *f, const char *text, int max_w, char lines[][LINE_CAP], int max)
{
    char s[512];
    plain(s, sizeof s, text);
    char all[24][LINE_CAP];
    int n = 0;
    const char *p = s;
    while (*p && n < 24) {
        while (*p == ' ') p++;
        if (!*p) break;
        int len = 0, fit = 0;                          // fit: the longest prefix ending at a word break that fits
        while (p[len]) {
            len++;
            if ((p[len] == ' ' || !p[len]) && ftext_width_n(f, p, len) <= max_w) fit = len;
            if (ftext_width_n(f, p, len) > max_w || len >= LINE_CAP - 1) break;
        }
        if (!fit) {                                    // one word wider than the line: cut it
            fit = len > 1 ? len - 1 : 1;
            if (fit > LINE_CAP - 1) fit = LINE_CAP - 1;
        }
        memcpy(all[n], p, fit);
        all[n++][fit] = 0;
        p += fit;
    }
    int from = n > max ? n - max : 0;
    for (int i = from; i < n; i++) strcpy(lines[i - from], all[i]);
    return n - from;
}

static void refresh(void)
{
    chat_get(&cs);
    if (cs.version == seen_version) return;
    seen_version = cs.version;
    n_heard = wrap(&font_label, cs.heard, W - 80, heard_l, HEARD_LINES);
    n_said = wrap(&font_body, cs.said, W - 64, said_l, SAID_LINES);
    dirty = true;
}

static void text_mid(uint16_t *buf, int y0, int h, int y, const font_t *f, const char *s, uint16_t col)
{
    draw_ftext(buf, y0, h, CX - ftext_width(f, s) / 2, y, f, s, col);
}

// ---------------------------------------------------------------- the buttons' pictures

static void line(uint16_t *buf, int y0, int h, float ax, float ay, float bx, float by, float w, uint16_t col)
{
    float p[4] = {ax, ay, bx, by};
    aa_path(buf, y0, h, p, 2, false, w, col);
}

// A slash through a glyph, with a gap cut round it in the button's colour
static void slash(uint16_t *buf, int y0, int h, float cx, float cy, float s, uint16_t fg, uint16_t bg)
{
    line(buf, y0, h, cx - 15 * s, cy - 17 * s, cx + 15 * s, cy + 17 * s, 7 * s, bg);
    line(buf, y0, h, cx - 15 * s, cy - 17 * s, cx + 15 * s, cy + 17 * s, 3 * s, fg);
}

static void mic_glyph(uint16_t *buf, int y0, int h, float cx, float cy, float s, uint16_t col)
{
    aa_rrect(buf, y0, h, cx - 7 * s, cy - 19 * s, cx + 7 * s, cy + 3 * s, 7 * s, col);   // the capsule
    float arc[2 * 13];
    for (int i = 0; i <= 12; i++) {                   // the cradle: half a circle under it
        float a = (float)M_PI * i / 12;
        arc[2 * i] = cx - 12.5f * s * cosf(a);
        arc[2 * i + 1] = cy - 5 * s + 12.5f * s * sinf(a);
    }
    aa_path(buf, y0, h, arc, 13, false, 2.8f * s, col);
    line(buf, y0, h, cx, cy + 7.5f * s, cx, cy + 15 * s, 2.8f * s, col);               // stand
    line(buf, y0, h, cx - 7 * s, cy + 15 * s, cx + 7 * s, cy + 15 * s, 2.8f * s, col);  // foot
}

static void speaker_glyph(uint16_t *buf, int y0, int h, float cx, float cy, float s, uint16_t col)
{
    float bx = cx - 6 * s;                             // (the whole thing sits a little left of centre)
    aa_rrect(buf, y0, h, bx - 12 * s, cy - 5 * s, bx - 5 * s, cy + 5 * s, 1.5f * s, col);
    fill_tri(buf, y0, h, bx - 6 * s, cy - 5 * s, bx + 3 * s, cy - 12 * s, bx + 3 * s, cy + 12 * s, col);
    fill_tri(buf, y0, h, bx - 6 * s, cy - 5 * s, bx + 3 * s, cy + 12 * s, bx - 6 * s, cy + 5 * s, col);
    float cone[8] = {bx - 6 * s, cy - 5 * s, bx + 3 * s, cy - 12 * s, bx + 3 * s, cy + 12 * s, bx - 6 * s, cy + 5 * s};
    aa_path(buf, y0, h, cone, 4, true, 1.6f * s, col);   // (smooths the triangles' edges)
    for (int k = 0; k < 2; k++) {                      // two waves
        float r = (k ? 15 : 9) * s, pts[2 * 9];
        for (int i = 0; i <= 8; i++) {
            float a = -0.8f + 1.6f * i / 8;
            pts[2 * i] = bx + 3 * s + r * cosf(a);
            pts[2 * i + 1] = cy + r * sinf(a);
        }
        aa_path(buf, y0, h, pts, 9, false, 2.6f * s, col);
    }
}

// ---------------------------------------------------------------- the menu (hold the speaker)

#define MENU_W 320
#define ROW_H 54
static int menu_rows(void) { return cs.n_models + 1; }   // the models, then New chat
static int menu_top(void) { return 230 - (menu_rows() * ROW_H + 40) / 2; }

static int menu_hit(int x, int y)
{
    int top = menu_top() + 40;
    if (x < CX - MENU_W / 2 || x >= CX + MENU_W / 2 || y < top) return -2;   // outside: close
    int r = (y - top) / ROW_H;
    return r < menu_rows() ? r : -2;
}

static void draw_menu(uint16_t *buf, int y0, int h)
{
    int top = menu_top(), bot = top + 40 + menu_rows() * ROW_H, x0 = CX - MENU_W / 2, x1 = CX + MENU_W / 2;
    aa_rrect(buf, y0, h, x0, top, x1, bot, 18, C_PANEL);
    draw_ftext(buf, y0, h, x0 + 22, top + 12, &font_small, "MODEL", C_GREY);
    for (int i = 0; i < menu_rows(); i++) {
        int ry = top + 40 + i * ROW_H;
        if (i == menu_press) aa_rrect(buf, y0, h, x0 + 6, ry + 3, x1 - 6, ry + ROW_H - 3, 12, C_BTN_DN);
        if (i > 0) fill_rect(buf, y0, h, x0 + 22, ry, x1 - 22, ry + 1, C_LINE);
        int ty = ry + (ROW_H - font_body.line_h) / 2;
        if (i < cs.n_models) {
            draw_ftext(buf, y0, h, x0 + 22, ty, &font_body, cs.models[i], C_WHITE);
            if (i == cs.model) {                       // a tick
                float p[6] = {x1 - 48, ry + 27, x1 - 40, ry + 35, x1 - 26, ry + 19};
                aa_path(buf, y0, h, p, 3, false, 3.2f, C_BLUE);
            }
        } else {
            draw_ftext(buf, y0, h, x0 + 22, ty, &font_body, "New chat", C_BLUE);
        }
    }
}

// ---------------------------------------------------------------- drawing

void chat_ui_draw(uint16_t *buf, int y0, int h)
{
    // the model, top centre, with Claude's spark
    if (cs.n_models && 48 < y0 + h && 90 > y0) {
        char m[40];
        snprintf(m, sizeof m, "%s", cs.models[cs.model]);
        int tw = ftext_width(&font_label, m), x = CX - (tw + 30) / 2;
        pet_ui_spark(buf, y0, h, x + 10, 68, claude[0], claude[1], claude[2], 0.45f);
        draw_ftext(buf, y0, h, x + 30, 68 - font_label.line_h / 2, &font_label, m, C_GREY);
    }

    // the conversation: what you said (grey), then what Claude's saying (white)
    if (!cs.active && !cs.said[0]) {
        pet_ui_spark(buf, y0, h, CX, 190, claude[0], claude[1], claude[2], 1.6f);
        const char *hint = !cs.mac ? "Open Bobble on your Mac to talk" : "Tap the mic to talk to Claude";
        text_mid(buf, y0, h, 262, &font_label, hint, C_GREY);
    } else {
        int y = 104;
        for (int i = 0; i < n_heard; i++, y += font_label.line_h) text_mid(buf, y0, h, y, &font_label, heard_l[i], C_GREY);
        if (n_heard) y += 14;
        for (int i = 0; i < n_said; i++, y += font_body.line_h) text_mid(buf, y0, h, y, &font_body, said_l[i], C_WHITE);
    }

    // the line over the buttons: what's going on
    const char *note = cs.status[0] ? cs.status
                     : !cs.active ? ""
                     : cs.muted ? "You're muted"
                     : cs.state == CS_THINKING ? "Thinking"
                     : cs.state == CS_LISTENING ? "Listening" : "";
    if (*note) {
        char n[96];
        plain(n, sizeof n, note);
        text_mid(buf, y0, h, 294, &font_small, n, cs.status[0] ? rgb(255, 159, 10) : C_DIM);
    }

    if (BTN_Y + MIC_R + 40 < y0 || BTN_Y - MIC_R - 40 > y0 + h) { if (menu) draw_menu(buf, y0, h); return; }

    // the mic: grey before a chat, Claude's orange while it listens (a halo that grows with your voice, or with
    // Claude's while it speaks), white with a slash when muted
    bool live = cs.active && !cs.muted;
    if (cs.active && level > 0.02f) {
        float k = 0.18f + 0.3f * level;
        aa_disc(buf, y0, h, CX, BTN_Y, MIC_R + 6 + 26 * level, rgb(claude[0] * k, claude[1] * k, claude[2] * k));
    }
    if (cs.active && cs.state == CS_THINKING && !cs.muted)   // three dots going round while it thinks
        for (int i = 0; i < 3; i++) {
            float a = t_anim * 3.2f + i * 2.094f;
            aa_disc(buf, y0, h, CX + (MIC_R + 14) * cosf(a), BTN_Y + (MIC_R + 14) * sinf(a), 4, C_CLAUDE);
        }
    uint16_t mic_bg = cs.muted ? C_WHITE : live ? C_CLAUDE : pressed == B_MIC ? C_BTN_DN : C_BTN;
    if (pressed == B_MIC && (live || cs.muted)) mic_bg = cs.muted ? rgb(210, 210, 214) : rgb(190, 100, 72);
    aa_disc(buf, y0, h, CX, BTN_Y, MIC_R, mic_bg);
    mic_glyph(buf, y0, h, CX, BTN_Y + 2, 1.35f, cs.muted ? rgb(20, 20, 24) : C_WHITE);
    if (cs.muted) slash(buf, y0, h, CX, BTN_Y + 1, 1.35f, rgb(20, 20, 24), C_WHITE);

    // the speaker (white with a slash when Claude's muted)
    float sx = CX + SIDE_DX;
    uint16_t sp_bg = cs.speaker_muted ? (pressed == B_SPEAKER ? rgb(210, 210, 214) : C_WHITE) : pressed == B_SPEAKER ? C_BTN_DN : C_BTN;
    aa_disc(buf, y0, h, sx, BTN_Y, SIDE_R, sp_bg);
    uint16_t sp_fg = cs.speaker_muted ? rgb(20, 20, 24) : C_WHITE;
    speaker_glyph(buf, y0, h, sx + 2, BTN_Y, 0.9f, sp_fg);
    if (cs.speaker_muted) slash(buf, y0, h, sx, BTN_Y, 0.9f, sp_fg, sp_bg);

    // the X: ends the chat
    if (cs.active) {
        float xx = CX - SIDE_DX;
        aa_disc(buf, y0, h, xx, BTN_Y, SIDE_R, pressed == B_CLOSE ? rgb(200, 50, 42) : C_RED);
        line(buf, y0, h, xx - 9, BTN_Y - 9, xx + 9, BTN_Y + 9, 3.4f, C_WHITE);
        line(buf, y0, h, xx - 9, BTN_Y + 9, xx + 9, BTN_Y - 9, 3.4f, C_WHITE);
    }
    if (menu) draw_menu(buf, y0, h);
}

// ---------------------------------------------------------------- touch

static int hit(int x, int y)
{
    float dy = y - BTN_Y, dm = hypotf(x - CX, dy), ds = hypotf(x - (CX + SIDE_DX), dy), dc = hypotf(x - (CX - SIDE_DX), dy);
    if (dm < MIC_R + 14) return B_MIC;
    if (ds < SIDE_R + 16) return B_SPEAKER;
    if (cs.active && dc < SIDE_R + 16) return B_CLOSE;
    return B_NONE;
}

void chat_ui_cancel_touch(void)
{
    if (pressed || menu_press >= 0) dirty = true;
    pressed = B_NONE;
    menu_press = -1;
    held = false;
}

void chat_ui_leave(void)
{
    chat_ui_cancel_touch();
    menu = false;
    chat_close();
    dirty = true;
}

void chat_ui_update(int tx, int ty, int64_t now)
{
    static int64_t last;
    static bool was_down;
    float dt = last ? (now - last) / 1e6f : 0;
    last = now;
    refresh();
    bool down = tx >= 0;

    if (menu) {
        int r = down ? menu_hit(tx, ty) : -1;
        if (down && !was_down) { menu_press = r; dirty = true; }
        if (down && menu_press >= 0 && r != menu_press) { menu_press = -1; dirty = true; }   // slid off the row
        if (!down && was_down) {
            if (menu_press >= 0 && menu_press < cs.n_models) chat_pick_model(menu_press);
            else if (menu_press == cs.n_models) chat_new();
            if (menu_press != -1 || !held) menu = false;   // (a tap on a row, or outside it, closes it)
            menu_press = -1;
            held = false;
            dirty = true;
        }
    } else {
        if (down && !was_down) { pressed = hit(tx, ty); pressed_at = now; held = false; if (pressed) dirty = true; }
        if (down && pressed && hit(tx, ty) != pressed) { pressed = B_NONE; dirty = true; }   // slid off
        if (down && pressed == B_SPEAKER && !held && now - pressed_at > HOLD_US && cs.n_models) {
            held = true;                               // held: the menu (and no mute when the finger lifts)
            menu = true;
            pressed = B_NONE;
            dirty = true;
        }
        if (!down && was_down && pressed) {
            if (pressed == B_MIC) chat_tap_mic();
            else if (pressed == B_SPEAKER) chat_toggle_speaker();
            else if (pressed == B_CLOSE) chat_close();
            pressed = B_NONE;
            dirty = true;
        }
    }
    was_down = down;

    // the halo follows the sound; the dots go round while it thinks
    float target = chat_level();
    float nl = level + (target - level) * (dt > 0 ? fminf(1, dt * 14) : 1);
    if (fabsf(nl - level) > 0.004f || (level > 0 && target == 0)) { level = nl < 0.003f ? 0 : nl; dirty = true; }
    if (cs.active && cs.state == CS_THINKING) { t_anim += dt; dirty = true; }
}

bool chat_ui_dirty(void)
{
    bool d = dirty;
    dirty = false;
    return d;
}

bool chat_ui_animating(void) { return cs.active && (level > 0 || cs.state == CS_THINKING); }
