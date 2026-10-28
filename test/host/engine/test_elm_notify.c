#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

typedef int esp_gatt_if_t;
typedef int esp_gatt_status_t;
typedef int esp_err_t;
typedef struct { uint16_t handle; } esp_gattc_descr_elem_t;
typedef struct { int len; union { uint16_t uuid16; } uuid; } esp_bt_uuid_t;
#define ESP_GATT_OK 0
#define ESP_GATT_NOT_FOUND 10
#define ESP_OK 0
#define ESP_UUID_LEN_16 2
#define UUID16_CCCD 0x2902
#define ESP_GATT_WRITE_TYPE_RSP 0
#define ESP_GATT_AUTH_REQ_NONE 0
#define ESP_LOGW(tag, ...) ((void)(tag))
static const char *TAG = "test";
static bool s_connected, s_notify_indicate, s_notify_configured;
static uint16_t s_notify_handle, s_cccd_handle, s_conn_id;
static int lookup_status, write_status, closes, writes, ready_calls;
static uint16_t descriptor_count, written_value;
static void esp_ble_gattc_close(int gif, uint16_t conn) { (void)gif; assert(conn == s_conn_id); closes++; }
static int esp_ble_gattc_get_descr_by_char_handle(int gif, uint16_t conn, uint16_t handle, esp_bt_uuid_t uuid, esp_gattc_descr_elem_t *d, uint16_t *n)
{ (void)gif; assert(conn == s_conn_id && handle == s_notify_handle && uuid.uuid.uuid16 == UUID16_CCCD); *n = descriptor_count; d[0].handle = 42; return lookup_status; }
static int esp_ble_gattc_write_char_descr(int gif, uint16_t conn, uint16_t handle, unsigned len, const uint8_t *value, int type, int auth)
{ (void)gif; (void)type; (void)auth; assert(conn == s_conn_id && handle == 42 && len == 2); written_value = value[0]; writes++; return write_status; }
static void notify_ready(void) { ready_calls++; }
#include "elm_notify.inc"
int main(void)
{
    s_connected = true; s_conn_id = 7; s_notify_handle = 12;
    notify_registered(1, 0, 99); assert(!writes && !closes);
    notify_registered(1, 9, 12); assert(closes == 1 && !writes && !s_notify_configured);
    descriptor_count = 1; notify_registered(1, 0, 12);
    assert(writes == 1 && written_value == 1 && s_cccd_handle == 42 && !s_notify_configured && !ready_calls);
    s_notify_indicate = true; write_status = 1; notify_registered(1, 0, 12);
    assert(writes == 2 && written_value == 2 && closes == 2 && !s_notify_configured);
    lookup_status = 3; notify_registered(1, 0, 12);
    assert(closes == 3 && !s_notify_configured);
    lookup_status = ESP_GATT_NOT_FOUND; descriptor_count = 0; write_status = 0; notify_registered(1, 0, 12);
    assert(s_notify_configured && ready_calls == 1);
    s_connected = false; notify_registered(1, 0, 12); assert(ready_calls == 1);
    return 0;
}
