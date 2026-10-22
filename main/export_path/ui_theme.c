// ============================================================
//  ui_theme.c — active-theme state and accessors
//
//  The theme DEFINITIONS live in ui_theme_generated.c, produced by
//  tools/gen_themes.py from themes/registry.txt + themes/*/<id>/theme.toml.
//  Nothing in this file needs to change when a theme is added.
//
//  Only DECORATIVE colors are themed. Alert/ON/warning colors are global
//  (UI_SEM_* in ui_theme.h) so their meaning never changes with the skin.
// ============================================================

#include "ui_theme.h"
#include "bsp_obd_dsp/nvs_storage.h"
#include "esp_log.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "ui_theme";

static uint8_t s_active = 0;

void ui_theme_init(void)
{
    // Reuses the (previously unused) theme_cfg.theme byte — no NVS layout change.
    uint8_t idx = nvs_cfg_read().theme_cfg.theme;
    s_active = (idx < ui_theme_count()) ? idx : 0;
    ESP_LOGD(TAG, "active slot=%u id=%s name=%s (%u registered)",
             s_active, g_ui_themes[s_active]->id, g_ui_themes[s_active]->name,
             ui_theme_count());
}

uint8_t ui_theme_count(void)
{
    return g_ui_theme_count;
}

const ui_theme_t *ui_theme_get(uint8_t idx)
{
    return (idx < ui_theme_count()) ? g_ui_themes[idx] : g_ui_themes[0];
}

const ui_theme_t *ui_theme_active(void)
{
    return g_ui_themes[s_active];
}

void ui_theme_set_active(uint8_t idx)
{
    if (idx >= ui_theme_count()) idx = 0;
    s_active = idx;
    nvs_user_cfg_t base;
    nvs_cfg_snapshot(&base);
    nvs_user_cfg_t cfg = base;
    cfg.theme_cfg.theme = idx;
    nvs_cfg_update(&base, &cfg);
}

const char *ui_theme_names_joined(void)
{
    static char *s_joined = NULL;
    if (s_joined) return s_joined;

    size_t need = 1;   // trailing NUL
    for (uint8_t i = 0; i < g_ui_theme_count; i++) {
        need += strlen(g_ui_themes[i]->name);
    }
    if (g_ui_theme_count > 1) need += g_ui_theme_count - 1;

    char *buf = malloc(need);
    if (!buf) {
        // Out of memory this early is fatal elsewhere; degrade to one option
        // rather than handing the roller a NULL.
        ESP_LOGE(TAG, "name buffer alloc failed (%u bytes)", (unsigned)need);
        return g_ui_themes[0]->name;
    }

    char *dst = buf;
    for (uint8_t i = 0; i < g_ui_theme_count; i++) {
        if (i > 0) *dst++ = '\n';
        size_t name_len = strlen(g_ui_themes[i]->name);
        memcpy(dst, g_ui_themes[i]->name, name_len);
        dst += name_len;
    }
    *dst = '\0';
    s_joined = buf;
    return s_joined;
}

uint32_t ui_theme_color(ui_color_role_t role)
{
    if ((unsigned)role >= UI_COLOR__COUNT) return 0xFFFFFF;
    return g_ui_themes[s_active]->colors[role];
}

lv_color_t ui_theme_color_lv(ui_color_role_t role)
{
    return lv_color_hex(ui_theme_color(role));
}
