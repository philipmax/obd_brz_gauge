/* Host-only access to the actual private conversion helpers. */
#include "rpn_integer_probe.h"
#define vp_rpn_op_arity ri_op_arity
#define vp_rpn_op_name ri_op_name
#define vp_rpn_err_str ri_err_str
#define vp_rpn_verify ri_verify
#define vp_rpn_extract_raw ri_extract_raw
#define vp_rpn_eval ri_eval
#ifndef RI_SOURCE
#define RI_SOURCE "vp_rpn.c"
#endif
#include RI_SOURCE
bool ri_to_u32(float f, uint32_t *out) { return to_u32(f, out); }
bool ri_to_shift(float f, uint32_t *out) { return to_shift(f, out); }
