#pragma once
/* Host-only platform shims. LVGL, rules and page code are real; hardware,
 * allocator capabilities and scheduling are outside this test's scope. */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
typedef int esp_err_t;
typedef int esp_partition_type_t;
typedef int esp_partition_subtype_t;
typedef struct { int unused; uint32_t address, size; int type, subtype; const char *label; } esp_partition_t;
typedef uintptr_t esp_partition_mmap_handle_t;
typedef void *SemaphoreHandle_t;
typedef uint32_t TickType_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_NO_MEM 2
#define ESP_ERR_INVALID_STATE 3
#define MALLOC_CAP_SPIRAM 1u
#define MALLOC_CAP_8BIT 2u
#define CONFIG_OBD_PERF_STATS 0
#define ESP_LOGW(tag, ...) do { (void)(tag); if (0) fprintf(stderr, __VA_ARGS__); } while (0)
#define ESP_LOGE(tag, ...) do { (void)(tag); if (0) fprintf(stderr, __VA_ARGS__); } while (0)
#define ESP_LOGI(tag, ...) do { (void)(tag); if (0) fprintf(stderr, __VA_ARGS__); } while (0)
#ifdef TE_ACQUIRE_TRACE
void *ta_heap_malloc(size_t n);
void *ta_heap_calloc(size_t n, size_t z);
void ta_heap_free(void *p);
size_t ta_heap_free_size(void);
static inline void *heap_caps_malloc(size_t n, unsigned caps)
{ (void)caps; return ta_heap_malloc(n); }
static inline void *heap_caps_calloc(size_t n, size_t z, unsigned caps)
{ (void)caps; return ta_heap_calloc(n, z); }
static inline void heap_caps_free(void *p) { ta_heap_free(p); }
static inline size_t heap_caps_get_free_size(unsigned caps)
{ (void)caps; return ta_heap_free_size(); }
#else
static inline void *heap_caps_malloc(size_t n, unsigned caps)
{ (void)caps; return malloc(n); }
static inline void *heap_caps_calloc(size_t n, size_t s, unsigned caps)
{ (void)caps; return calloc(n, s); }
static inline void heap_caps_free(void *p) { free(p); }
static inline size_t heap_caps_get_free_size(unsigned caps)
{ (void)caps; return 8u * 1024u * 1024u; }
#endif
int64_t esp_timer_get_time(void);
static inline const char *esp_err_to_name(esp_err_t e) { (void)e; return "host"; }

#define ESP_ERR_NOT_FOUND 4
#define ESP_ERR_INVALID_SIZE 5
#define ESP_ERR_NOT_SUPPORTED 6
#define ESP_ERR_TIMEOUT 7
#define ESP_ERR_INVALID_RESPONSE 8
#define ESP_ERR_INVALID_CRC 9
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define pdTRUE 1
#define pdPASS 1
#define tskIDLE_PRIORITY 0
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define taskENTER_CRITICAL(m) ((void)(m))
#define taskEXIT_CRITICAL(m) ((void)(m))
