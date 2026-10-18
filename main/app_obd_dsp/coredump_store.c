// coredump_store.c — read-only access to the `coredump` partition (ELF +
// CRC32) for GET /ota/coredump, plus erase and a one-line boot summary.
//
// "Present" means: the IDF size header is sane AND the image CRC matches.
// The CRC walk reads the whole image (<= 128 KB), so its result is cached
// per image size and dropped on erase.
#include "app_obd_dsp/coredump_store.h"

#include <stdio.h>
#include "esp_core_dump.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "sdkconfig.h"

static const char *TAG = "coredump";

static const esp_partition_t *s_part = NULL;
static volatile int8_t s_valid = -1;   // -1 unknown, 0 no/invalid dump, 1 valid
static volatile size_t s_size = 0;

static const esp_partition_t *get_partition(void)
{
    if (!s_part) {
        s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                          ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    }
    return s_part;
}

bool coredump_store_present(size_t *size)
{
    if (size) {
        *size = 0;
    }
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    if (!get_partition()) {
        return false;
    }
    if (s_valid == 0 && s_size == 0) {
        return false;   // known blank since the last check/erase
    }
    size_t addr = 0, sz = 0;
    if (esp_core_dump_image_get(&addr, &sz) != ESP_OK) {
        s_valid = 0;
        s_size = 0;
        return false;
    }
    if (s_valid < 0 || sz != s_size) {
        s_size = sz;
        s_valid = (esp_core_dump_image_check() == ESP_OK) ? 1 : 0;
    }
    if (s_valid != 1) {
        return false;
    }
    if (size) {
        *size = sz;
    }
    return true;
#else
    // Without the flash backend this firmware never writes a dump, and the
    // partition may hold anything (old bootmedia tail before migration).
    return false;
#endif
}

esp_err_t coredump_store_read(uint32_t off, void *buf, size_t len)
{
    if (!buf && len) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t size = 0;
    if (!coredump_store_present(&size)) {
        return ESP_ERR_NOT_FOUND;
    }
    if ((uint64_t)off + len > size) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (len == 0) {
        return ESP_OK;
    }
    return esp_partition_read(get_partition(), off, buf, len);
}

esp_err_t coredump_store_erase(void)
{
    if (!get_partition()) {
        ESP_LOGE(TAG, "coredump partition not found");
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = esp_core_dump_image_erase();
    s_valid = (err == ESP_OK) ? 0 : -1;
    s_size = 0;
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "core dump erased");
    } else {
        ESP_LOGE(TAG, "erase failed: %s", esp_err_to_name(err));
    }
    return err;
}

void coredump_store_boot_check(void)
{
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    s_valid = -1;
    size_t size = 0;
    if (!coredump_store_present(&size)) {
        ESP_LOGI(TAG, "no core dump stored");
        return;
    }
    ESP_LOGW(TAG, "core dump stored: %u bytes (GET /ota/coredump)", (unsigned)size);
#if CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
    esp_core_dump_summary_t *sum = heap_caps_malloc(sizeof(*sum), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!sum) {
        sum = heap_caps_malloc(sizeof(*sum), MALLOC_CAP_8BIT);
    }
    if (!sum) {
        return;
    }
    if (esp_core_dump_get_summary(sum) == ESP_OK) {
        char bt[16 * 11 + 1];
        size_t n = 0;
        bt[0] = '\0';
        uint32_t depth = sum->exc_bt_info.depth < 16 ? sum->exc_bt_info.depth : 16;
        for (uint32_t i = 0; i < depth && n + 12 < sizeof(bt); i++) {
            n += (size_t)snprintf(bt + n, sizeof(bt) - n, " 0x%08lx", (unsigned long)sum->exc_bt_info.bt[i]);
        }
        ESP_LOGW(TAG, "crash: task '%.16s' pc 0x%08lx cause %lu vaddr 0x%08lx%s",
                 sum->exc_task, (unsigned long)sum->exc_pc,
                 (unsigned long)sum->ex_info.exc_cause, (unsigned long)sum->ex_info.exc_vaddr,
                 sum->exc_bt_info.corrupted ? " (backtrace corrupted)" : "");
        ESP_LOGW(TAG, "backtrace:%s", bt);
        char reason[96];
        if (esp_core_dump_get_panic_reason(reason, sizeof(reason)) == ESP_OK) {
            ESP_LOGW(TAG, "panic reason: %s", reason);
        }
    } else {
        ESP_LOGW(TAG, "core dump summary unavailable");
    }
    heap_caps_free(sum);
#endif
#else
    ESP_LOGI(TAG, "core dump to flash disabled (sdkconfig)");
#endif
}
