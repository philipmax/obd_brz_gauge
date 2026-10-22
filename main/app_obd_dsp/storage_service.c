// storage_service.c — boot-time storage bring-up and the background SD task
// (insert/remove polling, inbox import, config mirror, /OBD/IMPORT scan).
// Contract: see storage_service.h.
#include "app_obd_dsp/storage_service.h"

#include <stdint.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp_obd_dsp/config_part.h"
#include "bsp_obd_dsp/sd_card.h"
#include "app_obd_dsp/first_boot_migration.h"
#include "app_obd_dsp/inbox_import.h"
#include "app_obd_dsp/ota_wifi_server.h"
#include "app_obd_dsp/sd_import.h"
#include "app_obd_dsp/storage_util.h"
#include "app_obd_dsp/theme_library.h"
#include "app_obd_dsp/theme_bundle.h"

static const char *TAG = "storage";

#if CONFIG_OBD_SD_SLOT_SDMMC_1BIT
#define TASK_STACK     8192
#define TASK_PRIO      (tskIDLE_PRIORITY + 1)
#define FIRST_RUN_MS   5000     /* let the boot video / UI start first */
#define RETRY_MS       10000    /* busy (theme write, UI job): try again */
#define STACK_WARN     1024     /* log when the high-water mark leaves less */

#define JOB_RESUME     (1u << 0)
#define JOB_INBOX      (1u << 1)
#define JOB_MIRROR     (1u << 2)
#define JOB_SCAN       (1u << 3)
#define JOB_VEHICLE    (1u << 4)
#define JOB_ALL        (JOB_RESUME | JOB_INBOX | JOB_MIRROR | JOB_SCAN | JOB_VEHICLE)

static TaskHandle_t s_task;
#endif

esp_err_t storage_service_boot(void)
{
    esp_err_t ret = first_boot_migration_needed() ? first_boot_migration_run()
                                                  : first_boot_migration_mount_config();

    /* before theme_engine_init(): nothing may read a package while SD storage
     * recovery or selection is rewriting it. */
    esp_err_t err = ESP_OK;
    if (sd_card_is_mounted()) {
        err = theme_bundle_recover();
        if (err != ESP_OK) ESP_LOGW(TAG, "USB theme recovery: %s", esp_err_to_name(err));
    }
    err = theme_library_resume();
    if (err != ESP_OK) ESP_LOGW(TAG, "theme resume: %s", esp_err_to_name(err));

    if (sd_card_is_mounted()) {
        err = sd_import_scan_boot();
        if (err != ESP_OK) ESP_LOGW(TAG, "boot import scan: %s", esp_err_to_name(err));
    }
    first_boot_migration_retry_vehicle();
    return ret;
}

#if CONFIG_OBD_SD_SLOT_SDMMC_1BIT
/* Runs the card jobs in `jobs`; returns the ones to retry later (busy). */
static uint32_t run_jobs(uint32_t jobs)
{
    if (jobs & JOB_RESUME) {
        if (theme_library_resume() != ESP_ERR_INVALID_STATE) jobs &= ~JOB_RESUME;
    }
    if (jobs & JOB_INBOX) {
        if (inbox_import_run() != ESP_ERR_INVALID_STATE) jobs &= ~JOB_INBOX;
    }
    if (jobs & JOB_MIRROR) {
        if (storage_job_lock(pdMS_TO_TICKS(1000))) {
            config_part_mirror_to_sd();
            storage_job_unlock();
            jobs &= ~JOB_MIRROR;
        }
    }
    if (jobs & JOB_SCAN) {
        if (sd_import_scan() != ESP_ERR_INVALID_STATE) jobs &= ~JOB_SCAN;
    }
    if (jobs & JOB_VEHICLE) {
        /* after the scan: a profile dropped into /OBD/IMPORT counts; still
         * waiting for its JSON -> next insert / boot */
        first_boot_migration_retry_vehicle();
        jobs &= ~JOB_VEHICLE;
    }
    return jobs;
}

static void service_task(void *arg)
{
    (void)arg;
    uint32_t jobs = JOB_ALL;
    bool was_mounted = sd_card_is_mounted();
    bool was_ota = false;
    int64_t next_run = esp_timer_get_time() + (int64_t)FIRST_RUN_MS * 1000;

    for (;;) {
        sd_card_poll();
        bool mounted = sd_card_is_mounted();
        bool ota = ota_wifi_server_is_active();
        int64_t now = esp_timer_get_time();
        if ((mounted && !was_mounted) || (was_ota && !ota)) {
            jobs = JOB_ALL;     /* card inserted / OTA server stopped */
            next_run = now;
        }
        was_mounted = mounted;
        was_ota = ota;

        if (jobs && mounted && !ota && now >= next_run) {
            jobs = run_jobs(jobs);
            if (jobs) next_run = esp_timer_get_time() + (int64_t)RETRY_MS * 1000;
            UBaseType_t free_b = uxTaskGetStackHighWaterMark(NULL);   /* bytes on ESP-IDF */
            if (free_b < STACK_WARN) ESP_LOGW(TAG, "stack headroom low: %u bytes", (unsigned)free_b);
            else ESP_LOGD(TAG, "stack headroom %u bytes", (unsigned)free_b);
        }
        uint32_t wait_ms = sd_card_poll_wait_ms();
        if (mounted && wait_ms > 1000) wait_ms = 1000; /* OTA and CMD13 responsiveness */
        if (jobs && mounted && !ota && now < next_run) {
            uint32_t retry_ms = (uint32_t)((next_run - now + 999) / 1000);
            if (retry_ms < wait_ms) wait_ms = retry_ms;
        }
        TickType_t wait_ticks = pdMS_TO_TICKS(wait_ms);
        vTaskDelay(wait_ticks ? wait_ticks : 1);
    }
}

#endif /* CONFIG_OBD_SD_SLOT_SDMMC_1BIT */

esp_err_t storage_service_start(void)
{
    /* No slot means mounted is always false: the worker could never run jobs.
     * Keep boot bring-up and busy reporting independent of this SD worker. */
#if CONFIG_OBD_SD_SLOT_SDMMC_1BIT
    if (s_task) return ESP_OK;
    /* internal-RAM stack (xTaskCreate default): the jobs write flash / nvs_app */
    if (xTaskCreate(service_task, "storage", TASK_STACK, NULL, TASK_PRIO, &s_task) != pdPASS) {
        s_task = NULL;
        ESP_LOGE(TAG, "task create failed");
        return ESP_ERR_NO_MEM;
    }
#endif
    return ESP_OK;
}

bool storage_service_busy(void)
{
    return storage_job_busy() || theme_library_get_state(NULL) == THEME_LIB_WRITING;
}
