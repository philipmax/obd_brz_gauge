#include <stdbool.h>
#include <string.h>
#include "th_util.h"
#include "esp_partition.h"
#include "app_obd_dsp/boot_media_mount.h"

static esp_partition_t partition = {
    .size = 0x4c0000, .type = ESP_PARTITION_TYPE_DATA,
    .subtype = ESP_PARTITION_SUBTYPE_DATA_SPIFFS, .label = "bootmedia"
};
static bool present;
static esp_err_t erase_result;
static unsigned erases;

const esp_partition_t *esp_partition_find_first(esp_partition_type_t type,
                                               esp_partition_subtype_t subtype,
                                               const char *label)
{
    TH_CHECK(type == ESP_PARTITION_TYPE_DATA && subtype == ESP_PARTITION_SUBTYPE_ANY);
    TH_CHECK(label && strcmp(label, "bootmedia") == 0);
    return present ? &partition : NULL;
}

esp_err_t esp_partition_erase_range(const esp_partition_t *part, size_t offset, size_t len)
{
    TH_CHECK(part == &partition && offset == 0 && len == BOOT_MEDIA_MANIFEST_MAX);
    erases++;
    return erase_result;
}

int main(void)
{
    TH_CHECK(!boot_media_invalidate_manifest() && erases == 0);
    present = true;
    partition.size = BOOT_MEDIA_BIN_OFFSET;
    TH_CHECK(!boot_media_invalidate_manifest() && erases == 0);
    partition.size = 0x4c0000;
    erase_result = ESP_FAIL;
    TH_CHECK(!boot_media_invalidate_manifest() && erases == 1);
    erase_result = ESP_OK;
    TH_CHECK(boot_media_invalidate_manifest() && erases == 2);
    return th_report("boot_media_invalidate");
}
