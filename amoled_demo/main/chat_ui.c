// The chat page: talking with Claude through the square board's speaker and microphones (chat.c does the talking).
// Styled after the iPhone's call screen and Siri: on black, an orb in Claude's orange that shows what's going on
// (breathing while it listens, swelling with its voice, pulsing while it thinks, grey while you're muted), the words
// under it, and round buttons with their names beneath: speaker, mute, end.
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "esp_attr.h"
#include "gfx.h"
#include "chat.h"
#include "pet_ui.h"
#include "chat_ui.h"

// iOS's dark colours
#define C_WHITE   rgb(255, 255, 255)
#define C_LABEL2  rgb(152, 152, 159)                   // secondary label
#define C_LABEL3  rgb(99, 99, 104)                     // tertiary
#define C_FILL    rgb(44, 44, 46)                      // a button
#define C_FILL_DN rgb(72, 72, 76)                      // ...pressed
#define C_ON_DN   rgb(205, 205, 210)                   // a lit button, pressed
#define C_INK     rgb(18, 18, 20)                      // a glyph on a lit button
#define C_RED     rgb(255, 59, 48)
#define C_RED_DN  rgb(200, 45, 37)
#define C_ORANGE  rgb(255, 159, 10)                    // a warning
#define C_BLUE    rgb(10, 132, 255)
#define C_SHEET   rgb(37, 37, 39)
#define C_SEP     rgb(60, 60, 64)
#define C_CLAUDE  rgb(217, 119, 87)
#define C_CLAUDE_DN rgb(180, 96, 68)

#define ORB_Y 154
#define ORB_R 42
#define ORB_GROW 14                                     // how much bigger it gets with the sound
#define ORB_BOX 96                                      // the orb at its biggest, glow and all (r * 1.65)
#define TEXT_TOP 238                                    // what you said (or what's wrong), then the reply, between
#define TEXT_BOT 368                                    // the orb and the buttons
#define BTN_Y 406
#define MIC_R 34
#define SIDE_R 30
#define SIDE_DX 112
#define LABEL_Y (BTN_Y + MIC_R + 10)
#define PILL_Y 50
#define PILL_H 32
#define HOLD_US 500000                                  // holding the speaker this long opens the menu

enum { B_NONE, B_MIC, B_SPEAKER, B_CLOSE, B_PILL };

EXT_RAM_BSS_ATTR static chat_status_t cs;              // (PSRAM: internal RAM is short)
static uint32_t seen_version = 0xFFFFFFFF;
static bool dirty = true;                               // something changed: the whole page again
static bool anim_dirty;                                 // just the orb moved
static int pressed = B_NONE;                           // what the finger went down on
static int64_t pressed_at;
static bool held;                                       // the speaker was held: the menu opened, the tap doesn't count
static bool menu;
static int menu_press = -1;
static float t_anim;                                    // seconds, for the orb's breathing and pulse
static float level;                                     // how loud it is (eased): the orb grows with it

// What's shown, wrapped to the glass once whenever it changes (draw just copies it out, strip by strip)
#define LINE_CAP 72
#define SAID_LINES 3                                    // (the rest of a long reply rolls up as it's spoken)
#define HEARD_LINES 5                                   // what you said: all of it while there's no reply, else 3 lines
EXT_RAM_BSS_ATTR static char heard_l[HEARD_LINES][LINE_CAP], said_l[SAID_LINES][LINE_CAP];
static int said_top = TEXT_TOP;                         // where the reply starts (under what you said)
static int n_heard, n_said;
static const font_t *said_f = &font_body;               // the reply's size: large, or smaller when it's long
static bool heard_cut;                                  // what you said is longer than its lines: "..." before it

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
    // what you said, wrapped: all of it (5 lines) till Claude answers, then 3 lines; longer, its end with "..." first
    char all[24][LINE_CAP];
    int hn = wrap(&font_small, cs.heard, W - 96, all, 24), hmax = cs.said[0] ? 3 : HEARD_LINES;
    n_heard = hn < hmax ? hn : hmax;
    heard_cut = hn > hmax;
    for (int i = 0; i < n_heard; i++) {
        if (i == 0 && heard_cut) snprintf(heard_l[0], LINE_CAP, "...%s", all[hn - n_heard]);
        else snprintf(heard_l[i], LINE_CAP, "%s", all[hn - n_heard + i]);
    }
    // the reply under it: large if it fits in two lines there, else smaller, as many lines as fit (the newest, as
    // it's spoken)
    int lines_above = cs.status[0] ? 1 : n_heard;
    said_top = TEXT_TOP + lines_above * font_small.line_h + (lines_above ? 10 : 0);
    int room = TEXT_BOT - said_top;
    if (wrap(&font_body, cs.said, W - 56, all, 24) <= 2 && 2 * font_body.line_h <= room) {
        said_f = &font_body;
        n_said = wrap(said_f, cs.said, W - 56, said_l, 2);
    } else {
        said_f = &font_label;
        int fit = room / font_label.line_h;
        n_said = wrap(said_f, cs.said, W - 56, said_l, fit < 1 ? 1 : fit > SAID_LINES ? SAID_LINES : fit);
    }
    dirty = true;
}

static void text_mid(uint16_t *buf, int y0, int h, int y, const font_t *f, const char *s, uint16_t col)
{
    draw_ftext(buf, y0, h, CX - ftext_width(f, s) / 2, y, f, s, col);
}

// ---------------------------------------------------------------- the pictures

static void line(uint16_t *buf, int y0, int h, float ax, float ay, float bx, float by, float w, uint16_t col)
{
    float p[4] = {ax, ay, bx, by};
    aa_path(buf, y0, h, p, 2, false, w, col);
}

// A slash through a glyph, with a gap cut round it in the button's colour
static void slash(uint16_t *buf, int y0, int h, float cx, float cy, float s, uint16_t fg, uint16_t bg)
{
    line(buf, y0, h, cx - 13 * s, cy - 15 * s, cx + 13 * s, cy + 15 * s, 6.5f * s, bg);
    line(buf, y0, h, cx - 13 * s, cy - 15 * s, cx + 13 * s, cy + 15 * s, 2.6f * s, fg);
}

static void mic_glyph(uint16_t *buf, int y0, int h, float cx, float cy, float s, uint16_t col)
{
    aa_rrect(buf, y0, h, cx - 6 * s, cy - 16 * s, cx + 6 * s, cy + 3 * s, 6 * s, col);   // the capsule
    float arc[2 * 13];
    for (int i = 0; i <= 12; i++) {                   // the cradle: half a circle under it
        float a = (float)M_PI * i / 12;
        arc[2 * i] = cx - 10.5f * s * cosf(a);
        arc[2 * i + 1] = cy - 4 * s + 10.5f * s * sinf(a);
    }
    aa_path(buf, y0, h, arc, 13, false, 2.3f * s, col);
    line(buf, y0, h, cx, cy + 6.5f * s, cx, cy + 12 * s, 2.3f * s, col);              // stand
}

static void speaker_glyph(uint16_t *buf, int y0, int h, float cx, float cy, float s, uint16_t col)
{
    float bx = cx - 5 * s;                             // (the whole thing sits a little left of centre)
    aa_rrect(buf, y0, h, bx - 10 * s, cy - 4.5f * s, bx - 3 * s, cy + 4.5f * s, 1.5f * s, col);
    fill_tri(buf, y0, h, bx - 4 * s, cy - 4.5f * s, bx + 4 * s, cy - 11 * s, bx + 4 * s, cy + 11 * s, col);
    fill_tri(buf, y0, h, bx - 4 * s, cy - 4.5f * s, bx + 4 * s, cy + 11 * s, bx - 4 * s, cy + 4.5f * s, col);
    float cone[8] = {bx - 4 * s, cy - 4.5f * s, bx + 4 * s, cy - 11 * s, bx + 4 * s, cy + 11 * s, bx - 4 * s, cy + 4.5f * s};
    aa_path(buf, y0, h, cone, 4, true, 1.4f * s, col);   // (smooths the triangles' edges)
    for (int k = 0; k < 2; k++) {                      // two waves
        float r = (k ? 13 : 7.5f) * s, pts[2 * 9];
        for (int i = 0; i <= 8; i++) {
            float a = -0.85f + 1.7f * i / 8;
            pts[2 * i] = bx + 4 * s + r * cosf(a);
            pts[2 * i + 1] = cy + r * sinf(a);
        }
        aa_path(buf, y0, h, pts, 9, false, 2.2f * s, col);
    }
}

// The orb: a sphere in Claude's orange, lit from above (a soft highlight that drifts round, faster while it thinks),
// with a glow fading out round it. Drawn a pixel at a time over its box (black behind it). r: its size now; lit: 1
// in a chat, dimmer before one; grey while you're muted.
static void draw_orb(uint16_t *buf, int y0, int h, float r, float lit, bool grey, float spin)
{
    static const float warm[4][3] = {{247, 176, 140}, {205, 104, 72}, {255, 226, 206}, {217, 119, 87}};   // centre, rim, shine, glow
    static const float cool[4][3] = {{150, 150, 157}, {78, 78, 84}, {205, 205, 212}, {110, 110, 118}};
    const float (*c)[3] = grey ? cool : warm;
    int ya = ORB_Y - ORB_BOX > y0 ? ORB_Y - ORB_BOX : y0, yb = ORB_Y + ORB_BOX < y0 + h ? ORB_Y + ORB_BOX : y0 + h;
    float glow_r = r * 1.65f, hx = CX - r * 0.28f + r * 0.12f * cosf(spin), hy = ORB_Y - r * 0.34f + r * 0.08f * sinf(spin);
    for (int y = ya; y < yb; y++) {
        float dy = y + 0.5f - ORB_Y;
        if (fabsf(dy) >= glow_r) continue;
        uint16_t *row = buf + (y - y0) * W;
        for (int x = CX - (int)glow_r - 1; x <= CX + (int)glow_r + 1; x++) {
            float dx = x + 0.5f - CX, d2 = dx * dx + dy * dy;
            if (d2 >= glow_r * glow_r) continue;
            float d = sqrtf(d2), out[3];
            float g = 1 - (d - r) / (glow_r - r);       // the glow: fading to nothing
            g = g < 0 ? 0 : (g > 1 ? 1 : g);
            g = g * g * 0.42f;
            float in = r - d + 0.5f;                    // inside the sphere (anti-aliased at its edge)
            in = in < 0 ? 0 : (in > 1 ? 1 : in);
            float t = d / r, k = t * t;                 // centre to rim
            float sx = x + 0.5f - hx, sy = y + 0.5f - hy, sh = 1 - sqrtf(sx * sx + sy * sy) / (r * 0.75f);
            sh = sh < 0 ? 0 : sh * sh * 0.85f;          // the shine
            for (int i = 0; i < 3; i++) {
                float body = c[0][i] + (c[1][i] - c[0][i]) * k;
                body += (c[2][i] - body) * sh;
                out[i] = (body * in + c[3][i] * g * (1 - in)) * lit;
            }
            row[x] = rgb((int)out[0], (int)out[1], (int)out[2]);
        }
    }
}

// A round button with its name under it: dark, or lit (white, a dark glyph) when it's on
static void button(uint16_t *buf, int y0, int h, float cx, float rad, uint16_t bg, const char *label)
{
    aa_disc(buf, y0, h, cx, BTN_Y, rad, bg);
    draw_ftext(buf, y0, h, (int)cx - ftext_width(&font_small, label) / 2, LABEL_Y, &font_small, label, C_LABEL2);
}

// ---------------------------------------------------------------- the menu (tap the model, or hold the speaker)

#define MENU_W 300
#define ROW_H 50
#define MENU_HEAD 38
static int menu_rows(void) { return cs.n_models + 1; }   // the models, then New Chat
static int menu_top(void) { return 236 - (menu_rows() * ROW_H + MENU_HEAD) / 2; }

static int menu_hit(int x, int y)
{
    int top = menu_top() + MENU_HEAD;
    if (x < CX - MENU_W / 2 || x >= CX + MENU_W / 2 || y < top) return -2;   // outside: close
    int r = (y - top) / ROW_H;
    return r < menu_rows() ? r : -2;
}

// The page behind it, dimmed to a third (a modal sheet's backdrop)
static void dim(uint16_t *buf, int h)
{
    for (int i = 0; i < W * h; i++) {
        uint16_t c = SWAP16(buf[i]);
        c = (c >> 2) & 0x39E7;                         // each channel a quarter...
        c += (c >> 1) & 0x39E7;                        // ...and a bit more
        buf[i] = SWAP16(c);
    }
}

static void draw_menu(uint16_t *buf, int y0, int h)
{
    dim(buf, h);
    int top = menu_top(), bot = top + MENU_HEAD + menu_rows() * ROW_H, x0 = CX - MENU_W / 2, x1 = CX + MENU_W / 2;
    aa_rrect(buf, y0, h, x0, top, x1, bot, 14, C_SHEET);
    draw_ftext(buf, y0, h, x0 + 18, top + 12, &font_small, "Model", C_LABEL2);
    for (int i = 0; i < menu_rows(); i++) {
        int ry = top + MENU_HEAD + i * ROW_H;
        bool last = i == menu_rows() - 1;
        if (i == menu_press) aa_rrect(buf, y0, h, x0, ry, x1, ry + ROW_H, last ? 14 : 0, C_FILL_DN);
        if (i == menu_press && last) fill_rect(buf, y0, h, x0, ry, x1, ry + 14, C_FILL_DN);   // (square at the top)
        fill_rect(buf, y0, h, x0 + (last ? 0 : 18), ry, x1, ry + 1, last ? rgb(52, 52, 56) : C_SEP);
        int ty = ry + (ROW_H - font_label.line_h) / 2;
        if (i < cs.n_models) {
            draw_ftext(buf, y0, h, x0 + 18, ty, &font_label, cs.models[i], C_WHITE);
            if (i == cs.model) {                       // a tick
                float p[6] = {x1 - 38, ry + 25, x1 - 32, ry + 31, x1 - 21, ry + 19};
                aa_path(buf, y0, h, p, 3, false, 2.6f, C_BLUE);
            }
        } else {
            draw_ftext(buf, y0, h, x0 + 18, ty, &font_label, "New Chat", C_BLUE);
            float px = x1 - 30, py = ry + ROW_H / 2.0f;   // a plus
            line(buf, y0, h, px - 7, py, px + 7, py, 2.4f, C_BLUE);
            line(buf, y0, h, px, py - 7, px, py + 7, 2.4f, C_BLUE);
        }
    }
}

// ---------------------------------------------------------------- drawing

static float orb_size(void)
{
    if (!cs.active || cs.muted) return ORB_R;
    if (cs.state == CS_THINKING) return ORB_R + 3 * sinf(t_anim * 6);
    if (cs.state == CS_SPEAKING) return ORB_R + ORB_GROW * level;
    return ORB_R + 2 * sinf(t_anim * 2.2f) + (ORB_GROW - 2) * level;   // listening: breathing, and growing with your voice
}

void chat_ui_draw(uint16_t *buf, int y0, int h)
{
    // the orb first (its glow goes behind the pill)
    draw_orb(buf, y0, h, orb_size(), cs.active ? 1 : 0.6f, cs.active && cs.muted,
             t_anim * (cs.state == CS_THINKING ? 3.0f : 0.6f));

    // the model, in a pill at the top (tap it for the menu)
    if (cs.n_models && PILL_Y + PILL_H > y0 && PILL_Y < y0 + h) {
        const char *m = cs.models[cs.model];
        int tw = ftext_width(&font_small, m), pw = 22 + 8 + tw + 8 + 12 + 16, px = CX - pw / 2;
        aa_rrect(buf, y0, h, px, PILL_Y, px + pw, PILL_Y + PILL_H, PILL_H / 2.0f, pressed == B_PILL ? C_FILL_DN : rgb(28, 28, 30));
        pet_ui_spark(buf, y0, h, px + 16 + 4, PILL_Y + PILL_H / 2, 217, 119, 87, 0.32f);
        draw_ftext(buf, y0, h, px + 36, PILL_Y + (PILL_H - font_small.line_h) / 2, &font_small, m, C_WHITE);
        float vx = px + 36 + tw + 12, vy = PILL_Y + PILL_H / 2.0f;   // a chevron: it opens
        float v[6] = {vx - 4, vy - 2, vx, vy + 2, vx + 4, vy - 2};
        aa_path(buf, y0, h, v, 3, false, 1.8f, C_LABEL2);
    }

    // what you said (or what's wrong), then what Claude's saying, or what's going on
    if (cs.status[0]) {
        char t[LINE_CAP + 4];
        plain(t, sizeof t, cs.status);
        text_mid(buf, y0, h, TEXT_TOP, &font_small, t, C_ORANGE);
    } else {
        for (int i = 0; i < n_heard; i++) text_mid(buf, y0, h, TEXT_TOP + i * font_small.line_h, &font_small, heard_l[i], C_LABEL2);
    }
    if (n_said) {
        for (int i = 0; i < n_said; i++) text_mid(buf, y0, h, said_top + i * said_f->line_h, said_f, said_l[i], C_WHITE);
    } else if (said_top + font_label.line_h <= TEXT_BOT) {
        const char *hint = !cs.active ? (cs.mac ? "Tap to talk to Claude" : "Open Bobble on your Mac to talk")
                         : cs.muted ? "You're muted" : cs.state == CS_THINKING ? "Thinking..." : "Listening...";
        text_mid(buf, y0, h, said_top + (n_heard ? 0 : 8), &font_label, hint, C_LABEL3);
    }

    if (BTN_Y + MIC_R + 40 >= y0 && BTN_Y - MIC_R <= y0 + h) {
        // speaker (left), in a chat: lit when Claude's muted
        float sx = CX - SIDE_DX;
        bool sp = cs.speaker_muted;
        if (cs.active) {
            button(buf, y0, h, sx, SIDE_R, sp ? (pressed == B_SPEAKER ? C_ON_DN : C_WHITE) : pressed == B_SPEAKER ? C_FILL_DN : C_FILL, "speaker");
            speaker_glyph(buf, y0, h, sx + 2, BTN_Y, 0.95f, sp ? C_INK : C_WHITE);
            if (sp) slash(buf, y0, h, sx, BTN_Y, 0.95f, C_INK, pressed == B_SPEAKER ? C_ON_DN : C_WHITE);
        }

        // the mic (middle): Claude's orange to start a chat; in one, dark, or lit with a slash when you're muted
        uint16_t mb = !cs.active ? (pressed == B_MIC ? C_CLAUDE_DN : C_CLAUDE)
                    : cs.muted ? (pressed == B_MIC ? C_ON_DN : C_WHITE) : pressed == B_MIC ? C_FILL_DN : C_FILL;
        button(buf, y0, h, CX, MIC_R, mb, !cs.active ? "talk" : cs.muted ? "unmute" : "mute");
        mic_glyph(buf, y0, h, CX, BTN_Y + 1, 1.15f, cs.active && cs.muted ? C_INK : C_WHITE);
        if (cs.active && cs.muted) slash(buf, y0, h, CX, BTN_Y, 1.15f, C_INK, pressed == B_MIC ? C_ON_DN : C_WHITE);

        // end (right), in a chat
        if (cs.active) {
            float ex = CX + SIDE_DX;
            button(buf, y0, h, ex, SIDE_R, pressed == B_CLOSE ? C_RED_DN : C_RED, "end");
            line(buf, y0, h, ex - 8, BTN_Y - 8, ex + 8, BTN_Y + 8, 3, C_WHITE);
            line(buf, y0, h, ex - 8, BTN_Y + 8, ex + 8, BTN_Y - 8, 3, C_WHITE);
        }
    }
    if (menu) draw_menu(buf, y0, h);
}

// ---------------------------------------------------------------- touch

static int hit(int x, int y)
{
    float dy = y - BTN_Y, dm = hypotf(x - CX, dy), ds = hypotf(x - (CX - SIDE_DX), dy), dc = hypotf(x - (CX + SIDE_DX), dy);
    if (dm < MIC_R + 14) return B_MIC;
    if (cs.active && ds < SIDE_R + 16) return B_SPEAKER;
    if (cs.active && dc < SIDE_R + 16) return B_CLOSE;
    if (cs.n_models && y >= PILL_Y - 8 && y < PILL_Y + PILL_H + 8 && x > CX - 110 && x < CX + 110) return B_PILL;
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
            else if (pressed == B_PILL) menu = true;
            pressed = B_NONE;
            dirty = true;
        }
    }
    was_down = down;

    // the orb follows the sound, breathes and pulses. At most 15 times a second, and only its box is redrawn for it
    // (chat_ui_take_rect): whole frames at full speed beside the echo canceller were more than PSRAM could keep up
    // with, and the panel showed broken rows.
    static int64_t anim_at;
    if (now - anim_at < 66000) return;
    anim_at = now;
    float target = chat_level();
    float nl = level + (target - level) * (dt > 0 ? fminf(1, dt * 14) : 1);
    if (fabsf(nl - level) > 0.004f || (level > 0 && target == 0)) { level = nl < 0.003f ? 0 : nl; anim_dirty = true; }
    if (cs.active && !cs.muted) { t_anim += 0.066f; anim_dirty = true; }   // (breathing, pulsing, the sheen going round)
}

bool chat_ui_dirty(void) { return dirty || anim_dirty; }

int chat_ui_take_rect(int r[4])
{
    bool whole = dirty;
    dirty = anim_dirty = false;
    if (whole) return 0;
    r[0] = CX - ORB_BOX; r[1] = ORB_Y - ORB_BOX; r[2] = CX + ORB_BOX; r[3] = ORB_Y + ORB_BOX;
    return 1;
}

bool chat_ui_animating(void) { return cs.active && !cs.muted; }
