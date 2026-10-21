// The square board's pixel characters: a head and torso from their grids (sprites_data.h), arms and legs posed here,
// laid into a 16x21 grid of colours and drawn as blocks.
#include <math.h>
#include "gfx.h"
#include "sprites_ui.h"

#if !SCREEN_ROUND
typedef struct {
    const char *head[11], *torso[6];
    uint8_t pal[6][3];        // A B C D F G
    uint8_t eye[3];
    char sleeve, hand, pants, feet, blink;   // the colours (letters) of those parts; blink: a shut eye's (0: none)
} sprite_t;
#include "sprites_data.h"

static const uint8_t skin[3] = {240, 192, 150};

static const uint8_t *colour(const sprite_t *s, char c, bool blink)
{
    if (c == 'E' && blink && s->blink) c = s->blink;
    switch (c) {
    case 'S': return skin;
    case 'E': return s->eye;
    case 'A': return s->pal[0];
    case 'B': return s->pal[1];
    case 'C': return s->pal[2];
    case 'D': return s->pal[3];
    case 'F': return s->pal[4];
    default: return s->pal[5];   // G
    }
}

void sprite_draw(uint16_t *buf, int y0, int h, int who, const pose_t *p, float cx, float bottom, float px, float k)
{
    const sprite_t *s = &sprites[(who % SPRITE_N + SPRITE_N) % SPRITE_N];
    const uint8_t *g[SPRITE_H][SPRITE_W] = {{0}};     // each sprite pixel's colour (NULL: clear)
#define PUT(x, y, c) do { int x_ = (x), y_ = (y); if (x_ >= 0 && x_ < SPRITE_W && y_ >= 0 && y_ < SPRITE_H) g[y_][x_] = (c); } while (0)
    const uint8_t *pants = colour(s, s->pants, false), *feet = colour(s, s->feet, false);
    for (int side = 0; side < 2; side++) {            // legs, a gap between them; a lifted one is a pixel shorter
        int lx = side ? 9 : 5, fx = side ? 9 : 4;
        bool up = p->step == side + 1;
        for (int y = 17; y < (up ? 19 : 20); y++) { PUT(lx, y, pants); PUT(lx + 1, y, pants); }
        for (int x = fx; x < fx + 3; x++) PUT(x, up ? 19 : 20, feet);
    }
    for (int j = 0; j < 6; j++)
        for (int i = 0; i < 8; i++)
            if (s->torso[j][i] != '.') PUT(4 + i, 11 + j, colour(s, s->torso[j][i], p->blink));
    for (int j = 0; j < 11; j++)
        for (int i = 0; i < 12; i++)
            if (s->head[j][i] != '.') PUT(2 + i, j, colour(s, s->head[j][i], p->blink));
    const uint8_t *sl = colour(s, s->sleeve, false), *hd = colour(s, s->hand, false);
    for (int side = 0; side < 2; side++) {            // arms (the right one mirrored)
        int pose = side ? p->arm_r : p->arm_l;
#define X(x) (side ? SPRITE_W - 1 - (x) : (x))
        if (pose == ARM_UP) {                         // up beside the head
            PUT(X(2), 11, sl); PUT(X(3), 11, sl); PUT(X(2), 10, sl);
            for (int y = 7; y <= 10; y++) { PUT(X(0), y, sl); PUT(X(1), y, sl); }
            for (int y = 5; y <= 6; y++) { PUT(X(0), y, hd); PUT(X(1), y, hd); }
        } else if (pose == ARM_OUT) {                 // elbow out, hand up at the shoulder
            for (int y = 11; y <= 12; y++) { PUT(X(2), y, sl); PUT(X(3), y, sl); }
            PUT(X(0), 11, sl); PUT(X(1), 11, sl); PUT(X(1), 12, sl);
            for (int y = 9; y <= 10; y++) { PUT(X(0), y, hd); PUT(X(1), y, hd); }
        } else {                                      // down at its side
            for (int y = 11; y <= 13; y++) { PUT(X(2), y, sl); PUT(X(3), y, sl); }
            for (int y = 14; y <= 15; y++) { PUT(X(2), y, hd); PUT(X(3), y, hd); }
        }
#undef X
    }
#undef PUT
    float left = cx - SPRITE_W * px / 2, top = bottom - SPRITE_H * px;
    for (int j = 0; j < SPRITE_H; j++) {
        int ya = (int)lroundf(top + j * px), yb = (int)lroundf(top + (j + 1) * px);
        if (yb <= y0 || ya >= y0 + h) continue;
        for (int i = 0; i < SPRITE_W; i++) {
            const uint8_t *c = g[j][i];
            if (!c) continue;
            int xa = (int)lroundf(left + i * px), xb = (int)lroundf(left + (i + 1) * px);
            fill_rect(buf, y0, h, xa, ya, xb, yb, rgb((int)(c[0] * k), (int)(c[1] * k), (int)(c[2] * k)));
        }
    }
}
#endif
