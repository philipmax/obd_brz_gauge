/*
 * api_v1_handlers.c — /api/v1/* REST endpoint handlers
 */

#include "api_v1_handlers.h"
#include "bsp_obd_dsp/nvs_storage.h"
#include "bsp_obd_dsp/nvs_app.h"
#include "vehicle/vp_profile.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "api_v1";

/* OTA state */
static esp_ota_handle_t s_ota_handle = 0;
static const esp_partition_t *s_ota_partition = NULL;
static size_t s_ota_written = 0;

/* Helper: send JSON response */
static esp_err_t send_json_response(httpd_req_t *req, int status, cJSON *json) {
    char *json_str = cJSON_PrintUnformatted(json);
    if (!json_str) {
        cJSON_Delete(json);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_status(req, status == 200 ? "200 OK" :
                              status == 400 ? "400 Bad Request" :
                              status == 404 ? "404 Not Found" : "500 Internal Error");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);

    free(json_str);
    cJSON_Delete(json);
    return ESP_OK;
}

/* Helper: send error JSON */
static esp_err_t send_error(httpd_req_t *req, int status, const char *message) {
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "success", false);
    cJSON_AddStringToObject(json, "error", message);
    return send_json_response(req, status, json);
}

/* POST /api/v1/ota/chunk */
esp_err_t api_v1_ota_chunk_handler(httpd_req_t *req) {
    /* First chunk: start OTA */
    if (s_ota_handle == 0) {
        s_ota_partition = esp_ota_get_next_update_partition(NULL);
        if (!s_ota_partition) {
            return send_error(req, 500, "No OTA partition");
        }

        esp_err_t err = esp_ota_begin(s_ota_partition, OTA_SIZE_UNKNOWN, &s_ota_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA begin failed: %d", err);
            return send_error(req, 500, "OTA begin failed");
        }

        s_ota_written = 0;
        ESP_LOGI(TAG, "OTA started, target: %s", s_ota_partition->label);
    }

    /* Read chunk */
    char buf[4096];
    int remaining = req->content_len;

    while (remaining > 0) {
        int recv = httpd_req_recv(req, buf, remaining > sizeof(buf) ? sizeof(buf) : remaining);
        if (recv <= 0) {
            if (recv == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            esp_ota_abort(s_ota_handle);
            s_ota_handle = 0;
            return send_error(req, 400, "Receive failed");
        }

        esp_err_t err = esp_ota_write(s_ota_handle, buf, recv);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA write failed: %d", err);
            esp_ota_abort(s_ota_handle);
            s_ota_handle = 0;
            return send_error(req, 500, "Write failed");
        }

        remaining -= recv;
        s_ota_written += recv;
    }

    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "success", true);
    cJSON_AddNumberToObject(json, "written", s_ota_written);
    return send_json_response(req, 200, json);
}

/* POST /api/v1/ota/finalize */
esp_err_t api_v1_ota_finalize_handler(httpd_req_t *req) {
    if (s_ota_handle == 0) {
        return send_error(req, 400, "No active OTA");
    }

    esp_err_t err = esp_ota_end(s_ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA end failed: %d", err);
        s_ota_handle = 0;
        return send_error(req, 500, "OTA end failed");
    }

    err = esp_ota_set_boot_partition(s_ota_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Set boot partition failed: %d", err);
        s_ota_handle = 0;
        return send_error(req, 500, "Set boot failed");
    }

    s_ota_handle = 0;

    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "success", true);
    cJSON_AddNumberToObject(json, "total_bytes", s_ota_written);
    send_json_response(req, 200, json);

    ESP_LOGI(TAG, "OTA complete, rebooting in 2s...");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();

    return ESP_OK;
}

/* POST /api/v1/ota/abort */
esp_err_t api_v1_ota_abort_handler(httpd_req_t *req) {
    if (s_ota_handle != 0) {
        esp_ota_abort(s_ota_handle);
        s_ota_handle = 0;
    }

    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "success", true);
    return send_json_response(req, 200, json);
}

/* GET /api/v1/config */
esp_err_t api_v1_config_get_handler(httpd_req_t *req) {
    const nvs_user_cfg_t *cfg = nvs_cfg_get();

    cJSON *json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "theme_slot", cfg->theme_slot);
    cJSON_AddNumberToObject(json, "brightness", cfg->brightness);
    cJSON_AddNumberToObject(json, "protocol", cfg->protocol);
    cJSON_AddNumberToObject(json, "volume", cfg->boot_media_volume);

    return send_json_response(req, 200, json);
}

/* POST /api/v1/config */
esp_err_t api_v1_config_post_handler(httpd_req_t *req) {
    char buf[512];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        return send_error(req, 400, "No data");
    }
    buf[ret] = '\0';

    cJSON *json = cJSON_Parse(buf);
    if (!json) {
        return send_error(req, 400, "Invalid JSON");
    }

    nvs_user_cfg_t cfg = *nvs_cfg_get();

    cJSON *item;
    if ((item = cJSON_GetObjectItem(json, "theme_slot"))) {
        cfg.theme_slot = item->valueint;
    }
    if ((item = cJSON_GetObjectItem(json, "brightness"))) {
        cfg.brightness = item->valueint;
    }
    if ((item = cJSON_GetObjectItem(json, "protocol"))) {
        cfg.protocol = item->valueint;
    }
    if ((item = cJSON_GetObjectItem(json, "volume"))) {
        cfg.boot_media_volume = item->valueint;
    }

    cJSON_Delete(json);

    nvs_cfg_set(&cfg);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", true);
    return send_json_response(req, 200, resp);
}

/* GET /api/v1/vehicle/list */
esp_err_t api_v1_vehicle_list_handler(httpd_req_t *req) {
    cJSON *json = cJSON_CreateObject();
    cJSON *profiles = cJSON_CreateArray();

    /* Add built-in generic profile */
    cJSON *generic = cJSON_CreateObject();
    cJSON_AddStringToObject(generic, "id", "generic");
    cJSON_AddStringToObject(generic, "name", "OBD2 Generic");
    cJSON_AddStringToObject(generic, "type", "builtin");
    cJSON_AddItemToArray(profiles, generic);

    /* TODO: scan /config/vehicles/ for custom profiles */

    cJSON_AddItemToObject(json, "profiles", profiles);
    return send_json_response(req, 200, json);
}

/* GET /api/v1/vehicle/active */
esp_err_t api_v1_vehicle_active_get_handler(httpd_req_t *req) {
    char active_id[64] = "generic";
    nvs_app_get_str("veh", "active", active_id, sizeof(active_id));

    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "profile", active_id);
    return send_json_response(req, 200, json);
}

/* POST /api/v1/vehicle/active */
esp_err_t api_v1_vehicle_active_post_handler(httpd_req_t *req) {
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        return send_error(req, 400, "No data");
    }
    buf[ret] = '\0';

    cJSON *json = cJSON_Parse(buf);
    if (!json) {
        return send_error(req, 400, "Invalid JSON");
    }

    cJSON *profile = cJSON_GetObjectItem(json, "profile");
    if (!profile || !cJSON_IsString(profile)) {
        cJSON_Delete(json);
        return send_error(req, 400, "Missing profile");
    }

    nvs_app_set_str("veh", "active", profile->valuestring);
    cJSON_Delete(json);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", true);
    return send_json_response(req, 200, resp);
}

/* POST /api/v1/vehicle/upload */
esp_err_t api_v1_vehicle_upload_handler(httpd_req_t *req) {
    /* TODO: save uploaded JSON to /config/vehicles/ */
    return send_error(req, 501, "Not implemented");
}

/* GET /api/v1/system/info */
esp_err_t api_v1_system_info_handler(httpd_req_t *req) {
    const esp_app_desc_t *app_desc = esp_app_get_description();

    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "version", app_desc->version);
    cJSON_AddStringToObject(json, "idf_version", app_desc->idf_ver);
    cJSON_AddNumberToObject(json, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(json, "uptime", esp_timer_get_time() / 1000000);
    cJSON_AddBoolToObject(json, "elm_connected", false); /* TODO: get from lifecycle */

    char active_profile[64] = "generic";
    nvs_app_get_str("veh", "active", active_profile, sizeof(active_profile));
    cJSON_AddStringToObject(json, "vehicle_profile", active_profile);

    return send_json_response(req, 200, json);
}

/* POST /api/v1/system/reboot */
esp_err_t api_v1_system_reboot_handler(httpd_req_t *req) {
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "success", true);
    send_json_response(req, 200, json);

    ESP_LOGI(TAG, "Reboot requested, restarting in 1s...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

/* GET /api/v1/system/storage */
esp_err_t api_v1_system_storage_handler(httpd_req_t *req) {
    cJSON *json = cJSON_CreateObject();
    cJSON *partitions = cJSON_CreateArray();

    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (it) {
        const esp_partition_t *part = esp_partition_get(it);

        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "label", part->label);
        cJSON_AddNumberToObject(p, "size", part->size);
        cJSON_AddNumberToObject(p, "address", part->address);

        cJSON_AddItemToArray(partitions, p);
        it = esp_partition_next(it);
    }
    esp_partition_iterator_release(it);

    cJSON_AddItemToObject(json, "partitions", partitions);
    return send_json_response(req, 200, json);
}

esp_err_t api_v1_register_handlers(httpd_handle_t server) {
    httpd_uri_t ota_chunk = {
        .uri = "/api/v1/ota/chunk",
        .method = HTTP_POST,
        .handler = api_v1_ota_chunk_handler,
    };
    httpd_register_uri_handler(server, &ota_chunk);

    httpd_uri_t ota_finalize = {
        .uri = "/api/v1/ota/finalize",
        .method = HTTP_POST,
        .handler = api_v1_ota_finalize_handler,
    };
    httpd_register_uri_handler(server, &ota_finalize);

    httpd_uri_t ota_abort = {
        .uri = "/api/v1/ota/abort",
        .method = HTTP_POST,
        .handler = api_v1_ota_abort_handler,
    };
    httpd_register_uri_handler(server, &ota_abort);

    httpd_uri_t config_get = {
        .uri = "/api/v1/config",
        .method = HTTP_GET,
        .handler = api_v1_config_get_handler,
    };
    httpd_register_uri_handler(server, &config_get);

    httpd_uri_t config_post = {
        .uri = "/api/v1/config",
        .method = HTTP_POST,
        .handler = api_v1_config_post_handler,
    };
    httpd_register_uri_handler(server, &config_post);

    httpd_uri_t vehicle_list = {
        .uri = "/api/v1/vehicle/list",
        .method = HTTP_GET,
        .handler = api_v1_vehicle_list_handler,
    };
    httpd_register_uri_handler(server, &vehicle_list);

    httpd_uri_t vehicle_active_get = {
        .uri = "/api/v1/vehicle/active",
        .method = HTTP_GET,
        .handler = api_v1_vehicle_active_get_handler,
    };
    httpd_register_uri_handler(server, &vehicle_active_get);

    httpd_uri_t vehicle_active_post = {
        .uri = "/api/v1/vehicle/active",
        .method = HTTP_POST,
        .handler = api_v1_vehicle_active_post_handler,
    };
    httpd_register_uri_handler(server, &vehicle_active_post);

    httpd_uri_t system_info = {
        .uri = "/api/v1/system/info",
        .method = HTTP_GET,
        .handler = api_v1_system_info_handler,
    };
    httpd_register_uri_handler(server, &system_info);

    httpd_uri_t system_reboot = {
        .uri = "/api/v1/system/reboot",
        .method = HTTP_POST,
        .handler = api_v1_system_reboot_handler,
    };
    httpd_register_uri_handler(server, &system_reboot);

    httpd_uri_t system_storage = {
        .uri = "/api/v1/system/storage",
        .method = HTTP_GET,
        .handler = api_v1_system_storage_handler,
    };
    httpd_register_uri_handler(server, &system_storage);

    ESP_LOGI(TAG, "API v1 handlers registered");
    return ESP_OK;
}
