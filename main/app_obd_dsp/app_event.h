#pragma once
// ================================================================
//  app_event.h — cross-task event queue
//
//  Queue for the selected cross-task UI events below.
//  Producer (ESP-NOW receive callback): app_event_send()
//  Consumer (LVGL task):                app_event_recv()
// ================================================================

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    // ESP-NOW → UI
    APP_EVT_ESPNOW_SYNC_SLOT = 1,   // data.u8 = slot (0-7)
    APP_EVT_ESPNOW_INTRO_STEP,      // data.u8 = intro_step (boot animation progress)
    APP_EVT_ESPNOW_THRESH_SYNC,     // data.u16 = RPM alarm threshold synced from another gauge (linkage mode)
} app_event_type_t;

typedef struct {
    app_event_type_t type;
    union {
        uint8_t  u8;
        uint16_t u16;
        uint32_t u32;
    } data;
} app_event_t;

#define APP_EVENT_QUEUE_DEPTH 16

// Init (called once from app_main)
void app_event_init(void);

// Send an event (any task, non-blocking)
bool app_event_send(app_event_type_t type, uint32_t data);

// Receive an event (non-blocking, returns false when there is no event)
bool app_event_recv(app_event_t *out);

#ifdef __cplusplus
}
#endif
