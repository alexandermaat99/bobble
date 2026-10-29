// Settings card for the 466x466 round AMOLED (and the 480x480 square board, which lays the menu out as tiles and says
// which firmware it runs): a small menu (Wi-Fi, touch and tilt calibration, brightness, reset; the round board also lock,
// the square volume), plus
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
#include "audio.h"
#include "esp_app_desc.h"
#include "battery.h"

enum view { V_MENU, V_LIST, V_KEYS, V_STATUS, V_PHONE, V_RESET, V_BATTERY };
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
    ID_MENU, ID_WIFI, ID_CAL_TOUCH, ID_CAL_TILT, ID_BRIGHT, ID_LOCK, ID_RESET, ID_ERASE, ID_VOLUME, ID_BATTERY,
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
#if !SCREEN_ROUND
// The square's Wi-Fi pages: a "< Settings" link top left and the title in the middle, then rounded groups of rows
#define LIST_TOP 112
#define LIST_BOT 404
#define ROW_H 52
#define ROW_X0 16
#define ROW_X1 (W - 16)
#define NAV_BACK {8, 46, 160, 88}
static const button_t list_buttons[] = {
    {16, 414, 170, 456, ID_SCAN, "Scan again"},
    {180, 414, W - 16, 456, ID_PHONE, "Set up with phone"},
    {8, 46, 160, 88, ID_MENU, "Settings"},
};
static const button_t connected_buttons[] = {
    {16, 332, W - 16, 384, ID_NETWORKS, "Other networks"},
    {16, 396, W - 16, 448, ID_FORGET, "Forget this network"},
    {8, 46, 160, 88, ID_MENU, "Settings"},
};
static const button_t connecting_buttons[] = {{16, 380, W - 16, 432, ID_CANCEL, "Cancel"}};
static const button_t phone_buttons[] = {{8, 46, 160, 88, ID_BACK, "Wi-Fi"}};
#else
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
#endif
#define BUB_DX 74      // bubble columns either side of the middle
#define BUB_Y1 122     // bubble rows
#define BUB_Y2 256
#define BUB_R 62
#if !SCREEN_ROUND
// The square, like the phone's Control Center: a grid of rounded modules four columns wide, three rows high. Wi-Fi
// is wide (an icon disc, a name and a line about it), Tilt and Touch small tiles under it, brightness and volume
// tall sliders beside them, then Factory reset and Battery (which opens the charger's numbers, and says which board
// this is and what it runs), wide, along the bottom. (No Lock on the square: it still locks itself after 2 hours
// asleep on battery.)
#define CC_COL(c) (20 + (c) * 114)                 // a module's left edge: 98 wide, 16 apart
#define CC_ROW(r) (78 + (r) * 118)                 // and its top: 104 high, 14 apart
#define CC_W 98
#define CC_H 104
#define CC_R 24
static const button_t menu_buttons[] = {
    {CC_COL(0), CC_ROW(0), CC_COL(1) + CC_W, CC_ROW(0) + CC_H, ID_WIFI, "Wi-Fi"},
    {CC_COL(1), CC_ROW(1), CC_COL(1) + CC_W, CC_ROW(1) + CC_H, ID_CAL_TOUCH, "Touch"},
    {CC_COL(0), CC_ROW(1), CC_COL(0) + CC_W, CC_ROW(1) + CC_H, ID_CAL_TILT, "Tilt"},
    {-100, -100, -100, -100, ID_LOCK, "Lock"},         // (off the glass: never hit)
    {CC_COL(2), CC_ROW(0), CC_COL(2) + CC_W, CC_ROW(1) + CC_H, ID_BRIGHT, ""},   // brightness: drag up and down
    {CC_COL(0), CC_ROW(2), CC_COL(1) + CC_W, CC_ROW(2) + CC_H, ID_RESET, "Factory reset"},
    {CC_COL(3), CC_ROW(0), CC_COL(3) + CC_W, CC_ROW(1) + CC_H, ID_VOLUME, ""},   // volume: likewise
    {CC_COL(2), CC_ROW(2), CC_COL(3) + CC_W, CC_ROW(2) + CC_H, ID_BATTERY, "Battery"},
};
static const button_t battery_buttons[] = {{8, 46, 160, 88, ID_MENU, "Settings"}};
#else
static const button_t menu_buttons[] = {
    // round icon bubbles, two by two
    {CX - BUB_DX - BUB_R, BUB_Y1 - BUB_R, CX - BUB_DX + BUB_R, BUB_Y1 + BUB_R, ID_WIFI, "Wi-Fi"},
    {CX - BUB_DX - BUB_R, BUB_Y2 - BUB_R, CX - BUB_DX + BUB_R, BUB_Y2 + BUB_R, ID_CAL_TOUCH, "Touch"},
    {CX + BUB_DX - BUB_R, BUB_Y2 - BUB_R, CX + BUB_DX + BUB_R, BUB_Y2 + BUB_R, ID_CAL_TILT, "Tilt"},
    {CX + BUB_DX - BUB_R, BUB_Y1 - BUB_R, CX + BUB_DX + BUB_R, BUB_Y1 + BUB_R, ID_LOCK, "Lock"},
    {CX - 172, 338, CX + 172, 386, ID_BRIGHT, ""},   // brightness slider row
    {CX - 100, 396, CX + 100, 434, ID_RESET, "Factory reset"},
    {-100, -100, -100, -100, ID_VOLUME, ""},          // (no volume here: off the glass, never hit)
    {-100, -100, -100, -100, ID_BATTERY, ""},         // (nor a battery page)
};
#endif
#define ERASE_Y 262
static const button_t reset_buttons[] = {
    {CX - BUB_R, ERASE_Y - BUB_R, CX + BUB_R, ERASE_Y + BUB_R, ID_ERASE, "Hold"},
#if !SCREEN_ROUND
    {8, 46, 160, 88, ID_MENU, "Settings"},
#else
    {CX - 55, 340, CX + 55, 376, ID_MENU, "Back"},
#endif
};

// keyboard: 5 rows of keys on a 40 px pitch, sized to stay inside the round display
#define KEY_TOP 128
#define KEY_ROW 46
#define KEY_P 40
static const char *rows_lower[] = {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"};
static const char *rows_upper[] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
static const char *rows_sym[] = {"!@#$%^&*()", "-_=+[]{}\\|", ";:'\",.<>?", "/`~"};

#define MAX_KEYS 48
EXT_RAM_BSS_ATTR static button_t keys[MAX_KEYS];
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
    return (view == V_MENU || view == V_STATUS || view == V_PHONE || view == V_RESET || view == V_BATTERY) &&
           press_id != ID_BRIGHT &&
           press_id != ID_VOLUME && press_id != ID_ERASE;
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

// While a slider is held only it changes: its box on the card, so only those rows need drawing
bool wifi_ui_slider_rect(int r[4])
{
    if (view != V_MENU || !down || (press_id != ID_BRIGHT && press_id != ID_VOLUME)) return false;
    const button_t *b = &menu_buttons[press_id == ID_BRIGHT ? 4 : 6];
    r[0] = b->x0 - 2; r[1] = b->y0 - 2; r[2] = b->x1 + 2; r[3] = b->y1 + 2;
    return true;
}

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
enum { ICON_WIFI, ICON_TOUCH, ICON_TILT, ICON_SUN, ICON_LOCK, ICON_BATTERY };
static void draw_icon(uint16_t *buf, int y0, int h, int kind, int cx, int cy, int r, uint16_t col);
#if SCREEN_ROUND                                     // (the square's own come after its menu)
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
        draw_bars(buf, y0, h, ROW_X1 - 38, ry + 16, connected ? wifi_rssi() : nets[i].rssi);   // (live, like the status bar)
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
#endif

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
// A speaker, k px to a unit (about 22 k across), with as many sound waves as it's loud (0..2; -1: crossed out)
static void speaker_icon(uint16_t *buf, int y0, int h, float cx, float cy, float k, int waves, uint16_t col)
{
    float x = cx - 4 * k;                             // (the cone's mouth)
    const float body[] = {x - 7 * k, cy - 3.5f * k, x - 3 * k, cy - 3.5f * k, x + 2.5f * k, cy - 8 * k,
                          x + 2.5f * k, cy + 8 * k, x - 3 * k, cy + 3.5f * k, x - 7 * k, cy + 3.5f * k};
    aa_path(buf, y0, h, body, 6, true, 1.8f * k, col);
    fill_rect(buf, y0, h, (int)(x - 7 * k + 1), (int)(cy - 3.5f * k + 1), (int)(x - 3 * k), (int)(cy + 3.5f * k), col);
    fill_tri(buf, y0, h, (int)(x - 3 * k), (int)(cy - 3.5f * k), (int)(x + 2.5f * k), (int)(cy - 8 * k), (int)(x + 2.5f * k),
             (int)(cy + 8 * k), col);
    fill_tri(buf, y0, h, (int)(x - 3 * k), (int)(cy - 3.5f * k), (int)(x + 2.5f * k), (int)(cy + 8 * k), (int)(x - 3 * k),
             (int)(cy + 3.5f * k), col);
    if (waves < 0) {
        const float a[] = {x + 7 * k, cy - 4 * k, x + 15 * k, cy + 4 * k}, b[] = {x + 7 * k, cy + 4 * k, x + 15 * k, cy - 4 * k};
        aa_path(buf, y0, h, a, 2, false, 1.8f * k, col);
        aa_path(buf, y0, h, b, 2, false, 1.8f * k, col);
        return;
    }
    for (int w = 1; w <= waves; w++) {                // arcs round the cone's mouth
        float r = (w == 1 ? 6 : 11) * k, p[2 * 12];
        for (int i = 0; i < 12; i++) {
            float an = (-50 + 100 * i / 11.0f) * (float)M_PI / 180;
            p[2 * i] = x + r * cosf(an);
            p[2 * i + 1] = cy + r * sinf(an);
        }
        aa_path(buf, y0, h, p, 12, false, 1.8f * k, col);
    }
}

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

// Control Center's line icons, smooth, about 24 px across
static void cc_arc(uint16_t *buf, int y0, int h, float cx, float cy, float r, float a0, float a1, float w, uint16_t c)
{
    float p[2 * 24];                                // (angles in degrees, clockwise from up)
    int n = 24;
    for (int i = 0; i < n; i++) {
        float a = (a0 + (a1 - a0) * i / (n - 1)) * (float)M_PI / 180;
        p[2 * i] = cx + r * sinf(a);
        p[2 * i + 1] = cy - r * cosf(a);
    }
    aa_path(buf, y0, h, p, n, false, w, c);
}

static void cc_icon(uint16_t *buf, int y0, int h, int kind, float cx, float cy, uint16_t c, uint16_t bg)
{
    switch (kind) {
    case ICON_WIFI:
        aa_disc(buf, y0, h, cx, cy + 8, 2.6f, c);
        cc_arc(buf, y0, h, cx, cy + 8, 8, -45, 45, 2.8f, c);
        cc_arc(buf, y0, h, cx, cy + 8, 14.5f, -45, 45, 2.8f, c);
        break;
    case ICON_TOUCH:
        cc_arc(buf, y0, h, cx, cy, 10, 0, 360, 2.6f, c);
        aa_disc(buf, y0, h, cx, cy, 4, c);
        break;
    case ICON_TILT: {                               // a phone turned 25 degrees
        float co = 0.906f, si = 0.423f, q[8], hw = 6.5f, hh = 11;
        const float u[4] = {-hw, hw, hw, -hw}, v[4] = {-hh, -hh, hh, hh};
        for (int i = 0; i < 4; i++) { q[2 * i] = cx + co * u[i] - si * v[i]; q[2 * i + 1] = cy + si * u[i] + co * v[i]; }
        aa_path(buf, y0, h, q, 4, true, 2.6f, c);
        aa_disc(buf, y0, h, cx - si * 7.5f, cy + co * 7.5f, 1.4f, c);
        break;
    }
    case ICON_LOCK:
        cc_arc(buf, y0, h, cx, cy - 2, 5.5f, -90, 90, 2.6f, c);
        aa_rrect(buf, y0, h, cx - 8.5f, cy - 2.5f, cx + 8.5f, cy + 11, 3, c);
        aa_disc(buf, y0, h, cx, cy + 4, 1.8f, bg);
        break;
    case ICON_BATTERY: {                            // a cell on its side: a bolt in it while plugged in, else its level
        aa_rrect(buf, y0, h, cx - 12, cy - 6.5f, cx + 9, cy + 6.5f, 3.5f, c);
        aa_rrect(buf, y0, h, cx - 10, cy - 4.5f, cx + 7, cy + 4.5f, 2, bg);
        aa_rrect(buf, y0, h, cx + 10, cy - 2.5f, cx + 12.5f, cy + 2.5f, 1, c);
        int p = battery_percent();
        if (battery_charging()) {
            const float bolt[] = {cx + 1, cy - 6, cx - 4, cy + 0.8f, cx + 2, cy - 0.8f, cx - 3, cy + 6};
            aa_path(buf, y0, h, bolt, 4, false, 2.2f, c);
        } else if (p > 6) {
            aa_rrect(buf, y0, h, cx - 8.5f, cy - 3, cx - 8.5f + 14.0f * (p > 100 ? 100 : p) / 100, cy + 3, 1.2f, c);
        }
        break;
    }
    case ICON_SUN:
        aa_disc(buf, y0, h, cx, cy, 5, c);
        for (int i = 0; i < 8; i++) {
            float a = i * (float)M_PI / 4, p[4] = {cx + 8.5f * sinf(a), cy - 8.5f * cosf(a), cx + 11.5f * sinf(a), cy - 11.5f * cosf(a)};
            aa_path(buf, y0, h, p, 2, false, 2.2f, c);
        }
        break;
    }
}

static void ftext_mid_cc(uint16_t *buf, int y0, int h, int cx, int y, const font_t *f, const char *t, uint16_t col)
{
    draw_ftext(buf, y0, h, cx - ftext_width(f, t) / 2, y, f, t, col);
}

// a bin, for Factory reset
static void bin_icon(uint16_t *buf, int y0, int h, float cx, float cy, uint16_t c)
{
    static const float lid[] = {5, 6.5f, 19, 6.5f}, grip[] = {9.5f, 6.5f, 10, 3.5f, 14, 3.5f, 14.5f, 6.5f};
    static const float body[] = {7, 9.5f, 8, 20, 16, 20, 17, 9.5f};
    aa_icon(buf, y0, h, cx, cy, 22, lid, 2, false, 2.2f, c);
    aa_icon(buf, y0, h, cx, cy, 22, grip, 4, false, 2, c);
    aa_icon(buf, y0, h, cx, cy, 22, body, 4, false, 2.2f, c);
}

#define CC_MOD     rgb(36, 36, 40)                   // a module
#define CC_MOD_ON  rgb(58, 58, 64)                   // ...under the finger
#define CC_OFF     rgb(70, 70, 76)                   // an icon disc that's off
#define CC_BLUE    rgb(10, 132, 255)
#define CC_GREEN   rgb(48, 209, 88)
#define CC_TEXT2   rgb(150, 150, 158)

// a wide module: an icon disc on the left, its name and a line about it
static void cc_wide(uint16_t *buf, int y0, int h, const button_t *b, uint16_t disc, const char *name, const char *sub,
                    uint16_t sub_col, bool on)
{
    aa_rrect(buf, y0, h, b->x0, b->y0, b->x1, b->y1, CC_R, on ? CC_MOD_ON : CC_MOD);
    float cy = (b->y0 + b->y1) / 2.0f;
    aa_disc(buf, y0, h, b->x0 + 38, cy, 23, disc);
    int tx = b->x0 + 70, tw = b->x1 - 12 - tx;
    draw_fit(buf, y0, h, tx, (int)cy - 23, &font_label, name, tw, C_WHITE);
    draw_fit(buf, y0, h, tx, (int)cy + 2, &font_small, sub, tw, sub_col);
}

// a small module: its icon in a disc, its name under that
static void cc_small(uint16_t *buf, int y0, int h, const button_t *b, int kind, uint16_t disc, const char *name, bool on)
{
    aa_rrect(buf, y0, h, b->x0, b->y0, b->x1, b->y1, CC_R, on ? CC_MOD_ON : CC_MOD);
    float cx = (b->x0 + b->x1) / 2.0f;
    aa_disc(buf, y0, h, cx, b->y0 + 38, 23, disc);
    cc_icon(buf, y0, h, kind, cx, b->y0 + 38, C_WHITE, disc);
    ftext_mid_cc(buf, y0, h, (int)cx, b->y0 + 69, &font_label, name, C_WHITE);
}

// a tall slider module, lit from the bottom to frac (0..1): where the lit part starts
static float cc_slider(uint16_t *buf, int y0, int h, const button_t *b, float frac)
{
    aa_rrect(buf, y0, h, b->x0, b->y0, b->x1, b->y1, CC_R, CC_MOD);
    float fy = b->y1 - (b->y1 - b->y0) * frac;
    if (b->y1 - fy > 2) {                             // the lit part, clipped to the module
        int ct, cb;
        gfx_get_clip(&ct, &cb);
        gfx_clip((int)fy > ct ? (int)fy : ct, cb);
        aa_rrect(buf, y0, h, b->x0, b->y0, b->x1, b->y1, CC_R, rgb(228, 228, 234));
        gfx_clip(ct, cb);
    }
    return fy;
}

static void draw_menu(uint16_t *buf, int y0, int h)
{
    // Wi-Fi: blue while connected
    const button_t *wb = &menu_buttons[0];
    bool conn = wifi_state() == WS_CONNECTED;
    const char *sub = conn ? wifi_ssid() : wifi_state() == WS_CONNECTING ? "Connecting..." : "Not connected";
    cc_wide(buf, y0, h, wb, conn ? CC_BLUE : CC_OFF, "Wi-Fi", sub, CC_TEXT2, current_id() == ID_WIFI);
    cc_icon(buf, y0, h, ICON_WIFI, wb->x0 + 38, (wb->y0 + wb->y1) / 2.0f - 1, C_WHITE, 0);

    cc_small(buf, y0, h, &menu_buttons[2], ICON_TILT, CC_OFF, "Tilt", current_id() == ID_CAL_TILT);
    cc_small(buf, y0, h, &menu_buttons[1], ICON_TOUCH, CC_OFF, "Touch", current_id() == ID_CAL_TOUCH);

    // brightness: a tall module that fills from the bottom, the sun at its foot
    const button_t *b = &menu_buttons[4];
    float fy = cc_slider(buf, y0, h, b, (screen_brightness_get() - 10) / 245.0f);
    float sy = b->y1 - 30;
    cc_icon(buf, y0, h, ICON_SUN, (b->x0 + b->x1) / 2.0f, sy, sy > fy ? rgb(110, 110, 118) : rgb(200, 200, 206), 0);

    // volume, the same way, a speaker at its foot (crossed out when silent)
    const button_t *vb = &menu_buttons[6];
    int vol = device_volume_get();
    fy = cc_slider(buf, y0, h, vb, vol / 100.0f);
    sy = vb->y1 - 30;
    speaker_icon(buf, y0, h, (vb->x0 + vb->x1) / 2.0f + 2, sy, 1.15f, vol == 0 ? -1 : vol < 50 ? 1 : 2,
                 sy > fy ? rgb(110, 110, 118) : rgb(200, 200, 206));

    // Factory reset
    const button_t *rb = &menu_buttons[5];
    cc_wide(buf, y0, h, rb, rgb(255, 69, 58), "Factory reset", "Erase everything", CC_TEXT2, current_id() == ID_RESET);
    bin_icon(buf, y0, h, rb->x0 + 38, (rb->y0 + rb->y1) / 2.0f, C_WHITE);

    // Battery: its level and what the charger's doing (green while charging); opens the charger's numbers
    const button_t *bb = &menu_buttons[7];
    battery_diag_t bd;
    battery_diag(&bd);
    char bsub[48];
    int pct = battery_percent();
    if (pct >= 0) snprintf(bsub, sizeof bsub, "%d%%  %s", pct, battery_state_text(&bd));
    else strlcpy(bsub, battery_state_text(&bd), sizeof bsub);
    uint16_t bdisc = battery_charging_now(&bd) ? CC_GREEN : CC_OFF;
    cc_wide(buf, y0, h, bb, bdisc, "Battery", bsub, CC_TEXT2, current_id() == ID_BATTERY);
    cc_icon(buf, y0, h, ICON_BATTERY, bb->x0 + 38, (bb->y0 + bb->y1) / 2.0f, C_WHITE, bdisc);
}

// ---------------------------------------------------------------- the square's Wi-Fi pages, in the same style
#define SQ_GROUP rgb(36, 36, 40)
#define SQ_PRESS rgb(58, 58, 64)
#define SQ_SEP   rgb(52, 52, 58)
#define SQ_TEXT2 rgb(150, 150, 158)

static void ftext_mid(uint16_t *buf, int y0, int h, int cx, int cy, const font_t *f, const char *t, uint16_t col)
{
    draw_ftext(buf, y0, h, cx - ftext_width(f, t) / 2, cy - f->line_h / 2, f, t, col);
}

// "< Settings" in blue at the top left, the page's name in the middle
static void nav(uint16_t *buf, int y0, int h, const button_t *back, const char *title)
{
    static const float chev[] = {15, 5, 8, 12, 15, 19};
    uint16_t c = current_id() == back->id ? rgb(90, 170, 255) : CC_BLUE;
    aa_icon(buf, y0, h, 24, 67, 22, chev, 3, false, 2.4f, c);
    draw_ftext(buf, y0, h, 36, 67 - font_label.line_h / 2, &font_label, back->label, c);
    char t[48];
    strlcpy(t, title, sizeof t);
    int room = W - 2 * 150;
    draw_fit(buf, y0, h, CX - (ftext_width(&font_label, t) < room ? ftext_width(&font_label, t) : room) / 2,
             67 - font_label.line_h / 2, &font_label, t, room, C_WHITE);
}

// a row button: a rounded group of its own, its words centred (or in red / blue)
static void row_button(uint16_t *buf, int y0, int h, const button_t *b, uint16_t col)
{
    bool on = current_id() == b->id;
    aa_rrect(buf, y0, h, b->x0, b->y0, b->x1, b->y1, 14, on ? SQ_PRESS : SQ_GROUP);
    ftext_mid(buf, y0, h, (b->x0 + b->x1) / 2, (b->y0 + b->y1) / 2, &font_label, b->label, col);
}

// signal strength as the Wi-Fi fan, as many bands lit as it's strong
static void fan(uint16_t *buf, int y0, int h, float cx, float cy, int rssi)
{
    int level = rssi > -60 ? 3 : rssi > -72 ? 2 : 1;
    uint16_t on = C_WHITE, off = rgb(80, 80, 88);
    aa_disc(buf, y0, h, cx, cy + 6, 2.2f, on);
    cc_arc(buf, y0, h, cx, cy + 6, 6.5f, -45, 45, 2.4f, level >= 2 ? on : off);
    cc_arc(buf, y0, h, cx, cy + 6, 11.5f, -45, 45, 2.4f, level >= 3 ? on : off);
}

static void small_lock(uint16_t *buf, int y0, int h, float cx, float cy, uint16_t c, uint16_t bg)
{
    cc_arc(buf, y0, h, cx, cy - 2, 3.8f, -90, 90, 1.8f, c);
    aa_rrect(buf, y0, h, cx - 5.5f, cy - 2, cx + 5.5f, cy + 7, 2, c);
    (void)bg;
}

static void draw_list(uint16_t *buf, int y0, int h)
{
    nav(buf, y0, h, &list_buttons[2], "Wi-Fi");
    const char *note = error_msg[0] ? error_msg : wifi_scanning() && net_count == 0 ? "Looking for networks..." :
                       wifi_state() != WS_CONNECTED ? "Choose a network" : "";
    if (note[0]) ftext_mid(buf, y0, h, CX, 96, &font_small, note, error_msg[0] ? C_RED : SQ_TEXT2);

    gfx_clip(LIST_TOP, LIST_BOT);
    if (net_count > 0) {                              // the group the rows sit in
        int gt = LIST_TOP - scroll, gb = gt + net_count * ROW_H;
        aa_rrect(buf, y0, h, ROW_X0, gt, ROW_X1, gb, 14, SQ_GROUP);
    }
    for (int i = 0; i < net_count; i++) {
        int ry = LIST_TOP + i * ROW_H - scroll;
        if (ry + ROW_H <= y0 || ry >= y0 + h || ry + ROW_H <= LIST_TOP || ry >= LIST_BOT) continue;
        bool connected = wifi_state() == WS_CONNECTED && !strcmp(nets[i].ssid, wifi_ssid());
        if (current_id() == ID_ROW + i) {             // pressed: the row lights (rounded where it's the group's end)
            aa_rrect(buf, y0, h, ROW_X0, ry, ROW_X1, ry + ROW_H, 14, SQ_PRESS);
            if (i > 0) fill_rect(buf, y0, h, ROW_X0, ry, ROW_X1, ry + 14, SQ_PRESS);
            if (i < net_count - 1) fill_rect(buf, y0, h, ROW_X0, ry + ROW_H - 14, ROW_X1, ry + ROW_H, SQ_PRESS);
        }
        float mid = ry + ROW_H / 2.0f;
        if (connected) {
            static const float chk[] = {5, 12.5f, 10, 17.5f, 19, 7};
            aa_icon(buf, y0, h, 36, mid, 18, chk, 3, false, 2.4f, CC_BLUE);
        }
        draw_fit(buf, y0, h, 56, (int)mid - font_label.line_h / 2, &font_label, nets[i].ssid, ROW_X1 - 70 - 56, C_WHITE);
        if (nets[i].secure) small_lock(buf, y0, h, ROW_X1 - 48, mid, SQ_TEXT2, SQ_GROUP);
        fan(buf, y0, h, ROW_X1 - 22, mid - 4, connected ? wifi_rssi() : nets[i].rssi);   // (live, like the status bar)
        if (i < net_count - 1) fill_rect(buf, y0, h, 56, ry + ROW_H - 1, ROW_X1, ry + ROW_H, SQ_SEP);
    }
    if (net_count == 0 && !wifi_scanning()) ftext_mid(buf, y0, h, CX, 240, &font_label, "No networks found", SQ_TEXT2);
    if (wifi_scanning() && net_count == 0)
        for (int i = 0; i < 3; i++)
            aa_disc(buf, y0, h, CX - 24 + i * 24, 240, ((int)(esp_timer_get_time() / 200000) + i) % 3 == 0 ? 6 : 4, CC_BLUE);
    gfx_clip(0, H);
    int ms = max_scroll();
    if (ms > 0 && dragging) {
        int track = LIST_BOT - LIST_TOP - 20, th = track * (LIST_BOT - LIST_TOP) / (net_count * ROW_H);
        int ty = LIST_TOP + 10 + (track - th) * scroll / ms;
        aa_rrect(buf, y0, h, W - 9, ty, W - 5, ty + th, 2, rgb(110, 110, 118));
    }
    if (wifi_ap_active()) {
        row_button(buf, y0, h, &list_buttons[0], CC_BLUE);
        row_button(buf, y0, h, &list_buttons[1], CC_BLUE);
    } else {                                          // (no phone setup to offer: Scan takes the whole row)
        button_t b = list_buttons[0];
        b.x1 = list_buttons[1].x1;
        row_button(buf, y0, h, &b, CC_BLUE);
    }
}

static void draw_status(uint16_t *buf, int y0, int h)
{
    if (wifi_state() == WS_CONNECTED) {
        nav(buf, y0, h, &connected_buttons[2], "Wi-Fi");
        aa_disc(buf, y0, h, CX, 138, 34, CC_BLUE);
        cc_icon(buf, y0, h, ICON_WIFI, CX, 136, C_WHITE, CC_BLUE);
        ftext_mid(buf, y0, h, CX, 194, &font_label, "Connected", C_WHITE);
        const int gt = 216;                           // the network's name and address, in a group
        aa_rrect(buf, y0, h, ROW_X0, gt, ROW_X1, gt + 2 * 48, 14, SQ_GROUP);
        fill_rect(buf, y0, h, 32, gt + 48, ROW_X1, gt + 49, SQ_SEP);
        const char *lab[2] = {"Network", "IP address"}, *val[2] = {wifi_ssid(), wifi_ip()};
        for (int i = 0; i < 2; i++) {
            int my = gt + 24 + i * 48;
            draw_ftext(buf, y0, h, 32, my - font_label.line_h / 2, &font_label, lab[i], C_WHITE);
            int room = ROW_X1 - 16 - 160, vw = ftext_width(&font_label, val[i]);
            draw_fit(buf, y0, h, ROW_X1 - 16 - (vw < room ? vw : room), my - font_label.line_h / 2, &font_label, val[i], room, SQ_TEXT2);
        }
        row_button(buf, y0, h, &connected_buttons[0], CC_BLUE);
        row_button(buf, y0, h, &connected_buttons[1], rgb(255, 69, 58));
    } else {
        char t[64];
        ftext_mid(buf, y0, h, CX, 170, &font_label, "Connecting to", SQ_TEXT2);
        strlcpy(t, wifi_ssid(), sizeof t);
        ftext_mid(buf, y0, h, CX, 202, &font_body, t, C_WHITE);
        int phase = (int)(esp_timer_get_time() / 150000);
        for (int i = 0; i < 3; i++) aa_disc(buf, y0, h, CX - 24 + i * 24, 270 - ((phase + i) % 3 == 0 ? 6 : 0), 5, CC_BLUE);
        row_button(buf, y0, h, &connecting_buttons[0], rgb(255, 69, 58));
    }
}

static void draw_phone(uint16_t *buf, int y0, int h)
{
    char line[48];
    bool joined = wifi_ap_clients() > 0;
    nav(buf, y0, h, &phone_buttons[0], "Set up with phone");
    aa_rrect(buf, y0, h, CX - 104, 98, CX + 104, 306, 18, C_WHITE);   // a white card round the code
    draw_qr(buf, y0, h, 202, 196);
    ftext_mid(buf, y0, h, CX, 336, &font_label, joined ? "Now pick your network" : "Scan with your phone's camera", C_WHITE);
    if (joined) ftext_mid(buf, y0, h, CX, 364, &font_small, "on the page that opened, or 192.168.4.1", SQ_TEXT2);
    else {
        snprintf(line, sizeof line, "or join %s", wifi_ap_ssid());
        ftext_mid(buf, y0, h, CX, 364, &font_small, line, SQ_TEXT2);
        snprintf(line, sizeof line, "Password %s", wifi_ap_pass());
        ftext_mid(buf, y0, h, CX, 386, &font_small, line, SQ_TEXT2);
    }
}

static void volts_text(char *out, size_t cap, int mv)
{
    if (mv < 0) strlcpy(out, "-", cap);
    else snprintf(out, cap, "%d.%02d V", mv / 1000, (mv % 1000) / 10);
}

// The firmware's version, short: "build-2610282329-4-gf259f2c-dirty" (git describe) is "f259f2c, modified"
static void version_text(char *out, size_t cap)
{
    const char *v = esp_app_get_description()->version, *g = strstr(v, "-g");
    if (!g) { strlcpy(out, v, cap); return; }
    int n = strcspn(g + 2, "-");
    snprintf(out, cap, "%.*s%s", n, g + 2, strstr(g, "-dirty") ? ", modified" : "");
}

// a group of label / value rows, the value on the right in grey
static void rows(uint16_t *buf, int y0, int h, int top, int rh, int n, const char *const *lab, char (*val)[32])
{
    aa_rrect(buf, y0, h, ROW_X0, top, ROW_X1, top + n * rh, 14, SQ_GROUP);
    for (int i = 0; i < n; i++) {
        int t = top + i * rh, my = t + rh / 2;
        if (t + rh <= y0 || t >= y0 + h) continue;
        if (i) fill_rect(buf, y0, h, 32, t, ROW_X1, t + 1, SQ_SEP);
        draw_ftext(buf, y0, h, 32, my - font_label.line_h / 2, &font_label, lab[i], C_WHITE);
        int room = ROW_X1 - 16 - 200, vw = ftext_width(&font_label, val[i]);
        vw = vw < room ? vw : room;
        draw_fit(buf, y0, h, ROW_X1 - 16 - vw, my - font_label.line_h / 2, &font_label, val[i], room, SQ_TEXT2);
    }
}

// Battery: what the charger's doing (the square's power chip), then which board this is and what it runs
static void draw_battery(uint16_t *buf, int y0, int h)
{
    nav(buf, y0, h, &battery_buttons[0], "Battery");
    battery_diag_t d;
    battery_diag(&d);
    ftext_mid(buf, y0, h, CX, 106, &font_body, battery_state_text(&d), battery_charging_now(&d) ? CC_GREEN : C_WHITE);
    char head[16] = "";
    if (battery_percent() >= 0) snprintf(head, sizeof head, "%d%%", battery_percent());
    ftext_mid(buf, y0, h, CX, 136, &font_label, head, SQ_TEXT2);

    static const char *lab[6] = {"Battery", "Charging", "USB power", "Charge current", "Charges to", "Chip temperature"};
    char v[6][32];
    volts_text(v[0], sizeof v[0], d.vbat_mv);
    strlcpy(v[1], !d.chip ? "-" : !d.usb ? "Not plugged in" : !d.charge_on ? "Switched off" : battery_stage_text(d.stage),
            sizeof v[1]);
    if (d.usb && d.vbus_mv >= 0) volts_text(v[2], sizeof v[2], d.vbus_mv);
    else strlcpy(v[2], d.usb ? "Connected" : "Not connected", sizeof v[2]);
    if (d.charge_ma >= 0) snprintf(v[3], sizeof v[3], "%d mA", d.charge_ma); else strlcpy(v[3], "-", sizeof v[3]);
    volts_text(v[4], sizeof v[4], d.full_mv);
    if (d.chip) snprintf(v[5], sizeof v[5], "%d  C", (int)lroundf(d.chip_c)); else strlcpy(v[5], "-", sizeof v[5]);
    const int rh = 30, g1 = 158, g2 = g1 + 6 * rh + 14;
    rows(buf, y0, h, g1, rh, 6, lab, v);
    if (d.chip && g1 + 5 * rh + rh / 2 + 8 > y0 && g1 + 5 * rh < y0 + h) {   // (the font has no degree sign: a ring)
        int my = g1 + 5 * rh + rh / 2;
        float dx = ROW_X1 - 16 - ftext_width(&font_label, "C") - 4.5f;
        aa_disc(buf, y0, h, dx, my - 5, 2.6f, SQ_TEXT2);
        aa_disc(buf, y0, h, dx, my - 5, 1.2f, SQ_GROUP);
    }

    static const char *about[3] = {"Name", "Firmware", "Built"};
    char a[3][32];
    strlcpy(a[0], agent_device_name(), sizeof a[0]);
#if defined(BOBBLE_BUILD) && BOBBLE_BUILD
    snprintf(a[1], sizeof a[1], "Build %lld", (long long)BOBBLE_BUILD);
#else
    version_text(a[1], sizeof a[1]);
#endif
    strlcpy(a[2], ota_build_time(), sizeof a[2]);
    rows(buf, y0, h, g2, rh, 3, about, a);
}

static void draw_reset(uint16_t *buf, int y0, int h)
{
    nav(buf, y0, h, &reset_buttons[1], "Factory reset");
    ftext_mid(buf, y0, h, CX, 116, &font_label, "Erases Wi-Fi, pairing, calendars,", SQ_TEXT2);
    ftext_mid(buf, y0, h, CX, 142, &font_label, "Spotify and reminders", SQ_TEXT2);
    ftext_mid(buf, y0, h, CX, 168, &font_label, "for a new owner.", SQ_TEXT2);
    bool on = current_id() == ID_ERASE;
    aa_disc(buf, y0, h, CX, ERASE_Y, BUB_R, on ? rgb(200, 50, 44) : rgb(255, 69, 58));
    if (on) {
        int64_t held = esp_timer_get_time() - press_us;
        float frac = held >= LOCK_HOLD_US ? 1 : held / (float)LOCK_HOLD_US;
        cc_arc(buf, y0, h, CX, ERASE_Y, BUB_R + 8, 0, 360, 4, rgb(70, 70, 76));
        cc_arc(buf, y0, h, CX, ERASE_Y, BUB_R + 8, 0, 360 * frac, 4, rgb(255, 69, 58));
    }
    ftext_mid(buf, y0, h, CX, ERASE_Y, &font_body, "Hold", C_WHITE);
    ftext_mid(buf, y0, h, CX, 382, &font_small, "Hold for 3 seconds", SQ_TEXT2);
}

static void draw_keys(uint16_t *buf, int y0, int h)
{
    char line[64];
    snprintf(line, sizeof line, "Password for %s", join_ssid);
    ftext_mid(buf, y0, h, CX, 30, &font_label, line, C_WHITE);
    aa_rrect(buf, y0, h, 16, 58, W - 16, 104, 12, SQ_GROUP);
    if (error_msg[0]) aa_rrect(buf, y0, h, 16, 58, W - 16, 104, 12, rgb(70, 30, 32));
    int n = strlen(password);
    if (hide_pw) { memset(line, '*', n > 40 ? 40 : n); line[n > 40 ? 40 : n] = 0; }
    else strlcpy(line, password, sizeof line);
    while (line[0] && ftext_width(&font_label, line) > W - 64) memmove(line, line + 1, strlen(line));   // (the end shows)
    int tx = 30, ty = 81 - font_label.line_h / 2;
    draw_ftext(buf, y0, h, tx, ty, &font_label, line, C_WHITE);
    if ((esp_timer_get_time() / 500000) % 2 == 0) {
        int cx = tx + ftext_width(&font_label, line) + 1;
        aa_rrect(buf, y0, h, cx, ty + 2, cx + 2, ty + 22, 1, CC_BLUE);
    }
    if (error_msg[0]) ftext_mid(buf, y0, h, CX, 116, &font_small, error_msg, C_RED);
    for (int i = 0; i < key_count; i++) {
        button_t *k = &keys[i];
        if (k->y1 <= y0 || k->y0 >= y0 + h) continue;
        bool pressed = current_id() == k->id, letter = k->id >= ID_KEY, foot = k->id == ID_CANCEL || k->id == ID_SHOW;
        uint16_t bg = letter ? rgb(80, 80, 86) : rgb(50, 50, 55), fg = C_WHITE;
        if (k->id == ID_JOIN) bg = CC_BLUE;
        if (k->id == ID_SHIFT && caps) { bg = rgb(235, 235, 240); fg = rgb(20, 20, 24); }
        if (foot) bg = SQ_GROUP;
        if (pressed) bg = letter ? rgb(130, 130, 138) : rgb(90, 90, 98);
        aa_rrect(buf, y0, h, k->x0 + 2.5f, k->y0 + 3, k->x1 - 2.5f, k->y1 - 3, foot ? (k->y1 - k->y0 - 6) / 2.0f : 7, bg);
        const font_t *f = letter ? &font_label : &font_small;
        ftext_mid(buf, y0, h, (k->x0 + k->x1) / 2, (k->y0 + k->y1) / 2, f, k->label, foot ? CC_BLUE : fg);
    }
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

#if SCREEN_ROUND
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
#endif

void wifi_ui_draw(uint16_t *buf, int y0, int h)
{
    switch (view) {
    case V_MENU: draw_menu(buf, y0, h); break;
    case V_LIST: draw_list(buf, y0, h); break;
    case V_KEYS: draw_keys(buf, y0, h); break;
    case V_STATUS: draw_status(buf, y0, h); break;
    case V_PHONE: draw_phone(buf, y0, h); break;
    case V_RESET: draw_reset(buf, y0, h); break;
#if !SCREEN_ROUND
    case V_BATTERY: draw_battery(buf, y0, h); break;
#else
    case V_BATTERY: break;
#endif
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
    case ID_BATTERY: view = V_BATTERY; break;
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
        b = &menu_buttons[6];
        if (x >= b->x0 && x < b->x1 && y >= b->y0 - 4 && y < b->y1 + 4) return ID_VOLUME;
        if (hit(&menu_buttons[5], 1, x, y)) return ID_RESET;
        if (hit(&menu_buttons[7], 1, x, y)) return ID_BATTERY;
        int id = hit_pad(menu_buttons, 4, x, y, false);   // rows sit close together: a row you're on beats a neighbour's margin
        return id ? id : hit(menu_buttons, 4, x, y);
    }
    case V_LIST: {
        int id = list_hit(x, y);
        if (id) return id;
        id = hit(list_buttons, 3, x, y);
        if (id == ID_PHONE && !wifi_ap_active()) return SCREEN_ROUND ? ID_NONE : ID_SCAN;   // (square: Scan's whole row)
        return id;
    }
    case V_KEYS: {
        int id = hit_pad(keys, key_count - 2, x, y, false);           // letter keys
        return id ? id : hit(&keys[key_count - 2], 2, x, y);          // Cancel / Show below them
    }
    case V_STATUS:
        return wifi_state() == WS_CONNECTED ? hit(connected_buttons, 3, x, y) : hit(connecting_buttons, 1, x, y);
    case V_PHONE: return hit(phone_buttons, 1, x, y);
    case V_RESET: return hit(reset_buttons, 2, x, y);
#if !SCREEN_ROUND
    case V_BATTERY: return hit(battery_buttons, 1, x, y);
#else
    case V_BATTERY: return ID_NONE;
#endif
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
            else if (press_id != ID_BRIGHT && press_id != ID_VOLUME && hit_test(tx, ty) != press_id) press_id = ID_NONE;   // slid off the key
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
#if !SCREEN_ROUND                                    // (the square's slider stands up: its foot is the dimmest)
        int y = cur_y < b->y0 ? b->y0 : (cur_y > b->y1 ? b->y1 : cur_y);
        screen_brightness_set(10 + 245 * (b->y1 - y) / (b->y1 - b->y0), false);
#else
        int tx0 = b->x0 + 56, tx1 = b->x1 - 56, x = cur_x < tx0 ? tx0 : (cur_x > tx1 ? tx1 : cur_x);
        screen_brightness_set(10 + 245 * (x - tx0) / (tx1 - tx0), false);
#endif
    }
    if (down && press_id == ID_VOLUME) {               // and so does the volume (the square's)
        const button_t *b = &menu_buttons[6];
        int y = cur_y < b->y0 ? b->y0 : (cur_y > b->y1 ? b->y1 : cur_y);
        device_volume_set(100 * (b->y1 - y) / (b->y1 - b->y0), false);
    }
    if (!is_down && down && (press_id == ID_BRIGHT || press_id == ID_VOLUME)) {   // let go of a slider: remember the level
        down = false;
        if (press_id == ID_BRIGHT) screen_brightness_set(screen_brightness_get(), true);
        else { device_volume_set(device_volume_get(), true); audio_click(); }   // (a tick at the new level)
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
