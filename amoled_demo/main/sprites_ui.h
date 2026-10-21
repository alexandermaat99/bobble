#pragma once
#include <stdbool.h>
#include <stdint.h>

// The square board's pixel characters (one per chat: the knight, wizard, robot, ninja, viking and astronaut), 16x21
// sprite pixels each, posed in code: each arm down, up (cheering) or out (waving, typing); a step (one leg lifted); a
// blink. Grids in tools/make_sprites.py.
#define SPRITE_N 6
#define SPRITE_W 16
#define SPRITE_H 21
enum { ARM_DOWN, ARM_UP, ARM_OUT };
typedef struct { uint8_t arm_l, arm_r, step; bool blink; } pose_t;   // step: 0 standing, 1 left leg up, 2 right

// Character `who`, its feet's middle at (cx, bottom), px screen pixels to a sprite pixel, its colours scaled by k
// (0 .. 1: fading in, dimmed)
void sprite_draw(uint16_t *buf, int y0, int h, int who, const pose_t *p, float cx, float bottom, float px, float k);
