#include <math.h>
#include "boot_ui.h"
#include "gfx.h"

#define N_SHAPES 5
#define R 54.0f                           // the shapes' size (their outlines are about 0.75..1.2 of it)
#define HOLD 0.25f                        // each shape holds this long (s), then morphs into the next...
#define MORPH 0.6f                        // ...over this long, turning a quarter turn extra as it does
#define TURN (2 * (float)M_PI / 3)        // and the whole thing turns once every 3 s

// the outlines, as a radius for each direction (1: a circle)
static float shape(int k, float th)
{
    switch (k) {
    case 0: return 1 + 0.07f * cosf(9 * th);                           // cookie: nine soft scallops
    case 1: return 1 + 0.1f * cosf(5 * th) + 0.02f * cosf(10 * th);  // pentagon, well rounded
    case 2: {                                                          // pill (an oval)
        float c = cosf(th) / 1.2f, d = sinf(th) / 0.74f;
        return 1 / sqrtf(c * c + d * d);
    }
    case 3: { float g = 0.5f + 0.5f * cosf(8 * th); return 0.93f + 0.17f * g * g; }   // burst: eight soft points
    default: return 1 + 0.15f * cosf(4 * th);                          // clover: four leaves
    }
}

static const uint8_t cols[4][3] = {{66, 133, 244}, {234, 67, 53}, {251, 188, 4}, {52, 168, 83}};   // blue red yellow green

static float ease(float p)                // ease out with a little overshoot (as a spring would)
{
    const float c = 1.4f;
    p -= 1;
    return 1 + (c + 1) * p * p * p + c * p * p;
}

static uint16_t mix(uint16_t dst, int r, int g, int b, float a)   // (panel byte order, as rgb() makes)
{
    uint16_t c = SWAP16(dst);
    int dr = (c >> 8) & 0xF8, dg = (c >> 3) & 0xFC, db = (c << 3) & 0xF8;
    return rgb(dr + (int)((r - dr) * a), dg + (int)((g - dg) * a), db + (int)((b - db) * a));
}

void boot_ui_draw(uint16_t *buf, int y0, int h, float t, float a, const char *what)
{
    if (a <= 0) return;
    float step = HOLD + MORPH, ft = t / step;
    int i = (int)ft;
    float p = (ft - i) * step - HOLD;
    p = p < 0 ? 0 : p / MORPH;
    float e = ease(p), ec = e < 0 ? 0 : e > 1 ? 1 : e;
    int from = i % N_SHAPES, to = (i + 1) % N_SHAPES;
    const uint8_t *c0 = cols[i % 4], *c1 = cols[(i + 1) % 4];
    int r = c0[0] + (int)((c1[0] - c0[0]) * ec), g = c0[1] + (int)((c1[1] - c0[1]) * ec), b = c0[2] + (int)((c1[2] - c0[2]) * ec);
    float rot = t * TURN + (i + e) * (float)M_PI / 2;
    float size = R * (0.85f + 0.15f * a);                // (fading out: it shrinks a little too)

    float outer = size * 1.3f, inner = size * 0.7f;      // every shape is round within 0.7: no need to look closer there
    int ya = (int)(CY - outer), yb = (int)(CY + outer) + 1;
    if (ya < y0) ya = y0;
    if (yb > y0 + h) yb = y0 + h;
    for (int y = ya; y < yb; y++) {
        uint16_t *row = buf + (y - y0) * W;
        float dy = y + 0.5f - CY;
        for (int x = (int)(CX - outer); x <= (int)(CX + outer); x++) {
            float dx = x + 0.5f - CX, d2 = dx * dx + dy * dy;
            if (d2 > outer * outer) continue;
            float cov = 1;
            if (d2 > inner * inner) {
                float rr = sqrtf(d2), th = atan2f(dy, dx) - rot;
                float edge = size * (shape(from, th) + (shape(to, th) - shape(from, th)) * e);
                cov = edge - rr + 0.5f;
                if (cov <= 0) continue;
                if (cov > 1) cov = 1;
            }
            row[x] = mix(row[x], r, g, b, cov * a);
        }
    }

    if (what && a > 0.05f) {
        int v = (int)(150 * a), tw = ftext_width(&font_label, what);
        draw_ftext(buf, y0, h, CX - tw / 2, CY + 104, &font_label, what, rgb(v, v, v + 10));
    }
}
