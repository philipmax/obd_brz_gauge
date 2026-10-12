#pragma once
/*
 * theme_img_cache.h — PSRAM LRU image cache
 *
 * Caches converted theme assets in PSRAM to avoid repeated conversion.
 * Key: (ledger_crc, asset_id, variant)
 * Budget: 2.5MB when no video, 1.5MB when video active
 */

#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CACHE_BUDGET_NO_VIDEO  (2500 * 1024)  /* 2.5MB */
#define CACHE_BUDGET_WITH_VIDEO (1500 * 1024)  /* 1.5MB */

/* Cache variant type */
typedef enum {
    CACHE_VARIANT_ORIGINAL = 0,
    CACHE_VARIANT_SCALED,
    CACHE_VARIANT_ROTATED,
    CACHE_VARIANT_RECOLORED,
} cache_variant_t;

/**
 * Initialize image cache.
 */
esp_err_t theme_img_cache_init(void);

/**
 * Set budget (call when entering/exiting video mode).
 */
void theme_img_cache_set_budget(uint32_t bytes);

/**
 * Get cached image.
 * Returns NULL if not in cache.
 */
const lv_img_dsc_t *theme_img_cache_get(uint32_t ledger_crc, uint32_t asset_id,
                                          cache_variant_t variant, uint32_t variant_param);

/**
 * Put image in cache.
 * Takes ownership of data (must be heap-allocated).
 * Returns ESP_OK on success, ESP_ERR_NO_MEM if cache full.
 */
esp_err_t theme_img_cache_put(uint32_t ledger_crc, uint32_t asset_id,
                                cache_variant_t variant, uint32_t variant_param,
                                const lv_img_dsc_t *dsc, void *data, uint32_t data_size);

/**
 * Clear all cached assets for a ledger (call when switching themes).
 */
void theme_img_cache_clear_ledger(uint32_t ledger_crc);

/**
 * Clear entire cache (call before OTA, theme erase).
 */
void theme_img_cache_clear_all(void);

/**
 * Pre-warm cache for adjacent pages.
 * Called in background when page changes.
 */
void theme_img_cache_prewarm(int page_offset);

/**
 * Get cache statistics.
 */
typedef struct {
    uint32_t entries;
    uint32_t bytes_used;
    uint32_t bytes_budget;
    uint32_t hits;
    uint32_t misses;
    uint32_t evictions;
} cache_stats_t;

void theme_img_cache_get_stats(cache_stats_t *stats);

#ifdef __cplusplus
}
#endif
