#pragma once
/*
 * Private glue between vp_validate.c and vp_compile.c: JSON field readers
 * and the request-key derivation both of them must agree on.
 */

#include "vp_profile.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Resolve absent standard signals. Returns root unchanged when no expansion is
 * needed, otherwise a new tree in *owned (caller deletes it). NULL means OOM.
 * Explicit declarations are retained in order; inherited signals are appended.
 */
const cJSON *vp_int_resolve_defaults(const cJSON *root, cJSON **owned, vp_issues_t *issues);

/* Validate an already resolved tree; used by the compiler to avoid expanding twice. */
size_t vp_int_validate_resolved(const cJSON *root, const vp_validate_cfg_t *cfg, vp_issues_t *out);

/* Number that is an integer in [lo, hi]. */
bool vp_int_json_int(const cJSON *it, double lo, double hi, long *out);

/* Finite number in [lo, hi]. */
bool vp_int_json_num(const cJSON *it, double lo, double hi, double *out);

/* [lo, hi] pair of finite numbers, lo <= hi, |v| <= VP_RPN_MAX_CONST. */
bool vp_int_json_pair(const cJSON *it, double *lo, double *hi);

/* Effective default header of the profile (bus.hdr, else 7E0). */
uint32_t vp_int_bus_hdr(const cJSON *root, bool *functional);

/*
 * Build the transport key of a request source (req/raw). The key is what
 * the compiler stores in the request table and what both modules use to
 * count distinct requests. Returns false if src is not a well-formed
 * request source.
 */
bool vp_int_req_key(const cJSON *src, uint32_t bus_hdr, vp_request_t *key);

/* Number of distinct request keys over all request sources. */
size_t vp_int_count_requests(const cJSON *signals, uint32_t bus_hdr);

/* Source kind from its keys: VP_SRC_REQ, VP_SRC_CAN or 0 (malformed). */
int vp_int_src_kind(const cJSON *src);

/* X field of a source; false when the source has none. */
bool vp_int_src_xfield(const cJSON *src, vp_rpn_raw_t *x, uint16_t *data_len);

/* Iterate sources over all signals (si, sj start at 0). Returns NULL at the
 * end; after a hit, si is the signal index and sj - 1 the source index. */
const cJSON *vp_int_next_src(const cJSON *signals, int *si, int *sj, const cJSON **sig);

#ifdef __cplusplus
}
#endif
