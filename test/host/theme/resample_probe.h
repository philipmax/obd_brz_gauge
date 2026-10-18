#pragma once
#include "theme_pixconv.h"
uint8_t rs_actual_u8(float value);
uint8_t rs_reference_u8(float value);
tpx_err_t rs_actual(const tpx_src_t *, uint16_t, uint16_t, const tpx_alloc_t *, uint8_t **);
tpx_err_t rs_reference(const tpx_src_t *, uint16_t, uint16_t, const tpx_alloc_t *, uint8_t **);
tpx_err_t rs_previous(const tpx_src_t *, uint16_t, uint16_t, const tpx_alloc_t *, uint8_t **);
bool rs_axis_equal(uint16_t sn, uint16_t dn, size_t *old_bytes, size_t *new_bytes);
