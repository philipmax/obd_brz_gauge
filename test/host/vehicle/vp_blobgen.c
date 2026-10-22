/*
 * vp_blobgen — compile one profile into the vp_profile_t blob the firmware
 * embeds (used by tools/vehicles/gen_builtin.py).
 *
 *   vp_blobgen <profile.json> <out.bin>
 *
 * Compiles with the firmware configuration (vp_validate_default_cfg()).
 */
#include <stdio.h>
#include <stdlib.h>

#include "vp_profile.h"
#include "vt_support.h"

int main(int argc, char **argv)
{
    const vp_validate_cfg_t *cfg = vp_validate_default_cfg();
    const char *in = argc == 3 ? argv[1] : NULL;
    const char *out = argc == 3 ? argv[2] : NULL;
    vp_issue_t store[32];
    vp_issues_t l;
    vp_profile_t *p = NULL;
    size_t len = 0;
    char *text;
    FILE *f;
    int rc = 0;

    if (!in || !out) {
        fprintf(stderr, "usage: %s <profile.json> <out.bin>\n", argv[0]);
        return 2;
    }
    text = vt_read_file(in, &len);
    if (!text) {
        fprintf(stderr, "cannot read %s\n", in);
        return 1;
    }
    vp_issues_init(&l, store, 32);
    if (vp_compile_json(text, len, cfg, &vt_malloc_allocator, &p, &l) != VP_COMPILE_OK) {
        fprintf(stderr, "%s: compile failed\n", in);
        vt_print_issues(&l, "  ");
        free(text);
        return 1;
    }
    f = fopen(out, "wb");
    if (!f || fwrite(p, 1, p->total_size, f) != p->total_size) {
        fprintf(stderr, "cannot write %s\n", out);
        rc = 1;
    }
    if (f) fclose(f);
    if (!rc) printf("%s: %u bytes, crc32 0x%08x\n", out, (unsigned)p->total_size, (unsigned)p->crc32);
    vp_profile_free(p, &vt_malloc_allocator);
    free(text);
    return rc;
}
