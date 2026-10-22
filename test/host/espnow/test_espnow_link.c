/* Real link worker/callback/OTA path; deterministic event/tick injection.
 * Independent ramp oracle checks both override and decoded v5/v6 wire values.
 * POSIX child per scenario gives each run a fresh firmware boot. No RF/RTOS
 * simulation: work counts do not prove ESP32 time, heap or power savings. */
#include "espnow_test_sdk.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "bsp_obd_dsp/espnow_link.h"
#include "bsp_obd_dsp/espnow_v6.h"
#include "bsp_obd_dsp/nvs_storage.h"
#include "vehicle/vp_manager.h"
#include "app_obd_dsp/app_event.h"
#include "app_obd_dsp/obd_data_cache.h"
#include <setjmp.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
#define ORIGIN INT64_C(1000000)
typedef enum {
    IDLE, LOCAL, REMOTE, V6, RESTART, THRESHOLD, SATURATE, STORM,
    PRE_TAKE, OTA_ACTIVE, OTA_IDLE, CREATE_FAIL, SLAVE, HIGH_CLOCK,
    ZERO_THRESHOLD, REMOTE_IDLE, SNAPSHOT_RESTART, SCENARIO_COUNT
} scenario_t;
static const char *const names[] = {
    "idle", "local", "remote", "v6", "restart", "threshold", "saturate", "storm",
    "before_take", "ota_active", "ota_idle", "create_fail", "slave", "64bit_clock",
    "zero_threshold", "remote_idle", "snapshot_restart"
};
static scenario_t scenario;
static TaskFunction_t worker;
static esp_now_recv_cb_t recv_fn, late_recv_fn;
static SemaphoreHandle_t wake_sem;
static jmp_buf task_exit;
static int64_t now_us, origin, end_us, last_frame_us = -1, test_start_us;
static unsigned creates, deletes, packets, configs, overrides, waits, gives, lock_depth, deinit_count;
static unsigned active_packets, idle_packets, slave_requests;
static bool reference_active, override_active, deleted, pre_injected, deinitialized;
static uint16_t override_rpm, expected_rpm;
static bool expected_active, snapshot_injected;
static bool oracle(uint16_t *rpm);
static nvs_user_cfg_t cfg;
typedef enum { START_LOCAL, START_REMOTE, SET_THRESHOLD, STOP_OTA } action_t;
static struct { int64_t at; action_t action; } events[1100];
static unsigned event_count, event_index;

void ble_test_enter(portMUX_TYPE *mux) { CHECK(!mux->locked); mux->locked = true; lock_depth++; }
void ble_test_exit(portMUX_TYPE *mux) { CHECK(mux->locked && lock_depth); mux->locked = false; lock_depth--; }
void link_test_log(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; CHECK(!lock_depth); }
const char *esp_err_to_name(esp_err_t err) { (void)err; return "stub"; }
int64_t esp_timer_get_time(void) { return now_us; }
static void remote_start(esp_now_recv_cb_t cb)
{
    uint8_t mac[6] = {1};
    esp_now_recv_info_t info = {.src_addr = mac};
    uint8_t ctrl[] = {(uint8_t)ESPNOW_MAGIC, (uint8_t)(ESPNOW_MAGIC >> 8), ESPNOW_V5_VERSION, 1, 0, 0};
    CHECK(cb); cb(&info, ctrl, sizeof(ctrl));
}
static void start(bool remote)
{
    test_start_us = now_us; reference_active = true;
    if (remote) remote_start(recv_fn); else espnow_link_trigger_linked_test();
    CHECK(espnow_link_linktest_active());
}
static void inject(action_t action)
{
    switch (action) {
    case START_LOCAL: start(false); break;
    case START_REMOTE: start(true); break;
    case SET_THRESHOLD: cfg.rpm_warn_threshold = 9000; break;
    case STOP_OTA: {
        espnow_link_pause_for_ota();
        CHECK(deleted && !espnow_link_linktest_active());
        CHECK(!override_active && !override_rpm);
        unsigned old_gives = gives;
        espnow_link_trigger_linked_test(); remote_start(late_recv_fn);
        CHECK(!espnow_link_linktest_active() && gives == old_gives);
        espnow_link_pause_for_ota();
        CHECK(deletes == 1 && deinit_count == 1);
        break;
    }
    }
}
static void event(int64_t relative, action_t action)
{
    CHECK(event_count < sizeof(events) / sizeof(events[0]));
    events[event_count].at = origin + relative;
    events[event_count++].action = action;
}
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *storage)
{ CHECK(!wake_sem); storage->token = false; wake_sem = storage; return storage; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem)
{ CHECK(sem == wake_sem && !lock_depth); gives++; sem->token = true; return pdTRUE; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks)
{
    CHECK(sem == wake_sem && !lock_depth && ticks && ticks <= 100);
    CHECK(++waits < 100000);  // a stale token/event must not cause a busy loop
    if (scenario == PRE_TAKE && !pre_injected) { pre_injected = true; start(false); }
    if (sem->token) { sem->token = false; return pdTRUE; }
    int64_t deadline = now_us + (int64_t)ticks * 1000;
    while (event_index < event_count && events[event_index].at <= deadline) {
        now_us = events[event_index].at;
        inject(events[event_index++].action);
        if (deleted) longjmp(task_exit, 1);
        if (sem->token) { sem->token = false; return pdTRUE; }
    }
    if (deadline >= end_us) { now_us = end_us; longjmp(task_exit, 1); }
    now_us = deadline;
    return pdFALSE;
}
BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack,
                       void *arg, UBaseType_t priority, TaskHandle_t *handle)
{
    (void)arg; CHECK(!lock_depth && priority == 4); creates++;
    CHECK(creates == 1);  // no independent 3KB link-test task
    if (scenario == SLAVE) { CHECK(!strcmp(name, "espnow_pres") && stack == 2560); }
    else CHECK(!strcmp(name, "espnow_tx") && stack == 3584);
    if (scenario == CREATE_FAIL) return pdFALSE;
    worker = fn; *handle = &worker; return pdPASS;
}
void vTaskDelete(TaskHandle_t task) { CHECK(task == &worker && !lock_depth); deletes++; deleted = true; }
void vTaskDelay(TickType_t ticks) { (void)ticks; CHECK(false); }
nvs_user_cfg_t nvs_cfg_read(void)
{ CHECK(!lock_depth); configs++; expected_active = oracle(&expected_rpm); return cfg; }
esp_err_t nvs_cfg_snapshot(nvs_user_cfg_t *out) { CHECK(!lock_depth); *out = cfg; return ESP_OK; }
esp_err_t nvs_cfg_update(const nvs_user_cfg_t *base, const nvs_user_cfg_t *updated)
{ (void)base; cfg = *updated; return ESP_OK; }
uint8_t nvs_device_position_get(void) { return 1; }
static bool oracle(uint16_t *rpm)
{
    int64_t ms = (now_us - test_start_us) / 1000;
    if (!reference_active || ms < 0 || ms >= 8300) { reference_active = false; *rpm = 0; return false; }
    uint64_t peak = (uint32_t)cfg.rpm_warn_threshold + 200;
    uint64_t result = ms < 5000 ? peak * (uint64_t)ms / 5000 :
        ms < 5800 ? peak : peak - peak * (uint64_t)(ms - 5800) / 2500;
    *rpm = result > UINT16_MAX ? UINT16_MAX : (uint16_t)result;
    return true;
}
void obd_data_rpm_override_set(bool enabled, uint16_t rpm)
{ CHECK(!lock_depth); override_active = enabled; override_rpm = rpm; overrides++; }
int ui_sweep_get_step(void) { return 77; }
int ui_intro_get_step(void) { return 9; }
const vp_profile_t *vp_manager_active(void) { return NULL; }
const char *vp_manager_active_id(void) { return "test.profile"; }
uint64_t signal_store_get_many(const int *slots, size_t count, int32_t *values)
{
    CHECK(!lock_depth);
    if (scenario == SNAPSHOT_RESTART && now_us == origin + 8300000 && !snapshot_injected) {
        snapshot_injected = true; start(true); // after completion sample, before packet assembly
    }
    uint64_t valid = 0;
    for (size_t i = 0; i < count; i++) {
        values[i] = 0;
        if (slots[i] == SIG_OBD_RPM) { values[i] = 1234; valid |= UINT64_C(1) << i; }
        if (slots[i] == SIG_SYS_OBD_LINK) { values[i] = SIG_LINK_DATA; valid |= UINT64_C(1) << i; }
    }
    return valid;
}
int signal_store_veh_count(void) { return 0; }
int signal_store_slot_by_id(uint16_t id) { for (int i = 0; i < SIG_STD_COUNT; i++) if (sig_registry[i].num_id == id) return i; return SIG_SLOT_NONE; }
bool signal_store_is_local(int slot) { (void)slot; return false; }
bool signal_store_set(int slot, int32_t value) { (void)slot; (void)value; return true; }
void signal_store_set_many(const int *slots, const int32_t *values, size_t count)
{ (void)slots; (void)values; (void)count; }
void signal_store_invalidate(int slot) { (void)slot; }
void signal_store_invalidate_many(const int *slots, size_t count) { (void)slots; (void)count; }
void signal_store_invalidate_vehicle(void) {}
bool app_event_send(app_event_type_t type, uint32_t data) { (void)type; (void)data; return true; }

void *heap_caps_malloc(size_t bytes, unsigned caps) { (void)caps; return malloc(bytes); }
esp_err_t esp_netif_init(void) { return ESP_OK; }
esp_err_t esp_event_loop_create_default(void) { return ESP_OK; }
esp_err_t esp_wifi_init(const wifi_init_config_t *c) { (void)c; return ESP_OK; }
esp_err_t esp_wifi_set_storage(int storage) { (void)storage; return ESP_OK; }
esp_err_t esp_wifi_set_mode(int mode) { (void)mode; return ESP_OK; }
esp_err_t esp_wifi_start(void) { return ESP_OK; }
esp_err_t esp_wifi_set_channel(uint8_t channel, int secondary) { (void)channel; (void)secondary; return ESP_OK; }
esp_err_t esp_wifi_set_ps(int ps) { (void)ps; return ESP_OK; }
esp_err_t esp_now_init(void) { return ESP_OK; }
esp_err_t esp_now_add_peer(const esp_now_peer_info_t *peer) { (void)peer; return ESP_OK; }
esp_err_t esp_now_register_recv_cb(esp_now_recv_cb_t cb) { recv_fn = late_recv_fn = cb; return ESP_OK; }
esp_err_t esp_now_unregister_recv_cb(void) { recv_fn = NULL; return ESP_OK; }
esp_err_t esp_now_unregister_send_cb(void) { return ESP_OK; }
esp_err_t esp_now_deinit(void) { deinit_count++; deinitialized = true; return ESP_OK; }
esp_err_t esp_now_send(const uint8_t *mac, const uint8_t *data, size_t len)
{
    (void)mac; CHECK(!lock_depth && !deinitialized);
    if (scenario == SLAVE) {
        CHECK(espnow_classify(data, len) == ESPNOW_PKT_V5_CTRL && len == 6 && data[3] == 1);
        slave_requests++; return ESP_OK;
    }
    const uint16_t rpm = expected_rpm;
    const bool active = expected_active;
    espnow_rx_frame_t frame;
    CHECK((scenario == V6 ? espnow_v6_decode(data, len, &frame) : espnow_v5_decode(data, len, &frame)));
    CHECK(frame.flags == (ESPNOW_FLAG_ELM | ESPNOW_FLAG_LINKED_EN | (active ? ESPNOW_FLAG_LINKTEST : 0)));
    CHECK(frame.sweep_step == 77 && frame.intro_step == 9);
    bool found = false;
    for (int i = 0; i < frame.n_rec; i++) {
        uint16_t id; int32_t value;
        CHECK(espnow_rx_record(&frame, i, &id, &value));
        if (id == ESPNOW_ID_RPM) { CHECK(value == (active ? rpm : 1234)); found = true; }
    }
    CHECK(found && override_active == active && override_rpm == (active ? rpm : 0));
    if (last_frame_us >= 0) CHECK(now_us - last_frame_us >= 20000);
    if (scenario == IDLE) CHECK(now_us == origin + (int64_t)packets * 100000);
    if (scenario == LOCAL || scenario == REMOTE || scenario == V6 || scenario == SATURATE || scenario == HIGH_CLOCK || scenario == ZERO_THRESHOLD) {
        int64_t offset = packets <= 415 ? (int64_t)packets * 20000 : 8300000 + (int64_t)(packets - 415) * 100000;
        CHECK(now_us == origin + offset);
    }
    if (scenario == PRE_TAKE) CHECK(now_us == origin + (int64_t)packets * 20000);
    last_frame_us = now_us;
    packets++; if (active) active_packets++; else idle_packets++;
    return ESP_OK;
}
static void setup(void)
{
    origin = scenario == HIGH_CLOCK ? INT64_C(5000000000000) : ORIGIN;
    now_us = origin; end_us = origin + 9000000;
    cfg.rpm_warn_threshold = scenario == SATURATE ? UINT16_MAX : scenario == ZERO_THRESHOLD ? 0 : 7000;
    cfg.rpm_warn_linked_en = 1;
    if (scenario == SLAVE) {
        espnow_link_start_slave(); espnow_link_trigger_linked_test();
        CHECK(slave_requests == 1 && !espnow_link_linktest_active()); return;
    }
    espnow_link_start_master(); CHECK(creates == 1 && wake_sem);
    if (scenario == CREATE_FAIL) {
        espnow_link_trigger_linked_test(); remote_start(recv_fn);
        CHECK(!espnow_link_linktest_active() && !gives && !worker);
        espnow_link_pause_for_ota(); CHECK(!deletes && deinit_count == 1); return;
    }
    switch (scenario) {
    case IDLE: end_us = origin + 1000000; break;
    case REMOTE: start(true); break;
    case V6: {
        uint8_t mac[6] = {1}, hello[ESPNOW_V6_HELLO_LEN];
        esp_now_recv_info_t info = {.src_addr = mac};
        recv_fn(&info, hello, (int)espnow_hello_encode(1, ESPNOW_CAP_V6_DATA, hello, sizeof(hello)));
        end_us = origin + 1000000; start(false); break;
    }
    case RESTART: start(false); event(8299000, START_REMOTE); end_us = origin + 17000000; break;
    case THRESHOLD: start(false); event(4999000, SET_THRESHOLD); break;
    case STORM:
        start(false); end_us = origin + 1000000;
        for (int64_t t = 1000; t < 1000000; t += 1000) event(t, START_REMOTE);
        break;
    case PRE_TAKE: end_us = origin + 1000000; break;
    case REMOTE_IDLE: event(23456, START_REMOTE); end_us = origin + 1000000; break;
    case SNAPSHOT_RESTART: start(false); end_us = origin + 17000000; break;
    case OTA_ACTIVE: start(false); event(123456, STOP_OTA); break;
    case OTA_IDLE: event(123456, STOP_OTA); break;
    default: start(false); break;
    }
}
static void run(scenario_t value)
{
    scenario = value; setup();
    if (worker && scenario != SLAVE) { if (!setjmp(task_exit)) worker(NULL); }
    CHECK(!lock_depth);
    if (scenario != SLAVE && scenario != CREATE_FAIL) CHECK(configs == packets);
    if (scenario == IDLE) CHECK(packets == 10 && waits == 10 && !overrides);
    if (scenario == LOCAL || scenario == REMOTE || scenario == SATURATE || scenario == HIGH_CLOCK || scenario == ZERO_THRESHOLD)
        CHECK(active_packets == 415 && idle_packets == 7 && overrides == 416);
    if (scenario == V6) CHECK(packets == 50 && active_packets == 50);
    if (scenario == STORM || scenario == PRE_TAKE) CHECK(packets == 50);
    if (scenario == REMOTE_IDLE) CHECK(packets == 50 && active_packets == 49 && last_frame_us == origin + 983456);
    if (scenario == SNAPSHOT_RESTART) CHECK(snapshot_injected && active_packets == 829 && idle_packets == 5);
    if (scenario == OTA_ACTIVE || scenario == OTA_IDLE) CHECK(deleted && deinit_count == 1);
    printf("%-14s packets=%u cfg=%u override=%u waits=%u gives=%u tasks=%u\n",
           names[value], packets, configs, overrides, waits, gives, creates);
    fflush(stdout);
}
int main(void)
{
    for (unsigned i = 0; i < SCENARIO_COUNT; i++) {
        pid_t pid = fork(); CHECK(pid >= 0);
        if (!pid) { run((scenario_t)i); _exit(0); }
        int status; CHECK(waitpid(pid, &status, 0) == pid);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    puts("17 real ESP-NOW worker/callback/lifecycle scenarios passed");
    return 0;
}
