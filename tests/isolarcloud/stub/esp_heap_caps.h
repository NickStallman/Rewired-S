// Host stub for the miniz-check target (device zcodec path on the host).
#pragma once
#include <cstdlib>
#define MALLOC_CAP_SPIRAM (1 << 10)
#define MALLOC_CAP_8BIT (1 << 2)
inline void *heap_caps_malloc(size_t n, unsigned) { return std::malloc(n); }
inline void heap_caps_free(void *p) { std::free(p); }
