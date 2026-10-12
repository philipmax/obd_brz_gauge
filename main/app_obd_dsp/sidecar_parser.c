/*
 * Sidecar JSON Parser (.gaugeproj.json)
 * Parses theme rules and element metadata from editor export
 */

#include "sidecar_parser.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "sidecar";

#define MAX_RULES 16
#define MAX_CONDITIONS 4
#define MAX_ACTIONS 8
#define MAX_ELEMENTS 32

typedef struct {
    char element_id[32];
    char element_kind[16];
    int page_index;
} element_info_t;

typedef struct {
    char theme_id[64];
    int element_count;
    element_info_t elements[MAX_ELEMENTS];
    // Rules stored separately in /config/themes/<id>/rules.json
} sidecar_data_t;

static sidecar_data_t s_sidecar = {0};

esp_err_t sidecar_parse_file(const char *file_path) {
    FILE *f = fopen(file_path, "r");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s", file_path);
        return ESP_ERR_NOT_FOUND;
    }

    // Read file into buffer
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0 || fsize > 512 * 1024) {  // Max 512KB
        ESP_LOGE(TAG, "Invalid file size: %ld", fsize);
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }

    char *buffer = malloc(fsize + 1);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate buffer");
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    size_t read_size = fread(buffer, 1, fsize, f);
    fclose(f);
    buffer[read_size] = '\0';

    // Parse JSON
    cJSON *root = cJSON_ParseWithLength(buffer, read_size);
    free(buffer);

    if (!root) {
        ESP_LOGE(TAG, "Failed to parse JSON");
        return ESP_ERR_INVALID_ARG;
    }

    // Extract theme ID
    cJSON *id_obj = cJSON_GetObjectItem(root, "id");
    if (cJSON_IsString(id_obj)) {
        strncpy(s_sidecar.theme_id, id_obj->valuestring, sizeof(s_sidecar.theme_id) - 1);
    }

    // Extract pages and elements
    s_sidecar.element_count = 0;
    cJSON *pages = cJSON_GetObjectItem(root, "pages");
    if (cJSON_IsArray(pages)) {
        int page_idx = 0;
        cJSON *page = NULL;
        cJSON_ArrayForEach(page, pages) {
            cJSON *elements = cJSON_GetObjectItem(page, "elements");
            if (cJSON_IsArray(elements)) {
                cJSON *elem = NULL;
                cJSON_ArrayForEach(elem, elements) {
                    if (s_sidecar.element_count >= MAX_ELEMENTS) break;

                    element_info_t *info = &s_sidecar.elements[s_sidecar.element_count];

                    cJSON *elem_id = cJSON_GetObjectItem(elem, "id");
                    cJSON *elem_kind = cJSON_GetObjectItem(elem, "kind");

                    if (cJSON_IsString(elem_id)) {
                        strncpy(info->element_id, elem_id->valuestring, sizeof(info->element_id) - 1);
                    }
                    if (cJSON_IsString(elem_kind)) {
                        strncpy(info->element_kind, elem_kind->valuestring, sizeof(info->element_kind) - 1);
                    }
                    info->page_index = page_idx;

                    s_sidecar.element_count++;
                }
            }
            page_idx++;
        }
    }

    // Extract rules (TODO: implement full rule parsing)
    cJSON *rules = cJSON_GetObjectItem(root, "rules");
    if (cJSON_IsArray(rules)) {
        int rule_count = cJSON_GetArraySize(rules);
        ESP_LOGI(TAG, "Found %d rules (parsing not yet implemented)", rule_count);
    }

    cJSON_Delete(root);

    ESP_LOGI(TAG, "Parsed sidecar: theme_id=%s, %d elements",
             s_sidecar.theme_id, s_sidecar.element_count);

    return ESP_OK;
}

esp_err_t sidecar_save_to_config(const char *theme_id) {
    // TODO: Save extracted sidecar data to /config/themes/<id>/sidecar.json
    (void)theme_id;
    ESP_LOGW(TAG, "sidecar_save_to_config not yet implemented");
    return ESP_ERR_NOT_SUPPORTED;
}

const char *sidecar_get_theme_id(void) {
    return s_sidecar.theme_id;
}

int sidecar_get_element_count(void) {
    return s_sidecar.element_count;
}

void sidecar_clear(void) {
    memset(&s_sidecar, 0, sizeof(s_sidecar));
}
