/* Frozen pre-optimization acquisition path; shared helpers remain unchanged. */
static tic_entry_t *ta_reference(te_build_t *b, const tl_elem_t *el, const te_vkey_t *k, int pol,
                               int *status)
{
    te_ctx_t *c = b->c;
    const te_image_t *im = &c->images[k->asset];
    tic_variant_t v;
    *status = TE_ACQ_OK;
    if (!te_make_variant(el, im, k, &v)) {
        *status = TE_ACQ_EMPTY;
        return NULL;
    }
    tic_entry_t *e = tic_lookup(&c->cache, c->manifest_crc, (uint16_t)k->asset, &v);
    if (e) {
        if (pol != TE_POL_REQUIRED && b->cost + e->cost > b->budget) {
            tic_release(&c->cache, e);
            *status = TE_ACQ_BUDGET;
            return NULL;
        }
        b->cost += e->cost;
        return e;
    }

    tpx_alloc_t a = TE_ALLOC_INIT;
    tpx_src_t src = {c->map + im->offset, im->size, (tpx_fmt_t)im->fmt, im->w, im->h};
    uint8_t *rs = NULL;
    tpx_plan_t plan;
    memset(&plan, 0, sizeof(plan));
    tpx_err_t r;
    if (v.kind == TIC_KIND_POINTER) {
        r = tpx_resample(&src, v.dw, v.dh, &a, &rs);
        if (r == TPX_ERR_NOMEM) {
            te_evict_all(c);
            r = tpx_resample(&src, v.dw, v.dh, &a, &rs);
        }
        if (r != TPX_OK) {
            *status = r == TPX_ERR_NOMEM ? TE_ACQ_NOMEM : TE_ACQ_ERR;
            return NULL;
        }
        src.data = rs;
        src.size = (size_t)v.dw * v.dh * 4u;
        src.fmt = TPX_FMT_RGBA8888;
        src.w = v.dw;
        src.h = v.dh;
        tpx_opts_t o = {0, 0, v.dw, v.dh, false, 0, 0, TPX_CROP_ALL};
        r = tpx_plan(&src, &o, &plan);
        if (r == TPX_OK && plan.cf != TPX_CF_NONE) {
            // keep the pivot inside the bitmap so LVGL rotates around a real pixel
            int32_t px = te_max32(0, te_min32(v.piv_x, v.dw - 1));
            int32_t py = te_max32(0, te_min32(v.piv_y, v.dh - 1));
            int32_t x0 = te_min32(plan.ox, px), y0 = te_min32(plan.oy, py);
            int32_t x1 = te_max32(plan.ox + plan.w, px + 1), y1 = te_max32(plan.oy + plan.h, py + 1);
            tpx_opts_t o2 = {x0, y0, x1 - x0, y1 - y0, false, 0, 0, TPX_CROP_NONE};
            r = tpx_plan(&src, &o2, &plan);
        }
    } else {
        tpx_opts_t o = {0, 0, v.win_w, v.win_h, v.round_dc != 0, v.scr_x, v.scr_y, v.crop};
        r = tpx_plan(&src, &o, &plan);
    }
    if (r != TPX_OK) {
        te_free(rs);
        *status = TE_ACQ_ERR;
        return NULL;
    }
    if (v.has_tint) plan.zero_copy = false;   // never recolor the mapped flash
    size_t cost = (plan.cf == TPX_CF_NONE || plan.zero_copy) ? 0 : plan.size;
    if (pol != TE_POL_REQUIRED && b->cost + cost > b->budget) {
        te_free(rs);
        *status = TE_ACQ_BUDGET;
        return NULL;
    }
    if (pol == TE_POL_DECOR && cost &&
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < cost + TE_PSRAM_RESERVE) {
        te_free(rs);
        *status = TE_ACQ_BUDGET;
        return NULL;
    }
    tic_make_room(&c->cache, cost, c->cache.pinned + b->budget);

    tpx_img_t img;
    r = tpx_render(&src, &plan, &a, &img);
    if (r == TPX_ERR_NOMEM) {
        te_evict_all(c);
        r = tpx_render(&src, &plan, &a, &img);
    }
    te_free(rs);   // the render always copies out of the resampled bitmap
    if (r != TPX_OK) {
        *status = r == TPX_ERR_NOMEM ? TE_ACQ_NOMEM : TE_ACQ_ERR;
        return NULL;
    }
    if (v.has_tint && img.owned) {
        tpx_recolor(img.owned, (size_t)plan.w * plan.h, v.tint, TR_RECOLOR_OPA,
                    LV_COLOR_MIX_ROUND_OFS);
    }
    e = tic_insert(&c->cache, c->manifest_crc, (uint16_t)k->asset, &v, &img);
    if (!e) {
        *status = TE_ACQ_NOMEM;
        return NULL;
    }
    b->cost += e->cost;
    return e;
}
