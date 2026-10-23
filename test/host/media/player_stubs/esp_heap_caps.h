#pragma once
#include <stddef.h>
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_SPIRAM 4
#define MALLOC_CAP_DMA 8
void *heap_caps_malloc(size_t n, unsigned caps);
void *heap_caps_calloc(size_t n, size_t size, unsigned caps);
void *heap_caps_aligned_alloc(size_t alignment, size_t n, unsigned caps);
void heap_caps_free(void *ptr);
size_t heap_caps_get_free_size(unsigned caps);
