#include <stdio.h>
#include "gfx.h"
#include "battery.h"
#include "statusbar.h"

void draw_status_bar(uint16_t *buf, int y0, int h, int top)
{
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
}
