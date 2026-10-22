/* Compile the actual service with the actual project headers in both slot
 * configurations. Fork isolates boot-lifetime statics without test-only
 * production hooks. Virtual time verifies policy, not RTOS/FAT/flash behavior. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <setjmp.h>
#include <sys/wait.h>
#include <unistd.h>
#include "sdkconfig.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "app_obd_dsp/storage_service.h"
#include "app_obd_dsp/first_boot_migration.h"
#include "app_obd_dsp/inbox_import.h"
#include "app_obd_dsp/ota_wifi_server.h"
#include "app_obd_dsp/sd_import.h"
#include "app_obd_dsp/storage_util.h"
#include "app_obd_dsp/theme_library.h"
#include "bsp_obd_dsp/config_part.h"
#include "bsp_obd_dsp/sd_card.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))

typedef struct { char op; int64_t us; } event_t;
static event_t events[128];
static unsigned event_count, creates, polls, delays, high_water_checks;
static unsigned delay_ms[128], sd_wait_ms = 1000;
static unsigned resume_calls, inbox_calls, lock_calls, scan_calls;
static bool needed, mounted, ota, job_busy;
static theme_lib_state_t theme_state;
static esp_err_t boot_result = ESP_OK, resume_result = ESP_OK, scan_result = ESP_OK;
static bool fail_create, retry_once, lock_held;
static int64_t now_us, scan_cost_us;
#if CONFIG_OBD_SD_SLOT_SDMMC_1BIT
static int64_t origin_us;
#endif
static unsigned stop_after;
static jmp_buf stop_task;
static TaskFunction_t worker;
static void (*poll_hook)(unsigned);

static void record(char op)
{
    CHECK(event_count < COUNT_OF(events));
    events[event_count++] = (event_t){ op, now_us };
}
#if CONFIG_OBD_SD_SLOT_SDMMC_1BIT
static unsigned count(char op)
{
    unsigned n = 0;
    for (unsigned i = 0; i < event_count; i++) n += events[i].op == op;
    return n;
}
#endif
static void sequence(const char *expected)
{
    CHECK(event_count == strlen(expected));
    for (unsigned i = 0; i < event_count; i++) CHECK(events[i].op == expected[i]);
}
#if CONFIG_OBD_SD_SLOT_SDMMC_1BIT
static void at(char op, unsigned occurrence, int64_t ms)
{
    for (unsigned i = 0; i < event_count; i++) {
        if (events[i].op == op && occurrence-- == 0) {
            CHECK(events[i].us - origin_us == ms * 1000);
            return;
        }
    }
    CHECK(false);
}

#endif

/* SDK/RTOS stubs. Start captures the real worker; the test decides when it runs. */
const char *esp_err_to_name(esp_err_t err) { (void)err; return "stub"; }
void storage_test_log(const char *tag, const char *fmt, ...)
{
    CHECK(!strcmp(tag, "storage"));
    char buffer[256];
    va_list args;
    va_start(args, fmt);
    (void)vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
}
int64_t esp_timer_get_time(void) { return now_us; }
BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack,
                      void *arg, UBaseType_t prio, TaskHandle_t *handle)
{
    creates++;
    CHECK(fn && !strcmp(name, "storage") && stack == 8192 && !arg);
    CHECK(prio == tskIDLE_PRIORITY + 1 && handle);
    *handle = (TaskHandle_t)&creates; /* also written on failure: service must clear it */
    if (fail_create) return pdFAIL;
    worker = fn;
    return pdPASS;
}
void vTaskDelay(TickType_t ticks)
{
    CHECK(delays < COUNT_OF(delay_ms));
    delay_ms[delays] = (unsigned)ticks * 10u;
    CHECK(!lock_held);
    now_us += (int64_t)ticks * 10000;
    if (++delays >= stop_after) longjmp(stop_task, 1);
}
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task)
{
    CHECK(!task);
    high_water_checks++;
    return high_water_checks == 1 ? 512u : 2048u; /* exercise both log paths */
}

/* API stubs use the real declarations, so boundary signature drift fails build. */
bool first_boot_migration_needed(void) { record('N'); return needed; }
esp_err_t first_boot_migration_run(void) { record('G'); return boot_result; }
esp_err_t first_boot_migration_mount_config(void) { record('C'); return boot_result; }
esp_err_t first_boot_migration_retry_vehicle(void) { record('V'); return ESP_OK; }
bool sd_card_is_mounted(void) { return mounted; }
uint32_t sd_card_poll_wait_ms(void) { return sd_wait_ms; }
void sd_card_poll(void)
{
    if (poll_hook) poll_hook(polls);
    polls++;
}
bool ota_wifi_server_is_active(void) { return ota; }
esp_err_t theme_bundle_recover(void) { return ESP_OK; }
esp_err_t theme_library_resume(void)
{
    record('R');
    return retry_once && resume_calls++ == 0 ? ESP_ERR_INVALID_STATE : resume_result;
}
esp_err_t inbox_import_run(void)
{
    record('I');
    return retry_once && inbox_calls++ == 0 ? ESP_ERR_INVALID_STATE : ESP_OK;
}
bool storage_job_lock(TickType_t wait)
{
    CHECK(wait == pdMS_TO_TICKS(1000) && !lock_held);
    record('L');
    if (retry_once && lock_calls++ == 0) return false;
    lock_held = true;
    return true;
}
void storage_job_unlock(void) { CHECK(lock_held); lock_held = false; record('U'); }
void config_part_mirror_to_sd(void) { CHECK(lock_held); record('M'); }
esp_err_t sd_import_scan(void)
{
    record('S');
    now_us += scan_cost_us;
    return retry_once && scan_calls++ == 0 ? ESP_ERR_INVALID_STATE : scan_result;
}
esp_err_t sd_import_scan_boot(void) { record('B'); return scan_result; }
bool storage_job_busy(void) { return job_busy; }
theme_lib_state_t theme_library_get_state(uint8_t *pct)
{
    CHECK(!pct);
    return theme_state;
}

#if CONFIG_OBD_SD_SLOT_SDMMC_1BIT
static void run_worker(unsigned iterations)
{
    CHECK(worker && iterations);
    stop_after = iterations;
    origin_us = now_us;
    if (!setjmp(stop_task)) worker(NULL);
    CHECK(polls == iterations && delays == iterations && !lock_held);
}

#endif

/* Common boot and UI contracts must stay available without an SD worker. */
static void test_boot_mount(void)
{
    CHECK(storage_service_boot() == ESP_OK);
    sequence("NCRV");
    CHECK(!creates && !polls);
}
static void test_boot_migration_error(void)
{
    needed = true;
    boot_result = ESP_ERR_NO_MEM;
    resume_result = ESP_ERR_INVALID_STATE;
    CHECK(storage_service_boot() == ESP_ERR_NO_MEM);
    sequence("NGRV"); /* recovery and vehicle retry still run after migration error */
    CHECK(!creates);
}
static void test_boot_mount_error(void)
{
    boot_result = ESP_FAIL;
    resume_result = ESP_FAIL;
    CHECK(storage_service_boot() == ESP_FAIL);
    sequence("NCRV");
}
static void test_busy(void)
{
    for (unsigned j = 0; j < 2; j++) {
        for (unsigned state = THEME_LIB_IDLE; state <= THEME_LIB_ERROR; state++) {
            job_busy = j != 0;
            theme_state = (theme_lib_state_t)state;
            CHECK(storage_service_busy() == (job_busy || state == THEME_LIB_WRITING));
        }
    }
    CHECK(!creates && !event_count);
}
static void test_start(void)
{
    for (unsigned i = 0; i < 3; i++) CHECK(storage_service_start() == ESP_OK);
#if CONFIG_OBD_SD_SLOT_SDMMC_1BIT
    CHECK(creates == 1 && worker); /* absent card must still get an insertion poller */
#else
    CHECK(!creates && !worker && !polls && !delays && !event_count);
#endif
}
static void test_create_failure(void)
{
    fail_create = true;
#if CONFIG_OBD_SD_SLOT_SDMMC_1BIT
    CHECK(storage_service_start() == ESP_ERR_NO_MEM);
    CHECK(creates == 1 && !worker);
    fail_create = false;
    CHECK(storage_service_start() == ESP_OK);
    CHECK(storage_service_start() == ESP_OK);
    CHECK(creates == 2 && worker);
#else
    CHECK(storage_service_start() == ESP_OK);
    CHECK(!creates && !worker);
    CHECK(storage_service_boot() == ESP_OK);
    sequence("NCRV");
#endif
}

#if CONFIG_OBD_SD_SLOT_SDMMC_1BIT
static void test_boot_mounted(void)
{
    mounted = true;
    scan_result = ESP_FAIL;
    CHECK(storage_service_boot() == ESP_OK);
    sequence("NCRBV"); /* failed import scan is non-fatal and does not skip retry */
    CHECK(!creates);
}
static void test_initial_jobs(void)
{
    mounted = true;
    now_us = (INT64_C(1) << 34) * 1000; /* uptime past 32-bit milliseconds */
    CHECK(storage_service_start() == ESP_OK);
    run_worker(20);
    sequence("RILMUSV");
    for (unsigned i = 0; i < event_count; i++) CHECK(events[i].us - origin_us == 5000000);
    CHECK(high_water_checks == 1);
}
static void absent_hook(unsigned poll)
{
    ota = (poll >= 2 && poll < 8);
}
static void test_absent(void)
{
    sd_wait_ms = 3000;
    poll_hook = absent_hook;
    CHECK(storage_service_start() == ESP_OK);
    run_worker(20);
    CHECK(!event_count && !high_water_checks);
    for (unsigned i = 0; i < delays; i++) CHECK(delay_ms[i] == 3000);
}
static void test_mounted_wait_cap(void)
{
    mounted = true;
    sd_wait_ms = 10000;
    CHECK(storage_service_start() == ESP_OK);
    run_worker(8);
    for (unsigned i = 0; i < delays; i++) CHECK(delay_ms[i] == 1000);
}
static void test_error_card_wait(void)
{
    sd_wait_ms = 10000;
    CHECK(storage_service_start() == ESP_OK);
    run_worker(6);
    CHECK(now_us - origin_us == INT64_C(60000000));
    CHECK(!event_count && !high_water_checks);
    for (unsigned i = 0; i < delays; i++) CHECK(delay_ms[i] == 10000);
}
static void test_minimum_tick_wait(void)
{
    sd_wait_ms = 1; /* an already-due probe, shorter than the 10 ms host tick */
    CHECK(storage_service_start() == ESP_OK);
    run_worker(4);
    for (unsigned i = 0; i < delays; i++) CHECK(delay_ms[i] == 10);
}
static void insert_hook(unsigned poll)
{
    mounted = (poll >= 2 && poll < 5) || poll >= 7;
}
static void test_insert_remove(void)
{
    poll_hook = insert_hook;
    CHECK(storage_service_start() == ESP_OK);
    run_worker(20);
    sequence("RILMUSVRILMUSV");
    at('R', 0, 2000); at('R', 1, 7000);
    CHECK(high_water_checks == 2);
}
static void ota_hook(unsigned poll)
{
    ota = (poll >= 2 && poll < 8) || (poll >= 11 && poll < 13);
}
static void test_ota(void)
{
    mounted = true;
    poll_hook = ota_hook;
    CHECK(storage_service_start() == ESP_OK);
    run_worker(20);
    sequence("RILMUSVRILMUSV");
    at('R', 0, 8000); at('R', 1, 13000);
    CHECK(high_water_checks == 2);
}
static void insert_ota_hook(unsigned poll)
{
    mounted = poll >= 2;
    ota = poll < 8;
}
static void test_insert_during_ota(void)
{
    poll_hook = insert_ota_hook;
    CHECK(storage_service_start() == ESP_OK);
    run_worker(20);
    sequence("RILMUSV");
    at('R', 0, 8000);
}
static void test_busy_retry(void)
{
    mounted = true;
    retry_once = true;
    CHECK(storage_service_start() == ESP_OK);
    run_worker(21);
    sequence("RILSVRILMUS");
    at('R', 0, 5000); at('R', 1, 15000);
    CHECK(count('V') == 1 && high_water_checks == 2);
}
static void test_retry_completion_time(void)
{
    mounted = true;
    retry_once = true;
    scan_cost_us = 2000000;
    CHECK(storage_service_start() == ESP_OK);
    run_worker(21);
    sequence("RILSVRILMUS");
    at('R', 0, 5000); at('R', 1, 17000); /* retry deadline starts after job completion */
    CHECK(count('V') == 1);
}
static void test_nonbusy_error(void)
{
    mounted = true;
    resume_result = scan_result = ESP_FAIL;
    CHECK(storage_service_start() == ESP_OK);
    run_worker(21);
    sequence("RILMUSV"); /* only INVALID_STATE means retry */
    CHECK(high_water_checks == 1);
}
#else
static void test_no_slot_boot_and_busy(void)
{
    CHECK(storage_service_start() == ESP_OK);
    needed = true;
    CHECK(storage_service_boot() == ESP_OK);
    sequence("NGRV");
    theme_state = THEME_LIB_WRITING;
    CHECK(storage_service_busy());
    CHECK(!creates && !worker && !polls && !delays);
}
#endif

typedef void (*test_fn_t)(void);
typedef struct { const char *name; test_fn_t fn; } test_t;
int main(void)
{
    const test_t tests[] = {
#define TEST(f) { #f, f }
        TEST(test_boot_mount), TEST(test_boot_migration_error), TEST(test_boot_mount_error),
        TEST(test_busy), TEST(test_start), TEST(test_create_failure),
#if CONFIG_OBD_SD_SLOT_SDMMC_1BIT
        TEST(test_boot_mounted), TEST(test_initial_jobs), TEST(test_absent), TEST(test_mounted_wait_cap),
        TEST(test_error_card_wait), TEST(test_minimum_tick_wait),
        TEST(test_insert_remove), TEST(test_ota), TEST(test_insert_during_ota),
        TEST(test_busy_retry), TEST(test_retry_completion_time), TEST(test_nonbusy_error)
#else
        TEST(test_no_slot_boot_and_busy)
#endif
#undef TEST
    };
    for (unsigned i = 0; i < COUNT_OF(tests); i++) {
        fflush(NULL);
        pid_t pid = fork();
        CHECK(pid >= 0);
        if (pid == 0) { tests[i].fn(); exit(0); }
        int status;
        CHECK(waitpid(pid, &status, 0) == pid);
        if (!WIFEXITED(status) || WEXITSTATUS(status)) {
            fprintf(stderr, "FAIL %s (status=%d)\n", tests[i].name, status);
            return 1;
        }
        printf("PASS %s\n", tests[i].name);
    }
    printf("%zu storage scenarios passed (%s)\n", COUNT_OF(tests),
#if CONFIG_OBD_SD_SLOT_SDMMC_1BIT
           "SDMMC slot"
#else
           "no slot"
#endif
    );
    return 0;
}
