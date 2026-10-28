#pragma once
#include <stddef.h>
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1u
#define MALLOC_CAP_8BIT 2u
#define MALLOC_CAP_INTERNAL 4u
static inline size_t heap_caps_get_free_size(unsigned caps) { (void)caps; return 16u * 1024u * 1024u; }
static inline size_t heap_caps_get_largest_free_block(unsigned caps) { (void)caps; return 16u * 1024u * 1024u; }
static inline void *heap_caps_malloc(size_t size, unsigned caps) { (void)caps; return malloc(size); }
