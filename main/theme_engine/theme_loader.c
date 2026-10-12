/*
 * theme_loader.c — runtime theme loader with hardening
 */

#include "theme_loader.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_crc.h"
#include "esp_heap_caps.h"
#include <string.h>

static const char *TAG = "theme_loader";

#define THEME_MAGIC 0x454D4854  /* 'THME' */
#define THEME_VERSION 1
#define THEME_MAX_SIZE (8 * 1024 * 1024)  /* 8MB limit */

static theme_context_t s_ctx = {0};

/* Minimal fallback theme (no assets, basic colors only) */
static void load_minimal_theme(void) {
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.loaded = true;
    ESP_LOGW(TAG, "Loaded minimal fallback theme");
}

esp_err_t theme_loader_init(void) {
    memset(&s_ctx, 0, sizeof(s_ctx));
    ESP_LOGI(TAG, "Theme loader initialized");
    return ESP_OK;
}

esp_err_t theme_loader_validate(const theme_header_t *header) {
    if (!header) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Check magic */
    if (header->magic != THEME_MAGIC) {
        ESP_LOGE(TAG, "Invalid magic: 0x%08X", header->magic);
        return ESP_ERR_INVALID_CRC;
    }

    /* Check version */
    if (header->version != THEME_VERSION) {
        ESP_LOGE(TAG, "Unsupported version: %u", header->version);
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Check size */
    if (header->total_size > THEME_MAX_SIZE) {
        ESP_LOGE(TAG, "Theme too large: %u bytes", header->total_size);
        return ESP_ERR_INVALID_SIZE;
    }

    /* Check asset count */
    if (header->n_assets > THEME_MAX_ASSETS) {
        ESP_LOGE(TAG, "Too many assets: %u", header->n_assets);
        return ESP_ERR_INVALID_SIZE;
    }

    /* Check rule count */
    if (header->n_rules > THEME_MAX_RULES) {
        ESP_LOGE(TAG, "Too many rules: %u", header->n_rules);
        return ESP_ERR_INVALID_SIZE;
    }

    /* Validate offsets */
    if (header->asset_table_offset >= header->total_size ||
        header->rule_table_offset >= header->total_size ||
        header->data_offset >= header->total_size) {
        ESP_LOGE(TAG, "Invalid offsets");
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

esp_err_t theme_loader_load(uint8_t slot) {
    /* Unload existing theme */
    theme_loader_unload();

    /* Find theme partition */
    char part_name[16];
    snprintf(part_name, sizeof(part_name), "theme_%u", slot);

    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                            ESP_PARTITION_SUBTYPE_ANY,
                                                            part_name);
    if (!part) {
        ESP_LOGE(TAG, "Partition %s not found", part_name);
        load_minimal_theme();
        return ESP_ERR_NOT_FOUND;
    }

    /* Read header */
    theme_header_t header;
    esp_err_t err = esp_partition_read(part, 0, &header, sizeof(header));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read header: %d", err);
        load_minimal_theme();
        return err;
    }

    /* Validate header */
    err = theme_loader_validate(&header);
    if (err != ESP_OK) {
        load_minimal_theme();
        return err;
    }

    /* Map partition to PSRAM (read-only) */
    const void *mapped_ptr;
    err = esp_partition_mmap(part, 0, header.total_size, ESP_PARTITION_MMAP_DATA,
                             &mapped_ptr, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mmap partition: %d", err);
        load_minimal_theme();
        return err;
    }

    /* Verify CRC */
    const uint8_t *data = (const uint8_t *)mapped_ptr;
    uint32_t calc_crc = esp_crc32_le(0, data + sizeof(theme_header_t),
                                      header.total_size - sizeof(theme_header_t));

    if (calc_crc != header.crc32) {
        ESP_LOGE(TAG, "CRC mismatch: got 0x%08X, expected 0x%08X", calc_crc, header.crc32);
        load_minimal_theme();
        return ESP_ERR_INVALID_CRC;
    }

    /* Allocate asset table */
    s_ctx.assets = heap_caps_malloc(header.n_assets * sizeof(theme_asset_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ctx.assets && header.n_assets > 0) {
        ESP_LOGE(TAG, "Failed to allocate asset table");
        load_minimal_theme();
        return ESP_ERR_NO_MEM;
    }

    /* Copy asset table */
    memcpy(s_ctx.assets, data + header.asset_table_offset,
           header.n_assets * sizeof(theme_asset_t));

    /* Allocate rule table */
    size_t rule_size = header.rule_table_offset - header.data_offset;
    if (rule_size > 0) {
        s_ctx.rules = heap_caps_malloc(rule_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_ctx.rules) {
            ESP_LOGE(TAG, "Failed to allocate rule table");
            heap_caps_free(s_ctx.assets);
            load_minimal_theme();
            return ESP_ERR_NO_MEM;
        }
        memcpy(s_ctx.rules, data + header.rule_table_offset, rule_size);
    }

    /* Store context */
    s_ctx.header = header;
    s_ctx.data_base = data;
    s_ctx.loaded = true;

    ESP_LOGI(TAG, "Theme loaded: %s (slot %u, %u assets, %u rules)",
             header.name, slot, header.n_assets, header.n_rules);

    return ESP_OK;
}

const theme_context_t *theme_loader_get_context(void) {
    return &s_ctx;
}

const theme_asset_t *theme_loader_get_asset(uint32_t asset_id) {
    if (!s_ctx.loaded || !s_ctx.assets) {
        return NULL;
    }

    for (uint16_t i = 0; i < s_ctx.header.n_assets; i++) {
        if (s_ctx.assets[i].id == asset_id) {
            return &s_ctx.assets[i];
        }
    }

    return NULL;
}

const void *theme_loader_get_asset_data(uint32_t asset_id) {
    const theme_asset_t *asset = theme_loader_get_asset(asset_id);
    if (!asset) {
        return NULL;
    }

    /* Bounds check */
    if (asset->offset + asset->size > s_ctx.header.total_size) {
        ESP_LOGE(TAG, "Asset %u out of bounds", asset_id);
        return NULL;
    }

    return s_ctx.data_base + asset->offset;
}

const lv_img_dsc_t *theme_loader_get_image(uint32_t asset_id) {
    const theme_asset_t *asset = theme_loader_get_asset(asset_id);
    if (!asset || asset->type != THEME_ASSET_IMAGE) {
        return NULL;
    }

    const void *data = theme_loader_get_asset_data(asset_id);
    if (!data) {
        return NULL;
    }

    /* Create LVGL image descriptor (static storage, reused) */
    static lv_img_dsc_t img_dsc;
    img_dsc.header.always_zero = 0;
    img_dsc.header.w = asset->width;
    img_dsc.header.h = asset->height;
    img_dsc.header.cf = asset->color_format;
    img_dsc.data_size = asset->size;
    img_dsc.data = data;

    return &img_dsc;
}

void theme_loader_unload(void) {
    if (s_ctx.assets) {
        heap_caps_free(s_ctx.assets);
        s_ctx.assets = NULL;
    }

    if (s_ctx.rules) {
        heap_caps_free(s_ctx.rules);
        s_ctx.rules = NULL;
    }

    /* Note: data_base is mmap'd, should be unmapped properly
     * but esp_partition_munmap requires the handle which we don't store.
     * In practice, theme stays mapped for device lifetime. */

    memset(&s_ctx, 0, sizeof(s_ctx));
}
