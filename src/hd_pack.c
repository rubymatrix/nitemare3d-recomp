/*
 * Replacement art pack (see hd_pack.h).
 */
#include "hd_pack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SLOTS 8192 /* open addressing, power of two, well above the ~1.2k images */

static hd_pack_img *imgs;
static int nimgs, nreplaced;
static int slot[SLOTS]; /* index + 1 into imgs, 0 empty */
static int ui[64], nui;
static char pack_dir[1024];
static FILE *miss_log;

static uint64_t fnv64(const uint8_t *p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

static void *read_file(const char *name, size_t want) {
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", pack_dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    void *buf = malloc(want);
    size_t got = fread(buf, 1, want, f);
    fclose(f);
    if (got != want) { fprintf(stderr, "hd pack: %s: short file\n", path); free(buf); return NULL; }
    return buf;
}

static int load_rgba(hd_pack_img *m) {
    if (m->rgba) return 1;
    if (!m->scale) return 0;
    char name[64];
    snprintf(name, sizeof name, "%s.rgba", m->id);
    size_t n = (size_t)m->w * m->scale * m->h * m->scale;
    m->rgba = read_file(name, n * 4);
    if (!m->rgba) { m->scale = 0; return 0; }
    double sum[3] = {0, 0, 0}, alpha = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t c = m->rgba[i];
        sum[0] += (c >> 16) & 255; sum[1] += (c >> 8) & 255; sum[2] += c & 255;
        alpha += (c >> 24) / 255.0;
    }
    for (int k = 0; k < 3; k++) m->mean[k] = alpha > 0 ? (float)(sum[k] / alpha) : 0;
    return 1;
}

int hd_pack_load(const char *dir) {
    snprintf(pack_dir, sizeof pack_dir, "%s", dir);
    char path[1200];
    snprintf(path, sizeof path, "%s/index.txt", dir);
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "hd pack: no %s\n", path); return 0; }
    char line[256];
    int cap = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long long h;
        char kind[16], id[32];
        int w, hh, scale;
        if (sscanf(line, "%llx %15s %d %d %d %31s", &h, kind, &w, &hh, &scale, id) != 6) continue;
        if (nimgs == cap) { cap = cap ? cap * 2 : 1024; imgs = realloc(imgs, sizeof *imgs * (size_t)cap); }
        hd_pack_img *m = &imgs[nimgs];
        memset(m, 0, sizeof *m);
        m->hash = h;
        m->kind = !strcmp(kind, "ui") ? HDP_UI : !strcmp(kind, "uiwide") ? HDP_UIWIDE : !strcmp(kind, "sprite") ? HDP_SPRITE : HDP_WALL;
        m->w = w; m->h = hh; m->scale = scale;
        snprintf(m->id, sizeof m->id, "%s", id);
        nimgs++;
        if (scale) nreplaced++;
    }
    fclose(f);
    for (int i = 0; i < nimgs; i++) { /* wide versions hang off their screen, not the hash table */
        hd_pack_img *w = &imgs[i];
        if (w->kind != HDP_UIWIDE || !w->scale) continue;
        for (int j = 0; j < nimgs; j++)
            if (imgs[j].kind == HDP_UI && imgs[j].hash == w->hash && imgs[j].nwide < 4) {
                if (load_rgba(w)) imgs[j].wide[imgs[j].nwide++] = w;
                break;
            }
    }
    for (int i = 0; i < nimgs; i++) {
        hd_pack_img *m = &imgs[i];
        if (m->kind == HDP_UIWIDE) continue;
        uint32_t s = (uint32_t)(m->hash >> 32) & (SLOTS - 1);
        while (slot[s]) s = (s + 1) & (SLOTS - 1);
        slot[s] = i + 1;
        if (m->kind == HDP_UI && (m->scale || m->nwide) && nui < 64 && m->w == 320 && m->h == 200) {
            char name[64];
            snprintf(name, sizeof name, "%s.idx", m->id);
            m->idx = read_file(name, 64000);
            if (m->scale) load_rgba(m);
            if (m->idx && (m->rgba || m->nwide)) {
                int count[256] = {0};
                for (int k = 0; k < 64000; k++) count[m->idx[k]]++;
                for (int c = 1; c < 256; c++) if (count[c] > count[m->bg]) m->bg = c;
                m->idx_cm = malloc(64000);
                for (int x = 0; x < 320; x++)
                    for (int y = 0; y < 200; y++) m->idx_cm[x * 200 + y] = m->idx[y * 320 + x];
                ui[nui++] = i;
            }
        }
    }
    const char *log = getenv("N3D_HDPACK_LOG");
    if (log) miss_log = fopen(log, "a");
    fprintf(stderr, "hd pack: %s: %d of %d images replaced, %d ui screens\n", dir, nreplaced, nimgs, nui);
    return nreplaced;
}

int hd_pack_active(void) { return nreplaced > 0; }

const hd_pack_img *hd_pack_find(const uint8_t *idx, int w, int h) {
    if (!nimgs) return NULL;
    uint64_t hash = fnv64(idx, (size_t)w * h);
    for (uint32_t s = (uint32_t)(hash >> 32) & (SLOTS - 1); slot[s]; s = (s + 1) & (SLOTS - 1)) {
        hd_pack_img *m = &imgs[slot[s] - 1];
        if (m->hash != hash || m->w != w || m->h != h) continue;
        if (load_rgba(m)) return m;
        if (miss_log && !m->logged) { fprintf(miss_log, "%s %d %d\n", m->id, w, h); fflush(miss_log); }
        m->logged = 1;
        return NULL;
    }
    return NULL;
}

int hd_pack_ui_count(void) { return nui; }
const hd_pack_img *hd_pack_ui(int i) { return &imgs[ui[i]]; }
