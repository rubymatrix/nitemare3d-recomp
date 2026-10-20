/*
 * DLS -> SoundFont 2 converter.
 *
 * Windows' own General MIDI synth (the "Microsoft GS Wavetable SW Synth",
 * Roland-made) plays the instrument bank in C:\Windows\System32\drivers\gm.dls,
 * and macOS ships a similar Roland GS bank as gs_instruments.dls. Both are DLS
 * level 1 files. TinySoundFont only reads SoundFonts, so this rebuilds the
 * bank as an in-memory .sf2: every DLS instrument becomes one SF2 instrument
 * plus a preset with the same bank/program (drum kits go to bank 128), every
 * region a zone, every wave a sample. Articulation (volume envelope, pan,
 * gain, LFO vibrato/tremolo) is mapped to the matching SF2 generators;
 * DLS2-only features (filters, EG2 routing) are ignored.
 */
#include "dls.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint8_t *p; size_t n, cap; } buf_t;

static void put(buf_t *b, const void *d, size_t n) {
    if (b->n + n > b->cap) {
        size_t c = b->cap ? b->cap * 2 : 4096;
        while (c < b->n + n) c *= 2;
        uint8_t *q = realloc(b->p, c);
        if (!q) return;
        b->p = q;
        b->cap = c;
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}
static void put16(buf_t *b, unsigned v) { uint8_t x[2] = {(uint8_t)v, (uint8_t)(v >> 8)}; put(b, x, 2); }
static void put32(buf_t *b, uint32_t v) { uint8_t x[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)}; put(b, x, 4); }
static void putname(buf_t *b, const char *s) { char n[20] = {0}; strncpy(n, s, 19); put(b, n, 20); }

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* ---------------------------------------------------------------- DLS model */
typedef struct {
    int valid;
    int unity, fine, atten_cb, loop, loop_start, loop_len;
} wsmp_t;

typedef struct {
    int set[16];
    int val[16];
} art_t;
enum { A_ATTACK, A_DECAY, A_SUSTAIN, A_RELEASE, A_DELAY, A_HOLD, A_PAN, A_GAIN, A_LFOFREQ, A_LFODELAY, A_LFOPITCH, A_LFOVOL };

typedef struct {
    int key_lo, key_hi, vel_lo, vel_hi, key_group, wave;
    wsmp_t ws;
    art_t art;
} region_t;

typedef struct {
    char name[20];
    int bank, program, drums;
    region_t *rg;
    int nrg;
    art_t art;
} inst_t;

typedef struct {
    uint32_t start, frames; /* in the sf2 sample pool */
    int rate;
    wsmp_t ws;
} wave_t;

static int timecents(int32_t s) {
    if (s == (int32_t)0x80000000) return -12000;
    int v = s / 65536;
    return v < -12000 ? -12000 : v > 8000 ? 8000 : v;
}

static void parse_art(const uint8_t *c, uint32_t sz, art_t *a) {
    if (sz < 8) return;
    uint32_t cb = rd32(c), n = rd32(c + 4);
    if (cb > sz) return;
    for (uint32_t i = 0; i < n && cb + 12 * (i + 1) <= sz; i++) {
        const uint8_t *k = c + cb + 12 * i;
        int src = rd16(k), ctl = rd16(k + 2), dst = rd16(k + 4);
        int32_t scale = (int32_t)rd32(k + 8);
        if (ctl != 0) continue;
        int slot = -1, v = 0;
        if (src == 0) {
            switch (dst) {
            case 0x0206: slot = A_ATTACK; v = timecents(scale); break;
            case 0x0207: slot = A_DECAY; v = timecents(scale); break;
            case 0x0209: slot = A_RELEASE; v = timecents(scale); break;
            case 0x020b: slot = A_DELAY; v = timecents(scale); break;
            case 0x020c: slot = A_HOLD; v = timecents(scale); break;
            case 0x020a: { /* 0.1 % units -> attenuation in cB */
                /* 16.16 fixed point per the spec, but some banks (Apple's
                 * gs_instruments.dls) store the plain 0..1000 value */
                double lvl = (scale > 0 && scale <= 1000 ? scale : scale / 65536.0) / 1000.0;
                slot = A_SUSTAIN;
                v = lvl <= 0.0 ? 1440 : lvl >= 1.0 ? 0 : (int)(-200.0 * log10(lvl));
                if (v > 1440) v = 1440;
                break;
            }
            case 0x0004: slot = A_PAN; v = scale / 65536; break;            /* 0.1 % units, same as SF2 */
            case 0x0001: slot = A_GAIN; v = -scale / 65536; break;          /* 1/655360 dB -> cB */
            case 0x0104: slot = A_LFOFREQ; v = scale / 65536; break; /* absolute cents, same as SF2 */
            case 0x0105: slot = A_LFODELAY; v = timecents(scale); break;
            }
        } else if (src == 0x0001) { /* LFO */
            if (dst == 0x0003) { slot = A_LFOPITCH; v = scale / 65536; }
            else if (dst == 0x0001) { slot = A_LFOVOL; v = -scale / 65536; }
        }
        if (slot >= 0) { a->set[slot] = 1; a->val[slot] = v; }
    }
}

static void parse_wsmp(const uint8_t *c, uint32_t sz, wsmp_t *w) {
    if (sz < 20) return;
    uint32_t cb = rd32(c);
    w->valid = 1;
    w->unity = rd16(c + 4);
    w->fine = (int16_t)rd16(c + 6);
    w->atten_cb = -(int32_t)rd32(c + 8) / 65536;
    uint32_t nloops = rd32(c + 16);
    w->loop = 0;
    if (nloops >= 1 && cb + 16 <= sz) {
        w->loop = 1;
        w->loop_start = (int)rd32(c + cb + 8);
        w->loop_len = (int)rd32(c + cb + 12);
    }
}

typedef struct {
    const uint8_t *d;
    size_t n;
    inst_t *ins;
    int nins, cap_ins;
} dls_t;

/* iterate the subchunks of d[o, end) */
typedef struct { const uint8_t *d; size_t o, end; const uint8_t *id, *c; uint32_t sz; } it_t;
static int it_next(it_t *it) {
    if (it->o + 8 > it->end) return 0;
    uint32_t sz = rd32(it->d + it->o + 4);
    if (sz > it->end - it->o - 8) sz = (uint32_t)(it->end - it->o - 8);
    it->id = it->d + it->o;
    it->c = it->d + it->o + 8;
    it->sz = sz;
    it->o += 8 + (size_t)sz + (sz & 1);
    return 1;
}
#define FOR_CHUNKS(d, o0, e0, ID, C, SZ) \
    for (it_t it_ = {(d), (o0), (e0), 0, 0, 0}; it_next(&it_);) \
        for (const uint8_t *ID = it_.id, *C = it_.c, *once_ = it_.id; once_; once_ = NULL) \
            for (uint32_t SZ = it_.sz; once_; once_ = NULL)

static int is_list(const uint8_t *id, uint32_t sz, const uint8_t *c, const char *type) {
    return !memcmp(id, "LIST", 4) && sz >= 4 && !memcmp(c, type, 4);
}

static void parse_region(dls_t *L, const uint8_t *c, uint32_t sz, region_t *r) {
    const uint8_t *d = L->d;
    size_t base = (size_t)(c - d);
    *r = (region_t){0, 127, 0, 127, 0, -1};
    FOR_CHUNKS(d, base, base + sz, id, k, ksz) {
        if (!memcmp(id, "rgnh", 4) && ksz >= 12) {
            r->key_lo = rd16(k); r->key_hi = rd16(k + 2);
            r->vel_lo = rd16(k + 4); r->vel_hi = rd16(k + 6);
            r->key_group = rd16(k + 10);
        } else if (!memcmp(id, "wsmp", 4)) parse_wsmp(k, ksz, &r->ws);
        else if (!memcmp(id, "wlnk", 4) && ksz >= 12) r->wave = (int)rd32(k + 8);
        else if (is_list(id, ksz, k, "lart") || is_list(id, ksz, k, "lar2")) {
            size_t lb = (size_t)(k - d) + 4;
            FOR_CHUNKS(d, lb, lb + ksz - 4, aid, ac, asz)
                if (!memcmp(aid, "art1", 4) || !memcmp(aid, "art2", 4)) parse_art(ac, asz, &r->art);
        }
    }
    if (r->key_hi > 127) r->key_hi = 127;
    if (r->vel_hi > 127 || r->vel_hi < r->vel_lo) { r->vel_lo = 0; r->vel_hi = 127; }
}

static void parse_ins(dls_t *L, const uint8_t *c, uint32_t sz) {
    const uint8_t *d = L->d;
    size_t base = (size_t)(c - d);
    inst_t in = {{0}};
    int have_hdr = 0;
    FOR_CHUNKS(d, base, base + sz, id, k, ksz) {
        if (!memcmp(id, "insh", 4) && ksz >= 12) {
            uint32_t bank = rd32(k + 4), prog = rd32(k + 8);
            in.drums = (bank & 0x80000000u) != 0;
            in.bank = in.drums ? 128 : (int)((bank >> 8) & 0x7f); /* GS variation = bank select MSB */
            in.program = (int)(prog & 0x7f);
            have_hdr = 1;
        } else if (is_list(id, ksz, k, "lrgn")) {
            size_t lb = (size_t)(k - d) + 4;
            FOR_CHUNKS(d, lb, lb + ksz - 4, rid, rc, rsz) {
                if (is_list(rid, rsz, rc, "rgn ") || is_list(rid, rsz, rc, "rgn2")) {
                    region_t *nr = realloc(in.rg, sizeof *nr * (size_t)(in.nrg + 1));
                    if (!nr) continue;
                    in.rg = nr;
                    parse_region(L, rc + 4, rsz - 4, &in.rg[in.nrg++]);
                }
            }
        } else if (is_list(id, ksz, k, "lart") || is_list(id, ksz, k, "lar2")) {
            size_t lb = (size_t)(k - d) + 4;
            FOR_CHUNKS(d, lb, lb + ksz - 4, aid, ac, asz)
                if (!memcmp(aid, "art1", 4) || !memcmp(aid, "art2", 4)) parse_art(ac, asz, &in.art);
        } else if (is_list(id, ksz, k, "INFO")) {
            size_t lb = (size_t)(k - d) + 4;
            FOR_CHUNKS(d, lb, lb + ksz - 4, nid, nc, nsz)
                if (!memcmp(nid, "INAM", 4)) memcpy(in.name, nc, nsz < 19 ? nsz : 19);
        }
    }
    if (!have_hdr || !in.nrg) { free(in.rg); return; }
    if (!in.name[0]) snprintf(in.name, sizeof in.name, "%s %d:%d", in.drums ? "Drums" : "Inst", in.bank, in.program);
    if (L->nins == L->cap_ins) {
        int nc = L->cap_ins ? L->cap_ins * 2 : 256;
        inst_t *ni = realloc(L->ins, sizeof *ni * (size_t)nc);
        if (!ni) { free(in.rg); return; }
        L->ins = ni;
        L->cap_ins = nc;
    }
    L->ins[L->nins++] = in;
}

/* ---------------------------------------------------------------- SF2 writer */
static void gen(buf_t *g, int op, int amount) { put16(g, (unsigned)op); put16(g, (unsigned)(amount & 0xffff)); }

static void chunk(buf_t *out, const char *id, const buf_t *b) {
    put(out, id, 4);
    put32(out, (uint32_t)b->n);
    put(out, b->p, b->n);
    if (b->n & 1) put(out, "", 1);
}

uint8_t *dls_to_sf2(const uint8_t *d, size_t n, size_t *out_len) {
    if (n < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "DLS ", 4)) return NULL;
    size_t end = 8 + (size_t)rd32(d + 4);
    if (end > n) end = n;
    dls_t L = {d, n, NULL, 0, 0};
    const uint8_t *ptbl = NULL, *wvpl = NULL;
    uint32_t ptbl_sz = 0, wvpl_sz = 0;
    FOR_CHUNKS(d, 12, end, id, c, sz) {
        if (is_list(id, sz, c, "lins")) {
            size_t lb = (size_t)(c - d) + 4;
            FOR_CHUNKS(d, lb, lb + sz - 4, iid, ic, isz)
                if (is_list(iid, isz, ic, "ins ")) parse_ins(&L, ic + 4, isz - 4);
        } else if (!memcmp(id, "ptbl", 4)) { ptbl = c; ptbl_sz = sz; }
        else if (is_list(id, sz, c, "wvpl")) { wvpl = c + 4; wvpl_sz = sz - 4; }
    }
    if (!L.nins || !ptbl || !wvpl || ptbl_sz < 8) { free(L.ins); return NULL; }

    /* waves -> 16-bit mono sample pool */
    uint32_t ncues = rd32(ptbl + 4), cb = rd32(ptbl);
    if (cb + 4ull * ncues > ptbl_sz) ncues = cb < ptbl_sz ? (ptbl_sz - cb) / 4 : 0;
    wave_t *waves = calloc(ncues ? ncues : 1, sizeof *waves);
    buf_t smpl = {0};
    size_t wvpl_off = (size_t)(wvpl - d);
    for (uint32_t i = 0; i < ncues; i++) {
        size_t wo = rd32(ptbl + cb + 4 * i);
        /* offsets are relative to the wave pool data (after the 'wvpl' type) */
        if (wo + 12 > wvpl_sz || memcmp(wvpl + wo, "LIST", 4) || memcmp(wvpl + wo + 8, "wave", 4)) continue;
        uint32_t wsz = rd32(wvpl + wo + 4);
        size_t wb = wvpl_off + wo + 12, we = wvpl_off + wo + 8 + wsz;
        if (we > end) we = end;
        const uint8_t *fmt = NULL, *data = NULL;
        uint32_t data_len = 0;
        FOR_CHUNKS(d, wb, we, id, c, sz) {
            if (!memcmp(id, "fmt ", 4) && sz >= 16) fmt = c;
            else if (!memcmp(id, "data", 4)) { data = c; data_len = sz; }
            else if (!memcmp(id, "wsmp", 4)) parse_wsmp(c, sz, &waves[i].ws);
        }
        if (!fmt || !data || rd16(fmt) != 1) continue;
        int ch = rd16(fmt + 2), bits = rd16(fmt + 14);
        if (ch < 1 || (bits != 8 && bits != 16)) continue;
        uint32_t frames = data_len / (uint32_t)(ch * bits / 8);
        waves[i].start = (uint32_t)(smpl.n / 2);
        waves[i].frames = frames;
        waves[i].rate = (int)rd32(fmt + 4);
        for (uint32_t f = 0; f < frames; f++) {
            int s = bits == 16 ? (int16_t)rd16(data + (size_t)f * ch * 2) : (data[(size_t)f * ch] - 128) << 8;
            put16(&smpl, (unsigned)(s & 0xffff));
        }
        for (int z = 0; z < 46; z++) put16(&smpl, 0); /* SF2 requires 46 zero samples after each */
    }

    /* pdta */
    buf_t phdr = {0}, pbag = {0}, pgen = {0}, inst = {0}, ibag = {0}, igen = {0}, shdr = {0}, mod = {0};
    int npbag = 0, npgen = 0, nibag = 0, nigen = 0;
    for (int i = 0; i < L.nins; i++) {
        inst_t *in = &L.ins[i];
        putname(&phdr, in->name);
        put16(&phdr, (unsigned)in->program);
        put16(&phdr, (unsigned)in->bank);
        put16(&phdr, (unsigned)npbag);
        put32(&phdr, 0); put32(&phdr, 0); put32(&phdr, 0);
        put16(&pbag, (unsigned)npgen); put16(&pbag, 0); npbag++;
        gen(&pgen, 41, i); npgen++; /* instrument */

        putname(&inst, in->name);
        put16(&inst, (unsigned)nibag);
        for (int r = 0; r < in->nrg; r++) {
            region_t *rg = &in->rg[r];
            if (rg->wave < 0 || (uint32_t)rg->wave >= ncues || !waves[rg->wave].frames) continue;
            wave_t *w = &waves[rg->wave];
            const wsmp_t *ws = rg->ws.valid ? &rg->ws : &w->ws;
            art_t a = in->art; /* region articulation overrides the instrument's */
            for (int k = 0; k < 16; k++) if (rg->art.set[k]) { a.set[k] = 1; a.val[k] = rg->art.val[k]; }
            put16(&ibag, (unsigned)nigen); put16(&ibag, 0); nibag++;
            gen(&igen, 43, rg->key_lo | rg->key_hi << 8); nigen++;
            gen(&igen, 44, rg->vel_lo | rg->vel_hi << 8); nigen++;
            gen(&igen, 58, ws->valid ? ws->unity : 60); nigen++;
            if (ws->valid && ws->fine) { gen(&igen, 52, ws->fine); nigen++; }
            int att = (ws->valid ? ws->atten_cb : 0) + (a.set[A_GAIN] ? a.val[A_GAIN] : 0);
            if (att > 0) { gen(&igen, 48, att > 1440 ? 1440 : att); nigen++; }
            if (ws->valid && ws->loop && ws->loop_len > 0) {
                gen(&igen, 54, 1); nigen++;
                /* shdr carries the wave's own loop; per-region loops are offsets from it */
                int base_s = w->ws.valid && w->ws.loop ? w->ws.loop_start : 0;
                int base_e = w->ws.valid && w->ws.loop ? w->ws.loop_start + w->ws.loop_len : 0;
                int ds = ws->loop_start - base_s, de = ws->loop_start + ws->loop_len - base_e;
                if (ds) { gen(&igen, 2, ds % 32768); gen(&igen, 45, ds / 32768); nigen += 2; }
                if (de) { gen(&igen, 3, de % 32768); gen(&igen, 50, de / 32768); nigen += 2; }
            }
            static const int gmap[][2] = {{A_DELAY, 33}, {A_ATTACK, 34}, {A_HOLD, 35}, {A_DECAY, 36}, {A_SUSTAIN, 37},
                                          {A_RELEASE, 38}, {A_PAN, 17}, {A_LFOFREQ, 22}, {A_LFODELAY, 21},
                                          {A_LFOPITCH, 5}, {A_LFOVOL, 13}};
            for (size_t k = 0; k < sizeof gmap / sizeof gmap[0]; k++)
                if (a.set[gmap[k][0]]) { gen(&igen, gmap[k][1], a.val[gmap[k][0]]); nigen++; }
            if (!a.set[A_RELEASE]) { gen(&igen, 38, -1200); nigen++; } /* short default release, avoids clicks */
            if (rg->key_group) { gen(&igen, 57, rg->key_group); nigen++; }
            gen(&igen, 53, rg->wave); nigen++;
        }
    }
    /* terminal records */
    putname(&phdr, "EOP"); put16(&phdr, 0); put16(&phdr, 0); put16(&phdr, (unsigned)npbag);
    put32(&phdr, 0); put32(&phdr, 0); put32(&phdr, 0);
    put16(&pbag, (unsigned)npgen); put16(&pbag, 0);
    gen(&pgen, 0, 0);
    putname(&inst, "EOI"); put16(&inst, (unsigned)nibag);
    put16(&ibag, (unsigned)nigen); put16(&ibag, 0);
    gen(&igen, 0, 0);
    for (int z = 0; z < 10; z++) put(&mod, "", 1);
    for (uint32_t i = 0; i < ncues; i++) {
        wave_t *w = &waves[i];
        char nm[20];
        snprintf(nm, sizeof nm, "wave%u", i);
        putname(&shdr, nm);
        put32(&shdr, w->start);
        put32(&shdr, w->start + w->frames);
        uint32_t ls = w->start, le = w->start;
        if (w->ws.valid && w->ws.loop && w->ws.loop_len > 0) {
            ls = w->start + (uint32_t)w->ws.loop_start;
            le = ls + (uint32_t)w->ws.loop_len;
        }
        put32(&shdr, ls);
        put32(&shdr, le);
        put32(&shdr, (uint32_t)(w->rate ? w->rate : 22050));
        uint8_t pitch[2] = {(uint8_t)(w->ws.valid ? w->ws.unity : 60), 0};
        put(&shdr, pitch, 2);
        put16(&shdr, 0);
        put16(&shdr, 1); /* mono */
    }
    putname(&shdr, "EOS");
    for (int z = 0; z < 26; z++) put(&shdr, "", 1);

    buf_t info = {0}, sdta = {0}, pdta = {0}, body = {0}, out = {0};
    put(&info, "INFO", 4);
    { buf_t v = {0}; put16(&v, 2); put16(&v, 1); chunk(&info, "ifil", &v); free(v.p); }
    { buf_t v = {0}; put(&v, "EMU8000", 8); chunk(&info, "isng", &v); free(v.p); }
    { buf_t v = {0}; put(&v, "Converted DLS", 14); chunk(&info, "INAM", &v); free(v.p); }
    put(&sdta, "sdta", 4);
    chunk(&sdta, "smpl", &smpl);
    put(&pdta, "pdta", 4);
    chunk(&pdta, "phdr", &phdr);
    chunk(&pdta, "pbag", &pbag);
    chunk(&pdta, "pmod", &mod);
    chunk(&pdta, "pgen", &pgen);
    chunk(&pdta, "inst", &inst);
    chunk(&pdta, "ibag", &ibag);
    chunk(&pdta, "imod", &mod);
    chunk(&pdta, "igen", &igen);
    chunk(&pdta, "shdr", &shdr);
    put(&body, "sfbk", 4);
    chunk(&body, "LIST", &info);
    chunk(&body, "LIST", &sdta);
    chunk(&body, "LIST", &pdta);
    chunk(&out, "RIFF", &body);

    buf_t *all[] = {&smpl, &phdr, &pbag, &pgen, &inst, &ibag, &igen, &shdr, &mod, &info, &sdta, &pdta, &body};
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) free(all[i]->p);
    for (int i = 0; i < L.nins; i++) free(L.ins[i].rg);
    free(L.ins);
    free(waves);
    *out_len = out.n;
    return out.p;
}
