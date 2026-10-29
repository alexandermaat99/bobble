#include <stdio.h>
#include <math.h>
#include <time.h>
#include "gfx.h"
#include "battery.h"
#include "statusbar.h"
#include "wifi_setup.h"

#if !SCREEN_ROUND
#define SB_TOP 14                     // the square's corners: this far from the top, and from the sides
#define SB_SIDE 24
#define SB_MID (SB_TOP + 12)          // the row the time, Wi-Fi and battery centre on
// Wi-Fi: a fan of a wedge and two bands (45 degrees either side of up), anti-aliased; as many lit as the signal is
// strong (the rest dim), all dim while it's not connected. x, y: the fan's point.
static void draw_wifi(uint16_t *buf, int y0, int h, int x, int y, int bars, uint16_t on, uint16_t off)
{
    static const float r0[3] = {0, 6.5f, 11.5f}, r1[3] = {4.2f, 9.3f, 14.3f};
    for (int py = y - 16; py <= y + 1; py++) {
        if (py < y0 || py >= y0 + h) continue;
        for (int px = x - 13; px <= x + 13; px++) {
            if (px < 0 || px >= W) continue;
            float dx = px + 0.5f - x, dy = py + 0.5f - y, d = sqrtf(dx * dx + dy * dy);
            float wedge = (-dy - fabsf(dx)) * 0.7071f + 0.5f;                   // distance inside the 90 degree wedge
            for (int k = 0; k < 3; k++) {
                float a = fminf(fminf(d - r0[k], r1[k] - d) + 0.5f, wedge);
                if (!k) a = fminf(r1[0] - d + 0.5f, wedge + 1.2f);              // (the point, a little rounder)
                if (a <= 0) continue;
                if (a > 1) a = 1;
                uint16_t c = k < bars ? on : off, sc = SWAP16(c);             // (back to rgb to fade it)
                int r = (sc >> 11) << 3, g = ((sc >> 5) & 63) << 2, b = (sc & 31) << 3;
                buf[(py - y0) * W + px] = rgb((int)(r * a), (int)(g * a), (int)(b * a));   // (on black)
            }
        }
    }
}

static void draw_status_clock(uint16_t *buf, int y0, int h)
{
    if (SB_TOP + 24 <= y0 || SB_TOP >= y0 + h) return;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_year < 120) return;                     // (no time yet)
    char t[8];
    snprintf(t, sizeof t, "%d:%02d", tm.tm_hour % 12 ? tm.tm_hour % 12 : 12, tm.tm_min);
    draw_ftext(buf, y0, h, SB_SIDE + 8, SB_MID - font_label.line_h / 2 - 1, &font_label, t, rgb(255, 255, 255));
}
#endif

void draw_status_bar(uint16_t *buf, int y0, int h, int top)
{
#if !SCREEN_ROUND
    (void)top;
    if (SB_TOP + 24 <= y0 || SB_TOP >= y0 + h) return;
    int pct = battery_percent(), rssi = wifi_rssi(), bars = wifi_state() != WS_CONNECTED ? 0 : rssi > -60 ? 3 : rssi > -72 ? 2 : 1;
    uint16_t white = rgb(255, 255, 255);
    float bx1 = W - SB_SIDE, bx0 = bx1 - 27, my = SB_MID;      // the cell: 25 x 13 with a nub on the right
    if (pct >= 0) {
        bool chg = battery_charging();
        uint16_t fill = chg ? rgb(60, 220, 110) : pct <= 15 ? rgb(255, 80, 80) : white;
        aa_rrect(buf, y0, h, bx0, my - 6.5f, bx0 + 25, my + 6.5f, 4, rgb(120, 120, 128));
        aa_rrect(buf, y0, h, bx0 + 1.3f, my - 5.2f, bx0 + 23.7f, my + 5.2f, 2.8f, 0);
        float lvl = 21.0f * (pct > 100 ? 100 : pct < 4 ? 4 : pct) / 100;
        aa_rrect(buf, y0, h, bx0 + 2.5f, my - 4, bx0 + 2.5f + lvl, my + 4, 1.8f, fill);
        aa_rrect(buf, y0, h, bx0 + 26, my - 2.5f, bx1, my + 2.5f, 1, rgb(120, 120, 128));
        if (chg) {                                            // plugged in: a solid bolt over the middle, outlined in black
            float cx = bx0 + 12.5f;                           // top, left, inner left, bottom, right, inner right
            const float b[12] = {cx + 2, my - 7, cx - 4, my + 1.2f, cx - 0.4f, my + 1.2f,
                                 cx - 2, my + 7, cx + 4, my - 1.2f, cx + 0.4f, my - 1.2f};
            aa_path(buf, y0, h, b, 6, true, 3, 0);
            const int t[4][3] = {{0, 1, 2}, {0, 2, 5}, {2, 3, 4}, {2, 4, 5}};
            for (int i = 0; i < 4; i++)
                fill_tri(buf, y0, h, lroundf(b[2 * t[i][0]]), lroundf(b[2 * t[i][0] + 1]), lroundf(b[2 * t[i][1]]),
                         lroundf(b[2 * t[i][1] + 1]), lroundf(b[2 * t[i][2]]), lroundf(b[2 * t[i][2] + 1]), white);
            aa_path(buf, y0, h, b, 6, true, 1, white);
        }
    } else {
        bx0 = bx1 + 8;                                        // (no battery: the Wi-Fi takes the corner)
    }
    draw_wifi(buf, y0, h, (int)bx0 - 22, (int)my + 7, bars, white, rgb(70, 70, 80));
    draw_status_clock(buf, y0, h);
#else
    static const uint16_t bolt[14] = {   // 9 px wide, bit 8 = left column
        0x030, 0x030, 0x060, 0x060, 0x0C0, 0x1FE, 0x1FE, 0x00C, 0x018, 0x018, 0x030, 0x030, 0x060, 0x040,
    };
    if (top + 16 <= y0 || top >= y0 + h) return;
    int pct = battery_percent();
    if (pct < 0) return;
    bool chg = battery_charging();
    uint16_t c = pct <= 15 && !chg ? rgb(255, 95, 95) : rgb(60, 230, 120);
    char p[8];
    snprintf(p, sizeof p, "%d%%", pct > 100 ? 100 : pct);
    int tw = text_width(p, 2), bw = chg ? 13 : 0, x = CX - (bw + 31 + 6 + tw) / 2, by = top;
    if (chg)
        for (int r = 0; r < 14; r++)
            for (int k = 0; k < 9; k++)
                if (bolt[r] & (0x100 >> k)) fill_rect(buf, y0, h, x + k, by + 1 + r, x + k + 1, by + 2 + r, c);
    x += bw;
    fill_rrect(buf, y0, h, x, by + 1, x + 28, by + 15, 3, c);            // outline
    fill_rrect(buf, y0, h, x + 2, by + 3, x + 26, by + 13, 2, 0);
    fill_rect(buf, y0, h, x + 28, by + 5, x + 31, by + 11, c);           // terminal nub
    fill_rect(buf, y0, h, x + 4, by + 5, x + 4 + 20 * pct / 100, by + 11, c);
    draw_text(buf, y0, h, x + 37, by + 1, 2, p, c);
#endif
}
