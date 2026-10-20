/* Core of the recompiled runtime: memory, calls, traps, local heap. */
#include "rt/rt.h"
#include "gen/rc_funcs.h"
#include "rt/host.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(_MSC_VER)
__declspec(align(16)) uint8_t rt_regs[0x800];
#else
uint8_t rt_regs[0x800] __attribute__((aligned(16)));
#endif
uint8_t *rt_seg[0x10000];

/* Dynamic selectors are handed out from here upward in AHINCR steps. */
#define DYN_SEL_FIRST 0x2000
#define DYN_SEL_LAST 0x9ff8
static uint16_t sel_count[0x10000]; /* number of consecutive selectors in a block (on the first) */
static uint32_t sel_bytes[0x10000];

/* DS layout (see NE header): data 0x8e9e, stack 0x2ee0 above it, local heap above that. */
#define DS_DATA_END 0x8e9e
#define DS_STACK_TOP 0xbd7e
#define LHEAP_START 0xbd80
#define LHEAP_END 0xfff0

void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

void rt_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "FATAL: ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n  CS:IP? regs AX=%04x BX=%04x CX=%04x DX=%04x SI=%04x DI=%04x BP=%04x SP=%04x DS=%04x ES=%04x SS=%04x\n",
            rAX, rBX, rCX, rDX, rSI, rDI, rBP, rSP, rDS, rES, rSS);
    fflush(stderr);
    abort();
}

uint8_t *rt_bad_access(uint32_t lin) {
    static uint8_t scratch[RT_SEG_BYTES];
    static int reported;
    if (reported++ < 20)
        rt_log("warning: access to unmapped selector %04x:%04x", lin >> 16, lin & 0xffff);
    return scratch + (lin & 0xffff);
}

void rt_divide_error(void) { rt_fatal("divide by zero"); }

/* ---- function lookup ---- */
static int func_index(uint32_t lin) {
    int lo = 0, hi = rc_func_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (rc_func_table[mid].addr == lin) return mid;
        if (rc_func_table[mid].addr < lin) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

const char *rt_func_name(uint32_t lin) {
    int i = func_index(lin);
    return i >= 0 ? rc_func_table[i].name : "?";
}

void rt_call_ind(uint32_t lin) {
    if ((lin >> 16) == 0x1200) { rt_api(lin); return; }
    int i = func_index(lin);
    if (i < 0) rt_fatal("indirect call to unknown address %04x:%04x", lin >> 16, lin & 0xffff);
    rc_func_table[i].fn();
}

void rt_call_unknown(uint32_t lin) { rt_fatal("call to non-function address %04x:%04x", lin >> 16, lin & 0xffff); }
void rt_jump_ind(uint32_t lin) {
    int i = func_index(lin);
    if (i < 0) rt_fatal("indirect jump to unknown address %04x:%04x", lin >> 16, lin & 0xffff);
    rc_func_table[i].fn(); /* tail call */
}
void rt_jump_unknown(uint32_t lin) { rt_fatal("jump into the middle of another function at %04x:%04x", lin >> 16, lin & 0xffff); }

#ifdef RT_TRACE
void rt_trace_enter(uint32_t addr) {
    static FILE *f;
    static int all = -1;
    if (all < 0) all = getenv("N3D_TRACE_ALL") != NULL;
    if (!all && (addr >> 16) < 0x1010) return;
    if (!f) f = fopen("rt_trace.txt", "w");
    fprintf(f, "%04x:%04x %s SP=%04x args=%04x %04x %04x\n", addr >> 16, addr & 0xffff, rt_func_name(addr), rSP,
            MEMR16(SEGLIN(rSS, (uint16_t)(rSP + 4))), MEMR16(SEGLIN(rSS, (uint16_t)(rSP + 6))), MEMR16(SEGLIN(rSS, (uint16_t)(rSP + 8))));
    fflush(f);
}
#endif

/* ---- stack and host->guest calls ---- */
void rt_push16(uint16_t v) {
    rSP = (uint16_t)(rSP - 2);
    MEMW16(SEGLIN(rSS, rSP), v);
}
uint16_t rt_pop16(void) {
    uint16_t v = MEMR16(SEGLIN(rSS, rSP));
    rSP = (uint16_t)(rSP + 2);
    return v;
}

static uint32_t call_common(uint32_t fn, int nwords, const uint16_t *args, int pascal) {
    int i = func_index(fn);
    if (i < 0) rt_fatal("rt_callf: no function at %04x:%04x", fn >> 16, fn & 0xffff);
    uint16_t sp0 = rSP;
    uint16_t saved_cs = rCS, saved_ds = rDS;
    if (pascal) for (int k = 0; k < nwords; k++) rt_push16(args[k]);
    else for (int k = nwords - 1; k >= 0; k--) rt_push16(args[k]);
    rt_push16(0xFFFF); /* return CS (host) */
    rt_push16(0xFFFF); /* return IP (host) */
    rCS = (uint16_t)(fn >> 16);
    rDS = RT_SEL_DS;
    rc_func_table[i].fn();
    if (!pascal) rSP = (uint16_t)(rSP + 2 * nwords);
    if (rSP != sp0)
        rt_log("warning: stack imbalance after %s: SP=%04x expected %04x", rc_func_table[i].name, rSP, sp0);
    rSP = sp0;
    rCS = saved_cs;
    rDS = saved_ds;
    return ((uint32_t)rDX << 16) | rAX;
}

uint32_t rt_callf(uint32_t fn, int nwords, const uint16_t *args) { return call_common(fn, nwords, args, 0); }
uint32_t rt_callf_pascal(uint32_t fn, int nwords, const uint16_t *args) { return call_common(fn, nwords, args, 1); }

/* ---- selectors ---- */
uint16_t rt_alloc_selectors(uint32_t bytes) {
    uint32_t n = bytes ? (bytes + 0xffff) / 0x10000 : 1;
    for (uint32_t s = DYN_SEL_FIRST; s + (n - 1) * RT_AHINCR <= DYN_SEL_LAST; s += RT_AHINCR) {
        uint32_t k;
        for (k = 0; k < n; k++)
            if (rt_seg[s + k * RT_AHINCR]) break;
        if (k != n) { s += k * RT_AHINCR; continue; }
        for (k = 0; k < n; k++) {
            rt_seg[s + k * RT_AHINCR] = calloc(1, RT_SEG_BYTES);
            if (!rt_seg[s + k * RT_AHINCR]) rt_fatal("out of host memory");
        }
        sel_count[s] = (uint16_t)n;
        sel_bytes[s] = bytes;
        return (uint16_t)s;
    }
    return 0;
}

void rt_free_selectors(uint16_t sel) {
    if (sel < DYN_SEL_FIRST || sel > DYN_SEL_LAST || !sel_count[sel]) return;
    for (uint32_t k = 0; k < sel_count[sel]; k++) {
        free(rt_seg[sel + k * RT_AHINCR]);
        rt_seg[sel + k * RT_AHINCR] = NULL;
    }
    sel_count[sel] = 0;
    sel_bytes[sel] = 0;
}

uint32_t rt_selector_size(uint16_t sel) {
    if (sel_count[sel]) return sel_bytes[sel];
    return rt_seg[sel] ? 0x10000 : 0;
}

uint16_t rt_realloc_selectors(uint16_t sel, uint32_t bytes) {
    if (!sel_count[sel]) return 0;
    uint32_t need = bytes ? (bytes + 0xffff) / 0x10000 : 1;
    if (need <= sel_count[sel]) { sel_bytes[sel] = bytes; return sel; }
    /* Try to grow in place. */
    uint32_t k;
    for (k = sel_count[sel]; k < need; k++)
        if (sel + k * RT_AHINCR > DYN_SEL_LAST || rt_seg[sel + k * RT_AHINCR]) break;
    if (k == need) {
        for (k = sel_count[sel]; k < need; k++) rt_seg[sel + k * RT_AHINCR] = calloc(1, RT_SEG_BYTES);
        sel_count[sel] = (uint16_t)need;
        sel_bytes[sel] = bytes;
        return sel;
    }
    uint16_t ns = rt_alloc_selectors(bytes);
    if (!ns) return 0;
    for (k = 0; k < sel_count[sel]; k++) memcpy(rt_seg[ns + k * RT_AHINCR], rt_seg[sel + k * RT_AHINCR], 0x10000);
    rt_free_selectors(sel);
    return ns;
}

/* ---- local (near) heap inside DS: first-fit list of [size|used] headers ---- */
/* Block header: uint16 size (including header, multiple of 4), uint16 flags (1=used). */
static uint8_t *ds(void) { return rt_seg[RT_SEL_DS]; }
static uint16_t hdr_size(uint16_t h) { uint16_t v; memcpy(&v, ds() + h, 2); return v; }
static uint16_t hdr_used(uint16_t h) { uint16_t v; memcpy(&v, ds() + h + 2, 2); return v; }
static void hdr_set(uint16_t h, uint16_t size, uint16_t used) { memcpy(ds() + h, &size, 2); memcpy(ds() + h + 2, &used, 2); }

static void lheap_init(void) { hdr_set(LHEAP_START, (uint16_t)(LHEAP_END - LHEAP_START), 0); }

uint16_t rt_local_alloc(uint16_t flags, uint16_t size) {
    uint32_t need = ((uint32_t)size + 4 + 3) & ~3u;
    if (need < 8) need = 8;
    for (uint32_t h = LHEAP_START; h < LHEAP_END; h += hdr_size((uint16_t)h)) {
        uint16_t sz = hdr_size((uint16_t)h);
        if (!sz) break;
        if (hdr_used((uint16_t)h)) continue;
        /* coalesce following free blocks */
        while (h + sz < LHEAP_END && !hdr_used((uint16_t)(h + sz)) && hdr_size((uint16_t)(h + sz))) {
            sz = (uint16_t)(sz + hdr_size((uint16_t)(h + sz)));
            hdr_set((uint16_t)h, sz, 0);
        }
        if (sz < need) continue;
        if (sz - need >= 8) {
            hdr_set((uint16_t)(h + need), (uint16_t)(sz - need), 0);
            sz = (uint16_t)need;
        }
        hdr_set((uint16_t)h, sz, 1);
        if (flags & 0x40) memset(ds() + h + 4, 0, sz - 4); /* LMEM_ZEROINIT */
        return (uint16_t)(h + 4);
    }
    rt_log("warning: LocalAlloc(%u) failed", size);
    return 0;
}

uint16_t rt_local_free(uint16_t off) {
    if (off < LHEAP_START + 4 || off >= LHEAP_END) return off;
    hdr_set((uint16_t)(off - 4), hdr_size((uint16_t)(off - 4)), 0);
    return 0;
}

uint16_t rt_local_size(uint16_t off) {
    if (off < LHEAP_START + 4 || off >= LHEAP_END) return 0;
    return (uint16_t)(hdr_size((uint16_t)(off - 4)) - 4);
}

uint16_t rt_local_realloc(uint16_t off, uint16_t size, uint16_t flags) {
    if (!off) return rt_local_alloc(flags, size);
    uint16_t old = rt_local_size(off);
    if (size <= old) return off;
    uint16_t n = rt_local_alloc(flags, size);
    if (!n) return 0;
    memmove(ds() + n, ds() + off, old);
    rt_local_free(off);
    return n;
}

/* ---- guest strings ---- */
size_t rt_strlen_far(uint32_t lin) {
    size_t n = 0;
    while (MEMR8(lin + (uint32_t)n) && n < 0xffff) n++;
    return n;
}
char *rt_strdup_far(uint32_t lin) {
    size_t n = rt_strlen_far(lin);
    char *s = malloc(n + 1);
    for (size_t i = 0; i < n; i++) s[i] = (char)MEMR8(lin + (uint32_t)i);
    s[n] = 0;
    return s;
}
void rt_strcpy_to_far(uint32_t lin, const char *s, size_t max) {
    size_t i = 0;
    for (; s[i] && i + 1 < max; i++) MEMW8(lin + (uint32_t)i, (uint8_t)s[i]);
    if (max) MEMW8(lin + (uint32_t)i, 0);
}

/* ---- interrupts and ports ---- */
void rt_int(uint64_t n) {
    host_audio_poll();
    switch ((uint8_t)n) {
    case 0x21: dos_int21(); break;
    case 0x10: /* video BIOS: mode set for the DOS-style full-screen path; ignore */ break;
    case 0x2f: rAX = 0; break;
    default: rt_log("warning: INT %02x ignored (AX=%04x)", (unsigned)(uint8_t)n, rAX); break;
    }
}

/* VGA ports: the palette fades poll the input status register (0x3DA) for
 * vertical retrace; emulate a 70 Hz display. Other ports belong to the
 * optional full-screen (mode X) path and are ignored. */
uint8_t rt_in8(uint64_t port) {
    if ((port & 0xffff) == 0x3da) {
        uint64_t us = host_time_us() % 14286; /* 70 Hz frame */
        uint8_t v = 0;
        if (us < 1000) v |= 0x08 | 0x01;       /* in vertical retrace (display disabled) */
        else if ((us / 32) & 1) v |= 0x01;      /* horizontal blanking flicker */
        return v;
    }
    return 0;
}
uint16_t rt_in16(uint64_t port) { (void)port; return 0; }
uint32_t rt_in32(uint64_t port) { (void)port; return 0; }
void rt_out8(uint64_t port, uint64_t v) { (void)port; (void)v; }
void rt_out16(uint64_t port, uint64_t v) { (void)port; (void)v; }
void rt_out32(uint64_t port, uint64_t v) { (void)port; (void)v; }

/* ---- init ---- */
void rt_init(void) {
    memset(rt_regs, 0, sizeof rt_regs);
    for (int i = 0; i < rt_seg_image_count; i++) {
        const rt_segimage_t *s = &rt_seg_images[i];
        uint8_t *b = calloc(1, RT_SEG_BYTES);
        if (s->data) memcpy(b, s->data, s->data_len);
        rt_seg[s->sel] = b;
    }
    rt_seg[RT_SEL_A000] = calloc(1, RT_SEG_BYTES);
    /* Instance data header used by the C runtime: pStackTop/pStackMin/pStackBot. */
    uint8_t *d = rt_seg[RT_SEL_DS];
    uint16_t top = DS_DATA_END, bot = DS_STACK_TOP;
    memcpy(d + 0x0a, &top, 2);
    memcpy(d + 0x0c, &bot, 2);
    memcpy(d + 0x0e, &bot, 2);
    rSS = RT_SEL_DS;
    rDS = RT_SEL_DS;
    rES = RT_SEL_DS;
    rSP = DS_STACK_TOP;
    rBP = 0;
    lheap_init();
}
