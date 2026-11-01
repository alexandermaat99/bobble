#pragma once
// The Tiles look (Settings > Look), like a phone's widgets: rounded tiles in flat bright colours on black, smaller
// ones nested in bigger ones, black words on the light tiles and big numbers. Its colours, shared by the pages.
#include "gfx.h"

#define T_SKY    rgb(112, 182, 252)        // light blue
#define T_BLUE   rgb(48, 106, 246)         // cobalt
#define T_NAVY   rgb(16, 30, 66)
#define T_ORANGE rgb(248, 150, 50)
#define T_RED    rgb(242, 76, 46)
#define T_YELLOW rgb(250, 222, 62)
#define T_MIST   rgb(184, 202, 220)        // grey-blue
#define T_PAPER  rgb(232, 232, 234)        // off-white
#define T_CASE   rgb(24, 24, 24)           // a dark tile (that others sit in)
#define T_CASE2  rgb(48, 48, 48)           // ...a lighter one, or one under the finger
#define T_INK    rgb(16, 16, 16)           // words on a light tile
#define T_INK2   rgb(64, 64, 64)           // ...their second line
#define T_GREY   rgb(144, 144, 144)        // a second line on a dark tile

// c lit up a little (a tile under the finger)
static inline uint16_t tile_hot(uint16_t c)
{
    uint16_t s = SWAP16(c);
    int r = (s >> 11) << 3, g = ((s >> 5) & 63) << 2, b = (s & 31) << 3;
    return rgb(r + (255 - r) * 2 / 5, g + (255 - g) * 2 / 5, b + (255 - b) * 2 / 5);
}
