/*
 * theme_img_cache.c — PSRAM LRU image cache
 */

#include "theme_img_cache.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "img_cache";

#define MAX_CACHE_ENTRIES 128

typedef struct cache_entry {
    uint32_t ledger_crc;
    uint32_t asset_id;
    cache_variant_t variant;
    uint32_t variant_param;

    lv_img_dsc_t dsc;
    void *data;
    uint32_t data_size;

    uint32_t access_count;
    uint64_t last_access_time;

    struct cache_entry *next;
    struct cache_entry *prev;
} cache_entry_t;

static cache_entry_t *s_entries = NULL;
static cache_entry_t *s_lru_head = NULL;
static cache_entry_t *s_lru_tail = NULL;
static uint32_t s_entry_count = 0;
static uint32_t s_bytes_used = 0;
static uint32_t s_bytes_budget = CACHE_BUDGET_NO_VIDEO;
static cache_stats_t s_stats = {0};
static SemaphoreHandle_t s_mutex = NULL;

/* LRU list management */
static void lru_remove(cache_entry_t *entry) {
    if (entry->prev) entry->prev->next = entry->next;
    if (entry->next) entry->next->prev = entry->prev;
    if (s_lru_head == entry) s_lru_head = entry->next;
    if (s_lru_tail == entry) s_lru_tail = entry->prev;
    entry->prev = NULL;
    entry->next = NULL;
}

static void lru_push_front(cache_entry_t *entry) {
    entry->next = s_lru_head;
    entry->prev = NULL;
    if (s_lru_head) s_lru_head->prev = entry;
    s_lru_head = entry;
    if (!s_lru_tail) s_lru_tail = entry;
}

static void lru_touch(cache_entry_t *entry) {
    lru_remove(entry);
    lru_push_front(entry);
    entry->access_count++;
    entry->last_access_time = esp_timer_get_time();
}

static void free_entry(cache_entry_t *entry) {
    if (entry->data) {
        heap_caps_free(entry->data);
        s_bytes_used -= entry->data_size;
    }
    lru_remove(entry);
    s_entry_count--;
}

static cache_entry_t *find_entry(uint32_t ledger_crc, uint32_t asset_id,
                                  cache_variant_t variant, uint32_t variant_param) {
    cache_entry_t *entry = s_lru_head;
    while (entry) {
        if (entry->ledger_crc == ledger_crc &&
            entry->asset_id == asset_id &&
            entry->variant == variant &&
            entry->variant_param == variant_param) {
            return entry;
        }
        entry = entry->next;
    }
    return NULL;
}

static void evict_lru(void) {
    if (!s_lru_tail) {
        return;
    }

    cache_entry_t *victim = s_lru_tail;
    ESP_LOGD(TAG, "Evicting asset %u (ledger %08X)", victim->asset_id, victim->ledger_crc);

    free_entry(victim);
    s_stats.evictions++;
}

esp_err_t theme_img_cache_init(void) {
    if (s_mutex) {
        return ESP_OK; /* Already initialized */
    }

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        return ESP_ERR_NO_MEM;
    }

    /* Allocate entry pool */
    s_entries = heap_caps_calloc(MAX_CACHE_ENTRIES, sizeof(cache_entry_t),
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_entries) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_lru_head = NULL;
    s_lru_tail = NULL;
    s_entry_count = 0;
    s_bytes_used = 0;
    memset(&s_stats, 0, sizeof(s_stats));

    ESP_LOGI(TAG, "Image cache initialized (budget: %u KB)", s_bytes_budget / 1024);
    return ESP_OK;
}

void theme_img_cache_set_budget(uint32_t bytes) {
    if (!s_mutex) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_bytes_budget = bytes;

    /* Evict until under budget */
    while (s_bytes_used > s_bytes_budget && s_lru_tail) {
        evict_lru();
    }

    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Cache budget set to %u KB", bytes / 1024);
}

const lv_img_dsc_t *theme_img_cache_get(uint32_t ledger_crc, uint32_t asset_id,
                                          cache_variant_t variant, uint32_t variant_param) {
    if (!s_mutex) return NULL;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    cache_entry_t *entry = find_entry(ledger_crc, asset_id, variant, variant_param);

    if (entry) {
        lru_touch(entry);
        s_stats.hits++;
        xSemaphoreGive(s_mutex);
        return &entry->dsc;
    }

    s_stats.misses++;
    xSemaphoreGive(s_mutex);
    return NULL;
}

esp_err_t theme_img_cache_put(uint32_t ledger_crc, uint32_t asset_id,
                                cache_variant_t variant, uint32_t variant_param,
                                const lv_img_dsc_t *dsc, void *data, uint32_t data_size) {
    if (!s_mutex || !dsc || !data) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    /* Check if already cached */
    if (find_entry(ledger_crc, asset_id, variant, variant_param)) {
        xSemaphoreGive(s_mutex);
        heap_caps_free(data); /* Caller expects us to take ownership */
        return ESP_OK;
    }

    /* Evict until we have space */
    while (s_bytes_used + data_size > s_bytes_budget && s_lru_tail) {
        evict_lru();
    }

    /* Still no space after eviction */
    if (s_bytes_used + data_size > s_bytes_budget) {
        xSemaphoreGive(s_mutex);
        heap_caps_free(data);
        return ESP_ERR_NO_MEM;
    }

    /* Find free entry slot */
    cache_entry_t *entry = NULL;
    for (int i = 0; i < MAX_CACHE_ENTRIES; i++) {
        if (s_entries[i].data == NULL) {
            entry = &s_entries[i];
            break;
        }
    }

    if (!entry) {
        /* All slots used, evict one more */
        evict_lru();
        entry = &s_entries[s_entry_count]; /* Use newly freed slot */
    }

    /* Fill entry */
    entry->ledger_crc = ledger_crc;
    entry->asset_id = asset_id;
    entry->variant = variant;
    entry->variant_param = variant_param;
    entry->dsc = *dsc;
    entry->dsc.data = data;
    entry->data = data;
    entry->data_size = data_size;
    entry->access_count = 1;
    entry->last_access_time = esp_timer_get_time();

    lru_push_front(entry);
    s_entry_count++;
    s_bytes_used += data_size;
    s_stats.entries = s_entry_count;
    s_stats.bytes_used = s_bytes_used;

    xSemaphoreGive(s_mutex);

    ESP_LOGD(TAG, "Cached asset %u variant %d (%u bytes, total %u/%u KB)",
             asset_id, variant, data_size, s_bytes_used / 1024, s_bytes_budget / 1024);

    return ESP_OK;
}

void theme_img_cache_clear_ledger(uint32_t ledger_crc) {
    if (!s_mutex) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    cache_entry_t *entry = s_lru_head;
    while (entry) {
        cache_entry_t *next = entry->next;
        if (entry->ledger_crc == ledger_crc) {
            free_entry(entry);
        }
        entry = next;
    }

    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Cleared ledger %08X", ledger_crc);
}

void theme_img_cache_clear_all(void) {
    if (!s_mutex) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    cache_entry_t *entry = s_lru_head;
    while (entry) {
        cache_entry_t *next = entry->next;
        free_entry(entry);
        entry = next;
    }

    s_lru_head = NULL;
    s_lru_tail = NULL;
    s_entry_count = 0;
    s_bytes_used = 0;

    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Cache cleared");
}

void theme_img_cache_prewarm(int page_offset) {
    /* TODO: implement prewarming for adjacent pages */
    /* Load assets for page at current_page + page_offset in background */
}

void theme_img_cache_get_stats(cache_stats_t *stats) {
    if (!s_mutex || !stats) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_stats.entries = s_entry_count;
    s_stats.bytes_used = s_bytes_used;
    s_stats.bytes_budget = s_bytes_budget;
    *stats = s_stats;
    xSemaphoreGive(s_mutex);
}
