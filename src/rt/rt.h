/*
 * Nitemare-3D recompiled runtime.
 *
 * The game code in src/gen/ was produced by tools/recomp.py from the Ghidra
 * p-code of the original 16-bit Windows executable. It runs against the
 * emulated CPU state and segmented memory declared here; the Win16 API,
 * WinG, MMSYSTEM and DOS services it calls are re-implemented on top of SDL2
 * in src/rt/win16.c and src/rt/dos.c.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Values Windows patches into imported KERNEL constants. */
#define RT_AHSHIFT 3
#define RT_AHINCR 8
#define RT_WINFLAGS 0x0425 /* WF_PMODE | WF_CPU386 | WF_ENHANCED | WF_80x87 */
#define RT_SEL_A000 0xA000

/* ---- CPU state: register file indexed by Ghidra x86 register offsets ---- */
extern uint8_t rt_regs[0x800];
#define RG8(o) (*(uint8_t *)(rt_regs + (o)))
#define RG16(o) (*(uint16_t *)(rt_regs + (o)))
#define RG32(o) (*(uint32_t *)(rt_regs + (o)))
#define RG64(o) (*(uint64_t *)(rt_regs + (o)))

enum {
    R_AX = 0x00, R_CX = 0x04, R_DX = 0x08, R_BX = 0x0c,
    R_SP = 0x10, R_BP = 0x14, R_SI = 0x18, R_DI = 0x1c,
    R_ES = 0x100, R_CS = 0x102, R_SS = 0x104, R_DS = 0x106, R_FS = 0x108, R_GS = 0x10a,
    R_CF = 0x200, R_PF = 0x202, R_AF = 0x204, R_ZF = 0x206, R_SF = 0x207,
    R_TF = 0x208, R_IF = 0x209, R_DF = 0x20a, R_OF = 0x20b,
    R_IP = 0x284,
};
#define rAX RG16(R_AX)
#define rBX RG16(R_BX)
#define rCX RG16(R_CX)
#define rDX RG16(R_DX)
#define rSP RG16(R_SP)
#define rBP RG16(R_BP)
#define rSI RG16(R_SI)
#define rDI RG16(R_DI)
#define rAL RG8(R_AX)
#define rAH RG8(R_AX + 1)
#define rBL RG8(R_BX)
#define rBH RG8(R_BX + 1)
#define rCL RG8(R_CX)
#define rCH RG8(R_CX + 1)
#define rDL RG8(R_DX)
#define rDH RG8(R_DX + 1)
#define rES RG16(R_ES)
#define rCS RG16(R_CS)
#define rSS RG16(R_SS)
#define rDS RG16(R_DS)
#define rCF RG8(R_CF)

/* ---- Segmented memory ----
 * A linear address is (selector << 16) | offset, matching Ghidra's model of
 * the NE image. Each selector maps to its own 64 KiB (+ slack) buffer. */
#define RT_SEG_BYTES (0x10000 + 16)
extern uint8_t *rt_seg[0x10000];
uint8_t *rt_bad_access(uint32_t lin);

static inline uint8_t *LP(uint32_t a) {
    uint8_t *b = rt_seg[a >> 16];
    if (!b) return rt_bad_access(a);
    return b + (a & 0xffffu);
}
static inline uint32_t SEGLIN(uint32_t sel, uint32_t off) { return ((sel & 0xffffu) << 16) | (off & 0xffffu); }
static inline uint8_t MEMR8(uint32_t a) { return *LP(a); }
static inline uint16_t MEMR16(uint32_t a) { uint16_t v; memcpy(&v, LP(a), 2); return v; }
static inline uint32_t MEMR32(uint32_t a) { uint32_t v; memcpy(&v, LP(a), 4); return v; }
static inline uint64_t MEMR64(uint32_t a) { uint64_t v; memcpy(&v, LP(a), 8); return v; }
static inline void MEMW8(uint32_t a, uint64_t v) { *LP(a) = (uint8_t)v; }
static inline void MEMW16(uint32_t a, uint64_t v) { uint16_t x = (uint16_t)v; memcpy(LP(a), &x, 2); }
static inline void MEMW32(uint32_t a, uint64_t v) { uint32_t x = (uint32_t)v; memcpy(LP(a), &x, 4); }
static inline void MEMW64(uint32_t a, uint64_t v) { memcpy(LP(a), &v, 8); }

/* ---- p-code helpers ---- */
#define SX8(x) ((int64_t)(int8_t)(x))
#define SX16(x) ((int64_t)(int16_t)(x))
#define SX32(x) ((int64_t)(int32_t)(x))
#define SX64(x) ((int64_t)(x))
static inline uint8_t CARRY8(uint64_t a, uint64_t b) { return ((a & 0xff) + (b & 0xff)) > 0xff; }
static inline uint8_t CARRY16(uint64_t a, uint64_t b) { return ((a & 0xffff) + (b & 0xffff)) > 0xffff; }
static inline uint8_t CARRY32(uint64_t a, uint64_t b) { return ((a & 0xffffffffu) + (b & 0xffffffffu)) > 0xffffffffu; }
static inline uint8_t CARRY64(uint64_t a, uint64_t b) { return (a + b) < a; }
#define DEF_SFLAGS(n, T)                                                              \
    static inline uint8_t SCARRY##n(uint64_t a, uint64_t b) {                         \
        T x = (T)a, y = (T)b, r = (T)((uint64_t)x + (uint64_t)y);                      \
        return ((x < 0) == (y < 0)) && ((r < 0) != (x < 0));                           \
    }                                                                                  \
    static inline uint8_t SBORROW##n(uint64_t a, uint64_t b) {                        \
        T x = (T)a, y = (T)b, r = (T)((uint64_t)x - (uint64_t)y);                      \
        return ((x < 0) != (y < 0)) && ((r < 0) != (x < 0));                           \
    }
DEF_SFLAGS(8, int8_t)
DEF_SFLAGS(16, int16_t)
DEF_SFLAGS(32, int32_t)
DEF_SFLAGS(64, int64_t)
#undef DEF_SFLAGS

#define DEF_SHIFTS(n, U, S)                                                                             \
    static inline U SHL##n(uint64_t a, uint64_t b) { return b >= n ? 0 : (U)((U)a << b); }               \
    static inline U SHR##n(uint64_t a, uint64_t b) { return b >= n ? 0 : (U)((U)a >> b); }               \
    static inline U SAR##n(uint64_t a, uint64_t b) { return (U)(b >= n ? ((S)a < 0 ? -1 : 0) : ((S)a >> b)); }
DEF_SHIFTS(8, uint8_t, int8_t)
DEF_SHIFTS(16, uint16_t, int16_t)
DEF_SHIFTS(32, uint32_t, int32_t)
DEF_SHIFTS(64, uint64_t, int64_t)
#undef DEF_SHIFTS

void rt_divide_error(void);
static inline uint64_t DIVU(uint64_t a, uint64_t b) { if (!b) { rt_divide_error(); return 0; } return a / b; }
static inline uint64_t REMU(uint64_t a, uint64_t b) { if (!b) { rt_divide_error(); return 0; } return a % b; }
static inline uint64_t DIVS(int64_t a, int64_t b) { if (!b) { rt_divide_error(); return 0; } return (uint64_t)(a / b); }
static inline uint64_t REMS(int64_t a, int64_t b) { if (!b) { rt_divide_error(); return 0; } return (uint64_t)(a % b); }
static inline uint8_t POPCNT(uint64_t x) {
    uint8_t c = 0;
    while (x) { x &= x - 1; c++; }
    return c;
}

/* ---- control transfer out of generated code ---- */
void rt_api(uint32_t addr);          /* call into an imported Win16 API thunk */
void rt_call_ind(uint32_t lin);      /* indirect far/near call */
void rt_call_unknown(uint32_t lin);  /* direct call to an address that is not a known function */
void rt_jump_ind(uint32_t lin);      /* indirect jump outside the current function */
void rt_jump_unknown(uint32_t lin);  /* direct jump to a non-entry address in another function */
void rt_int(uint64_t n);             /* INT n */
uint8_t rt_in8(uint64_t port);
uint16_t rt_in16(uint64_t port);
uint32_t rt_in32(uint64_t port);
void rt_out8(uint64_t port, uint64_t v);
void rt_out16(uint64_t port, uint64_t v);
void rt_out32(uint64_t port, uint64_t v);

#ifdef RT_TRACE
void rt_trace_enter(uint32_t addr);
#define RT_ENTER(a) rt_trace_enter(a)
#else
#define RT_ENTER(a) ((void)0)
#endif

/* ---- tables generated from the executable ---- */
typedef struct { uint16_t sel; uint32_t size; const uint8_t *data; uint32_t data_len; } rt_segimage_t;
typedef struct { uint32_t addr; const char *lib; const char *name; } rt_import_t;
extern const rt_segimage_t rt_seg_images[];
extern const int rt_seg_image_count;
extern const rt_import_t rt_imports[];
extern const int rt_import_count;

/* ---- runtime services ---- */
#define RT_SEL_DS 0x1048
void rt_init(void);
void rt_fatal(const char *fmt, ...);
void rt_log(const char *fmt, ...);
uint16_t rt_alloc_selectors(uint32_t bytes);           /* returns first selector, AHINCR stride */
void rt_free_selectors(uint16_t sel);
uint32_t rt_selector_size(uint16_t sel);
uint16_t rt_realloc_selectors(uint16_t sel, uint32_t bytes);
void rt_push16(uint16_t v);
uint16_t rt_pop16(void);
/* Call a recompiled far __cdecl function with 16-bit word arguments (first
 * argument first). Returns DX:AX. */
uint32_t rt_callf(uint32_t fn, int nwords, const uint16_t *args);
/* Call a far __pascal (callee-pops) function. */
uint32_t rt_callf_pascal(uint32_t fn, int nwords, const uint16_t *args);
const char *rt_func_name(uint32_t lin);
/* String helpers for guest memory */
char *rt_strdup_far(uint32_t lin);
void rt_strcpy_to_far(uint32_t lin, const char *s, size_t max);
size_t rt_strlen_far(uint32_t lin);

/* Near (DS-relative) local heap */
uint16_t rt_local_alloc(uint16_t flags, uint16_t size);
uint16_t rt_local_free(uint16_t off);
uint16_t rt_local_realloc(uint16_t off, uint16_t size, uint16_t flags);
uint16_t rt_local_size(uint16_t off);

/* DOS emulation (dos.c) */
void dos_int21(void);
void dos_set_paths(const char *data_dir, const char *user_dir);
const char *dos_data_dir(void);
const char *dos_user_dir(void);
/* Open a host file following the DOS path rules (data dir, user dir). */
void *dos_fopen(const char *dospath, const char *mode);

#ifdef __cplusplus
}
#endif
