#pragma once

#include <stdbool.h>

#define SD_DIR_THEMES "/OBD/THEMES"
#define SD_DIR_VIDEO "/OBD/VIDEO"

bool sd_card_is_mounted(void);
bool sd_card_exists(const char *path);
