#pragma once
#include <stdlib.h>
#include <string.h>
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
static inline void *heap_caps_malloc(size_t size, unsigned caps) { (void)caps; return malloc(size); }
size_t strlcpy(char *dst, const char *src, size_t cap);
