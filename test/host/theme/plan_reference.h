#pragma once
#include "theme_pixconv.h"

/* Original per-pixel planner, compiled only into tests and benchmarks. */
tpx_err_t tpx_plan_reference(const tpx_src_t *src, const tpx_opts_t *opts, tpx_plan_t *plan);
