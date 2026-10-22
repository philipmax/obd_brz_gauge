#pragma once
#include <stdbool.h>
#include <stdint.h>
bool wr_identity(float value, int32_t *out);
bool wr_reference(float value, int32_t *out);
bool wr_compare(float value, unsigned config, unsigned index);
