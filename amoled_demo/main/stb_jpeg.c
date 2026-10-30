// stb_image (public domain, stb_image.h v2.30 from github.com/nothings/stb) for the JPEGs esp_jpeg can't read: just
// JPEG, its memory in PSRAM, no stdio, no thread-locals, no failure strings
#include <stdint.h>
#include <stddef.h>
#include "esp_heap_caps.h"
#include "stb_jpeg.h"

#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_FAILURE_STRINGS
#define STBI_NO_THREAD_LOCALS
#define STBI_MAX_DIMENSIONS 2048
#define STBI_MALLOC(sz) heap_caps_malloc(sz, MALLOC_CAP_SPIRAM)
#define STBI_REALLOC(p, sz) heap_caps_realloc(p, sz, MALLOC_CAP_SPIRAM)
#define STBI_FREE(p) heap_caps_free(p)
#define STB_IMAGE_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wshift-negative-value"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#include "stb_image.h"
#pragma GCC diagnostic pop

uint16_t *stb_jpeg_rgb565(const uint8_t *jpg, int len, int *w, int *h)
{
    int n;
    if (!stbi_info_from_memory(jpg, len, w, h, &n) || (long)*w * *h > 800L * 800) return NULL;   // (a 640 px cover is
    uint8_t *rgb = stbi_load_from_memory(jpg, len, w, h, &n, 3);                                  // the most we'd fetch)
    if (!rgb) return NULL;
    uint16_t *out = heap_caps_malloc((size_t)*w * *h * 2, MALLOC_CAP_SPIRAM);
    if (out)
        for (long i = 0, k = (long)*w * *h; i < k; i++) {
            const uint8_t *p = rgb + 3 * i;
            uint16_t c = (uint16_t)((p[0] >> 3) << 11 | (p[1] >> 2) << 5 | p[2] >> 3);
            out[i] = (uint16_t)(c >> 8 | c << 8);
        }
    stbi_image_free(rgb);
    return out;
}
