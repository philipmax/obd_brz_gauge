#pragma once
#include "vp_rpn.h"
bool ri_to_u32(float f, uint32_t *out);
bool ri_to_shift(float f, uint32_t *out);
vp_rpn_err_t ri_eval(const vp_rpn_insn_t *, size_t, const uint8_t *, size_t,
                     const vp_rpn_env_t *, float *);
