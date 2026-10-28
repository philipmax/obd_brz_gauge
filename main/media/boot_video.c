/*
 * boot_video.c — boot animation facade (see boot_video.h).
 *
 * SD OBV1: validated at create (header + index, and the jpeg self-test is
 *   warmed up so it does not delay the first frame), then played by
 *   video_player in FULLSCREEN mode from the first update. out_obj = NULL.
 * Legacy delta_varint (SD bootmedia.raw): streamed in
 *   16 KB chunks into a PSRAM canvas shown by a video_view; frames are
 *   applied in update() as elapsed time passes (no frame may be skipped,
 *   every frame is a delta) and only the dirty rectangle is invalidated.
 *   destroy() leaves the view on its parent showing the last frame; the
 *   canvas is freed when the parent screen is deleted.
 *
 * All calls from the LVGL task.
 */

#include "media/boot_video.h"

#include <stdio.h>
#include <string.h>

#include "app_obd_dsp/boot_media_mount.h" /* legacy format constants only */
#include "app_obd_dsp/storage_util.h"
#include "bsp_obd_dsp/sd_card.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "media/delta_varint.h"
#include "media/media_jpeg.h"
#include "media/obv1_format.h"
#include "media/video_player.h"
#include "media/video_player_internal.h"
#include "media/video_view.h"

static const char *TAG = "boot_video";

static void view_deleted_cb(lv_event_t *e);

#define BV_CHUNK            (16u * 1024u)
#define BV_OBV_GRACE_MS     3000u   /* finish even if the player never reports the end */
#define BV_LEGACY_GRACE_MS  1000u
#define BV_MAX_STEPS        8u      /* legacy frames applied per update */
#define BV_STILL_DURATION_MS 2500u
#define BV_JPEG_MAX_BYTES   (1024u * 1024u)
#define BV_CANVAS_MAX       360u

typedef struct {
    char             obv_path[128];
    boot_video_src_t src;
    uint32_t         duration_ms;
    uint32_t         elapsed_ms;
    /* OBV */
    uint32_t         session_id;
    bool             obv_started;
    bool             obv_failed;
    bool             timed;
    /* still JPEG */
    uint8_t         *image_data;
    /* legacy */
    dv_manifest_t    m;
    dv_stream_t      dv;
    FILE            *f;         /* SD bootmedia.raw */
    uint8_t         *mem;       /* chunk + cell edges (PSRAM) */
    uint16_t        *pixels;    /* canvas (PSRAM) */
    lv_obj_t        *view;
    uint32_t         applied;   /* frames on the canvas */
    bool             error;
} boot_video_t;

static boot_video_t *s_bv;

/* ------------------------------------------------------------- probes */

/* Header (and with index = true the index CRC + entries) of an OBV1 file. */
static bool obv_check(const char *path, bool index, obv1_info_t *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    bool ok = false;
    uint32_t hdr_words[OBV1_HEADER_SIZE / 4];
    uint8_t *hdr = (uint8_t *)hdr_words;
    obv1_info_t info;
    long size = -1;
    if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
    if (size > 0 && fseek(f, 0, SEEK_SET) == 0 &&
        sd_card_read(f, hdr, OBV1_HEADER_SIZE) == OBV1_HEADER_SIZE) {
        int rc = obv1_parse_header(hdr, OBV1_HEADER_SIZE, (uint64_t)size, 0, &info);
        ok = rc == OBV1_OK;
        if (!ok) ESP_LOGW(TAG, "%s: %s", path, obv1_strerror(rc));
    }
    if (ok && index) {
        size_t len = (size_t)info.frame_count * obv_index_stride(&info);
        uint8_t *buf = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ok = buf && fseek(f, (long)info.index_offset, SEEK_SET) == 0 &&
             sd_card_read(f, buf, len) == len;
        if (ok) {
            int rc = obv1_check_index(&info, buf, len);
            ok = rc == OBV1_OK;
            if (!ok) ESP_LOGW(TAG, "%s: index %s", path, obv1_strerror(rc));
        }
        heap_caps_free(buf);
    }
    fclose(f);
    if (ok && out) *out = info;
    return ok;
}

static bool jpeg_open(boot_video_t *bv, lv_obj_t *parent, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    bool ok = false;
    long size = -1;
    if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
    if (size < 4 || size > BV_JPEG_MAX_BYTES || fseek(f, 0, SEEK_SET) != 0) goto done;

    bv->image_data = heap_caps_aligned_alloc(16, (size_t)size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!bv->image_data || sd_card_read(f, bv->image_data, (size_t)size) != (size_t)size) goto done;

    media_jpeg_t *jpeg = NULL;
    media_jpeg_info_t info = {0};
    esp_err_t err = media_jpeg_open(false, &jpeg);
    if (err == ESP_OK) err = media_jpeg_begin(jpeg, bv->image_data, (size_t)size, &info);
    if (err != ESP_OK || !info.width || !info.height ||
        info.width > BV_CANVAS_MAX || info.height > BV_CANVAS_MAX) {
        ESP_LOGW(TAG, "%s: invalid boot JPEG (%s)", path, esp_err_to_name(err));
        media_jpeg_close(jpeg);
        goto done;
    }

    size_t bytes = (size_t)info.width * info.height * 2u;
    bv->pixels = heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!bv->pixels) {
        media_jpeg_close(jpeg);
        goto done;
    }
    err = media_jpeg_decode_frame(jpeg, (uint8_t *)bv->pixels, bytes);
    media_jpeg_close(jpeg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s: JPEG decode failed (%s)", path, esp_err_to_name(err));
        goto done;
    }

    bv->view = video_view_create(parent);
    if (!bv->view) goto done;
    lv_obj_add_event_cb(bv->view, view_deleted_cb, LV_EVENT_DELETE, NULL);
    lv_obj_set_size(bv->view, BV_CANVAS_MAX, BV_CANVAS_MAX);
    lv_obj_center(bv->view);
    lv_obj_update_layout(bv->view);
    video_view_set_frame(bv->view, bv->pixels, info.width, info.height);
    bv->duration_ms = BV_STILL_DURATION_MS;
    bv->src = BOOT_VIDEO_SRC_SD_JPEG;
    ok = true;

done:
    fclose(f);
    heap_caps_free(bv->image_data);
    bv->image_data = NULL;
    if (!ok) {
        heap_caps_free(bv->pixels);
        bv->pixels = NULL;
    }
    return ok;
}

/* Legacy manifest from the head of SD bootmedia.raw. */
static bool legacy_manifest(FILE *f, dv_manifest_t *m)
{
    uint8_t *txt = heap_caps_malloc(BOOT_MEDIA_MANIFEST_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!txt) return false;
    bool ok = f && fseek(f, 0, SEEK_SET) == 0 &&
              sd_card_read(f, txt, BOOT_MEDIA_MANIFEST_MAX) == BOOT_MEDIA_MANIFEST_MAX;
    ok = ok && dv_parse_manifest((const char *)txt, BOOT_MEDIA_MANIFEST_MAX, m) == DV_OK;
    heap_caps_free(txt);
    return ok;
}

/* ------------------------------------------------------------- legacy */

static size_t read_sd(void *ctx, uint32_t off, uint8_t *dst, size_t len)
{
    boot_video_t *bv = ctx;
    if (fseek(bv->f, (long)(BOOT_MEDIA_BIN_OFFSET + off), SEEK_SET) != 0) return 0;
    return sd_card_read(bv->f, dst, len);
}

static void view_deleted_cb(lv_event_t *e)
{
    if (s_bv && s_bv->view == lv_event_get_target(e)) s_bv->view = NULL;
}

/* After destroy the view keeps showing the last frame (screen fade, showroom)
 * and frees the canvas when its screen is deleted. Runs after video_view's own
 * DELETE handler, so nothing draws from the canvas any more. */
static void canvas_free_cb(lv_event_t *e)
{
    heap_caps_free(lv_event_get_user_data(e));
}

static void legacy_release(boot_video_t *bv)
{
    if (bv->view) {
        lv_obj_t *v = bv->view;
        bv->view = NULL;
        lv_obj_del(v);
    }
    heap_caps_free(bv->pixels);
    heap_caps_free(bv->mem);
    bv->pixels = NULL;
    bv->mem = NULL;
    if (bv->f) {
        fclose(bv->f);
        bv->f = NULL;
    }
}

static bool legacy_open(boot_video_t *bv, lv_obj_t *parent)
{
    bv->f = fopen(SD_BOOT_LEGACY, "rb");
    if (!bv->f) return false;
    long size = -1;
    if (fseek(bv->f, 0, SEEK_END) == 0) size = ftell(bv->f);
    if (size <= (long)BOOT_MEDIA_BIN_OFFSET || (uint64_t)size > UINT32_MAX) return false;
    uint32_t limit = (uint32_t)size - BOOT_MEDIA_BIN_OFFSET;
    if (!legacy_manifest(bv->f, &bv->m)) return false;

    const dv_manifest_t *m = &bv->m;
    size_t edges = ((size_t)m->grid_w + 1u + m->grid_h + 1u) * sizeof(uint16_t);
    size_t canvas = (size_t)m->canvas_w * m->canvas_h * sizeof(uint16_t);
    bv->mem = heap_caps_malloc(BV_CHUNK + edges, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bv->pixels = heap_caps_malloc(canvas, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!bv->mem || !bv->pixels) {
        ESP_LOGE(TAG, "no PSRAM for the legacy canvas (%u B)", (unsigned)canvas);
        return false;
    }
    uint16_t *x_edges = (uint16_t *)(bv->mem + BV_CHUNK);
    uint16_t *y_edges = x_edges + m->grid_w + 1u;
    int rc = dv_stream_init(&bv->dv, m, read_sd, bv, limit, bv->mem,
                            BV_CHUNK, x_edges, y_edges, bv->pixels, LV_COLOR_16_SWAP != 0);
    if (rc == DV_OK) rc = dv_stream_next_frame(&bv->dv);
    if (rc != DV_OK) {
        ESP_LOGW(TAG, "legacy stream invalid (%d)", rc);
        return false;
    }
    bv->applied = 1;

    bv->view = video_view_create(parent);
    if (!bv->view) return false;
    lv_obj_add_event_cb(bv->view, view_deleted_cb, LV_EVENT_DELETE, NULL);
    lv_obj_set_size(bv->view, m->canvas_w, m->canvas_h);
    lv_obj_center(bv->view);
    lv_obj_update_layout(bv->view);
    video_view_set_frame(bv->view, bv->pixels, m->canvas_w, m->canvas_h);
    bv->duration_ms = m->duration_ms;
    ESP_LOGI(TAG, "legacy %s: %ux%u grid %ux%u, %lu frames @ %u fps", "SD",
             m->canvas_w, m->canvas_h, m->grid_w, m->grid_h, (unsigned long)m->frame_count, m->fps);
    return true;
}

static void legacy_update(boot_video_t *bv, uint32_t elapsed_ms)
{
    if (bv->error) return;
    if (!bv->view) {   /* parent deleted under us */
        bv->error = true;
        return;
    }
    const dv_manifest_t *m = &bv->m;
    lv_area_t dirty = {0};
    bool any = false;
    uint32_t steps = 0;
    while (bv->applied < m->frame_count && steps < BV_MAX_STEPS &&
           (uint64_t)elapsed_ms * m->fps >= (uint64_t)bv->applied * 1000u) {
        int rc = dv_stream_next_frame(&bv->dv);
        if (rc != DV_OK) {
            ESP_LOGW(TAG, "legacy frame %lu: error %d", (unsigned long)bv->applied, rc);
            bv->error = true;
            break;
        }
        bv->applied++;
        steps++;
        const dv_stream_t *d = &bv->dv;
        if (!d->dirty_x1) continue;
        lv_area_t a = {d->dirty_x0, d->dirty_y0, d->dirty_x1 - 1, d->dirty_y1 - 1};
        if (any) _lv_area_join(&dirty, &dirty, &a);
        else dirty = a;
        any = true;
    }
    if (any) {
        const lv_area_t *c = &bv->view->coords;
        lv_area_t abs = {c->x1 + dirty.x1, c->y1 + dirty.y1, c->x1 + dirty.x2, c->y1 + dirty.y2};
        lv_obj_invalidate_area(bv->view, &abs);
    }
}

/* ------------------------------------------------------------------ API */

bool boot_video_create(lv_obj_t *parent, lv_obj_t **out_obj, boot_video_src_t *out_src)
{
    if (out_obj) *out_obj = NULL;
    if (out_src) *out_src = BOOT_VIDEO_SRC_NONE;
    boot_video_destroy();
    boot_video_t *bv = heap_caps_calloc(1, sizeof(*bv), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!bv) return false;
    s_bv = bv;
    bool sd = sd_card_is_mounted();

    obv1_info_t info;
    if (sd && sd_card_exists(SD_BOOT_OBV)) {
        strlcpy(bv->obv_path, SD_BOOT_OBV, sizeof(bv->obv_path));
        if (obv_check(bv->obv_path, true, &info)) {
            bv->src = BOOT_VIDEO_SRC_SD_OBV;
            bv->timed = info.version == OBV2_VERSION;
            bv->duration_ms = info.duration_ms ? info.duration_ms : obv1_duration_ms(&info);
            if (info.flags & OBV1_FLAG_BLOCK_SAFE) (void)media_jpeg_block_ok(info.subsampling);
            if (out_src) *out_src = bv->src;
            return true;
        }
    }

    if (sd && sd_card_exists(SD_BOOT_JPEG) && jpeg_open(bv, parent, SD_BOOT_JPEG)) {
        if (out_obj) *out_obj = bv->view;
        if (out_src) *out_src = bv->src;
        return true;
    }

    if (parent && sd) {
        if (legacy_open(bv, parent)) {
            bv->src = BOOT_VIDEO_SRC_SD_LEGACY;
            if (out_obj) *out_obj = bv->view;
            if (out_src) *out_src = bv->src;
            return true;
        }
        legacy_release(bv);
    }
    boot_video_destroy();
    return false;
}

void boot_video_update(uint32_t elapsed_ms)
{
    boot_video_t *bv = s_bv;
    if (!bv) return;
    bv->elapsed_ms = elapsed_ms;
    if (bv->src == BOOT_VIDEO_SRC_SD_OBV) {
        if (bv->obv_started || bv->obv_failed) return;
        video_config_t cfg = {
            .mode = VIDEO_MODE_FULLSCREEN,
            .source = VIDEO_SRC_SD,
            .path = bv->obv_path,
            .target_fps = 0,
            .loop = false,
            .video_view = NULL,
        };
        esp_err_t err = video_player_start(&cfg);
        bv->obv_started = err == ESP_OK;
        bv->session_id = bv->obv_started ? video_player_session_id() : 0;
        bv->obv_failed = !bv->obv_started;
        if (bv->obv_failed) ESP_LOGW(TAG, "player start failed: %s", esp_err_to_name(err));
    } else if (bv->src == BOOT_VIDEO_SRC_SD_LEGACY) {
        legacy_update(bv, elapsed_ms);
    }
}

boot_video_state_t boot_video_get_state(void)
{
    boot_video_t *bv = s_bv;
    if (!bv) return BOOT_VIDEO_FAILED;
    if (bv->src == BOOT_VIDEO_SRC_SD_OBV) {
        if (bv->obv_failed) return BOOT_VIDEO_FAILED;
        if (!bv->obv_started) return BOOT_VIDEO_RUNNING;
        if (video_player_session_id() != bv->session_id || !video_player_fullscreen_active())
            return BOOT_VIDEO_FAILED;
        video_state_t st = video_player_get_state();
        if (st == VIDEO_STATE_ERROR) return BOOT_VIDEO_FAILED;
        // A stopped session still owned by us reached EOF. Explicit stop()
        // removes its identity; a replacement has a different identity.
        if (st == VIDEO_STATE_STOPPED) return BOOT_VIDEO_COMPLETED;
        if (!bv->timed && bv->elapsed_ms > bv->duration_ms + BV_OBV_GRACE_MS)
            return BOOT_VIDEO_FAILED;
        return BOOT_VIDEO_RUNNING;
    }
    if (bv->src == BOOT_VIDEO_SRC_SD_JPEG)
        return bv->view ? (bv->elapsed_ms >= bv->duration_ms ? BOOT_VIDEO_COMPLETED : BOOT_VIDEO_RUNNING)
                        : BOOT_VIDEO_FAILED;
    if (bv->error || !bv->view) return BOOT_VIDEO_FAILED;
    if (bv->elapsed_ms < bv->duration_ms) return BOOT_VIDEO_RUNNING;
    if (bv->applied >= bv->m.frame_count) return BOOT_VIDEO_COMPLETED;
    return bv->elapsed_ms >= bv->duration_ms + BV_LEGACY_GRACE_MS ? BOOT_VIDEO_FAILED : BOOT_VIDEO_RUNNING;
}

bool boot_video_first_frame_presented(void)
{
    boot_video_t *bv = s_bv;
    if (!bv) return false;
    if (bv->src != BOOT_VIDEO_SRC_SD_OBV) return true;
    if (!bv->obv_started || video_player_session_id() != bv->session_id) return false;
    return video_player_first_frame_presented();
}

void boot_video_destroy(void)
{
    boot_video_t *bv = s_bv;
    if (!bv) return;
    /* only our own fullscreen session (a theme video may have replaced it) */
    if (bv->obv_started && video_player_session_id() == bv->session_id) video_player_stop();
    if (bv->view && bv->pixels && !bv->error) {
        lv_obj_remove_event_cb(bv->view, view_deleted_cb);
        lv_obj_add_event_cb(bv->view, canvas_free_cb, LV_EVENT_DELETE, bv->pixels);
        bv->view = NULL;
        bv->pixels = NULL;
    }
    legacy_release(bv);
    s_bv = NULL;
    heap_caps_free(bv);
}
