#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define BOOTMEDIA_SD_MARGIN (64 * 1024)
static bool mounted;
static uint64_t available;
static int reads;
static bool sd_card_is_mounted(void) { return mounted; }
static uint64_t sd_card_get_free_bytes(void) { reads++; return available; }
#include "ota_space.inc"
int main(void)
{
    available = UINT64_MAX; assert(sd_available_bytes() == 0 && reads == 0);
    mounted = true; available = BOOTMEDIA_SD_MARGIN - 1;
    assert(sd_available_bytes() == 0 && !sd_can_hold(1));
    available = BOOTMEDIA_SD_MARGIN; assert(sd_available_bytes() == 0 && !sd_can_hold(1));
    available++; assert(sd_available_bytes() == 1 && sd_can_hold(1) && !sd_can_hold(2));
    available = UINT64_MAX; assert(!sd_can_hold(UINT64_MAX));
    return 0;
}
