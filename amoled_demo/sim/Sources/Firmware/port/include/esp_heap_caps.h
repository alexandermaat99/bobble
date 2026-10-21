#pragma once
#include <stdlib.h>
#include <stddef.h>
#define MALLOC_CAP_DMA (1 << 3)
#define MALLOC_CAP_8BIT (1 << 2)
#define MALLOC_CAP_INTERNAL (1 << 11)
#define MALLOC_CAP_SPIRAM (1 << 10)
#define MALLOC_CAP_DEFAULT (1 << 12)
static inline void *heap_caps_malloc(size_t n, unsigned caps) { return malloc(n); }
static inline void *heap_caps_calloc(size_t n, size_t s, unsigned caps) { return calloc(n, s); }
static inline void *heap_caps_realloc(void *p, size_t n, unsigned caps) { return realloc(p, n); }
static inline void heap_caps_free(void *p) { free(p); }
static inline size_t heap_caps_get_free_size(unsigned caps) { return caps & MALLOC_CAP_SPIRAM ? 7 * 1024 * 1024 : 180 * 1024; }
static inline size_t heap_caps_get_largest_free_block(unsigned caps) { return heap_caps_get_free_size(caps); }
