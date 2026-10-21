/*
 * Win16 API surface used by NITE3W.EXE, re-implemented for the recompiled
 * game. Each handler reads its Pascal-convention arguments from the emulated
 * stack (last argument at SS:SP+4), sets AX/DX, and the dispatcher pops the
 * far return address plus the argument bytes, as the real callee would.
 */
#include "rt/rt.h"
#include "rt/host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- argument helpers ---- */
static uint16_t A16(int off) { return MEMR16(SEGLIN(rSS, (uint16_t)(rSP + 4 + off))); }
static uint32_t A32(int off) { return MEMR32(SEGLIN(rSS, (uint16_t)(rSP + 4 + off))); }
static void ret16(uint16_t v) { rAX = v; }
static void ret32(uint32_t v) { rAX = (uint16_t)v; rDX = (uint16_t)(v >> 16); }

/* Handles. */
#define HWND_FRAME 0x0100
#define HWND_VIEW 0x0102
#define HWND_TOOLBAR 0x0104
#define HWND_STATUS 0x0106
#define HWND_DESKTOP 0x0108
#define HDC_SCREEN 0x0200
#define HDC_WING 0x0202
#define HINSTANCE_APP 0x0300
#define HWAVEOUT_1 0x0400
#define HDRVR_JOY 0x0500
#define HPAL_FIRST 0x0600

/* ---- GDI palette objects ---- */
typedef struct { int used; uint8_t rgb[256][4]; uint16_t n; } pal_t;
static pal_t pals[16];
static uint16_t selected_pal = 0;
static uint16_t realized_pal = 0;

static pal_t *pal_get(uint16_t h) {
    if (h < HPAL_FIRST || h >= HPAL_FIRST + 16) return NULL;
    pal_t *p = &pals[h - HPAL_FIRST];
    return p->used ? p : NULL;
}

static void pal_apply_to_screen(pal_t *p, int start, int count) {
    for (int i = start; i < start + count && i < 256; i++) host_set_palette_entry(i, p->rgb[i][0], p->rgb[i][1], p->rgb[i][2]);
}

/* ---- WinG bitmap ---- */
static uint16_t wing_sel = 0;    /* selector holding the 320x200 DIB */
static int32_t wing_w = 320, wing_h = 200;

static void present_wing(void) {
    if (!wing_sel) return;
    host_present(rt_seg[wing_sel], wing_w, wing_h < 0 ? -wing_h : wing_h);
}

/* ---- profile (INI) emulation: a tiny in-memory store persisted to nite3w.ini ---- */
typedef struct { char sec[64], key[64], val[256]; } ini_ent_t;
static ini_ent_t ini[128];
static int ini_n;
static int ini_loaded;

static void ini_load(void) {
    if (ini_loaded) return;
    ini_loaded = 1;
    FILE *f = dos_fopen("nite3w.ini", "rb");
    if (!f) return;
    char line[512], sec[64] = "";
    while (fgets(line, sizeof line, f)) {
        char *p = line + strcspn(line, "\r\n");
        *p = 0;
        if (line[0] == '[') {
            char *e = strchr(line, ']');
            if (e) *e = 0;
            snprintf(sec, sizeof sec, "%s", line + 1);
        } else if (strchr(line, '=') && ini_n < 128) {
            char *eq = strchr(line, '=');
            *eq = 0;
            snprintf(ini[ini_n].sec, 64, "%s", sec);
            snprintf(ini[ini_n].key, 64, "%s", line);
            snprintf(ini[ini_n].val, 256, "%s", eq + 1);
            ini_n++;
        }
    }
    fclose(f);
}

static void ini_save(void) {
    FILE *f = dos_fopen("nite3w.ini", "wb");
    if (!f) return;
    for (int i = 0; i < ini_n; i++) {
        int first = 1;
        for (int j = 0; j < i; j++) if (!strcasecmp(ini[j].sec, ini[i].sec)) first = 0;
        if (!first) continue;
        fprintf(f, "[%s]\n", ini[i].sec);
        for (int j = i; j < ini_n; j++)
            if (!strcasecmp(ini[j].sec, ini[i].sec)) fprintf(f, "%s=%s\n", ini[j].key, ini[j].val);
    }
    fclose(f);
}

static const char *ini_get(const char *sec, const char *key) {
    ini_load();
    for (int i = 0; i < ini_n; i++)
        if (!strcasecmp(ini[i].sec, sec) && !strcasecmp(ini[i].key, key)) return ini[i].val;
    return NULL;
}

static void ini_set(const char *sec, const char *key, const char *val) {
    ini_load();
    for (int i = 0; i < ini_n; i++)
        if (!strcasecmp(ini[i].sec, sec) && !strcasecmp(ini[i].key, key)) {
            if (val) snprintf(ini[i].val, 256, "%s", val);
            else { ini[i] = ini[--ini_n]; }
            ini_save();
            return;
        }
    if (val && ini_n < 128) {
        snprintf(ini[ini_n].sec, 64, "%s", sec);
        snprintf(ini[ini_n].key, 64, "%s", key);
        snprintf(ini[ini_n].val, 256, "%s", val);
        ini_n++;
        ini_save();
    }
}

static uint16_t profile_string(uint32_t app, uint32_t key, uint32_t def, uint32_t ret, uint16_t size) {
    char *sapp = app ? rt_strdup_far(app) : NULL;
    char *skey = key ? rt_strdup_far(key) : NULL;
    char *sdef = def ? rt_strdup_far(def) : strdup("");
    const char *v = (sapp && skey) ? ini_get(sapp, skey) : NULL;
    if (!v) v = sdef;
    rt_strcpy_to_far(ret, v, size);
    uint16_t n = (uint16_t)strlen(v);
    if (n >= size) n = size ? size - 1 : 0;
    free(sapp); free(skey); free(sdef);
    return n;
}

/* ---- MCI sequencer (music) ---- */
static char mci_temp_name[128] = "";

/* ======================================================================= */
/* API handlers. Signature: void fn(void); argument bytes are in the table. */
/* ======================================================================= */

/* KERNEL */
static void k_GlobalAlloc(void) { /* (UINT flags, DWORD bytes) */
    uint32_t bytes = A32(0);
    uint16_t sel = rt_alloc_selectors(bytes ? bytes : 1);
    ret16(sel);
}
static void k_GlobalFree(void) { rt_free_selectors(A16(0)); ret16(0); }
static void k_GlobalLock(void) { uint16_t h = A16(0); ret32(h ? ((uint32_t)h << 16) : 0); }
static void k_GlobalUnlock(void) { ret16(0); }
static void k_GlobalSize(void) { ret32(rt_selector_size(A16(0))); }
static void k_GlobalReAlloc(void) { /* (HGLOBAL, DWORD, UINT) */
    uint16_t h = A16(6);
    uint32_t bytes = A32(2);
    ret16(rt_realloc_selectors(h, bytes));
}
static void k_GlobalCompact(void) { ret32(0x400000); }
static void k_LocalAlloc(void) { ret16(rt_local_alloc(A16(2), A16(0))); }
static void k_LocalFree(void) { ret16(rt_local_free(A16(0))); }
static void k_LocalReAlloc(void) { ret16(rt_local_realloc(A16(4), A16(2), A16(0))); }
static void k_LocalSize(void) { ret16(rt_local_size(A16(0))); }
static void k_LocalCompact(void) { ret16(0x4000); }
static void k_GetVersion(void) { ret32(0x0600030a); } /* Windows 3.10, DOS 6.0 */
static void k_GetProfileString(void) { ret16(profile_string(A32(14), A32(10), A32(6), A32(2), A16(0))); }
static void k_GetPrivateProfileString(void) { ret16(profile_string(A32(18), A32(14), A32(10), A32(6), A16(4))); }
static void k_GetPrivateProfileInt(void) { /* (app, key, int def, file) */
    char *sapp = rt_strdup_far(A32(10)), *skey = rt_strdup_far(A32(6));
    const char *v = ini_get(sapp, skey);
    ret16(v ? (uint16_t)atoi(v) : A16(4));
    free(sapp); free(skey);
}
static void k_WritePrivateProfileString(void) { /* (app, key, str, file) */
    char *sapp = rt_strdup_far(A32(12));
    char *skey = A32(8) ? rt_strdup_far(A32(8)) : NULL;
    char *sval = A32(4) ? rt_strdup_far(A32(4)) : NULL;
    if (skey) ini_set(sapp, skey, sval);
    free(sapp); free(skey); free(sval);
    ret16(1);
}
static void k_lstrlen(void) { ret16((uint16_t)rt_strlen_far(A32(0))); }
static void k_lstrcpy(void) {
    uint32_t dst = A32(4), src = A32(0);
    uint32_t i = 0;
    uint8_t c;
    do { c = MEMR8(src + i); MEMW8(dst + i, c); i++; } while (c);
    ret32(dst);
}
static void k_lstrcat(void) {
    uint32_t dst = A32(4), src = A32(0);
    uint32_t n = (uint32_t)rt_strlen_far(dst), i = 0;
    uint8_t c;
    do { c = MEMR8(src + i); MEMW8(dst + n + i, c); i++; } while (c);
    ret32(dst);
}
static void k_GetModuleHandle(void) { ret16(HINSTANCE_APP); }
static void k_GetModuleFileName(void) { rt_strcpy_to_far(A32(2), "C:\\NITE3W\\NITE3W.EXE", A16(0)); ret16(20); }
static void k_GetProcAddress(void) { ret32(0); }
static void k_SetErrorMode(void) { ret16(0); }
static void k_LockSegment(void) { ret16(A16(0)); }
static void k_UnlockSegment(void) { ret16(A16(0)); }
static void k_GetCurrentTask(void) { ret16(0x0700); }
static void k_WaitEvent(void) { ret16(0); }
static void k_FatalExit(void) { rt_fatal("FatalExit(%d)", (int16_t)A16(0)); }
static void k_FatalAppExit(void) { char *s = rt_strdup_far(A32(0)); rt_fatal("FatalAppExit: %s", s); }
static void k_IsDBCSLeadByte(void) { ret16(0); }
static void k_GetDOSEnvironment(void) {
    static uint16_t env_off;
    if (!env_off) {
        env_off = rt_local_alloc(0x40, 8); /* empty environment: two NULs */
    }
    ret32(((uint32_t)RT_SEL_DS << 16) | env_off);
}
static void k_DOS3Call(void) { dos_int21(); }
static void k_OpenFile(void) { /* (LPCSTR, OFSTRUCT FAR*, UINT style) */
    char *name = rt_strdup_far(A32(6));
    uint16_t style = A16(0);
    if (style & 0x0200) { /* OF_DELETE */
        char path[1400];
        snprintf(path, sizeof path, "%s/%s", dos_user_dir(), name);
        remove(path);
        ret16(1);
    } else {
        FILE *f = dos_fopen(name, "rb");
        if (f) { fclose(f); ret16(1); } else ret16(0xffff);
    }
    free(name);
}
/* _lcreat/_lwrite/_lclose are only used for the MCI temp file. */
static FILE *lfiles[8];
static void k__lcreat(void) {
    char *name = rt_strdup_far(A32(2));
    for (int i = 1; i < 8; i++)
        if (!lfiles[i]) {
            lfiles[i] = dos_fopen(name, "wb");
            snprintf(mci_temp_name, sizeof mci_temp_name, "%s", name);
            free(name);
            ret16(lfiles[i] ? (uint16_t)(0x10 + i) : 0xffff);
            return;
        }
    free(name);
    ret16(0xffff);
}
static void k__lwrite(void) { /* (HFILE, void huge*, UINT) */
    uint16_t h = A16(6) - 0x10, n = A16(0);
    uint32_t src = A32(2);
    if (h >= 8 || !lfiles[h]) { ret16(0xffff); return; }
    for (uint16_t i = 0; i < n; i++) {
        uint32_t off = (src & 0xffff) + i;
        uint32_t lin = ((((src >> 16) + (off >> 16) * RT_AHINCR) & 0xffff) << 16) | (off & 0xffff);
        fputc(MEMR8(lin), lfiles[h]);
    }
    ret16(n);
}
static void k__lclose(void) {
    uint16_t h = A16(0) - 0x10;
    if (h < 8 && lfiles[h]) { fclose(lfiles[h]); lfiles[h] = NULL; }
    ret16(0);
}
static void k_GetTempFileName(void) { /* (BYTE drive, LPCSTR prefix, UINT unique, LPSTR out) */
    rt_strcpy_to_far(A32(0), "N3WMIDI.TMP", 144);
    ret16(1);
}

/* USER */
static void u_MessageBox(void) { /* (HWND, LPCSTR text, LPCSTR caption, UINT) */
    char *text = rt_strdup_far(A32(6)), *cap = rt_strdup_far(A32(2));
    uint16_t type = A16(0);
    ret16((uint16_t)host_message_box(cap, text, type));
    free(text); free(cap);
}
static void u_GetTickCount(void) { ret32(host_ticks_ms()); }
static void u_GetCursorPos(void) { /* (POINT FAR*) screen coords == window coords here */
    int x, y;
    host_mouse_pos(&x, &y);
    uint32_t p = A32(0);
    MEMW16(p, (uint16_t)x); MEMW16(p + 2, (uint16_t)y);
}
static void u_ScreenToClient(void) { /* client origin == screen origin */ }
static void u_ClientToScreen(void) {}
static void set_rect(uint32_t p, int l, int t, int r, int b) {
    MEMW16(p, (uint16_t)l); MEMW16(p + 2, (uint16_t)t); MEMW16(p + 4, (uint16_t)r); MEMW16(p + 6, (uint16_t)b);
}
static int client_w = 320, client_h = 200;
static void u_GetWindowRect(void) {
    uint16_t h = A16(4);
    if (h == HWND_DESKTOP) set_rect(A32(0), 0, 0, 640, 480);
    else set_rect(A32(0), 0, 0, client_w, client_h);
}
static void u_GetClientRect(void) { set_rect(A32(0), 0, 0, client_w, client_h); }
static void u_MoveWindow(void) { /* (HWND, x, y, w, h, repaint) */
    uint16_t w = A16(4), h = A16(2);
    if (A16(10) == HWND_FRAME && w >= 320 && h >= 200) host_request_window_size(w, h);
    ret16(1);
}
static void u_SetWindowText(void) { char *s = rt_strdup_far(A32(0)); host_set_title(s); free(s); }
static void u_ShowWindow(void) { ret16(1); }
static void u_IsWindowVisible(void) { ret16(1); }
static void u_GetActiveWindow(void) { ret16(host_is_active() ? HWND_FRAME : 0); }
static void u_GetDesktopWindow(void) { ret16(HWND_DESKTOP); }
static void u_GetDC(void) { ret16(HDC_SCREEN); }
static void u_ReleaseDC(void) { ret16(1); }
static void u_SetCursor(void) { ret16(0); }
static void u_ShowCursor(void) { host_show_cursor((int16_t)A16(0) > 0); ret16(0); }
static void u_SetCapture(void) { ret16(0); }
static void u_ReleaseCapture(void) {}
static void u_EndDialog(void) {}
static void u_MessageBeep(void) {}
static void u_GetKeyState(void) { ret16(host_key_state(A16(0)) ? 0x8000 : 0); }
static void u_GetAsyncKeyState(void) { ret16(host_key_state(A16(0)) ? 0x8000 : 0); }
static void u_GetKeyboardState(void) {
    uint32_t p = A32(0);
    for (int vk = 0; vk < 256; vk++) MEMW8(p + vk, host_key_state(vk) ? 0x80 : 0);
}
static void u_SendMessage(void) { ret32(0); }
static void u_PostMessage(void) { ret16(1); }
static void u_PostQuitMessage(void) { host_request_quit(); }
static void u_InvalidateRect(void) { ret16(1); }
static void u_LoadCursor(void) { ret16(0x0800); }
static void u_DestroyWindow(void) { host_request_quit(); ret16(1); }
static void u_SetDlgItemText(void) {}
static void u_GetDlgItem(void) { ret16(0); }
static void u_SelectPalette(void) { /* (HDC, HPALETTE, BOOL) */
    uint16_t old = selected_pal;
    selected_pal = A16(2);
    ret16(old ? old : HPAL_FIRST + 15);
}
static void u_RealizePalette(void) {
    pal_t *p = pal_get(selected_pal);
    if (p) { pal_apply_to_screen(p, 0, p->n); realized_pal = selected_pal; }
    ret16(p ? p->n : 0);
}
static void u_AnsiNext(void) { uint32_t p = A32(0); ret32(MEMR8(p) ? p + 1 : p); }
static void u_OpenDriver(void) { ret16(0); } /* no joystick driver */
static void u_CloseDriver(void) { ret32(0); }
static void u_SendDriverMessage(void) { ret32(0); }
static void u_TabbedTextOut(void) { ret32(0); }

/* KEYBOARD */
static void kb_ToAscii(void) { /* (UINT vk, UINT scan, BYTE FAR* state, DWORD FAR* out, UINT flags) */
    uint16_t vk = A16(12);
    uint32_t out = A32(2);
    int c = host_vk_to_ascii(vk);
    if (c <= 0) { ret16(0); return; }
    MEMW16(out, (uint16_t)c);
    ret16(1);
}
static void kb_AnsiToOem(void) { /* (LPCSTR, LPSTR) */
    uint32_t src = A32(4), dst = A32(0), i = 0;
    uint8_t c;
    do { c = MEMR8(src + i); MEMW8(dst + i, c); i++; } while (c);
    ret16(1);
}

/* GDI */
static void g_GetDeviceCaps(void) { /* (HDC, int index) */
    switch (A16(0)) {
    case 38: ret16(0x0100 | 0x0001 | 0x0008); break; /* RASTERCAPS: RC_PALETTE | RC_BITBLT | RC_BITMAP64 */
    case 8: ret16(640); break;   /* HORZRES */
    case 10: ret16(480); break;  /* VERTRES */
    case 12: ret16(8); break;    /* BITSPIXEL */
    case 14: ret16(1); break;    /* PLANES */
    case 104: ret16(256); break; /* SIZEPALETTE */
    case 106: ret16(20); break;  /* NUMRESERVED */
    case 24: ret16(256); break;  /* NUMCOLORS */
    default: ret16(0); break;
    }
}
static void g_SelectObject(void) { ret16(0x0900); }
static void g_DeleteDC(void) { ret16(1); }
static void g_DeleteObject(void) {
    pal_t *p = pal_get(A16(0));
    if (p) p->used = 0;
    ret16(1);
}
static void g_CreateDC(void) { ret16(HDC_SCREEN); }
static void g_Escape(void) { ret16(0); }
static void g_GetTextMetrics(void) { uint32_t p = A32(0); for (int i = 0; i < 31; i++) MEMW8(p + i, 0); MEMW16(p, 16); ret16(1); }
static void g_CreatePalette(void) { /* (LOGPALETTE FAR*) */
    uint32_t lp = A32(0);
    uint16_t n = MEMR16(lp + 2);
    for (int i = 0; i < 15; i++)
        if (!pals[i].used) {
            pals[i].used = 1;
            pals[i].n = n > 256 ? 256 : n;
            for (int k = 0; k < pals[i].n; k++)
                for (int c = 0; c < 4; c++) pals[i].rgb[k][c] = MEMR8(lp + 4 + k * 4 + c);
            ret16((uint16_t)(HPAL_FIRST + i));
            return;
        }
    ret16(0);
}
static void g_GetPaletteEntries(void) { /* (HPALETTE, start, count, PALETTEENTRY FAR*) */
    pal_t *p = pal_get(A16(8));
    uint16_t start = A16(6), n = A16(4);
    uint32_t out = A32(0);
    if (!p) { ret16(0); return; }
    for (int k = 0; k < n && start + k < 256; k++)
        for (int c = 0; c < 4; c++) MEMW8(out + k * 4 + c, p->rgb[start + k][c]);
    ret16(n);
}
static void g_AnimatePalette(void) { /* (HPALETTE, start, count, const PALETTEENTRY FAR*) */
    pal_t *p = pal_get(A16(8));
    uint16_t start = A16(6), n = A16(4);
    uint32_t in = A32(0);
    if (!p) return;
    for (int k = 0; k < n && start + k < 256; k++)
        for (int c = 0; c < 4; c++) p->rgb[start + k][c] = MEMR8(in + k * 4 + c);
    if (A16(8) == realized_pal) pal_apply_to_screen(p, start, n);
    present_wing();
}
static void g_SetSystemPaletteUse(void) { ret16(1); }
static void g_GetSystemPaletteEntries(void) { /* (HDC, start, count, out) */
    uint16_t start = A16(6), n = A16(4);
    uint32_t out = A32(0);
    for (int k = 0; k < n; k++) {
        uint8_t r, g, b;
        host_get_palette_entry(start + k, &r, &g, &b);
        MEMW8(out + k * 4, r); MEMW8(out + k * 4 + 1, g); MEMW8(out + k * 4 + 2, b); MEMW8(out + k * 4 + 3, 0);
    }
    ret16(n);
}
static void g_StartDoc(void) { ret16(0xffff); }
static void g_EndDoc(void) { ret16(0); }
static void g_StartPage(void) { ret16(0); }
static void g_EndPage(void) { ret16(0); }

/* WING */
static void w_CreateDC(void) { ret16(HDC_WING); }
static void w_RecommendDIBFormat(void) { /* (BITMAPINFO FAR*) */
    uint32_t p = A32(0);
    MEMW32(p, 40); MEMW32(p + 4, 1); MEMW32(p + 8, (uint32_t)-1);
    MEMW16(p + 12, 1); MEMW16(p + 14, 8); MEMW32(p + 16, 0);
    ret16(1);
}
static void w_CreateBitmap(void) { /* (HDC, BITMAPINFO FAR*, void FAR* FAR* ppBits) */
    uint32_t bi = A32(4), pp = A32(0);
    wing_w = (int32_t)MEMR32(bi + 4);
    wing_h = (int32_t)MEMR32(bi + 8);
    uint32_t bytes = (uint32_t)wing_w * (uint32_t)(wing_h < 0 ? -wing_h : wing_h);
    wing_sel = rt_alloc_selectors(bytes);
    MEMW32(pp, (uint32_t)wing_sel << 16);
    for (int i = 0; i < 256; i++) /* initial colour table from BITMAPINFO */
        host_set_palette_entry(i, MEMR8(bi + 40 + i * 4 + 2), MEMR8(bi + 40 + i * 4 + 1), MEMR8(bi + 40 + i * 4));
    ret16(0x0a00);
}
static void w_SetDIBColorTable(void) { /* (HDC, start, count, RGBQUAD FAR*) */
    uint16_t start = A16(6), n = A16(4);
    uint32_t q = A32(0);
    for (int k = 0; k < n && start + k < 256; k++)
        host_set_palette_entry(start + k, MEMR8(q + k * 4 + 2), MEMR8(q + k * 4 + 1), MEMR8(q + k * 4));
    ret16(n);
}
static void w_StretchBlt(void) { present_wing(); ret16(1); }
static void w_BitBlt(void) { present_wing(); ret16(1); }

/* DISPDIB: full-screen mode is not supported; report failure. */
static void dd_DisplayDib(void) { ret16(1); }

/* MMSYSTEM */
static void m_sndPlaySound(void) { ret16(0); }
static void m_joyGetDevCaps(void) { ret16(1); }
static void m_joyGetPos(void) { ret16(167); /* JOYERR_UNPLUGGED */ }
static void m_midiOutGetNumDevs(void) { ret16(1); }
static void m_midiOutGetDevCaps(void) { /* (UINT, MIDIOUTCAPS FAR*, UINT size) */
    uint32_t p = A32(2);
    uint16_t sz = A16(0);
    for (uint16_t i = 0; i < sz; i++) MEMW8(p + i, 0);
    MEMW16(p, 1); MEMW16(p + 2, 1);
    rt_strcpy_to_far(p + 6, "Nitemare Synth", 32);
    if (sz >= 46) { MEMW16(p + 38, 4); /* wTechnology = MOD_FMSYNTH */ MEMW16(p + 40, 9); MEMW16(p + 42, 0xffff); }
    ret16(0);
}
static void m_midiOutSetVolume(void) { host_music_volume(A32(0)); ret16(0); }
static void m_waveOutGetNumDevs(void) { ret16(1); }
static void m_waveOutGetErrorText(void) { rt_strcpy_to_far(A32(2), "wave error", A16(0)); ret16(0); }
static uint32_t wave_fmt_rate = 11025;
static uint16_t wave_fmt_bits = 8, wave_fmt_ch = 1;
static void m_waveOutOpen(void) { /* (HWAVEOUT FAR*, UINT dev, WAVEFORMAT FAR*, DWORD cb, DWORD inst, DWORD flags) */
    uint32_t phw = A32(18), fmt = A32(12);
    if (fmt) {
        wave_fmt_ch = MEMR16(fmt + 2);
        wave_fmt_rate = MEMR32(fmt + 4);
        wave_fmt_bits = MEMR16(fmt + 14);
        if (!wave_fmt_bits) wave_fmt_bits = 8;
    }
    if (phw) MEMW16(phw, HWAVEOUT_1);
    ret16(0);
}
static void m_waveOutClose(void) { host_wave_stop(); ret16(0); }
static void m_waveOutPrepareHeader(void) { ret16(0); }
/* Never report WAVERR_STILLPLAYING: the game would then drop or cut off effects
 * to keep its single channel; the host mixes them all instead. */
static void m_waveOutUnprepareHeader(void) { ret16(0); }
static void m_waveOutWrite(void) { /* (HWAVEOUT, WAVEHDR FAR*, UINT) */
    uint32_t hdr = A32(2);
    uint32_t data = MEMR32(hdr);
    uint32_t len = MEMR32(hdr + 4);
    uint8_t *buf = malloc(len ? len : 1);
    for (uint32_t i = 0; i < len; i++) {
        uint32_t off = (data & 0xffff) + i;
        uint32_t lin = ((((data >> 16) + (off >> 16) * RT_AHINCR) & 0xffff) << 16) | (off & 0xffff);
        buf[i] = MEMR8(lin);
    }
    host_wave_play(buf, len, wave_fmt_rate, wave_fmt_bits, wave_fmt_ch);
    free(buf);
    ret16(0);
}
static void m_waveOutReset(void) { host_wave_stop(); ret16(0); }
static void m_waveOutSetVolume(void) { host_wave_volume(A32(0)); ret16(0); }
static void m_timeGetDevCaps(void) { uint32_t p = A32(2); MEMW16(p, 1); MEMW16(p + 2, 65535); ret16(0); }
static void m_timeBeginPeriod(void) { ret16(0); }
static void m_timeEndPeriod(void) { ret16(0); }
static void m_timeGetTime(void) { ret32(host_ticks_ms()); }
static void m_mciSendCommand(void) { /* (UINT dev, UINT msg, DWORD flags, DWORD param) */
    uint16_t msg = A16(8);
    uint32_t param = A32(0);
    switch (msg) {
    case 0x0803: /* MCI_OPEN: fill wDeviceID */
        if (param) MEMW16(param + 4, 1);
        ret32(0);
        break;
    case 0x0806: /* MCI_PLAY */
        host_music_play_file(mci_temp_name);
        ret32(0);
        break;
    case 0x0804: /* MCI_CLOSE */
    case 0x0808: /* MCI_STOP */
        host_music_stop();
        ret32(0);
        break;
    default:
        ret32(0);
        break;
    }
}
static void m_mciSendString(void) { /* (LPCSTR cmd, LPSTR ret, UINT, HWND) */
    char *cmd = rt_strdup_far(A32(8));
    uint32_t out = A32(4);
    if (out && strstr(cmd, "sysinfo sequencer quantity")) rt_strcpy_to_far(out, "1", A16(2));
    free(cmd);
    ret32(0);
}
static void m_mciGetErrorString(void) { rt_strcpy_to_far(A32(2), "MCI error", A16(0)); ret16(1); }

/* ---- dispatch table ---- */
typedef struct { const char *lib, *name; uint16_t argbytes; void (*fn)(void); } api_t;
static const api_t api_table[] = {
    {"KERNEL", "GLOBALALLOC", 6, k_GlobalAlloc},
    {"KERNEL", "GLOBALFREE", 2, k_GlobalFree},
    {"KERNEL", "GLOBALLOCK", 2, k_GlobalLock},
    {"KERNEL", "GLOBALUNLOCK", 2, k_GlobalUnlock},
    {"KERNEL", "GLOBALSIZE", 2, k_GlobalSize},
    {"KERNEL", "GLOBALREALLOC", 8, k_GlobalReAlloc},
    {"KERNEL", "GLOBALCOMPACT", 4, k_GlobalCompact},
    {"KERNEL", "LOCALALLOC", 4, k_LocalAlloc},
    {"KERNEL", "LOCALFREE", 2, k_LocalFree},
    {"KERNEL", "LOCALREALLOC", 6, k_LocalReAlloc},
    {"KERNEL", "LOCALSIZE", 2, k_LocalSize},
    {"KERNEL", "LOCALCOMPACT", 2, k_LocalCompact},
    {"KERNEL", "GETVERSION", 0, k_GetVersion},
    {"KERNEL", "GETPROFILESTRING", 18, k_GetProfileString},
    {"KERNEL", "GETPRIVATEPROFILESTRING", 22, k_GetPrivateProfileString},
    {"KERNEL", "GETPRIVATEPROFILEINT", 14, k_GetPrivateProfileInt},
    {"KERNEL", "WRITEPRIVATEPROFILESTRING", 16, k_WritePrivateProfileString},
    {"KERNEL", "LSTRLEN", 4, k_lstrlen},
    {"KERNEL", "LSTRCPY", 8, k_lstrcpy},
    {"KERNEL", "LSTRCAT", 8, k_lstrcat},
    {"KERNEL", "GETMODULEHANDLE", 4, k_GetModuleHandle},
    {"KERNEL", "GETMODULEFILENAME", 8, k_GetModuleFileName},
    {"KERNEL", "GETPROCADDRESS", 6, k_GetProcAddress},
    {"KERNEL", "SETERRORMODE", 2, k_SetErrorMode},
    {"KERNEL", "LOCKSEGMENT", 2, k_LockSegment},
    {"KERNEL", "UNLOCKSEGMENT", 2, k_UnlockSegment},
    {"KERNEL", "GETCURRENTTASK", 0, k_GetCurrentTask},
    {"KERNEL", "WAITEVENT", 2, k_WaitEvent},
    {"KERNEL", "FATALEXIT", 2, k_FatalExit},
    {"KERNEL", "FATALAPPEXIT", 6, k_FatalAppExit},
    {"KERNEL", "ISDBCSLEADBYTE", 2, k_IsDBCSLeadByte},
    {"KERNEL", "GETDOSENVIRONMENT", 0, k_GetDOSEnvironment},
    {"KERNEL", "DOS3CALL", 0, k_DOS3Call},
    {"KERNEL", "OPENFILE", 10, k_OpenFile},
    {"KERNEL", "_LCREAT", 6, k__lcreat},
    {"KERNEL", "_LWRITE", 8, k__lwrite},
    {"KERNEL", "_LCLOSE", 2, k__lclose},
    {"KERNEL", "GETTEMPFILENAME", 12, k_GetTempFileName},
    {"USER", "MESSAGEBOX", 12, u_MessageBox},
    {"USER", "GETTICKCOUNT", 0, u_GetTickCount},
    {"USER", "GETCURSORPOS", 4, u_GetCursorPos},
    {"USER", "SCREENTOCLIENT", 6, u_ScreenToClient},
    {"USER", "CLIENTTOSCREEN", 6, u_ClientToScreen},
    {"USER", "GETWINDOWRECT", 6, u_GetWindowRect},
    {"USER", "GETCLIENTRECT", 6, u_GetClientRect},
    {"USER", "MOVEWINDOW", 12, u_MoveWindow},
    {"USER", "SETWINDOWTEXT", 6, u_SetWindowText},
    {"USER", "SHOWWINDOW", 4, u_ShowWindow},
    {"USER", "ISWINDOWVISIBLE", 2, u_IsWindowVisible},
    {"USER", "GETACTIVEWINDOW", 0, u_GetActiveWindow},
    {"USER", "GETDESKTOPWINDOW", 0, u_GetDesktopWindow},
    {"USER", "GETDC", 2, u_GetDC},
    {"USER", "RELEASEDC", 4, u_ReleaseDC},
    {"USER", "SETCURSOR", 2, u_SetCursor},
    {"USER", "SHOWCURSOR", 2, u_ShowCursor},
    {"USER", "SETCAPTURE", 2, u_SetCapture},
    {"USER", "RELEASECAPTURE", 0, u_ReleaseCapture},
    {"USER", "ENDDIALOG", 4, u_EndDialog},
    {"USER", "MESSAGEBEEP", 2, u_MessageBeep},
    {"USER", "GETKEYSTATE", 2, u_GetKeyState},
    {"USER", "GETASYNCKEYSTATE", 2, u_GetAsyncKeyState},
    {"USER", "GETKEYBOARDSTATE", 4, u_GetKeyboardState},
    {"USER", "SENDMESSAGE", 10, u_SendMessage},
    {"USER", "POSTMESSAGE", 10, u_PostMessage},
    {"USER", "POSTQUITMESSAGE", 2, u_PostQuitMessage},
    {"USER", "INVALIDATERECT", 8, u_InvalidateRect},
    {"USER", "LOADCURSOR", 6, u_LoadCursor},
    {"USER", "DESTROYWINDOW", 2, u_DestroyWindow},
    {"USER", "SETDLGITEMTEXT", 8, u_SetDlgItemText},
    {"USER", "GETDLGITEM", 4, u_GetDlgItem},
    {"USER", "SELECTPALETTE", 6, u_SelectPalette},
    {"USER", "REALIZEPALETTE", 2, u_RealizePalette},
    {"USER", "ANSINEXT", 4, u_AnsiNext},
    {"USER", "OPENDRIVER", 12, u_OpenDriver},
    {"USER", "CLOSEDRIVER", 10, u_CloseDriver},
    {"USER", "SENDDRIVERMESSAGE", 12, u_SendDriverMessage},
    {"USER", "TABBEDTEXTOUT", 20, u_TabbedTextOut},
    {"KEYBOARD", "Ordinal_4", 14, kb_ToAscii},
    {"KEYBOARD", "Ordinal_5", 8, kb_AnsiToOem},
    {"KEYBOARD", "Ordinal_6", 8, kb_AnsiToOem},
    {"GDI", "GETDEVICECAPS", 4, g_GetDeviceCaps},
    {"GDI", "SELECTOBJECT", 4, g_SelectObject},
    {"GDI", "DELETEDC", 2, g_DeleteDC},
    {"GDI", "DELETEOBJECT", 2, g_DeleteObject},
    {"GDI", "CREATEDC", 16, g_CreateDC},
    {"GDI", "ESCAPE", 14, g_Escape},
    {"GDI", "GETTEXTMETRICS", 6, g_GetTextMetrics},
    {"GDI", "CREATEPALETTE", 4, g_CreatePalette},
    {"GDI", "GETPALETTEENTRIES", 10, g_GetPaletteEntries},
    {"GDI", "ANIMATEPALETTE", 10, g_AnimatePalette},
    {"GDI", "SETSYSTEMPALETTEUSE", 4, g_SetSystemPaletteUse},
    {"GDI", "GETSYSTEMPALETTEENTRIES", 10, g_GetSystemPaletteEntries},
    {"GDI", "STARTDOC", 6, g_StartDoc},
    {"GDI", "ENDDOC", 2, g_EndDoc},
    {"GDI", "STARTPAGE", 2, g_StartPage},
    {"GDI", "ENDPAGE", 2, g_EndPage},
    {"WING", "Ordinal_1001", 0, w_CreateDC},
    {"WING", "Ordinal_1002", 4, w_RecommendDIBFormat},
    {"WING", "Ordinal_1003", 10, w_CreateBitmap},
    {"WING", "Ordinal_1006", 10, w_SetDIBColorTable},
    {"WING", "Ordinal_1009", 20, w_StretchBlt},
    {"WING", "Ordinal_1010", 16, w_BitBlt},
    {"DISPDIB", "Ordinal_1", 10, dd_DisplayDib},
    {"MMSYSTEM", "SNDPLAYSOUND", 6, m_sndPlaySound},
    {"MMSYSTEM", "JOYGETDEVCAPS", 8, m_joyGetDevCaps},
    {"MMSYSTEM", "JOYGETPOS", 6, m_joyGetPos},
    {"MMSYSTEM", "MIDIOUTGETNUMDEVS", 0, m_midiOutGetNumDevs},
    {"MMSYSTEM", "MIDIOUTGETDEVCAPS", 8, m_midiOutGetDevCaps},
    {"MMSYSTEM", "MIDIOUTSETVOLUME", 6, m_midiOutSetVolume},
    {"MMSYSTEM", "WAVEOUTGETNUMDEVS", 0, m_waveOutGetNumDevs},
    {"MMSYSTEM", "WAVEOUTGETERRORTEXT", 8, m_waveOutGetErrorText},
    {"MMSYSTEM", "WAVEOUTOPEN", 22, m_waveOutOpen},
    {"MMSYSTEM", "WAVEOUTCLOSE", 2, m_waveOutClose},
    {"MMSYSTEM", "WAVEOUTPREPAREHEADER", 8, m_waveOutPrepareHeader},
    {"MMSYSTEM", "WAVEOUTUNPREPAREHEADER", 8, m_waveOutUnprepareHeader},
    {"MMSYSTEM", "WAVEOUTWRITE", 8, m_waveOutWrite},
    {"MMSYSTEM", "WAVEOUTRESET", 2, m_waveOutReset},
    {"MMSYSTEM", "WAVEOUTSETVOLUME", 6, m_waveOutSetVolume},
    {"MMSYSTEM", "TIMEGETDEVCAPS", 6, m_timeGetDevCaps},
    {"MMSYSTEM", "TIMEBEGINPERIOD", 2, m_timeBeginPeriod},
    {"MMSYSTEM", "TIMEENDPERIOD", 2, m_timeEndPeriod},
    {"MMSYSTEM", "TIMEGETTIME", 0, m_timeGetTime},
    {"MMSYSTEM", "MCISENDCOMMAND", 12, m_mciSendCommand},
    {"MMSYSTEM", "MCISENDSTRING", 12, m_mciSendString},
    {"MMSYSTEM", "MCIGETERRORSTRING", 10, m_mciGetErrorString},
};

static const api_t *import_api[0x10000 / 4];

static const api_t *lookup(uint32_t addr) {
    uint32_t slot = (addr & 0xffff) / 4;
    if (import_api[slot]) return import_api[slot];
    for (int i = 0; i < rt_import_count; i++) {
        if (rt_imports[i].addr != addr) continue;
        for (size_t k = 0; k < sizeof api_table / sizeof api_table[0]; k++)
            if (!strcmp(api_table[k].lib, rt_imports[i].lib) && !strcmp(api_table[k].name, rt_imports[i].name)) {
                import_api[slot] = &api_table[k];
                return &api_table[k];
            }
        rt_fatal("unimplemented API %s.%s (called at %04x:%04x)", rt_imports[i].lib, rt_imports[i].name,
                 MEMR16(SEGLIN(rSS, (uint16_t)(rSP + 2))), MEMR16(SEGLIN(rSS, rSP)));
    }
    rt_fatal("call to unknown import address %04x:%04x", addr >> 16, addr & 0xffff);
    return NULL;
}

void rt_api(uint32_t addr) {
    host_audio_poll();
    const api_t *a = lookup(addr);
    a->fn();
    rSP = (uint16_t)(rSP + 4 + a->argbytes);
}

/* ---- notifications the host forwards into the game ---- */
void win16_set_client_size(int w, int h) { client_w = w; client_h = h; }
