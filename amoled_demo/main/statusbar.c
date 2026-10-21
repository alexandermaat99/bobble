#include <stdio.h>
#include <math.h>
#include "gfx.h"
#include "battery.h"
#include "statusbar.h"
#include "wifi_setup.h"

#if !SCREEN_ROUND
#define SB_TOP 14                     // the square's corner: this far from the top and right edges
#define SB_RIGHT 14
// Wi-Fi: a dot and two arcs fanning up from it (45 degrees either side), anti-aliased; as many lit as the signal is
// strong (the rest dim), all dim while it's not connected. x, y: the dot's centre.
static void draw_wifi(uint16_t *buf, int y0, int h, int x, int y, int bars, uint16_t on, uint16_t off)
{
    static const float rad[3] = {0, 7.5f, 13.5f};
    for (int py = y - 16; py <= y + 3; py++) {
        if (py < y0 || py >= y0 + h) continue;
        for (int px = x - 13; px <= x + 13; px++) {
            if (px < 0 || px >= W) continue;
            float dx = px + 0.5f - x, dy = py + 0.5f - y, d = sqrtf(dx * dx + dy * dy);
            for (int k = 0; k < 3; k++) {
                float a;
                if (!k) a = 2.6f - d + 0.5f;                                  // the dot
                else {
                    if (dy > 0 || fabsf(dx) > -dy + 0.7f) continue;           // (just the wedge above)
                    a = 1.6f - fabsf(d - rad[k]) + 0.5f;                      // the arc's band
                    a = fminf(a, -dy - fabsf(dx) + 1.2f);                     // (soft ends)
                }
                if (a <= 0) continue;
                if (a > 1) a = 1;
                uint16_t c = k < bars ? on : off, sc = SWAP16(c);             // (back to rgb to fade it)
                int r = (sc >> 11) << 3, g = ((sc >> 5) & 63) << 2, b = (sc & 31) << 3;
                buf[(py - y0) * W + px] = rgb((int)(r * a), (int)(g * a), (int)(b * a));   // (on black)
            }
        }
    }
}
#endif

void draw_status_bar(uint16_t *buf, int y0, int h, int top)
{
    static const uint16_t bolt[14] = {   // 9 px wide, bit 8 = left column
        0x030, 0x030, 0x060, 0x060, 0x0C0, 0x1FE, 0x1FE, 0x00C, 0x018, 0x018, 0x030, 0x030, 0x060, 0x040,
    };
#if !SCREEN_ROUND
    top = SB_TOP;
#endif
    if (top + 16 <= y0 || top >= y0 + h) return;
    int pct = battery_percent();
#if !SCREEN_ROUND
    int rssi = wifi_rssi(), bars = wifi_state() != WS_CONNECTED ? 0 : rssi > -60 ? 3 : rssi > -72 ? 2 : 1;
    char p0[8] = "";
    if (pct >= 0) snprintf(p0, sizeof p0, "%d%%", pct > 100 ? 100 : pct);
    int bat_w = pct >= 0 ? (battery_charging() ? 13 : 0) + 31 + 6 + text_width(p0, 2) : 0;
    draw_wifi(buf, y0, h, W - SB_RIGHT - bat_w - (bat_w ? 14 : 0) - 13, top + 14, bars, rgb(225, 225, 232), rgb(70, 70, 80));
#endif
    if (pct < 0) return;
    bool chg = battery_charging();
    uint16_t c = pct <= 15 && !chg ? rgb(255, 95, 95) : rgb(60, 230, 120);
    char p[8];
    snprintf(p, sizeof p, "%d%%", pct > 100 ? 100 : pct);
    int tw = text_width(p, 2), bw = chg ? 13 : 0, x = CX - (bw + 31 + 6 + tw) / 2, by = top;
#if !SCREEN_ROUND
    x = W - SB_RIGHT - (bw + 31 + 6 + tw);           // (right-aligned in the corner)
#endif
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
}
