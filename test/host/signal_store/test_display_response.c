#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "export_path/ui_disp_item.h"
struct _lv_obj_t { char text[40]; unsigned text_updates, colors; };
static const char *lv_label_get_text(lv_obj_t *obj) { return obj->text; }
void disp_item_set_text(lv_obj_t *obj, disp_item_t item, int32_t value, bool valid)
{ (void)item; obj->text_updates++; if (valid) snprintf(obj->text, sizeof(obj->text), "%ld", (long)value); else strcpy(obj->text, "--"); }
static void disp_item_set_value_color_throttled(lv_obj_t *obj, disp_item_t item, int32_t value, bool valid)
{ (void)item; (void)value; (void)valid; obj->colors++; }
#include "display_update.inc"
int main(void)
{
    const disp_item_t dynamic[] = {DISP_ITEM_RPM, DISP_ITEM_SPEED, DISP_ITEM_BOOST, DISP_ITEM_TPS, DISP_ITEM_LOAD};
    const int32_t target[] = {4000, 120, -65, 90, 80};
    for (unsigned i=0; i<sizeof(dynamic)/sizeof(dynamic[0]); i++) {
        lv_obj_t obj = {.text="10"}; int32_t value = 10;
        disp_item_update(&value, &obj, dynamic[i], target[i], true, 5);
        assert(value == target[i] && obj.text_updates == 1); // latest sample on the first refresh
        disp_item_update(&value, &obj, dynamic[i], target[i], true, 5);
        assert(obj.text_updates == 1); // unchanged values do not redraw
        disp_item_update(&value, &obj, dynamic[i], 0, false, 5);
        assert(value == target[i] && !strcmp(obj.text, "--"));
        disp_item_update(&value, &obj, dynamic[i], target[i] + 1, true, 5);
        assert(value == target[i] + 1); // recovery also has no ramp
    }
    lv_obj_t temp = {.text="10"}; int32_t value = 10;
    disp_item_update(&value, &temp, DISP_ITEM_CLT, 80, true, 5);
    assert(value > 10 && value < 80); // slower sensors retain presentation smoothing
    return 0;
}
