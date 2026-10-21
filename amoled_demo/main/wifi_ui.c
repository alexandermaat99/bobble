// Settings card for the 466x466 round AMOLED (and the 480x480 square board, which lays the menu out as tiles and says
// which firmware it runs): a small menu (Wi-Fi, touch and tilt calibration, lock, reset), plus
// the Wi-Fi screens: pick a network from a scrollable list,
// type the password on an on-screen keyboard, then watch it connect (and show a synced clock).
// Phone setup through the hotspot / QR code is still available from the list.
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <stdlib.h>
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "qrcode.h"
#include "gfx.h"
#include "wifi_setup.h"
#include "wifi_ui.h"
#include "ota.h"
#include "screen.h"
#include "agent.h"
#include "esp_app_desc.h"

enum view { V_MENU, V_LIST, V_KEYS, V_STATUS, V_PHONE, V_RESET };
static enum view view;

// colours
#define C_WHITE  rgb(255, 255, 255)
#define C_GREY   rgb(130, 130, 150)
#define C_DIM    rgb(70, 70, 90)
#define C_PANEL  rgb(26, 26, 40)
#define C_PRESS  rgb(0, 110, 96)
#define C_ACCENT rgb(0, 230, 200)
#define C_RED    rgb(255, 95, 95)
#define C_GREEN  rgb(60, 230, 120)

// hit-test ids
enum {
    ID_NONE = 0, ID_SCAN, ID_PHONE, ID_EXIT, ID_BACK, ID_CANCEL, ID_NETWORKS, ID_FORGET,
    ID_SHIFT, ID_DEL, ID_SYM, ID_SPACE, ID_JOIN, ID_SHOW,
    ID_MENU, ID_WIFI, ID_CAL_TOUCH, ID_CAL_TILT, ID_BRIGHT, ID_LOCK, ID_RESET, ID_ERASE,
    ID_ROW = 100,     // + row index
    ID_KEY = 200,     // + character code
};

typedef struct { int x0, y0, x1, y1, id; const char *label; } button_t;

// ---------------------------------------------------------------- state
#define MAX_NETS 20
EXT_RAM_BSS_ATTR static wifi_net_t nets[MAX_NETS];
static int net_count;
static int64_t last_scan_us;

static char join_ssid[33], password[64], error_msg[64];
static bool join_secure, from_keys, caps, symbols, hide_pw;

// touch tracking
static bool down;
static int press_x, press_y, cur_x, cur_y, press_id, scroll, scroll_at_press;
static bool dragging;
static int64_t press_us, repeat_us;
#define LOCK_HOLD_US 3000000

// ---------------------------------------------------------------- layout
#define LIST_TOP 92
#define LIST_BOT 386
#define ROW_H 50
#define ROW_X0 (CX - 170)
#define ROW_X1 (CX + 170)

static const button_t list_buttons[] = {
    {CX - 124, 388, CX - 44, 428, ID_SCAN, "Scan"},
    {CX - 40, 388, CX + 40, 428, ID_PHONE, "Phone"},
    {CX + 44, 388, CX + 124, 428, ID_MENU, "Back"},
};
static const button_t connected_buttons[] = {
    {CX - 158, 310, CX - 6, 356, ID_NETWORKS, "Networks"},
    {CX + 6, 310, CX + 158, 356, ID_FORGET, "Forget"},
    {CX - 70, 366, CX + 70, 410, ID_MENU, "Back"},
};
static const button_t connecting_buttons[] = {{CX - 60, 370, CX + 60, 404, ID_CANCEL, "Cancel"}};
static const button_t phone_buttons[] = {{CX - 55, 408, CX + 55, 440, ID_BACK, "Back"}};
#define BUB_DX 74      // bubble columns either side of the middle
#define BUB_Y1 122     // bubble rows
#define BUB_Y2 256
#define BUB_R 62
#if !SCREEN_ROUND
// The square: a title, four big tiles two by two (icon, name and a line about it), the brightness slider across, then
// Factory reset and what this board is and runs
#define TILE_X0 20
#define TILE_X1 (W - 20)
#define TILE_MID (W / 2)
#define TILE_GAP 6                             // (half the gap between tiles)
#define TILE_Y1 76                             // tile rows: top edges, and their height
#define TILE_Y2 188
#define TILE_H 100
static const button_t menu_buttons[] = {
    {TILE_X0, TILE_Y1, TILE_MID - TILE_GAP, TILE_Y1 + TILE_H, ID_WIFI, "Wi-Fi"},
    {TILE_X0, TILE_Y2, TILE_MID - TILE_GAP, TILE_Y2 + TILE_H, ID_CAL_TOUCH, "Touch"},
    {TILE_MID + TILE_GAP, TILE_Y2, TILE_X1, TILE_Y2 + TILE_H, ID_CAL_TILT, "Tilt"},
    {TILE_MID + TILE_GAP, TILE_Y1, TILE_X1, TILE_Y1 + TILE_H, ID_LOCK, "Lock"},
    {TILE_X0, 304, TILE_X1, 352, ID_BRIGHT, ""},   // brightness slider row
    {TILE_X0, 374, 200, 422, ID_RESET, "Factory reset"},
};
#else
static const button_t menu_buttons[] = {
    // round icon bubbles, two by two
    {CX - BUB_DX - BUB_R, BUB_Y1 - BUB_R, CX - BUB_DX + BUB_R, BUB_Y1 + BUB_R, ID_WIFI, "Wi-Fi"},
    {CX - BUB_DX - BUB_R, BUB_Y2 - BUB_R, CX - BUB_DX + BUB_R, BUB_Y2 + BUB_R, ID_CAL_TOUCH, "Touch"},
    {CX + BUB_DX - BUB_R, BUB_Y2 - BUB_R, CX + BUB_DX + BUB_R, BUB_Y2 + BUB_R, ID_CAL_TILT, "Tilt"},
    {CX + BUB_DX - BUB_R, BUB_Y1 - BUB_R, CX + BUB_DX + BUB_R, BUB_Y1 + BUB_R, ID_LOCK, "Lock"},
    {CX - 172, 338, CX + 172, 386, ID_BRIGHT, ""},   // brightness slider row
    {CX - 100, 396, CX + 100, 434, ID_RESET, "Factory reset"},
};
#endif
#define ERASE_Y 262
static const button_t reset_buttons[] = {
    {CX - BUB_R, ERASE_Y - BUB_R, CX + BUB_R, ERASE_Y + BUB_R, ID_ERASE, "Hold"},
    {CX - 55, 340, CX + 55, 376, ID_MENU, "Back"},
};

// keyboard: 5 rows of keys on a 40 px pitch, sized to stay inside the round display
#define KEY_TOP 128
#define KEY_ROW 46
#define KEY_P 40
static const char *rows_lower[] = {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"};
static const char *rows_upper[] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
static const char *rows_sym[] = {"!@#$%^&*()", "-_=+[]{}\\|", ";:'\",.<>?", "/`~"};

#define MAX_KEYS 48
static button_t keys[MAX_KEYS];
static char key_labels[MAX_KEYS][2];
static int key_count;

static void build_keyboard(void)
{
    const char **rows = symbols ? rows_sym : (caps ? rows_upper : rows_lower);
    key_count = 0;
    for (int r = 0; r < 4; r++) {
        int n = strlen(rows[r]), y = KEY_TOP + r * KEY_ROW, x = CX - n * KEY_P / 2;
        for (int i = 0; i < n; i++, x += KEY_P) {
            key_labels[key_count][0] = rows[r][i];
            key_labels[key_count][1] = 0;
            keys[key_count] = (button_t){x, y, x + KEY_P, y + KEY_ROW, ID_KEY + (unsigned char)rows[r][i], key_labels[key_count]};
            key_count++;
        }
    }
    int y3 = KEY_TOP + 3 * KEY_ROW, y4 = KEY_TOP + 4 * KEY_ROW;
    if (!symbols) keys[key_count++] = (button_t){CX - 196, y3, CX - 140, y3 + KEY_ROW, ID_SHIFT, caps ? "abc" : "ABC"};
    keys[key_count++] = (button_t){CX + 140, y3, CX + 196, y3 + KEY_ROW, ID_DEL, "Del"};
    keys[key_count++] = (button_t){CX - 180, y4, CX - 108, y4 + KEY_ROW, ID_SYM, symbols ? "abc" : "#+="};
    keys[key_count++] = (button_t){CX - 104, y4, CX + 76, y4 + KEY_ROW, ID_SPACE, "space"};
    keys[key_count++] = (button_t){CX + 80, y4, CX + 180, y4 + KEY_ROW, ID_JOIN, "Join"};
    keys[key_count++] = (button_t){CX - 120, 366, CX - 5, 400, ID_CANCEL, "Cancel"};
    keys[key_count++] = (button_t){CX + 5, 366, CX + 120, 400, ID_SHOW, hide_pw ? "Show" : "Hide"};
}

// ---------------------------------------------------------------- QR for phone setup
#define QR_MAX 57
EXT_RAM_BSS_ATTR static uint8_t qr_mod[QR_MAX * QR_MAX];
static int qr_n;
static char qr_text[128];

static void qr_store(esp_qrcode_handle_t q)
{
    qr_n = esp_qrcode_get_size(q);
    if (qr_n > QR_MAX) { qr_n = 0; return; }
    for (int y = 0; y < qr_n; y++)
        for (int x = 0; x < qr_n; x++) qr_mod[y * qr_n + x] = esp_qrcode_get_module(q, x, y);
}

static void qr_set(const char *text)
{
    if (!strcmp(text, qr_text)) return;
    strlcpy(qr_text, text, sizeof qr_text);
    esp_qrcode_config_t cfg = {.display_func = qr_store, .max_qrcode_version = 10, .qrcode_ecc_level = ESP_QRCODE_ECC_LOW};
    esp_qrcode_generate(&cfg, text);
}

static void draw_qr(uint16_t *buf, int y0, int h, int cy, int size)
{
    if (!qr_n) return;
    int sc = size / (qr_n + 4), full = sc * (qr_n + 4), left = CX - full / 2, top = cy - full / 2;
    fill_rect(buf, y0, h, left, top, left + full, top + full, C_WHITE);   // includes the quiet zone
    for (int my = 0; my < qr_n; my++) {
        int py = top + (my + 2) * sc;
        if (py + sc <= y0 || py >= y0 + h) continue;
        for (int mx = 0; mx < qr_n; mx++)
            if (qr_mod[my * qr_n + mx]) {
                int px = left + (mx + 2) * sc;
                fill_rect(buf, y0, h, px, py, px + sc, py + sc, 0);
            }
    }
}

// ---------------------------------------------------------------- helpers
// Buttons accept touches a bit outside their outline (fingers land low, especially near the bottom
// edge); keyboard keys sit edge to edge so they don't.
static int hit_pad(const button_t *b, int n, int x, int y, bool pad)
{
    int px = pad ? 8 : 0, up = pad ? 10 : 0, dn = pad ? 24 : 0;
    for (int i = 0; i < n; i++)
        if (x >= b[i].x0 - px && x < b[i].x1 + px && y >= b[i].y0 - up && y < b[i].y1 + dn) return b[i].id;
    return ID_NONE;
}

static int hit(const button_t *b, int n, int x, int y) { return hit_pad(b, n, x, y, true); }

// Screens where an upward drag anywhere can close the card (no scrolling, typing, or slider under the finger)
bool wifi_ui_wants_status_bar(void) { return view != V_KEYS; }   // the keyboard needs the top rows

bool wifi_ui_swipe_ok(void)
{
    return (view == V_MENU || view == V_STATUS || view == V_PHONE || view == V_RESET) && press_id != ID_BRIGHT && press_id != ID_ERASE;
}

void wifi_ui_cancel_touch(void)
{
    down = dragging = false;
    press_id = ID_NONE;
}

static int list_hit(int x, int y)
{
    if (y < LIST_TOP || y >= LIST_BOT || x < ROW_X0 || x >= ROW_X1) return ID_NONE;
    int i = (y - LIST_TOP + scroll) / ROW_H;
    return i < net_count ? ID_ROW + i : ID_NONE;
}

static int max_scroll(void)
{
    int m = net_count * ROW_H - (LIST_BOT - LIST_TOP);
    return m > 0 ? m : 0;
}

static void fit(char *out, size_t cap, const char *in, int max_chars)
{
    if ((int)strlen(in) <= max_chars) { strlcpy(out, in, cap); return; }
    snprintf(out, cap, "%.*s..", max_chars - 2, in);
}

static int current_id(void) { return down && !dragging ? press_id : ID_NONE; }

static void draw_buttons(uint16_t *buf, int y0, int h, const button_t *b, int n, int sc, int accent_id)
{
    for (int i = 0; i < n; i++) {
        if (b[i].y1 <= y0 || b[i].y0 >= y0 + h) continue;
        bool pressed = current_id() == b[i].id;
        uint16_t bg = pressed ? C_PRESS : (b[i].id == accent_id ? C_ACCENT : C_PANEL);
        uint16_t fg = b[i].id == accent_id && !pressed ? rgb(0, 30, 30) : C_WHITE;
        fill_rrect(buf, y0, h, b[i].x0 + 2, b[i].y0 + 2, b[i].x1 - 2, b[i].y1 - 2, 8, bg);
        int lsc = text_width(b[i].label, sc) > b[i].x1 - b[i].x0 - 8 ? 1 : sc;
        draw_text(buf, y0, h, (b[i].x0 + b[i].x1) / 2 - text_width(b[i].label, lsc) / 2,
                  (b[i].y0 + b[i].y1) / 2 - 7 * lsc / 2, lsc, b[i].label, fg);
    }
}

static void draw_bars(uint16_t *buf, int y0, int h, int x, int y, int rssi)
{
    int level = rssi > -55 ? 4 : rssi > -67 ? 3 : rssi > -78 ? 2 : 1;
    for (int i = 0; i < 4; i++) {
        int bh = 5 + i * 4;
        fill_rect(buf, y0, h, x + i * 6, y + 18 - bh, x + i * 6 + 4, y + 18, i < level ? C_ACCENT : C_DIM);
    }
}

static void draw_lock(uint16_t *buf, int y0, int h, int x, int y, uint16_t c)
{
    fill_rect(buf, y0, h, x + 2, y, x + 10, y + 2, c);
    fill_rect(buf, y0, h, x + 2, y, x + 4, y + 8, c);
    fill_rect(buf, y0, h, x + 8, y, x + 10, y + 8, c);
    fill_rrect(buf, y0, h, x, y + 7, x + 12, y + 17, 2, c);
}

// ---------------------------------------------------------------- views
enum { ICON_WIFI, ICON_TOUCH, ICON_TILT, ICON_SUN, ICON_LOCK };
static void draw_icon(uint16_t *buf, int y0, int h, int kind, int cx, int cy, int r, uint16_t col);
static void draw_list(uint16_t *buf, int y0, int h)
{
    char line[48];
    text_c(buf, y0, h, 40, 3, "Wi-Fi", C_WHITE);
    if (error_msg[0]) text_c(buf, y0, h, 72, 2, error_msg, C_RED);
    else if (wifi_scanning() && net_count == 0) text_c(buf, y0, h, 72, 2, "Scanning...", C_GREY);
    else if (wifi_state() != WS_CONNECTED) text_c(buf, y0, h, 72, 2, "Tap a network to join", C_GREY);

    gfx_clip(LIST_TOP, LIST_BOT);
    for (int i = 0; i < net_count; i++) {
        int ry = LIST_TOP + i * ROW_H - scroll;
        if (ry + ROW_H <= y0 || ry >= y0 + h || ry + ROW_H <= LIST_TOP || ry >= LIST_BOT) continue;
        bool connected = wifi_state() == WS_CONNECTED && !strcmp(nets[i].ssid, wifi_ssid());
        fill_rrect(buf, y0, h, ROW_X0, ry + 2, ROW_X1, ry + ROW_H - 2, 10, current_id() == ID_ROW + i ? C_PRESS : C_PANEL);
        fit(line, sizeof line, nets[i].ssid, 21);
        draw_text(buf, y0, h, ROW_X0 + 14, ry + ROW_H / 2 - 7, 2, line, connected ? C_GREEN : C_WHITE);
        if (nets[i].secure) draw_lock(buf, y0, h, ROW_X1 - 58, ry + 16, C_GREY);
        draw_bars(buf, y0, h, ROW_X1 - 38, ry + 16, nets[i].rssi);
    }
    if (net_count == 0 && !wifi_scanning()) text_c(buf, y0, h, 220, 2, "No networks found", C_GREY);
    if (wifi_scanning() && net_count == 0)
        for (int i = 0; i < 3; i++) fill_circle(buf, y0, h, CX - 30 + i * 30, 230, ((int)(esp_timer_get_time() / 200000) + i) % 3 == 0 ? 8 : 5, C_ACCENT);
    gfx_clip(0, H);

    // scroll indicator on the right edge of the list
    int ms = max_scroll();
    if (ms > 0) {
        int track = LIST_BOT - LIST_TOP - 20, th = track * (LIST_BOT - LIST_TOP) / (net_count * ROW_H);
        int ty = LIST_TOP + 10 + (track - th) * scroll / ms;
        fill_rrect(buf, y0, h, ROW_X1 + 6, ty, ROW_X1 + 10, ty + th, 2, C_DIM);
    }
    int nb = wifi_ap_active() ? 3 : 2;
    if (nb == 3) draw_buttons(buf, y0, h, list_buttons, 3, 2, ID_NONE);
    else {
        draw_buttons(buf, y0, h, &list_buttons[0], 1, 2, ID_NONE);
        draw_buttons(buf, y0, h, &list_buttons[2], 1, 2, ID_NONE);
    }
}

static void draw_keys(uint16_t *buf, int y0, int h)
{
    char line[64];
    text_c(buf, y0, h, 20, 2, "Password for", C_GREY);
    fit(line, sizeof line, join_ssid, 20);
    text_c(buf, y0, h, 42, 2, line, C_WHITE);

    fill_rrect(buf, y0, h, CX - 160, 64, CX + 160, 104, 10, rgb(12, 12, 20));
    fill_rect(buf, y0, h, CX - 150, 101, CX + 150, 103, error_msg[0] ? C_RED : C_ACCENT);
    int n = strlen(password), shown = n > 24 ? 24 : n;
    if (hide_pw) {
        memset(line, '*', shown);
        line[shown] = 0;
    } else {
        strlcpy(line, password + n - shown, sizeof line);
    }
    int tx = CX - 150, ty = 77;
    draw_text(buf, y0, h, tx, ty, 2, line, C_WHITE);
    if ((esp_timer_get_time() / 500000) % 2 == 0) fill_rect(buf, y0, h, tx + shown * 12, ty - 2, tx + shown * 12 + 2, ty + 16, C_ACCENT);
    if (error_msg[0]) text_c(buf, y0, h, 110, 2, error_msg, C_RED);

    for (int i = 0; i < key_count; i++) {
        button_t *k = &keys[i];
        if (k->y1 <= y0 || k->y0 >= y0 + h) continue;
        bool pressed = current_id() == k->id;
        uint16_t bg = pressed ? C_PRESS : (k->id == ID_JOIN ? C_ACCENT : (k->id >= ID_KEY ? rgb(44, 44, 64) : C_PANEL));
        if (k->id == ID_SHIFT && caps && !pressed) bg = rgb(80, 80, 110);
        uint16_t fg = k->id == ID_JOIN && !pressed ? rgb(0, 30, 30) : C_WHITE;
        fill_rrect(buf, y0, h, k->x0 + 2, k->y0 + 2, k->x1 - 2, k->y1 - 2, 7, bg);
        int sc = text_width(k->label, 2) > k->x1 - k->x0 - 6 ? 1 : 2;
        draw_text(buf, y0, h, (k->x0 + k->x1) / 2 - text_width(k->label, sc) / 2, (k->y0 + k->y1) / 2 - 7 * sc / 2, sc, k->label, fg);
    }
}

static void draw_status(uint16_t *buf, int y0, int h)
{
    char line[64];
    if (wifi_state() == WS_CONNECTED) {
        text_c(buf, y0, h, 46, 3, "Wi-Fi", C_WHITE);
        fill_circle(buf, y0, h, CX, 138, 42, rgb(12, 44, 28));
        draw_icon(buf, y0, h, ICON_WIFI, CX, 138, 30, C_GREEN);
        text_c(buf, y0, h, 196, 3, "Connected", C_GREEN);
        fit(line, sizeof line, wifi_ssid(), 26);
        text_c(buf, y0, h, 246, 2, line, C_WHITE);
        text_c(buf, y0, h, 274, 2, wifi_ip(), C_GREY);
        draw_buttons(buf, y0, h, connected_buttons, 3, 2, ID_NONE);
    } else {
        text_c(buf, y0, h, 170, 3, "Connecting", C_WHITE);
        fit(line, sizeof line, wifi_ssid(), 26);
        text_c(buf, y0, h, 220, 2, line, C_ACCENT);
        int phase = (int)(esp_timer_get_time() / 150000);
        for (int i = 0; i < 3; i++) fill_circle(buf, y0, h, CX - 30 + i * 30, 290 - ((phase + i) % 3 == 0 ? 8 : 0), 7, C_ACCENT);
        draw_buttons(buf, y0, h, connecting_buttons, 1, 2, ID_NONE);
    }
}

static void draw_phone(uint16_t *buf, int y0, int h)
{
    char line[48];
    bool joined = wifi_ap_clients() > 0;
    text_c(buf, y0, h, 46, 2, joined ? "Now pick your network" : "Set up from your phone", C_WHITE);
    draw_qr(buf, y0, h, 200, 220);
    text_c(buf, y0, h, 326, 2, joined ? "on the page that opened" : "Scan to join setup Wi-Fi", C_ACCENT);
    if (joined) text_c(buf, y0, h, 354, 2, "or open 192.168.4.1", C_GREY);
    else {
        text_c(buf, y0, h, 352, 2, wifi_ap_ssid(), C_GREY);
        snprintf(line, sizeof line, "Password %s", wifi_ap_pass());
        text_c(buf, y0, h, 376, 2, line, C_GREY);
    }
    draw_buttons(buf, y0, h, phone_buttons, 1, 2, ID_NONE);
}

// ---------------------------------------------------------------- icons (drawn per pixel, size = radius)

static bool icon_px(int kind, float x, float y, float r)    // x, y relative to the icon centre
{
    switch (kind) {
    case ICON_WIFI: {                                        // dot + three arcs fanning upwards
        float by = y - r * 0.55f, d = sqrtf(x * x + by * by);
        if (d < r * 0.16f) return true;
        if (by > 0 || fabsf(x) > -by * 1.05f) return false;
        for (int k = 1; k <= 3; k++) {
            float rr = r * 0.36f * k;
            if (d > rr - r * 0.10f && d < rr + r * 0.10f) return true;
        }
        return false;
    }
    case ICON_TOUCH: {                                       // target: ring + centre dot
        float d = sqrtf(x * x + y * y);
        return d < r * 0.22f || (d > r * 0.55f && d < r * 0.78f);
    }
    case ICON_SUN: {                                         // disc + 8 rays
        float d = sqrtf(x * x + y * y);
        if (d < r * 0.40f) return true;
        if (d < r * 0.58f || d > r * 0.98f) return false;
        float a = atan2f(y, x), k = a / (float)(M_PI / 4);
        return fabsf(k - roundf(k)) * (float)(M_PI / 4) * d < r * 0.09f;
    }
    case ICON_LOCK: {                                        // padlock: body + shackle
        if (fabsf(x) < r * 0.70f && y > -r * 0.10f && y < r * 0.80f)
            return !(fabsf(x) < r * 0.12f && y > r * 0.18f && y < r * 0.50f);   // keyhole
        float sy = y + r * 0.10f, d = sqrtf(x * x + sy * sy);
        return y <= -r * 0.10f && d > r * 0.32f && d < r * 0.50f;
    }
    case ICON_TILT: {                                        // phone outline turned 25 degrees
        float c = 0.906f, sn = 0.423f, u = c * x + sn * y, v = -sn * x + c * y;
        float hw = r * 0.48f, hh = r * 0.80f, t = r * 0.17f;
        bool in = fabsf(u) < hw && fabsf(v) < hh, inner = fabsf(u) < hw - t && fabsf(v) < hh - t;
        return (in && !inner) || (fabsf(u) < r * 0.12f && v > hh - t * 2.2f && v < hh - t * 1.2f);
    }
    }
    return false;
}

static void draw_icon(uint16_t *buf, int y0, int h, int kind, int cx, int cy, int r, uint16_t col)
{
    for (int py = cy - r; py <= cy + r; py++) {
        if (py < y0 || py >= y0 + h) continue;
        for (int px = cx - r; px <= cx + r; px++)
            if (icon_px(kind, px - cx, py - cy, r)) fill_rect(buf, y0, h, px, py, px + 1, py + 1, col);
    }
}

static void draw_hold_ring(uint16_t *buf, int y0, int h, int cx, int cy, uint16_t col)   // fills clockwise over 3 s
{
    int64_t held = esp_timer_get_time() - press_us;
    float frac = held >= LOCK_HOLD_US ? 1 : held / (float)LOCK_HOLD_US;
    for (int y = cy - BUB_R - 9; y <= cy + BUB_R + 9; y++) {
        if (y < y0 || y >= y0 + h) continue;
        for (int x = cx - BUB_R - 9; x <= cx + BUB_R + 9; x++) {
            float dx = x - cx, dy = y - cy, d = sqrtf(dx * dx + dy * dy);
            if (d < BUB_R + 3 || d > BUB_R + 8) continue;
            float a = atan2f(dx, -dy) / (2 * (float)M_PI);   // 0 at the top, going clockwise
            if (a < 0) a += 1;
            buf[(y - y0) * W + x] = a <= frac ? col : C_DIM;
        }
    }
}

#if !SCREEN_ROUND
// Text in font f cut with "..." to fit w
static void draw_fit(uint16_t *buf, int y0, int h, int x, int y, const font_t *f, const char *s, int w, uint16_t col)
{
    char t[64];
    strlcpy(t, s, sizeof t);
    if (ftext_width(f, t) > w) {
        int n = strlen(t), dw = ftext_width(f, "...");
        while (n > 0 && ftext_width_n(f, t, n) + dw > w) n--;
        strcpy(t + n, "...");
    }
    draw_ftext(buf, y0, h, x, y, f, t, col);
}

// Which firmware: its version (git's, from the build) and, for a release, its build number; else when it was built
static void draw_about(uint16_t *buf, int y0, int h, int x1, int y)
{
    const font_t *f = &font_label;
    const esp_app_desc_t *d = esp_app_get_description();
    char v[48], b[40];
    snprintf(v, sizeof v, "Firmware %s", d->version);
#if defined(BOBBLE_BUILD) && BOBBLE_BUILD
    snprintf(b, sizeof b, "Build %lld", (long long)BOBBLE_BUILD);
#else
    snprintf(b, sizeof b, "Built %s", ota_build_time());
#endif
    const char *lines[3] = {agent_device_name(), v, b};
    const uint16_t cols[3] = {rgb(200, 200, 212), C_GREY, rgb(90, 90, 108)};
    for (int i = 0; i < 3; i++) {
        int w = ftext_width(f, lines[i]), room = x1 - 216;
        if (w > room) w = room;
        draw_fit(buf, y0, h, x1 - w, y + i * f->line_h, f, lines[i], room, cols[i]);
    }
}

static void draw_menu(uint16_t *buf, int y0, int h)
{
    static const int kinds[] = {ICON_WIFI, ICON_TOUCH, ICON_TILT, ICON_LOCK};
    const font_t *f = &font_label;
    draw_ftext(buf, y0, h, TILE_X0 + 4, 16, &font_body, "Settings", C_WHITE);
    for (int i = 0; i < 4; i++) {
        const button_t *b = &menu_buttons[i];
        if (b->y1 <= y0 || b->y0 >= y0 + h) continue;
        bool on = current_id() == b->id;
        int cy = (b->y0 + b->y1) / 2, tx = b->x0 + 84, tw = b->x1 - 12 - tx;
        fill_rrect(buf, y0, h, b->x0, b->y0, b->x1, b->y1, 18, on ? C_PRESS : C_PANEL);
        uint16_t icol = C_ACCENT;                      // Wi-Fi: green when connected, grey when not
        const char *sub = "Calibrate";
        if (i == 0) {
            bool conn = wifi_state() == WS_CONNECTED;
            icol = conn ? C_GREEN : C_GREY;
            sub = conn ? wifi_ssid() : wifi_state() == WS_CONNECTING ? "Connecting..." : "Not connected";
        }
        if (i == 3) sub = "Hold 3 s";
        draw_icon(buf, y0, h, kinds[i], b->x0 + 44, cy, 24, on ? C_WHITE : icol);
        draw_ftext(buf, y0, h, tx, cy - f->line_h + 1, f, b->label, C_WHITE);
        draw_fit(buf, y0, h, tx, cy + 3, f, sub, tw, on ? rgb(200, 240, 232) : C_GREY);
        if (i == 3 && on) {                            // holding Lock: a bar fills along the tile's foot
            int64_t held = esp_timer_get_time() - press_us;
            float frac = held >= LOCK_HOLD_US ? 1 : held / (float)LOCK_HOLD_US;
            fill_rrect(buf, y0, h, b->x0 + 16, b->y1 - 12, b->x1 - 16, b->y1 - 7, 2, C_DIM);
            fill_rrect(buf, y0, h, b->x0 + 16, b->y1 - 12, b->x0 + 16 + (int)((b->x1 - b->x0 - 32) * frac), b->y1 - 7, 2, C_ACCENT);
        }
    }
    const button_t *b = &menu_buttons[4];            // brightness: small sun, track with knob, big sun (as round)
    int cy = (b->y0 + b->y1) / 2, tx0 = b->x0 + 56, tx1 = b->x1 - 56;
    int kx = tx0 + (tx1 - tx0) * (screen_brightness_get() - 10) / 245;
    fill_rrect(buf, y0, h, b->x0, b->y0, b->x1, b->y1, 18, C_PANEL);
    draw_icon(buf, y0, h, ICON_SUN, b->x0 + 30, cy, 11, C_GREY);
    draw_icon(buf, y0, h, ICON_SUN, b->x1 - 30, cy, 17, C_WHITE);
    fill_rrect(buf, y0, h, tx0, cy - 4, tx1, cy + 4, 4, rgb(60, 60, 78));
    fill_rrect(buf, y0, h, tx0, cy - 4, kx, cy + 4, 4, C_ACCENT);
    fill_circle(buf, y0, h, kx, cy, current_id() == ID_BRIGHT ? 17 : 14, C_WHITE);

    const button_t *r = &menu_buttons[5];
    bool ron = current_id() == ID_RESET;
    fill_rrect(buf, y0, h, r->x0, r->y0, r->x1, r->y1, 18, ron ? C_PRESS : C_PANEL);
    draw_ftext(buf, y0, h, (r->x0 + r->x1 - ftext_width(f, r->label)) / 2, (r->y0 + r->y1 - f->line_h) / 2, f, r->label,
               ron ? C_WHITE : C_RED);
    draw_about(buf, y0, h, TILE_X1, 366);
}
#else
static void draw_menu(uint16_t *buf, int y0, int h)
{
    static const int kinds[] = {ICON_WIFI, ICON_TOUCH, ICON_TILT, ICON_LOCK};
    for (int i = 0; i < 4; i++) {
        const button_t *b = &menu_buttons[i];
        if (b->y1 <= y0 || b->y0 >= y0 + h) continue;
        bool on = current_id() == b->id;
        int cx = (b->x0 + b->x1) / 2, cy = (b->y0 + b->y1) / 2;
        fill_circle(buf, y0, h, cx, cy, BUB_R, on ? C_PRESS : C_PANEL);
        if (i == 3 && on) draw_hold_ring(buf, y0, h, cx, cy, C_ACCENT);   // holding Lock
        uint16_t icol = C_ACCENT;                      // Wi-Fi: green when connected, grey when not
        if (i == 0) icol = wifi_state() == WS_CONNECTED ? C_GREEN : C_GREY;
        draw_icon(buf, y0, h, kinds[i], cx, cy, 32, on ? C_WHITE : icol);
    }
    // brightness: small sun, track with knob, big sun
    const button_t *b = &menu_buttons[4];
    int cy = (b->y0 + b->y1) / 2, tx0 = b->x0 + 56, tx1 = b->x1 - 56;
    int kx = tx0 + (tx1 - tx0) * (screen_brightness_get() - 10) / 245;
    fill_rrect(buf, y0, h, b->x0, b->y0 + 3, b->x1, b->y1 - 3, 18, C_PANEL);
    draw_icon(buf, y0, h, ICON_SUN, b->x0 + 30, cy, 11, C_GREY);
    draw_icon(buf, y0, h, ICON_SUN, b->x1 - 30, cy, 17, C_WHITE);
    fill_rrect(buf, y0, h, tx0, cy - 4, tx1, cy + 4, 4, rgb(60, 60, 78));
    fill_rrect(buf, y0, h, tx0, cy - 4, kx, cy + 4, 4, C_ACCENT);
    fill_circle(buf, y0, h, kx, cy, current_id() == ID_BRIGHT ? 17 : 14, C_WHITE);

    const button_t *r = &menu_buttons[5];
    fill_rrect(buf, y0, h, r->x0, r->y0, r->x1, r->y1, 18, current_id() == ID_RESET ? C_PRESS : C_PANEL);
    text_c(buf, y0, h, (r->y0 + r->y1) / 2 - 7, 2, r->label, current_id() == ID_RESET ? C_WHITE : C_GREY);
}
#endif

// Factory reset: says what goes, then a red bubble you hold for 3 s (main.c does the wiping)
static void draw_reset(uint16_t *buf, int y0, int h)
{
    text_c(buf, y0, h, 62, 3, "Factory reset", C_WHITE);
    text_c(buf, y0, h, 112, 2, "Erases Wi-Fi, pairing,", C_GREY);
    text_c(buf, y0, h, 136, 2, "calendars, Spotify and", C_GREY);
    text_c(buf, y0, h, 160, 2, "reminders for a new owner", C_GREY);
    const button_t *b = &reset_buttons[0];
    bool on = current_id() == ID_ERASE;
    fill_circle(buf, y0, h, CX, ERASE_Y, BUB_R, on ? rgb(150, 40, 40) : rgb(60, 22, 26));
    if (on) draw_hold_ring(buf, y0, h, CX, ERASE_Y, C_RED);
    text_c(buf, y0, h, ERASE_Y - 10, 3, b->label, on ? C_WHITE : C_RED);
    draw_buttons(buf, y0, h, &reset_buttons[1], 1, 2, ID_NONE);
    text_c(buf, y0, h, 388, 2, agent_device_name(), C_GREY);   // (which board this is, and what it runs)
    text_c(buf, y0, h, 412, 2, ota_build_time(), rgb(70, 70, 88));
}

void wifi_ui_draw(uint16_t *buf, int y0, int h)
{
    switch (view) {
    case V_MENU: draw_menu(buf, y0, h); break;
    case V_LIST: draw_list(buf, y0, h); break;
    case V_KEYS: draw_keys(buf, y0, h); break;
    case V_STATUS: draw_status(buf, y0, h); break;
    case V_PHONE: draw_phone(buf, y0, h); break;
    case V_RESET: draw_reset(buf, y0, h); break;
    }
}

// ---------------------------------------------------------------- behaviour
static void go_list(void)
{
    view = V_LIST;
    scroll = 0;
    wifi_pause_retries(true);
    wifi_scan();
    last_scan_us = esp_timer_get_time();
}

static void go_keys(const char *ssid)
{
    if (strcmp(ssid, join_ssid)) password[0] = 0;   // keep the typed password when retrying the same network
    strlcpy(join_ssid, ssid, sizeof join_ssid);
    view = V_KEYS;
    caps = symbols = false;
    build_keyboard();
}

static void start_join(void)
{
    error_msg[0] = 0;
    wifi_join(join_ssid, join_secure ? password : "");
    view = V_STATUS;
}

void wifi_ui_enter(void)
{
    down = false;
    error_msg[0] = 0;
    view = V_MENU;
}

void wifi_ui_show_phone(void) { view = V_PHONE; }

void wifi_ui_leave(void) { wifi_pause_retries(false); }

static int activate(int id)
{
    if (id >= ID_ROW && id < ID_KEY) {
        wifi_net_t *n = &nets[id - ID_ROW];
        error_msg[0] = 0;
        if (wifi_state() == WS_CONNECTED && !strcmp(n->ssid, wifi_ssid())) { view = V_STATUS; return WIFI_UI_NONE; }
        join_secure = n->secure;
        if (n->secure) {
            from_keys = true;
            go_keys(n->ssid);
        } else {
            from_keys = false;
            strlcpy(join_ssid, n->ssid, sizeof join_ssid);
            start_join();
        }
        return WIFI_UI_NONE;
    }
    if (id >= ID_KEY) {
        int n = strlen(password);
        if (n < 63) { password[n] = (char)(id - ID_KEY); password[n + 1] = 0; }
        if (caps && !symbols) { caps = false; build_keyboard(); }   // shift applies to one letter
        error_msg[0] = 0;
        return WIFI_UI_NONE;
    }
    switch (id) {
    case ID_MENU: view = V_MENU; error_msg[0] = 0; wifi_pause_retries(false); break;
    case ID_WIFI: if (wifi_state() == WS_PORTAL) go_list(); else view = V_STATUS; break;
    case ID_CAL_TOUCH: return WIFI_UI_CAL_TOUCH;
    case ID_CAL_TILT: return WIFI_UI_CAL_TILT;
    case ID_RESET: view = V_RESET; break;
    case ID_SCAN: wifi_scan(); last_scan_us = esp_timer_get_time(); break;
    case ID_PHONE: view = V_PHONE; break;
    case ID_BACK: go_list(); break;
    case ID_EXIT: return WIFI_UI_CLOSE;
    case ID_NETWORKS: go_list(); break;
    case ID_FORGET: wifi_forget(); go_list(); break;
    case ID_CANCEL:
        if (view == V_STATUS) wifi_forget();
        error_msg[0] = 0;
        go_list();
        break;
    case ID_SHIFT: caps = !caps; build_keyboard(); break;
    case ID_SYM: symbols = !symbols; build_keyboard(); break;
    case ID_SHOW: hide_pw = !hide_pw; build_keyboard(); break;
    case ID_SPACE: { int n = strlen(password); if (n < 63) { password[n] = ' '; password[n + 1] = 0; } break; }
    case ID_DEL: { int n = strlen(password); if (n) password[n - 1] = 0; break; }
    case ID_JOIN: if (strlen(password) >= 8 || !password[0]) start_join();
                  else strlcpy(error_msg, "At least 8 characters", sizeof error_msg);
                  break;
    }
    return WIFI_UI_NONE;
}

static int hit_test(int x, int y)
{
    switch (view) {
    case V_MENU: {
        const button_t *b = &menu_buttons[4];          // the slider row takes priority over padding around it
        if (x >= b->x0 && x < b->x1 && y >= b->y0 - 4 && y < b->y1 + 4) return ID_BRIGHT;
        if (hit(&menu_buttons[5], 1, x, y)) return ID_RESET;
        int id = hit_pad(menu_buttons, 4, x, y, false);   // rows sit close together: a row you're on beats a neighbour's margin
        return id ? id : hit(menu_buttons, 4, x, y);
    }
    case V_LIST: {
        int id = list_hit(x, y);
        if (id) return id;
        id = hit(list_buttons, 3, x, y);
        return id == ID_PHONE && !wifi_ap_active() ? ID_NONE : id;
    }
    case V_KEYS: {
        int id = hit_pad(keys, key_count - 2, x, y, false);           // letter keys
        return id ? id : hit(&keys[key_count - 2], 2, x, y);          // Cancel / Show below them
    }
    case V_STATUS:
        return wifi_state() == WS_CONNECTED ? hit(connected_buttons, 3, x, y) : hit(connecting_buttons, 1, x, y);
    case V_PHONE: return hit(phone_buttons, 1, x, y);
    case V_RESET: return hit(reset_buttons, 2, x, y);
    }
    return ID_NONE;
}

int wifi_ui_update(int tx, int ty, int64_t now)
{
    int action = WIFI_UI_NONE;

    // follow the connection: a finished attempt sends us to the clock, a failed one back to the keyboard/list
    if (view == V_STATUS && wifi_state() == WS_PORTAL) {
        if (wifi_last_failed()) {
            char s[40];
            fit(s, sizeof s, join_ssid[0] ? join_ssid : wifi_ssid(), 14);
            if (from_keys && join_secure) {
                go_keys(join_ssid);
                snprintf(error_msg, sizeof error_msg, "Wrong password - try again");
            } else {
                go_list();
                snprintf(error_msg, sizeof error_msg, "Couldn't join %s", s);
            }
        } else {
            go_list();
        }
    }
    if (view == V_LIST) {
        net_count = wifi_networks(nets, MAX_NETS);
        if (scroll > max_scroll()) scroll = max_scroll();
        // rescan every 12 s, or every 2 s while we have nothing (scans fail while a join attempt is running)
        if (!wifi_scanning() && now - last_scan_us > (net_count ? 12000000 : 2000000)) { wifi_scan(); last_scan_us = now; }
    }
    if (view == V_PHONE) {
        char t[128];
        if (wifi_ap_clients() == 0) snprintf(t, sizeof t, "WIFI:T:WPA;S:%s;P:%s;;", wifi_ap_ssid(), wifi_ap_pass());
        else strlcpy(t, "http://192.168.4.1/", sizeof t);
        qr_set(t);
        if (!wifi_ap_active()) go_list();
    }

    bool is_down = tx >= 0;
    if (is_down && !down) {                          // finger down
        down = true;
        dragging = false;
        press_x = cur_x = tx; press_y = cur_y = ty;
        press_us = repeat_us = now;
        scroll_at_press = scroll;
        press_id = hit_test(tx, ty);
        ESP_LOGI("wifi_ui", "touch down %d,%d view %d -> id %d", tx, ty, view, press_id);
    } else if (is_down) {                             // finger moving
        cur_x = tx; cur_y = ty;
        if (abs(ty - press_y) > 14 || abs(tx - press_x) > 14) {
            if (view == V_LIST && press_y >= LIST_TOP && press_y < LIST_BOT) dragging = true;
            else if (press_id != ID_BRIGHT && hit_test(tx, ty) != press_id) press_id = ID_NONE;   // slid off the key
        }
        if (dragging) {
            scroll = scroll_at_press - (ty - press_y);
            if (scroll < 0) scroll = 0;
            if (scroll > max_scroll()) scroll = max_scroll();
        }
        if (press_id == ID_DEL && now - press_us > 500000 && now - repeat_us > 70000) {   // hold to keep deleting
            int n = strlen(password);
            if (n) password[n - 1] = 0;
            repeat_us = now;
        }
    }
    if (down && press_id == ID_LOCK && now - press_us >= LOCK_HOLD_US) {   // held long enough: lock (once)
        press_id = ID_NONE;
        action = WIFI_UI_LOCK;
    }
    if (down && press_id == ID_ERASE && now - press_us >= LOCK_HOLD_US) {   // held long enough: factory reset
        press_id = ID_NONE;
        action = WIFI_UI_RESET;
    }
    if (down && press_id == ID_BRIGHT) {               // brightness follows the finger along the track
        const button_t *b = &menu_buttons[4];
        int tx0 = b->x0 + 56, tx1 = b->x1 - 56, x = cur_x < tx0 ? tx0 : (cur_x > tx1 ? tx1 : cur_x);
        screen_brightness_set(10 + 245 * (x - tx0) / (tx1 - tx0), false);
    }
    if (!is_down && down && press_id == ID_BRIGHT) {   // let go of the slider: remember the level
        down = false;
        screen_brightness_set(screen_brightness_get(), true);
        press_id = ID_NONE;
    } else if (!is_down && down) {                    // finger up
        down = false;
        ESP_LOGI("wifi_ui", "touch up at %d,%d, id %d%s", cur_x, cur_y, press_id, dragging ? " (drag)" : "");
        if (!dragging && press_id != ID_NONE && !(press_id == ID_DEL && now - press_us > 500000)) action = activate(press_id);
        press_id = ID_NONE;
        dragging = false;
    }
    return action;
}
