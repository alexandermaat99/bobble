#pragma once
#include <stdint.h>

// A JPEG the board's own decoder (esp_jpeg) can't read, progressive ones above all (Spotify serves some covers that
// way), decoded with stb_image instead: RGB565 with its bytes swapped, as esp_jpeg gives them, in PSRAM (free with
// heap_caps_free). NULL if it can't either.
uint16_t *stb_jpeg_rgb565(const uint8_t *jpg, int len, int *w, int *h);
