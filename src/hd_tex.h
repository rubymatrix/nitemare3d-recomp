/*
 * Texture quality for the hi-res renderer (src/hd_tex.c): pixel-art
 * upscaling in palette-index space, true-colour mip chains built from the
 * current palette, and nearest / bilinear / trilinear / anisotropic sampling.
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { HQ_NEAREST, HQ_BILINEAR, HQ_TRILINEAR, HQ_ANISO };

typedef struct {
    int filter;  /* HQ_* */
    int aniso;   /* max anisotropic taps (1..16) */
    int upscale; /* 1 (none), 2, 3, 4, 6 */
} hq_settings_t;

extern hq_settings_t hq;

/* Option parsing helpers; return 0 on an unknown name. */
int hq_parse_filter(const char *s);
int hq_parse_upscale(const char *s);
const char *hq_describe(void);

/* Colours: called once per drawn frame with the current palette; textures are
 * rebuilt lazily when it changed. */
void hq_set_palette(const uint8_t pal[256][3]);

/* Current palette colour (XRGB) of an index. */
uint32_t hq_palette_color(int i);

typedef struct hq_tex hq_tex;

/* Texture for raw column-major palette data (w columns of h texels).
 * key identifies the source (file offset); remap (256 entries) or NULL is an
 * index remap applied before the palette; sprite: index 0x29 is transparent
 * and coordinates clamp, otherwise u wraps. Not thread-safe: resolve every
 * texture a frame needs before drawing in parallel. */
const hq_tex *hq_get(uint32_t key, const uint8_t *idx, int w, int h, int sprite, const uint8_t *remap);

/* Texture of a full-screen UI replacement: picture is the screen (an
 * hd_pack_img of kind HDP_UI) or one of its wide versions; colours follow the
 * screen's original indices under the current palette. Texel coordinates are
 * in the picture's own w x h. */
const hq_tex *hq_get_ui(const void *screen, const void *picture);

/* Premultiplied RGBA (A in bits 24..31) at texture coordinate (u, v) in source
 * texels, for a pixel covering fu x fv source texels (u along screen x, v
 * along screen y). Thread-safe. */
uint32_t hq_sample(const hq_tex *t, float u, float v, float fu, float fv);

/* n samples down a screen column: u fixed (fu texels per pixel across), v
 * from v0 in steps of dv; written to out[k * stride]. Same filtering as
 * hq_sample, much cheaper per texel. Thread-safe. */
void hq_column(const hq_tex *t, float u, float fu, float v0, float dv, int n, uint32_t *out, int stride);
/* Same, with filtering kept inside the texel span [umin, umax) of the
 * texture that the surface actually shows (wall pieces). */
void hq_column_span(const hq_tex *t, float u, float fu, float umin, float umax, float v0, float dv, int n, uint32_t *out, int stride);

/* Build the colours of every texture hq_get returned since the last call
 * (in parallel). Call before sampling them. */
void hq_build_pending(void);
unsigned hq_take_build_count(void); /* textures (re)built since the last call */

/* Run fn(ctx, begin, end) over [0, n) split across worker threads. */
void hq_parallel(void (*fn)(void *ctx, int begin, int end), void *ctx, int n);

#ifdef __cplusplus
}
#endif
