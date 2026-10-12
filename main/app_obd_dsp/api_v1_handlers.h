#pragma once
/*
 * api_v1_handlers.h — /api/v1/* REST endpoint handlers
 *
 * Implements the device control API for the web interface.
 */

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Register all /api/v1/* endpoints with the HTTP server.
 */
esp_err_t api_v1_register_handlers(httpd_handle_t server);

/**
 * Chunk sink for OTA updates.
 * POST /api/v1/ota/chunk - receives firmware chunks
 * POST /api/v1/ota/finalize - validates and activates update
 * POST /api/v1/ota/abort - cancels current update
 */
esp_err_t api_v1_ota_chunk_handler(httpd_req_t *req);
esp_err_t api_v1_ota_finalize_handler(httpd_req_t *req);
esp_err_t api_v1_ota_abort_handler(httpd_req_t *req);

/**
 * Configuration endpoints.
 * GET /api/v1/config - get current config
 * POST /api/v1/config - update config
 */
esp_err_t api_v1_config_get_handler(httpd_req_t *req);
esp_err_t api_v1_config_post_handler(httpd_req_t *req);

/**
 * Vehicle profile endpoints.
 * GET /api/v1/vehicle/list - list available profiles
 * GET /api/v1/vehicle/active - get active profile
 * POST /api/v1/vehicle/active - set active profile
 * POST /api/v1/vehicle/upload - upload custom profile
 */
esp_err_t api_v1_vehicle_list_handler(httpd_req_t *req);
esp_err_t api_v1_vehicle_active_get_handler(httpd_req_t *req);
esp_err_t api_v1_vehicle_active_post_handler(httpd_req_t *req);
esp_err_t api_v1_vehicle_upload_handler(httpd_req_t *req);

/**
 * System information endpoints.
 * GET /api/v1/system/info - device info (version, heap, uptime)
 * POST /api/v1/system/reboot - reboot device
 * GET /api/v1/system/storage - storage info (partitions)
 */
esp_err_t api_v1_system_info_handler(httpd_req_t *req);
esp_err_t api_v1_system_reboot_handler(httpd_req_t *req);
esp_err_t api_v1_system_storage_handler(httpd_req_t *req);

#ifdef __cplusplus
}
#endif
