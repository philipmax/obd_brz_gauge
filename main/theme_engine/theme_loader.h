#pragma once
/*
 * theme_loader.h — runtime theme loader with hardening
 *
 * Replaces compile-time theme generation with runtime loading from theme_0 partition.
 * Hardening:
 *   - CRC validation
 *   - Size limits
 *   - Asset bounds checking
 *   - Fallback to minimal safe theme on corruption
 */

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define THEME_MAX_ASSETS 128
#define THEME_MAX_RULES 64

/* Theme asset types */
typedef enum {
    THEME_ASSET_IMAGE = 0,
    THEME_ASSET_FONT,
    THEME_ASSET_STYLE,
} theme_asset_type_t;

/* Theme asset descriptor */
typedef struct {
    uint32_t id;                  /* Asset ID (from editor) */
    theme_asset_type_t type;
    uint32_t offset;              /* Offset in partition */
    uint32_t size;                /* Size in bytes */
    uint16_t width;               /* For images */
    uint16_t height;
    lv_img_cf_t color_format;     /* LV_IMG_CF_* */
} theme_asset_t;

/* Theme metadata header */
typedef struct {
    uint32_t magic;               /* 'THME' */
    uint32_t version;             /* Format version */
    uint32_t crc32;               /* CRC of entire theme data */
    uint32_t total_size;          /* Total theme size */
    uint16_t n_assets;
    uint16_t n_rules;
    uint32_t asset_table_offset;
    uint32_t rule_table_offset;
    uint32_t data_offset;
    char name[32];
    char author[32];
} theme_header_t;

/* Loaded theme context */
typedef struct {
    theme_header_t header;
    theme_asset_t *assets;
    uint8_t *rules;               /* Packed rule bytecode */
    const uint8_t *data_base;     /* PSRAM mapping of theme data */
    bool loaded;
} theme_context_t;

/**
 * Initialize theme loader.
 */
esp_err_t theme_loader_init(void);

/**
 * Load theme from partition slot (0-2).
 * Returns ESP_OK on success, falls back to minimal theme on error.
 */
esp_err_t theme_loader_load(uint8_t slot);

/**
 * Get loaded theme context.
 */
const theme_context_t *theme_loader_get_context(void);

/**
 * Get asset by ID.
 * Returns NULL if not found or invalid.
 */
const theme_asset_t *theme_loader_get_asset(uint32_t asset_id);

/**
 * Get asset data pointer (validated).
 * Returns NULL if asset invalid or out of bounds.
 */
const void *theme_loader_get_asset_data(uint32_t asset_id);

/**
 * Create LVGL image descriptor from asset.
 * Returns NULL on error.
 */
const lv_img_dsc_t *theme_loader_get_image(uint32_t asset_id);

/**
 * Validate theme integrity.
 * Called on load, returns ESP_OK if valid.
 */
esp_err_t theme_loader_validate(const theme_header_t *header);

/**
 * Unload current theme and free resources.
 */
void theme_loader_unload(void);

#ifdef __cplusplus
}
#endif
