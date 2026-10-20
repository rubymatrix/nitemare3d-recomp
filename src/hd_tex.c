/*
 * Texture quality for the hi-res renderer (see hd_tex.h).
 *
 * The game's art is 8-bit paletted and the palette changes at run time
 * (fades, flashes), and some walls go through an index remap (DS:8084). So:
 *  - upscaling runs on palette indices with the Scale2x / Scale3x rules
 *    (AdvMAME2x/3x, EPX family): every output texel is one of the source
 *    texels, so the result stays valid for any palette and transparent
 *    texels stay a clean cut-out. 4x = 2x twice, 6x = 3x then 2x.
 *  - the true-colour mip chain (premultiplied alpha, box filtered) is built
 *    from the current palette and rebuilt lazily whenever palette or remap
 *    changed.
 *  - sampling: nearest; bilinear (+ nearest mip); trilinear; anisotropic:
 *    the pixel footprint fu x fv (texels across / along a column) gives the
 *    number of taps along the major axis (up to hq.aniso), each trilinear at
 *    the level of the minor extent.
 */
#include "hd_tex.h"
#include "hd_pack.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt/host.h"

#if !defined(_MSC_VER)
#include <pthread.h>
#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
#define HQ_THREADS 1
#endif
#ifdef _WIN32
#include <windows.h>
static long cpu_count(void) { SYSTEM_INFO si; GetSystemInfo(&si); return (long)si.dwNumberOfProcessors; }
#elif defined(__EMSCRIPTEN_PTHREADS__)
#include <emscripten/threading.h>
/* workers come from the pre-started pool (-sPTHREAD_POOL_SIZE), one per core */
static long cpu_count(void) { return emscripten_num_logical_cores(); }
#elif defined(HQ_THREADS)
#include <unistd.h>
static long cpu_count(void) { return sysconf(_SC_NPROCESSORS_ONLN); }
#endif
#endif

#define TRANSPARENT_INDEX 0x29
#define MAX_LEVELS 16

hq_settings_t hq = {HQ_ANISO, 16, 4};

int hq_parse_filter(const char *s) {
    static const char *names[] = {"nearest", "bilinear", "trilinear", "aniso"};
    for (int i = 0; i < 4; i++)
        if (!strcmp(s, names[i])) { hq.filter = i; return 1; }
    return 0;
}

int hq_parse_upscale(const char *s) {
    static const struct { const char *n; int f; } m[] = {{"none", 1}, {"scale2x", 2}, {"scale3x", 3}, {"scale4x", 4}, {"scale6x", 6},
                                                          {"2x", 2}, {"3x", 3}, {"4x", 4}, {"6x", 6}};
    for (size_t i = 0; i < sizeof m / sizeof m[0]; i++)
        if (!strcmp(s, m[i].n)) { hq.upscale = m[i].f; return 1; }
    return 0;
}

const char *hq_describe(void) {
    static char buf[64];
    static const char *names[] = {"nearest", "bilinear", "trilinear", "aniso"};
    if (hq.filter == HQ_ANISO) snprintf(buf, sizeof buf, "aniso %dx, upscale %dx", hq.aniso, hq.upscale);
    else snprintf(buf, sizeof buf, "%s, upscale %dx", names[hq.filter], hq.upscale);
    return buf;
}

/* ---------------------------------------------------------------- palette */
static uint32_t lut[256];
static uint32_t pal_serial = 1;
static uint32_t pal_hash;

static uint32_t fnv(const uint8_t *p, size_t n, uint32_t h) {
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

void hq_set_palette(const uint8_t pal[256][3]) {
    uint32_t h = fnv(&pal[0][0], 768, 2166136261u);
    if (h == pal_hash) return;
    pal_hash = h;
    pal_serial++;
    for (int i = 0; i < 256; i++) lut[i] = 0xff000000u | ((uint32_t)pal[i][0] << 16) | ((uint32_t)pal[i][1] << 8) | pal[i][2];
}

uint32_t hq_palette_color(int i) { return lut[i & 255]; }

/* ---------------------------------------------------------------- index upscaler */
/* Column-major images: texel (u, v) at [u * h + v]; u wraps for walls. */
typedef struct { const uint8_t *p; int w, h, wrap; } img_t;

static inline uint8_t at(const img_t *s, int u, int v) {
    if (u < 0) u = s->wrap ? u + s->w : 0;
    if (u >= s->w) u = s->wrap ? u - s->w : s->w - 1;
    if (v < 0) v = 0;
    if (v >= s->h) v = s->h - 1;
    return s->p[u * s->h + v];
}

static uint8_t *scale2x(const img_t *s) {
    int W = s->w * 2, H = s->h * 2;
    uint8_t *d = malloc((size_t)W * H);
    for (int u = 0; u < s->w; u++)
        for (int v = 0; v < s->h; v++) {
            /* A above (v-1), B right (u+1), C left (u-1), D below (v+1) */
            uint8_t P = at(s, u, v), A = at(s, u, v - 1), B = at(s, u + 1, v), C = at(s, u - 1, v), D = at(s, u, v + 1);
            uint8_t e0 = P, e1 = P, e2 = P, e3 = P;
            if (C == A && C != D && A != B) e0 = A;
            if (A == B && A != C && B != D) e1 = B;
            if (D == C && D != B && C != A) e2 = C;
            if (B == D && B != A && D != C) e3 = D;
            int U = u * 2, V = v * 2;
            d[U * H + V] = e0;           /* top-left */
            d[(U + 1) * H + V] = e1;     /* top-right */
            d[U * H + V + 1] = e2;       /* bottom-left */
            d[(U + 1) * H + V + 1] = e3; /* bottom-right */
        }
    return d;
}

static uint8_t *scale3x(const img_t *s) {
    int W = s->w * 3, H = s->h * 3;
    uint8_t *d = malloc((size_t)W * H);
    for (int u = 0; u < s->w; u++)
        for (int v = 0; v < s->h; v++) {
            /* A B C / D E F / G H I around E; rows are v, columns u */
            uint8_t A = at(s, u - 1, v - 1), B = at(s, u, v - 1), C = at(s, u + 1, v - 1);
            uint8_t D = at(s, u - 1, v), E = at(s, u, v), F = at(s, u + 1, v);
            uint8_t G = at(s, u - 1, v + 1), Hh = at(s, u, v + 1), I = at(s, u + 1, v + 1);
            uint8_t o[9];
            for (int k = 0; k < 9; k++) o[k] = E;
            if (B != Hh && D != F) {
                if (D == B) o[0] = D;
                if ((D == B && E != C) || (B == F && E != A)) o[1] = B;
                if (B == F) o[2] = F;
                if ((D == B && E != G) || (D == Hh && E != A)) o[3] = D;
                if ((B == F && E != I) || (Hh == F && E != C)) o[5] = F;
                if (D == Hh) o[6] = D;
                if ((D == Hh && E != I) || (Hh == F && E != G)) o[7] = Hh;
                if (Hh == F) o[8] = F;
            }
            int U = u * 3, V = v * 3;
            for (int r = 0; r < 3; r++)
                for (int c = 0; c < 3; c++) d[(U + c) * H + V + r] = o[r * 3 + c];
        }
    return d;
}

static uint8_t *upscale(const uint8_t *src, int w, int h, int wrap, int factor, int *ow, int *oh) {
    uint8_t *cur = malloc((size_t)w * h);
    memcpy(cur, src, (size_t)w * h);
    int steps[2] = {0, 0}, n = 0;
    if (factor == 2) steps[n++] = 2;
    else if (factor == 3) steps[n++] = 3;
    else if (factor == 4) { steps[n++] = 2; steps[n++] = 2; }
    else if (factor == 6) { steps[n++] = 3; steps[n++] = 2; }
    for (int i = 0; i < n; i++) {
        img_t s = {cur, w, h, wrap};
        uint8_t *next = steps[i] == 2 ? scale2x(&s) : scale3x(&s);
        free(cur);
        cur = next;
        w *= steps[i];
        h *= steps[i];
    }
    *ow = w;
    *oh = h;
    return cur;
}

/* ---------------------------------------------------------------- textures */
struct hq_tex {
    int nlev, wrap, scale, pending;
    int lw[MAX_LEVELS], lh[MAX_LEVELS];
    float su[MAX_LEVELS], sv[MAX_LEVELS]; /* level size / level-0 size */
    uint32_t *lev[MAX_LEVELS];
    uint32_t serial; /* palette serial + remap hash it was built for */
    uint32_t remap_hash;
};

typedef struct {
    uint32_t key;
    int sprite, w, h, scale, used;
    uint8_t *up;   /* upscaled indices */
    int uw, uh;
    const hd_pack_img *ov; /* replacement art (then up is NULL) */
    uint8_t *src;  /* with ov: copy of the original indices, for palette tracking */
    hq_tex var[2]; /* plain, remapped */
} entry_t;

#define ENTRIES 4096
static entry_t entries[ENTRIES];
static size_t cache_bytes;

static void free_tex(hq_tex *t) {
    for (int l = 0; l < t->nlev; l++) free(t->lev[l]);
    memset(t, 0, sizeof *t);
}

static void flush_all(void) {
    for (int i = 0; i < ENTRIES; i++) {
        free(entries[i].up);
        free(entries[i].src);
        free_tex(&entries[i].var[0]);
        free_tex(&entries[i].var[1]);
        memset(&entries[i], 0, sizeof entries[i]);
    }
    cache_bytes = 0;
}

/* Replacement art is true colour, but the game animates its palette (fades,
 * damage flashes) and shades some walls through an index remap. Follow that
 * with a per-channel gain: the average colour the original indices have now
 * over the average colour of the replacement. */
static void ov_colours(uint32_t *d, const entry_t *e, const uint8_t *remap) {
    double sum[3] = {0, 0, 0};
    int n = 0;
    for (int i = 0; i < e->w * e->h; i++) {
        uint8_t ix = e->src[i];
        if (e->sprite == 1 && ix == TRANSPARENT_INDEX) continue;
        uint32_t c = lut[remap ? remap[ix] : ix];
        sum[0] += (c >> 16) & 255; sum[1] += (c >> 8) & 255; sum[2] += c & 255;
        n++;
    }
    uint32_t g[3]; /* 8.8 fixed point */
    for (int k = 0; k < 3; k++) {
        double m = e->ov->mean[k], gain = n && m > 1 ? sum[k] / n / m : 1;
        g[k] = (uint32_t)((gain > 4 ? 4 : gain) * 256 + 0.5);
    }
    const uint32_t *s = e->ov->rgba;
    for (int i = 0; i < e->uw * e->uh; i++) {
        uint32_t c = s[i], a = c >> 24;
        uint32_t r = (((c >> 16) & 255) * g[0]) >> 8, gr = (((c >> 8) & 255) * g[1]) >> 8, b = ((c & 255) * g[2]) >> 8;
        if (r > a) r = a; /* premultiplied: a channel never exceeds alpha */
        if (gr > a) gr = a;
        if (b > a) b = a;
        d[i] = (a << 24) | (r << 16) | (gr << 8) | b;
    }
}

static unsigned builds;
unsigned hq_take_build_count(void) { unsigned b = builds; builds = 0; return b; }

static size_t build_rgb(hq_tex *t, const entry_t *e, const uint8_t *remap) {
    size_t bytes = 0;
    for (int l = 0; l < t->nlev; l++) free(t->lev[l]);
    t->nlev = 0;
    int w = e->uw, h = e->uh;
    t->wrap = !e->sprite;
    t->scale = e->ov ? e->ov->scale : e->uw / e->w;
    uint32_t *l0 = malloc(sizeof(uint32_t) * (size_t)w * h);
    if (e->ov) ov_colours(l0, e, remap);
    else
        for (int i = 0; i < w * h; i++) {
            uint8_t ix = e->up[i];
            l0[i] = e->sprite && ix == TRANSPARENT_INDEX ? 0 : lut[remap ? remap[ix] : ix];
        }
    t->lev[0] = l0;
    t->lw[0] = w;
    t->lh[0] = h;
    t->su[0] = t->sv[0] = 1;
    t->nlev = 1;
    bytes += sizeof(uint32_t) * (size_t)w * h;
    while (t->nlev < MAX_LEVELS && (w > 1 || h > 1)) {
        int nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
        const uint32_t *s = t->lev[t->nlev - 1];
        uint32_t *d = malloc(sizeof(uint32_t) * (size_t)nw * nh);
        for (int u = 0; u < nw; u++)
            for (int v = 0; v < nh; v++) {
                int u0 = u * 2, u1 = u0 + 1 < w ? u0 + 1 : u0, v0 = v * 2, v1 = v0 + 1 < h ? v0 + 1 : v0;
                uint32_t c[4] = {s[u0 * h + v0], s[u0 * h + v1], s[u1 * h + v0], s[u1 * h + v1]};
                uint32_t out = 0;
                for (int sh = 0; sh < 32; sh += 8) {
                    uint32_t sum = ((c[0] >> sh) & 255) + ((c[1] >> sh) & 255) + ((c[2] >> sh) & 255) + ((c[3] >> sh) & 255);
                    out |= ((sum + 2) / 4) << sh;
                }
                d[u * nh + v] = out;
            }
        t->lev[t->nlev] = d;
        t->lw[t->nlev] = nw;
        t->lh[t->nlev] = nh;
        t->su[t->nlev] = (float)nw / t->lw[0];
        t->sv[t->nlev] = (float)nh / t->lh[0];
        t->nlev++;
        bytes += sizeof(uint32_t) * (size_t)nw * nh;
        w = nw;
        h = nh;
    }
    return bytes;
}

/* Textures waiting for their colour build: done in parallel by hq_build_pending. */
typedef struct { hq_tex *t; const entry_t *e; int remapped; uint8_t remap[256]; size_t bytes; } pending_t;
static pending_t *pend;
static int npend, pend_cap;

static void build_job(void *ctx, int b, int e) {
    (void)ctx;
    for (int i = b; i < e; i++) pend[i].bytes = build_rgb(pend[i].t, pend[i].e, pend[i].remapped ? pend[i].remap : NULL);
}

void hq_build_pending(void) {
    if (!npend) return;
    hq_parallel(build_job, NULL, npend);
    for (int i = 0; i < npend; i++) { cache_bytes += pend[i].bytes; pend[i].t->pending = 0; }
    builds += (unsigned)npend;
    npend = 0;
}

static const hq_tex *get(uint32_t key, const uint8_t *idx, int w, int h, int sprite, const uint8_t *remap, const hd_pack_img *ui);

const hq_tex *hq_get(uint32_t key, const uint8_t *idx, int w, int h, int sprite, const uint8_t *remap) {
    return get(key, idx, w, h, sprite, remap, NULL);
}

const hq_tex *hq_get_ui(const void *screen, const void *picture) {
    const hd_pack_img *s = screen, *p = picture;
    /* colours follow the screen's original indices; the picture may be a wider version */
    return get((uint32_t)s->hash ^ (uint32_t)p->w * 2654435761u, s->idx_cm, s->w, s->h, 2, NULL, p);
}

static const hq_tex *get(uint32_t key, const uint8_t *idx, int w, int h, int sprite, const uint8_t *remap, const hd_pack_img *ui) {
    if (cache_bytes > (size_t)768 << 20 && !npend) flush_all();
    uint32_t hsh = (key * 2654435761u ^ (uint32_t)sprite * 0x9e3779b9u ^ (uint32_t)hq.upscale * 0x85ebca6bu) >> 20;
    entry_t *e = NULL;
    for (int probe = 0; probe < ENTRIES; probe++) {
        entry_t *c = &entries[(hsh + probe) & (ENTRIES - 1)];
        if (c->used && c->key == key && c->sprite == sprite && c->w == w && c->h == h && c->scale == hq.upscale) { e = c; break; }
        if (!c->used) {
            e = c;
            e->used = 1;
            e->key = key;
            e->sprite = sprite;
            e->w = w;
            e->h = h;
            e->scale = hq.upscale;
            e->ov = ui ? ui : sprite != 2 && hd_pack_active() ? hd_pack_find(idx, w, h) : NULL;
            if (e->ov) {
                e->uw = e->ov->w * e->ov->scale;
                e->uh = e->ov->h * e->ov->scale;
                e->src = malloc((size_t)w * h);
                memcpy(e->src, idx, (size_t)w * h);
            } else {
                e->up = upscale(idx, w, h, !sprite, hq.upscale, &e->uw, &e->uh);
                cache_bytes += (size_t)e->uw * e->uh;
            }
            break;
        }
    }
    if (!e && !npend) { flush_all(); return get(key, idx, w, h, sprite, remap, ui); }
    if (!e) return NULL;
    hq_tex *t = &e->var[remap ? 1 : 0];
    uint32_t rh = remap ? fnv(remap, 256, 2166136261u) : 0;
    if ((!t->nlev && !t->pending) || t->serial != pal_serial || t->remap_hash != rh) {
        if (npend == pend_cap) { pend_cap = pend_cap ? pend_cap * 2 : 256; pend = realloc(pend, sizeof *pend * (size_t)pend_cap); }
        pending_t *p = &pend[npend++];
        p->t = t;
        p->e = e;
        p->remapped = remap != NULL;
        if (remap) memcpy(p->remap, remap, 256);
        t->pending = 1;
        t->serial = pal_serial;
        t->remap_hash = rh;
        t->wrap = !e->sprite;
        t->scale = e->ov ? e->ov->scale : e->uw / e->w;
    }
    return t;
}

/* ---------------------------------------------------------------- sampling */
static inline uint32_t fetch(const hq_tex *t, int l, int x, int y) {
    int w = t->lw[l], h = t->lh[l];
    if (t->wrap) { x %= w; if (x < 0) x += w; }
    else x = x < 0 ? 0 : x >= w ? w - 1 : x;
    y = y < 0 ? 0 : y >= h ? h - 1 : y;
    return t->lev[l][x * h + y];
}

/* blend two premultiplied colours, f = 0..256 weight of b */
static inline uint32_t lerp32(uint32_t a, uint32_t b, uint32_t f) {
    uint32_t g = 256 - f;
    uint32_t rb = ((a & 0x00ff00ffu) * g + (b & 0x00ff00ffu) * f) >> 8;
    uint32_t ag = (((a >> 8) & 0x00ff00ffu) * g + ((b >> 8) & 0x00ff00ffu) * f) >> 8;
    return (rb & 0x00ff00ffu) | ((ag & 0x00ff00ffu) << 8);
}

/* U, V in level-0 texels */
static inline uint32_t bilinear(const hq_tex *t, int l, float U, float V) {
    float x = U * t->su[l] - 0.5f, y = V * t->sv[l] - 0.5f;
    float fx0 = floorf(x), fy0 = floorf(y);
    int x0 = (int)fx0, y0 = (int)fy0;
    uint32_t fx = (uint32_t)((x - fx0) * 256.0f), fy = (uint32_t)((y - fy0) * 256.0f);
    uint32_t top = lerp32(fetch(t, l, x0, y0), fetch(t, l, x0 + 1, y0), fx);
    uint32_t bot = lerp32(fetch(t, l, x0, y0 + 1), fetch(t, l, x0 + 1, y0 + 1), fx);
    return lerp32(top, bot, fy);
}

static inline uint32_t trilinear(const hq_tex *t, float lod, float U, float V) {
    if (lod <= 0) return bilinear(t, 0, U, V);
    int l = (int)lod;
    if (l >= t->nlev - 1) return bilinear(t, t->nlev - 1, U, V);
    uint32_t f = (uint32_t)((lod - l) * 256.0f);
    return lerp32(bilinear(t, l, U, V), bilinear(t, l + 1, U, V), f);
}

uint32_t hq_sample(const hq_tex *t, float u, float v, float fu, float fv) {
    float S = (float)t->scale;
    float U = u * S, V = v * S, FU = fu * S, FV = fv * S;
    switch (hq.filter) {
    case HQ_NEAREST:
        return fetch(t, 0, (int)floorf(U), (int)floorf(V));
    case HQ_BILINEAR: {
        float m = FU > FV ? FU : FV;
        int l = m > 1 ? (int)(log2f(m) + 0.5f) : 0;
        if (l > t->nlev - 1) l = t->nlev - 1;
        return bilinear(t, l, U, V);
    }
    case HQ_TRILINEAR: {
        float m = FU > FV ? FU : FV;
        return trilinear(t, m > 1 ? log2f(m) : 0, U, V);
    }
    default: {
        float major = FU > FV ? FU : FV, minor = FU > FV ? FV : FU;
        if (major <= 1) return bilinear(t, 0, U, V); /* magnified: nothing to average */
        if (minor < 1e-4f) minor = 1e-4f;
        int n = (int)ceilf(major / minor);
        if (n > hq.aniso) n = hq.aniso;
        if (n < 1) n = 1;
        float lod = log2f(major / n);
        if (n == 1) return trilinear(t, lod, U, V);
        uint32_t rb = 0, ag = 0;
        float step = major / n, off = -0.5f * major + 0.5f * step;
        for (int i = 0; i < n; i++, off += step) {
            uint32_t c = FU > FV ? trilinear(t, lod, U + off, V) : trilinear(t, lod, U, V + off);
            rb += c & 0x00ff00ffu; /* up to 16 taps: 16 * 255 fits in 16 bits per lane */
            ag += (c >> 8) & 0x00ff00ffu;
        }
        uint32_t r = ((rb >> 16) & 0xffff) / n, b = (rb & 0xffff) / n, a = ((ag >> 16) & 0xffff) / n, g = (ag & 0xffff) / n;
        return (a << 24) | (r << 16) | (g << 8) | b;
    }
    }
}

/* ---------------------------------------------------------------- column sampler */
/* u is constant down a screen column for walls and sprites, so the mip
 * level(s), the anisotropic taps and the texture columns they read are
 * resolved once per column; per row only the v position changes. */
typedef struct { const uint32_t *c0, *c1; uint32_t fx; } colpair_t;

static inline void colpair(const hq_tex *t, int l, float U, colpair_t *p) {
    float x = U * t->su[l] - 0.5f, fl = floorf(x);
    int w = t->lw[l], h = t->lh[l], xa = (int)fl, xb = xa + 1;
    p->fx = (uint32_t)((x - fl) * 256.0f);
    if (t->wrap) {
        xa %= w; if (xa < 0) xa += w;
        xb %= w; if (xb < 0) xb += w;
    } else {
        xa = xa < 0 ? 0 : xa >= w ? w - 1 : xa;
        xb = xb < 0 ? 0 : xb >= w ? w - 1 : xb;
    }
    p->c0 = t->lev[l] + (size_t)xa * h;
    p->c1 = t->lev[l] + (size_t)xb * h;
}

static inline uint32_t colpair_sample(const colpair_t *p, int h, float y) {
    float fl = floorf(y);
    int ya = (int)fl, yb = ya + 1;
    uint32_t fy = (uint32_t)((y - fl) * 256.0f);
    ya = ya < 0 ? 0 : ya >= h ? h - 1 : ya;
    yb = yb < 0 ? 0 : yb >= h ? h - 1 : yb;
    return lerp32(lerp32(p->c0[ya], p->c1[ya], p->fx), lerp32(p->c0[yb], p->c1[yb], p->fx), fy);
}

void hq_column(const hq_tex *t, float u, float fu, float v0, float dv, int n, uint32_t *out, int stride) {
    float S = (float)t->scale;
    float U = u * S, FU = fabsf(fu) * S, V0 = v0 * S, DV = dv * S, FV = fabsf(DV);
    int H0 = t->lh[0];
    if (hq.filter == HQ_NEAREST) {
        int x = (int)floorf(U), w = t->lw[0];
        if (t->wrap) { x %= w; if (x < 0) x += w; }
        else x = x < 0 ? 0 : x >= w ? w - 1 : x;
        const uint32_t *col = t->lev[0] + (size_t)x * H0;
        float V = V0;
        for (int r = 0; r < n; r++, V += DV, out += stride) {
            int y = (int)V;
            *out = col[y < 0 ? 0 : y >= H0 ? H0 - 1 : y];
        }
        return;
    }
    /* level(s) and taps */
    float major = FU > FV ? FU : FV, minor = FU > FV ? FV : FU, lod;
    int taps = 1, along_u = FU > FV;
    if (hq.filter == HQ_ANISO && major > 1) {
        if (minor < 1e-4f) minor = 1e-4f;
        taps = (int)ceilf(major / minor);
        if (taps > hq.aniso) taps = hq.aniso;
        if (taps < 1) taps = 1;
        lod = log2f(major / taps);
    } else lod = major > 1 ? log2f(major) : 0;
    if (lod < 0) lod = 0;
    int l0, nl = 1;
    uint32_t lf = 0;
    if (hq.filter == HQ_BILINEAR) l0 = (int)(lod + 0.5f);
    else { l0 = (int)lod; lf = (uint32_t)((lod - l0) * 256.0f); nl = lf ? 2 : 1; }
    if (l0 >= t->nlev - 1) { l0 = t->nlev - 1; nl = 1; }
    float step = major / taps, first = -0.5f * major + 0.5f * step;
    colpair_t cp[2][16];
    int ncp = along_u ? taps : 1;
    for (int li = 0; li < nl; li++)
        for (int k = 0; k < ncp; k++) colpair(t, l0 + li, along_u ? U + first + k * step : U, &cp[li][k]);
    float V = V0;
    for (int r = 0; r < n; r++, V += DV, out += stride) {
        uint32_t res[2];
        for (int li = 0; li < nl; li++) {
            int l = l0 + li, h = t->lh[l];
            float sv = t->sv[l];
            if (taps == 1) { res[li] = colpair_sample(&cp[li][0], h, V * sv - 0.5f); continue; }
            uint32_t rb = 0, ag = 0;
            for (int k = 0; k < taps; k++) {
                uint32_t c = along_u ? colpair_sample(&cp[li][k], h, V * sv - 0.5f)
                                     : colpair_sample(&cp[li][0], h, (V + first + k * step) * sv - 0.5f);
                rb += c & 0x00ff00ffu;
                ag += (c >> 8) & 0x00ff00ffu;
            }
            uint32_t R = ((rb >> 16) & 0xffff) / taps, B = (rb & 0xffff) / taps, A = ((ag >> 16) & 0xffff) / taps, G = (ag & 0xffff) / taps;
            res[li] = (A << 24) | (R << 16) | (G << 8) | B;
        }
        *out = nl == 2 ? lerp32(res[0], res[1], lf) : res[0];
    }
}

/* ---------------------------------------------------------------- worker threads */
#ifdef HQ_THREADS
/* Persistent workers. A job is split into chunks claimed under the lock
 * (with the job's fn/ctx read at the same time); the caller waits until every
 * chunk is done, so a worker that wakes late can never touch a finished job. */
static pthread_mutex_t pool_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pool_cv = PTHREAD_COND_INITIALIZER, done_cv = PTHREAD_COND_INITIALIZER;
static int pool_n;          /* worker threads, -1: none */
static unsigned pool_gen;   /* bumped for every job */
static void (*job_fn)(void *, int, int);
static void *job_ctx;
static int job_total, job_chunk, job_next, job_chunks, job_done;

static void run_chunks(void) {
    pthread_mutex_lock(&pool_mx);
    while (job_next < job_total) {
        int b = job_next, e = b + job_chunk < job_total ? b + job_chunk : job_total;
        void (*fn)(void *, int, int) = job_fn;
        void *ctx = job_ctx;
        job_next = e;
        pthread_mutex_unlock(&pool_mx);
        fn(ctx, b, e);
        pthread_mutex_lock(&pool_mx);
        if (++job_done == job_chunks) pthread_cond_broadcast(&done_cv);
    }
    pthread_mutex_unlock(&pool_mx);
}

static void *worker(void *arg) {
    (void)arg;
    unsigned seen = 0;
    for (;;) {
        pthread_mutex_lock(&pool_mx);
        while (pool_gen == seen) pthread_cond_wait(&pool_cv, &pool_mx);
        seen = pool_gen;
        pthread_mutex_unlock(&pool_mx);
        run_chunks();
    }
    return NULL;
}

void hq_parallel(void (*fn)(void *, int, int), void *ctx, int n) {
    if (!pool_n) {
        long cpus = cpu_count();
        int want = cpus > 1 ? (int)(cpus > 12 ? 11 : cpus - 1) : 0;
        if (getenv("N3D_THREADS")) want = atoi(getenv("N3D_THREADS")) - 1;
        for (int i = 0; i < want; i++) {
            pthread_t th;
            if (pthread_create(&th, NULL, worker, NULL) == 0) { pthread_detach(th); pool_n++; }
        }
        if (!pool_n) pool_n = -1;
    }
    if (n <= 0) return;
    if (pool_n < 0 || n < 64) { fn(ctx, 0, n); return; }
    pthread_mutex_lock(&pool_mx);
    job_fn = fn;
    job_ctx = ctx;
    job_total = n;
    job_chunk = n / ((pool_n + 1) * 6) + 1;
    job_chunks = (n + job_chunk - 1) / job_chunk;
    job_next = 0;
    job_done = 0;
    pool_gen++;
    pthread_cond_broadcast(&pool_cv);
    pthread_mutex_unlock(&pool_mx);
    run_chunks();
    pthread_mutex_lock(&pool_mx);
    while (job_done < job_chunks) pthread_cond_wait(&done_cv, &pool_mx);
    pthread_mutex_unlock(&pool_mx);
}
#else
/* Single-threaded (browser build without workers): run in slices and let the
 * host top up its audio queue in between, so a full texture rebuild doesn't
 * starve the sound. */
void hq_parallel(void (*fn)(void *, int, int), void *ctx, int n) {
    int step = n >= 32 ? n / 32 : 1;
    for (int b = 0; b < n; b += step) {
        fn(ctx, b, b + step < n ? b + step : n);
        host_audio_poll();
    }
}
#endif
