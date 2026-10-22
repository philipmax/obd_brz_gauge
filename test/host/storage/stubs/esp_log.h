#pragma once
typedef enum { ESP_LOG_NONE, ESP_LOG_ERROR, ESP_LOG_WARN, ESP_LOG_INFO, ESP_LOG_DEBUG } esp_log_level_t;
esp_log_level_t esp_log_level_get(const char *tag);
void esp_log_level_set(const char *tag, esp_log_level_t level);
void storage_test_log(const char *tag, const char *fmt, ...);
#define ESP_LOGI(...) storage_test_log(__VA_ARGS__)
#define ESP_LOGW(...) storage_test_log(__VA_ARGS__)
#define ESP_LOGE(...) storage_test_log(__VA_ARGS__)
#define ESP_LOGD(...) storage_test_log(__VA_ARGS__)
