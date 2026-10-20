/*
 * Replacement art pack for the hi-res renderer (src/hd_pack.c).
 *
 * A pack is a folder made by re/upscale/pack.py: index.txt maps the FNV-1a
 * 64 hash of an image's original palette indices (exactly the bytes the
 * renderer reads: IMG.n records column-major, UIF screens row-major) to a
 * true-colour replacement at `scale` times the original size. Matching by
 * content means one pack serves every episode and duplicate records.
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { HDP_WALL, HDP_SPRITE, HDP_UI, HDP_UIWIDE };

typedef struct hd_pack_img hd_pack_img;
struct hd_pack_img {
    uint64_t hash;
    int kind, w, h, scale; /* scale 0: known image, no replacement yet */
    char id[32];
    uint32_t *rgba;        /* premultiplied ARGB, column-major, (w*scale) x (h*scale); loaded on first use */
    uint8_t *idx;          /* ui only: original indices, row-major 320x200 */
    uint8_t *idx_cm;       /* ui only: the same, column-major */
    int bg;                /* ui only: most common index (large flat areas match too easily) */
    float mean[3];         /* average opaque colour of rgba (unpremultiplied), for palette tracking */
    int logged;
    /* ui only: wider versions of the screen (HDP_UIWIDE, same hash). Their
     * w x h is in texels of the original screen (200 high, 1 x 1.2 display
     * units each as at 4:3), the original 320 centred. */
    hd_pack_img *wide[4];
    int nwide;
};

/* Load dir/index.txt; returns the number of replacements (0: none / no pack). */
int hd_pack_load(const char *dir);
int hd_pack_active(void);

/* Replacement for these indices (w columns of h texels), with rgba loaded,
 * or NULL. With N3D_HDPACK_LOG=file set, known images that have no
 * replacement yet are appended to that file once each ("id w h"). */
const hd_pack_img *hd_pack_find(const uint8_t *idx, int w, int h);

/* Full-screen UI pictures that have a replacement or wide versions. */
int hd_pack_ui_count(void);
const hd_pack_img *hd_pack_ui(int i);

#ifdef __cplusplus
}
#endif
