/**
 * vp_loader.c - Vehicle profile loader implementation
 */

#include "vp_loader.h"
#include "vp_builtin.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "vp_loader";

// Global active profile pointer
static const vp_profile_t *s_active_profile = NULL;

void vp_loader_init(void)
{
    // Default to generic OBD2 profile
    s_active_profile = vp_builtin_generic();
    ESP_LOGI(TAG, "Initialized with generic OBD2 profile");
}

const vp_profile_t* vp_loader_get_active(void)
{
    if (!s_active_profile) {
        ESP_LOGW(TAG, "Active profile is NULL, using generic fallback");
        return vp_builtin_generic();
    }
    return s_active_profile;
}

bool vp_loader_load_by_index(uint8_t index)
{
    if (index != 0) {
        ESP_LOGE(TAG, "Profile index %d out of range (only generic available)", index);
        return false;
    }
    
    s_active_profile = vp_builtin_generic();
    ESP_LOGI(TAG, "Loaded profile: %s", s_active_profile->name);
    return true;
}

uint8_t vp_loader_get_count(void)
{
    return 1;  // Only generic profile for now
}

const char* vp_loader_get_name_by_index(uint8_t index)
{
    if (index != 0) {
        return NULL;
    }
    return vp_builtin_generic()->name;
}
