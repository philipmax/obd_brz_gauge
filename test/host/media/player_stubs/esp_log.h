#pragma once
void player_test_log(const char *tag, const char *fmt, ...);
#define ESP_LOGW(...) player_test_log(__VA_ARGS__)
#define ESP_LOGE(...) player_test_log(__VA_ARGS__)
#define ESP_LOGD(...) player_test_log(__VA_ARGS__)
#define ESP_LOGI(...) player_test_log(__VA_ARGS__)
