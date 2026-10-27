#include "device_serial_rpc.h"
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_log.h"
#include "vehicle/vp_manager.h"
#include "export_path/ui_ext.h"
#include "app_obd_dsp/theme_library.h"
#include "app_obd_dsp/first_boot_migration.h"
#include "app_obd_dsp/ota_wifi_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

extern SemaphoreHandle_t lvgl_mux;

char *device_serial_rpc(const char *json, bool *restart)
{
    cJSON *request = cJSON_Parse(json);
    cJSON *response = cJSON_CreateObject();
    const cJSON *op = request ? cJSON_GetObjectItemCaseSensitive(request, "op") : NULL;
    const char *name = cJSON_IsString(op) ? op->valuestring : "";
    bool ok = false;
    *restart = false;
    if (!response) goto done;
    if (!cJSON_IsObject(request) || !cJSON_IsString(op)) {
        cJSON_AddStringToObject(response, "error", "invalid USB management request");
        goto finish;
    }
    if (ota_wifi_server_is_active() && strcmp(name, "hello") && strcmp(name, "ota.stop")) {
        cJSON_AddStringToObject(response, "error", "exit OTA mode before USB management");
        goto finish;
    }
    if (!strcmp(name, "ota.stop")) {
        /* The USB worker owns no LVGL/storage locks here. Stop joins the HTTP
         * task before aborting its staged upload, so USB can take over safely. */
        ok = ota_wifi_server_stop();
        if (!ok) {
            cJSON_AddStringToObject(response, "error", "OTA start/stop busy; retry shortly");
            cJSON_AddBoolToObject(response, "retryable", true);
        }
    } else if (!strcmp(name, "vehicle.install")) {
        vp_issue_t *items = calloc(64, sizeof(*items));
        if (items) {
            vp_issues_t issues;
            vp_issues_init(&issues, items, 64);
            char id[VPM_ID_LEN] = {0};
            const cJSON *profile = cJSON_GetObjectItemCaseSensitive(request, "profile");
            char *raw = cJSON_IsObject(profile) ? cJSON_PrintUnformatted(profile) : NULL;
            if (raw) {
                esp_err_t err = vp_manager_install_json(raw, strlen(raw), &issues, id, sizeof(id));
                ok = err == ESP_OK;
                if (ok) cJSON_AddStringToObject(response, "id", id);
                else cJSON_AddStringToObject(response, "error", vp_manager_last_error());
                cJSON *list = cJSON_AddArrayToObject(response, "issues");
                for (size_t i = 0; i < issues.count; i++) {
                    cJSON *item = cJSON_CreateObject();
                    cJSON_AddStringToObject(item, "severity", items[i].severity == VP_SEV_ERROR ? "error" : "warning");
                    cJSON_AddStringToObject(item, "path", items[i].path);
                    cJSON_AddStringToObject(item, "msg", items[i].msg);
                    cJSON_AddItemToArray(list, item);
                }
                cJSON_AddNumberToObject(response, "warnings", (double)issues.n_warnings);
            } else {
                cJSON_AddStringToObject(response, "error", "missing or invalid vehicle profile");
            }
            free(raw);
            free(items);
        } else {
            cJSON_AddStringToObject(response, "error", "insufficient memory");
        }
    } else if (!strcmp(name, "vehicle.activate")) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(request, "id");
        if (cJSON_IsString(id) && vp_manager_activate(id->valuestring) == ESP_OK) {
            ok = true;
            if (!strcmp(id->valuestring, VPM_GENERIC_ID)) first_boot_migration_drop_vehicle();
            cJSON_AddStringToObject(response, "pending", id->valuestring);
        } else {
            cJSON_AddStringToObject(response, "error", cJSON_IsString(id) ? vp_manager_last_error() : "missing vehicle id");
        }
    } else if (!strcmp(name, "vehicle.clear")) {
        const cJSON *confirm = cJSON_GetObjectItemCaseSensitive(request, "confirm");
        if (!cJSON_IsTrue(confirm)) {
            cJSON_AddStringToObject(response, "error", "explicit confirmation required");
        } else {
            esp_err_t err = vp_manager_clear_installed();
            ok = err == ESP_OK;
            if (ok) {
                first_boot_migration_drop_vehicle();
                cJSON_AddBoolToObject(response, "cleared", true);
                cJSON_AddStringToObject(response, "pending", VPM_GENERIC_ID);
            } else {
                cJSON_AddStringToObject(response, "error", err == ESP_ERR_INVALID_STATE
                    ? "SD card is not mounted" : vp_manager_last_error());
            }
        }
    } else if (!strcmp(name, "theme.erase")) {
        bool deleted = false;
        if (!lvgl_mux || xSemaphoreTake(lvgl_mux, pdMS_TO_TICKS(1000)) != pdTRUE) {
            cJSON_AddStringToObject(response, "error", "display busy");
            cJSON_AddBoolToObject(response, "retryable", true);
            cJSON_AddStringToObject(response, "stage", "prepare");
            cJSON_AddStringToObject(response, "code", esp_err_to_name(ESP_ERR_TIMEOUT));
        } else {
            esp_err_t err = ui_ext_theme_prepare_install();
            bool preparing = err == ESP_ERR_INVALID_STATE;
            bool prepared = err == ESP_OK;
            if (err == ESP_OK) err = theme_library_delete_current(&deleted);
            xSemaphoreGive(lvgl_mux);
            ok = err == ESP_OK;
            cJSON_AddBoolToObject(response, "deleted", deleted);
            if (!ok) {
                const char *stage = prepared ? "delete" : "prepare";
                const char *message = preparing ? "theme preparation busy"
                    : !prepared ? "theme preparation failed"
                    : err == ESP_ERR_INVALID_STATE ? "SD card is not mounted"
                    : "theme delete failed";
                cJSON_AddStringToObject(response, "error", message);
                cJSON_AddStringToObject(response, "stage", stage);
                cJSON_AddStringToObject(response, "code", esp_err_to_name(err));
                ESP_LOGW("usb_rpc", "theme.erase %s failed: %s", stage, esp_err_to_name(err));
                /* Preparation can be refused while boot preloading still reads
                 * the selected package. No files have been deleted in this case. */
                cJSON_AddBoolToObject(response, "retryable",
                                     err == ESP_ERR_TIMEOUT || preparing);
            }
        }
    } else if (!strcmp(name, "hello")) {
        ok = true;
        cJSON_AddNumberToObject(response, "usb_management", 1);
        cJSON_AddBoolToObject(response, "ota_active", ota_wifi_server_is_active());
    } else if (!strcmp(name, "reboot")) {
        ok = true;
        *restart = true;
    } else {
        cJSON_AddStringToObject(response, "error", "unknown USB management operation");
    }
finish:
    cJSON_AddBoolToObject(response, "ok", ok);
done:
    if (request) cJSON_Delete(request);
    char *result = response ? cJSON_PrintUnformatted(response) : NULL;
    cJSON_Delete(response);
    return result;
}

