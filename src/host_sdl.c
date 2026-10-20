/*
 * SDL2 host for the recompiled Nitemare-3D.
 *
 * Plays the role the MFC framework and Windows played for NITE3W.EXE: creates
 * the window, forwards keyboard input to the game's OnKeyDown/OnKeyUp handler,
 * and calls the game's OnIdle tick in a loop. Video, palette and audio requests
 * from the Win16 layer (src/rt/win16.c) end up here.
 */
#include <SDL.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#else
#include <dirent.h>
#include <strings.h>
#endif

#include "rt/host.h"
#include "rt/rt.h"
#include "music.h"
#include "hd.h"
#include "hd_tex.h"
#include "hd_pack.h"

/* Game entry points in the recompiled code (see re/names.txt). */
#define G_INIT_INSTANCE 0x101010ccu /* parse command line + initialise all subsystems */
#define G_VIEW_ONSIZE 0x10102b9au   /* (hwnd, type, cx, cy) */
#define G_VIEW_ONCREATE 0x101030d6u /* (hwnd) palette realisation */
#define G_KEY_EVENT 0x10108c2eu     /* (vk, flags) shared by OnKeyDown/OnKeyUp/OnSysKey* */
#define G_ON_IDLE 0x1010dae6u       /* per-iteration game tick */
#define G_ON_ACTIVATE 0x10100e1eu   /* (active) */
#define G_SHUTDOWN 0x10100f18u

/* Game globals (DS offsets). */
#define DS_HWND_MAIN 0x00b6
#define DS_HWND_VIEW 0x00b8
#define DS_HWND_TOOLBAR 0x00ba
#define DS_HWND_STATUS 0x00bc

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static SDL_AudioDeviceID audio_dev;
static int quit_requested;
static int window_active = 1;
/* Windows 3.1 reserves palette entries 0-9 and 246-255 for its static system
 * colours. The game asks GetSystemPaletteEntries for them and some sprites
 * (e.g. the white lab coat) are drawn with them, so start from those values. */
static uint8_t palette[256][3] = {
    [0] = {0x00, 0x00, 0x00}, [1] = {0x80, 0x00, 0x00}, [2] = {0x00, 0x80, 0x00}, [3] = {0x80, 0x80, 0x00},
    [4] = {0x00, 0x00, 0x80}, [5] = {0x80, 0x00, 0x80}, [6] = {0x00, 0x80, 0x80}, [7] = {0xc0, 0xc0, 0xc0},
    [8] = {0xc0, 0xdc, 0xc0}, [9] = {0xa6, 0xca, 0xf0},
    [246] = {0xff, 0xfb, 0xf0}, [247] = {0xa0, 0xa0, 0xa4}, [248] = {0x80, 0x80, 0x80}, [249] = {0xff, 0x00, 0x00},
    [250] = {0x00, 0xff, 0x00}, [251] = {0xff, 0xff, 0x00}, [252] = {0x00, 0x00, 0xff}, [253] = {0xff, 0x00, 0xff},
    [254] = {0x00, 0xff, 0xff}, [255] = {0xff, 0xff, 0xff},
};
static uint8_t last_frame[320 * 200];
static int have_frame;
static int cursor_visible = 1;
static int rel_mode; /* mouse captured (SDL relative mode + grab) */
static int vcur_x = -1, vcur_y = -1; /* virtual cursor in window pixels while captured outside play */
static double mouse_accum;         /* turn in degrees not yet applied to the game's angle */

/* Render resolution: 320x200 = the original frame scaled up (classic);
 * anything else uses the native hi-res 3D view renderer (src/hd.c). */
enum { RES_FIXED, RES_WINDOW, RES_DESKTOP };
static int res_mode = RES_WINDOW;
static int render_w = 320, render_h = 200;

static void setup_render_target(int w, int h) {
    if (w < 64) w = 64;
    if (h < 48) h = 48;
    if (w == render_w && h == render_h && texture) return;
    render_w = w;
    render_h = h;
    if (texture) SDL_DestroyTexture(texture);
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
    if (w == 320 && h == 200) {
        SDL_RenderSetLogicalSize(renderer, 320, 240); /* non-square pixels: present at 4:3 */
        hd_set_output(0, 0);
    } else {
        SDL_RenderSetLogicalSize(renderer, w, h);
        hd_set_output(w, h);
    }
}

static void follow_window_size(void) {
    if (res_mode != RES_WINDOW) return;
    int w, h;
    if (SDL_GetRendererOutputSize(renderer, &w, &h) == 0) setup_render_target(w, h);
}

/* ------------------------------------------------------------------ video */
void host_set_palette_entry(int i, uint8_t r, uint8_t g, uint8_t b) {
    if (i < 10 || i > 245) return; /* static system colours never change */
    palette[i][0] = r; palette[i][1] = g; palette[i][2] = b;
}
void host_get_palette_entry(int i, uint8_t *r, uint8_t *g, uint8_t *b) {
    *r = palette[i & 255][0]; *g = palette[i & 255][1]; *b = palette[i & 255][2];
}

#ifdef __EMSCRIPTEN__
static int target_fps = 0;       /* --fps; the browser already paces frames to the display */
#else
static int target_fps = 60;      /* --fps */
#endif
static uint64_t next_due;        /* when the next paced frame is due */
static uint64_t last_redraw_us;

static void redraw(void) {
    if (!have_frame) return;
    last_redraw_us = host_time_us();
    if (getenv("N3D_TURNLOG"))
        fprintf(stderr, "turn %u %.2f\n", SDL_GetTicks(), (int16_t)MEMR16(SEGLIN(RT_SEL_DS, 0x4bda)) + mouse_accum);
    void *pixels;
    int pitch;
    if (hd_enabled()) {
        uint64_t c0 = host_time_us();
        if (SDL_LockTexture(texture, NULL, &pixels, &pitch) == 0) {
            hd_compose(last_frame, (const uint8_t(*)[3])palette, pixels, pitch / 4);
            SDL_UnlockTexture(texture);
        }
        if (getenv("N3D_HDPROF")) {
            static uint64_t acc, first;
            static int cnt;
            if (!first) first = c0;
            acc += host_time_us() - c0;
            if (++cnt == 120) {
                fprintf(stderr, "compose %dx%d: %.2f ms, %.1f redraws/s\n", render_w, render_h, acc / 120000.0, 120e6 / (double)(host_time_us() - first));
                acc = 0; cnt = 0; first = host_time_us();
            }
        }
    } else if (SDL_LockTexture(texture, NULL, &pixels, &pitch) == 0) {
        for (int y = 0; y < 200; y++) {
            uint32_t *row = (uint32_t *)((uint8_t *)pixels + y * pitch);
            const uint8_t *src = last_frame + y * 320;
            for (int x = 0; x < 320; x++) {
                const uint8_t *c = palette[src[x]];
                row[x] = 0xff000000u | ((uint32_t)c[0] << 16) | ((uint32_t)c[1] << 8) | c[2];
            }
        }
        SDL_UnlockTexture(texture);
    }
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
}

/* Debug aid: N3D_SHOTS=dir saves every 30th presented frame as BMP. */
static void debug_shot(void) {
    static int count;
    const char *dir = getenv("N3D_SHOTS");
    static int every = 0, seen = 0;
    if (!dir) return;
    if (!every) every = getenv("N3D_SHOTS_EVERY") ? atoi(getenv("N3D_SHOTS_EVERY")) : 1;
    if (every < 1) every = 1;
    if (seen++ % every || count >= 2000) return;
    count++;
    SDL_Surface *s;
    if (hd_enabled()) {
        s = SDL_CreateRGBSurfaceWithFormat(0, render_w, render_h, 32, SDL_PIXELFORMAT_ARGB8888);
        if (!s) return;
        hd_compose(last_frame, (const uint8_t(*)[3])palette, s->pixels, s->pitch / 4);
        goto save;
    }
    s = SDL_CreateRGBSurfaceWithFormat(0, 320, 200, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!s) return;
    for (int y = 0; y < 200; y++)
        for (int x = 0; x < 320; x++) {
            const uint8_t *c = palette[last_frame[y * 320 + x]];
            ((uint32_t *)((uint8_t *)s->pixels + y * s->pitch))[x] = 0xff000000u | ((uint32_t)c[0] << 16) | ((uint32_t)c[1] << 8) | c[2];
        }
save:;
    char path[1024];
    snprintf(path, sizeof path, "%s/shot_%04d.bmp", dir, count);
    SDL_SaveBMP(s, path);
    SDL_FreeSurface(s);
}

static int presented; /* the game presented a frame in this loop iteration */

void host_present(const uint8_t *pixels, int w, int h) {
    if (w != 320 || h != 200) return;
    int again = presented; /* more than one present in one game call: a blocking loop (fades) */
    presented = 1;
    memcpy(last_frame, pixels, sizeof last_frame);
    have_frame = 1;
    /* While the 3D view runs, frames go out on the --fps schedule from the
     * main loop instead (evenly spaced, interpolated); otherwise right away. */
    if (hd_view_live()) {
        /* the paced loop shows it; but when the game keeps presenting without
         * returning (palette fades run in a loop paced by the display), show
         * those frames here, like the classic path does */
        if (!again) return;
        hd_rerender();
    }
    redraw();
    debug_shot();
}

void host_request_window_size(int w, int h) { (void)w; (void)h; /* window size is user-controlled */ }
void host_set_title(const char *title) { SDL_SetWindowTitle(window, title); }
void host_show_cursor(int show) { cursor_visible = show; SDL_ShowCursor(show ? SDL_ENABLE : SDL_DISABLE); }

/* ------------------------------------------------------------------ input */
static int vk_from_sdl(SDL_Keycode k) {
    if (k >= SDLK_a && k <= SDLK_z) return 'A' + (k - SDLK_a);
    if (k >= SDLK_0 && k <= SDLK_9) return '0' + (k - SDLK_0);
    if (k >= SDLK_F1 && k <= SDLK_F12) return 0x70 + (k - SDLK_F1);
    switch (k) {
    case SDLK_BACKSPACE: return 0x08;
    case SDLK_TAB: return 0x09;
    case SDLK_RETURN: case SDLK_KP_ENTER: return 0x0d;
    case SDLK_LSHIFT: case SDLK_RSHIFT: return 0x10;
    case SDLK_LCTRL: case SDLK_RCTRL: return 0x11;
    case SDLK_LALT: case SDLK_RALT: return 0x12;
    case SDLK_PAUSE: return 0x13;
    case SDLK_CAPSLOCK: return 0x14;
    case SDLK_ESCAPE: return 0x1b;
    case SDLK_SPACE: return 0x20;
    case SDLK_PAGEUP: return 0x21;
    case SDLK_PAGEDOWN: return 0x22;
    case SDLK_END: return 0x23;
    case SDLK_HOME: return 0x24;
    case SDLK_LEFT: return 0x25;
    case SDLK_UP: return 0x26;
    case SDLK_RIGHT: return 0x27;
    case SDLK_DOWN: return 0x28;
    case SDLK_INSERT: return 0x2d;
    case SDLK_DELETE: return 0x2e;
    case SDLK_KP_0: return 0x60; case SDLK_KP_1: return 0x61; case SDLK_KP_2: return 0x62;
    case SDLK_KP_3: return 0x63; case SDLK_KP_4: return 0x64; case SDLK_KP_5: return 0x65;
    case SDLK_KP_6: return 0x66; case SDLK_KP_7: return 0x67; case SDLK_KP_8: return 0x68;
    case SDLK_KP_9: return 0x69;
    case SDLK_KP_MULTIPLY: return 0x6a; case SDLK_KP_PLUS: return 0x6b; case SDLK_KP_MINUS: return 0x6d;
    case SDLK_KP_PERIOD: return 0x6e; case SDLK_KP_DIVIDE: return 0x6f;
    case SDLK_SEMICOLON: return 0xba; case SDLK_EQUALS: return 0xbb; case SDLK_COMMA: return 0xbc;
    case SDLK_MINUS: return 0xbd; case SDLK_PERIOD: return 0xbe; case SDLK_SLASH: return 0xbf;
    case SDLK_BACKQUOTE: return 0xc0; case SDLK_LEFTBRACKET: return 0xdb; case SDLK_BACKSLASH: return 0xdc;
    case SDLK_RIGHTBRACKET: return 0xdd; case SDLK_QUOTE: return 0xde;
    default: return 0;
    }
}

/* PC/AT set-1 scan codes, which the game inspects to tell left/right shift apart. */
static int pc_scancode(SDL_Scancode s) {
    static const struct { SDL_Scancode s; int pc; } map[] = {
        {SDL_SCANCODE_ESCAPE, 0x01}, {SDL_SCANCODE_1, 0x02}, {SDL_SCANCODE_2, 0x03}, {SDL_SCANCODE_3, 0x04},
        {SDL_SCANCODE_4, 0x05}, {SDL_SCANCODE_5, 0x06}, {SDL_SCANCODE_6, 0x07}, {SDL_SCANCODE_7, 0x08},
        {SDL_SCANCODE_8, 0x09}, {SDL_SCANCODE_9, 0x0a}, {SDL_SCANCODE_0, 0x0b}, {SDL_SCANCODE_MINUS, 0x0c},
        {SDL_SCANCODE_EQUALS, 0x0d}, {SDL_SCANCODE_BACKSPACE, 0x0e}, {SDL_SCANCODE_TAB, 0x0f},
        {SDL_SCANCODE_Q, 0x10}, {SDL_SCANCODE_W, 0x11}, {SDL_SCANCODE_E, 0x12}, {SDL_SCANCODE_R, 0x13},
        {SDL_SCANCODE_T, 0x14}, {SDL_SCANCODE_Y, 0x15}, {SDL_SCANCODE_U, 0x16}, {SDL_SCANCODE_I, 0x17},
        {SDL_SCANCODE_O, 0x18}, {SDL_SCANCODE_P, 0x19}, {SDL_SCANCODE_LEFTBRACKET, 0x1a},
        {SDL_SCANCODE_RIGHTBRACKET, 0x1b}, {SDL_SCANCODE_RETURN, 0x1c}, {SDL_SCANCODE_LCTRL, 0x1d},
        {SDL_SCANCODE_A, 0x1e}, {SDL_SCANCODE_S, 0x1f}, {SDL_SCANCODE_D, 0x20}, {SDL_SCANCODE_F, 0x21},
        {SDL_SCANCODE_G, 0x22}, {SDL_SCANCODE_H, 0x23}, {SDL_SCANCODE_J, 0x24}, {SDL_SCANCODE_K, 0x25},
        {SDL_SCANCODE_L, 0x26}, {SDL_SCANCODE_SEMICOLON, 0x27}, {SDL_SCANCODE_APOSTROPHE, 0x28},
        {SDL_SCANCODE_GRAVE, 0x29}, {SDL_SCANCODE_LSHIFT, 0x2a}, {SDL_SCANCODE_BACKSLASH, 0x2b},
        {SDL_SCANCODE_Z, 0x2c}, {SDL_SCANCODE_X, 0x2d}, {SDL_SCANCODE_C, 0x2e}, {SDL_SCANCODE_V, 0x2f},
        {SDL_SCANCODE_B, 0x30}, {SDL_SCANCODE_N, 0x31}, {SDL_SCANCODE_M, 0x32}, {SDL_SCANCODE_COMMA, 0x33},
        {SDL_SCANCODE_PERIOD, 0x34}, {SDL_SCANCODE_SLASH, 0x35}, {SDL_SCANCODE_RSHIFT, 0x36},
        {SDL_SCANCODE_LALT, 0x38}, {SDL_SCANCODE_SPACE, 0x39}, {SDL_SCANCODE_CAPSLOCK, 0x3a},
        {SDL_SCANCODE_F1, 0x3b}, {SDL_SCANCODE_F2, 0x3c}, {SDL_SCANCODE_F3, 0x3d}, {SDL_SCANCODE_F4, 0x3e},
        {SDL_SCANCODE_F5, 0x3f}, {SDL_SCANCODE_F6, 0x40}, {SDL_SCANCODE_F7, 0x41}, {SDL_SCANCODE_F8, 0x42},
        {SDL_SCANCODE_F9, 0x43}, {SDL_SCANCODE_F10, 0x44}, {SDL_SCANCODE_HOME, 0x47}, {SDL_SCANCODE_UP, 0x48},
        {SDL_SCANCODE_PAGEUP, 0x49}, {SDL_SCANCODE_LEFT, 0x4b}, {SDL_SCANCODE_RIGHT, 0x4d},
        {SDL_SCANCODE_END, 0x4f}, {SDL_SCANCODE_DOWN, 0x50}, {SDL_SCANCODE_PAGEDOWN, 0x51},
        {SDL_SCANCODE_INSERT, 0x52}, {SDL_SCANCODE_DELETE, 0x53}, {SDL_SCANCODE_RCTRL, 0x1d},
        {SDL_SCANCODE_RALT, 0x38},
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (map[i].s == s) return map[i].pc;
    return 0;
}

static uint8_t vk_down[256];

int host_key_state(int vk) {
    vk &= 0xff;
    if (vk == 1 || vk == 2 || vk == 4) {
        uint32_t b = SDL_GetMouseState(NULL, NULL);
        if (vk == 1) return (b & SDL_BUTTON_LMASK) != 0;
        if (vk == 2) return (b & SDL_BUTTON_RMASK) != 0;
        return (b & SDL_BUTTON_MMASK) != 0;
    }
    return vk_down[vk];
}

int host_vk_to_ascii(int vk) {
    int shift = vk_down[0x10];
    if (vk >= 'A' && vk <= 'Z') return shift ? vk : vk + 32;
    if (vk >= '0' && vk <= '9') return vk;
    switch (vk) {
    case 0x20: return ' ';
    case 0x0d: return '\r';
    case 0x08: return 8;
    case 0x1b: return 27;
    case 0x09: return 9;
    case 0xbd: return shift ? '_' : '-';
    case 0xbe: return '.';
    case 0xbc: return ',';
    case 0xbf: return shift ? '?' : '/';
    case 0xba: return shift ? ':' : ';';
    case 0xde: return shift ? '"' : '\'';
    default: return 0;
    }
}

static int playing_now(void);
static int looking(void) { return rel_mode && playing_now(); }

void host_mouse_pos(int *x, int *y) {
    if (looking()) { /* mouse look: keep the game's own cursor steering centred */
        *x = (int16_t)MEMR16(SEGLIN(RT_SEL_DS, 0x53dc));
        *y = (int16_t)MEMR16(SEGLIN(RT_SEL_DS, 0x53de));
        return;
    }
    int mx, my;
    float lx, ly;
    SDL_GetMouseState(&mx, &my);
    if (rel_mode) { mx = vcur_x; my = vcur_y; } /* captured: menus use a virtual cursor */
    SDL_RenderWindowToLogical(renderer, mx, my, &lx, &ly);
    if (hd_enabled()) {
        hd_output_to_game((int)lx, (int)ly, x, y);
        return;
    }
    *x = (int)lx;
    *y = (int)(ly * 200 / 240);
    if (*x < 0) *x = 0;
    if (*x > 319) *x = 319;
    if (*y < 0) *y = 0;
    if (*y > 199) *y = 199;
}

int host_is_active(void) { return window_active; }

/* ---- modern controls: W/A/S/D, mouse look, mouse buttons ---- */
static int mouselook = 1;          /* --no-mouselook disables */
static double mouse_sens = 0.12;   /* degrees per mouse count (--sensitivity) */
static uint32_t last_play_ms;      /* last player tick that accepted modern input */
static int kb_fire, mouse_fire, fire_sent, kb_use, mouse_use, use_sent;
static int pad_fire, pad_use;      /* gamepad trigger / face buttons (pad_* below) */
static int pad_forward, pad_strafe; /* left stick during play, -1..1 */

int host_modern_input(int *forward, int *strafe, int *turn) {
    last_play_ms = SDL_GetTicks();
    *forward = (vk_down['W'] ? 1 : 0) - (vk_down['S'] ? 1 : 0) + pad_forward;
    *strafe = (vk_down['D'] ? 1 : 0) - (vk_down['A'] ? 1 : 0) + pad_strafe;
    if (*forward > 1) *forward = 1;
    if (*forward < -1) *forward = -1;
    if (*strafe > 1) *strafe = 1;
    if (*strafe < -1) *strafe = -1;
    /* The game's view angle is in whole degrees; the remainder (|r| <= 0.5)
     * carries over and the hi-res view is drawn with it (hd_set_turn_offset). */
    int t = (int)lround(mouse_accum);
    mouse_accum -= t;
    hd_set_turn_offset(mouse_accum);
    hd_note_mouse_turn(t);
    *turn = t;
    return 1;
}

static int playing_now(void) { return last_play_ms && SDL_GetTicks() - last_play_ms < 250; }

/* ------------------------------------------------------------------ system */
#ifdef __EMSCRIPTEN__
static void web_audio_pump(void);
/* The game's blocking loops (palette fades poll the retrace port, waits read
 * the clock) keep the browser's main thread busy for up to half a second;
 * every clock read tops up the audio queue so the sound keeps going. */
uint32_t host_ticks_ms(void) { web_audio_pump(); return SDL_GetTicks(); }
uint64_t host_time_us(void) { web_audio_pump(); return (uint64_t)(emscripten_get_now() * 1000.0); }
#else
uint32_t host_ticks_ms(void) { return SDL_GetTicks(); }
uint64_t host_time_us(void) { return SDL_GetPerformanceCounter() * 1000000ull / SDL_GetPerformanceFrequency(); }
#endif

int host_message_box(const char *caption, const char *text, unsigned type) {
    fprintf(stderr, "[%s] %s\n", caption, text);
    unsigned kind = type & 0x0f;
    SDL_MessageBoxButtonData buttons[3];
    int n = 0;
    /* MB_OK=0, MB_OKCANCEL=1, MB_YESNOCANCEL=3, MB_YESNO=4 ; IDOK=1 IDCANCEL=2 IDYES=6 IDNO=7 */
    if (kind == 4 || kind == 3) {
        buttons[n++] = (SDL_MessageBoxButtonData){SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 6, "Yes"};
        buttons[n++] = (SDL_MessageBoxButtonData){0, 7, "No"};
        if (kind == 3) buttons[n++] = (SDL_MessageBoxButtonData){SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 2, "Cancel"};
    } else if (kind == 1) {
        buttons[n++] = (SDL_MessageBoxButtonData){SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "OK"};
        buttons[n++] = (SDL_MessageBoxButtonData){SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 2, "Cancel"};
    } else {
        buttons[n++] = (SDL_MessageBoxButtonData){SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 1, "OK"};
    }
#ifdef __EMSCRIPTEN__
    /* the browser's own dialogs; Yes/No and OK/Cancel map to confirm() */
    if (n == 1) {
        EM_ASM({ alert(UTF8ToString($0) + "\n\n" + UTF8ToString($1)); }, caption, text);
        return buttons[0].buttonid;
    }
    return EM_ASM_INT({ return confirm(UTF8ToString($0) + "\n\n" + UTF8ToString($1)) ? 1 : 0; }, caption, text)
               ? buttons[0].buttonid : buttons[1].buttonid;
#else
    SDL_MessageBoxData box = {SDL_MESSAGEBOX_INFORMATION, window, caption, text, n, buttons, NULL};
    int result = 1;
    if (getenv("N3D_HEADLESS") || SDL_ShowMessageBox(&box, &result) != 0) result = buttons[0].buttonid;
    return result;
#endif
}

void host_request_quit(void) { quit_requested = 1; }

/* ------------------------------------------------------------------ audio */
#define MIX_RATE 44100
static int16_t *wave_buf;     /* converted to MIX_RATE mono s16 */
static uint32_t wave_len, wave_pos;
static int wave_vol_l = 65535, wave_vol_r = 65535;

/* Debug aid: N3D_WAVOUT=file.wav records the mixed output. */
static FILE *wav_out;
static uint32_t wav_bytes;
static void wav_header(FILE *f, uint32_t data) {
    uint32_t v;
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); v = 36 + data; fwrite(&v, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f); v = 16; fwrite(&v, 4, 1, f);
    uint16_t fmt[2] = {1, 2}; fwrite(fmt, 2, 2, f);
    v = MIX_RATE; fwrite(&v, 4, 1, f); v = MIX_RATE * 4; fwrite(&v, 4, 1, f);
    uint16_t ba[2] = {4, 16}; fwrite(ba, 2, 2, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    fseek(f, 0, SEEK_END);
}

static void audio_callback(void *ud, Uint8 *stream, int len) {
    (void)ud;
    int16_t *out = (int16_t *)stream;
    int frames = len / 4;
    music_render(out, frames, MIX_RATE);
    for (int i = 0; i < frames; i++) {
        if (wave_pos < wave_len) {
            int s = wave_buf[wave_pos++];
            int l = out[2 * i] + (s * wave_vol_l >> 16);
            int r = out[2 * i + 1] + (s * wave_vol_r >> 16);
            out[2 * i] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
            out[2 * i + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
        }
    }
    if (wav_out) { fwrite(stream, 1, (size_t)len, wav_out); wav_bytes += (uint32_t)len; }
}

#ifdef __EMSCRIPTEN__
/* Browser audio. SDL's Emscripten backend mixes on the main thread through a
 * ScriptProcessorNode with a ~23 ms buffer, so every blocking game loop (the
 * palette fades between screens take ~0.5 s) starves it and the sound skips.
 * Instead an AudioWorklet plays a queue of chunks on the audio thread, and the
 * main thread renders ahead into it: from the main loop and from every clock
 * read (web_audio_pump). Messages to the worklet arrive while the main thread
 * is still busy, so the queue keeps filling during the fades. */
#define WEB_AUDIO_LEAD_MS 60   /* rendered ahead of playback */
#define WEB_AUDIO_CHUNK 512    /* frames per message */

EM_JS(void, web_audio_init, (int rate, int lead_frames), {
    const AC = window.AudioContext || window.webkitAudioContext;
    if (!AC) return;
    const ctx = new AC({ sampleRate: rate, latencyHint: 'interactive' });
    const st = Module.n3dAudio = { ctx, node: null, queued: 0, at: 0, sent: 0, lead: lead_frames, rate };
    const src = `
class N3DOut extends AudioWorkletProcessor {
  constructor() {
    super();
    this.q = []; this.off = 0; this.queued = 0; this.n = 0;
    this.port.onmessage = (e) => {
      if (this.queued > ${rate}) return;              /* main thread far ahead: drop */
      this.q.push(e.data); this.queued += e.data.length >> 1;
    };
  }
  process(inputs, outputs) {
    const L = outputs[0][0], R = outputs[0][1] || outputs[0][0], N = L.length;
    let k = 0;
    while (k < N && this.q.length) {
      const c = this.q[0], frames = c.length >> 1, take = Math.min(N - k, frames - this.off);
      for (let j = 0; j < take; j++) { L[k + j] = c[2 * (this.off + j)]; R[k + j] = c[2 * (this.off + j) + 1]; }
      k += take; this.off += take; this.queued -= take;
      if (this.off >= frames) { this.q.shift(); this.off = 0; }
    }
    for (; k < N; k++) { L[k] = 0; R[k] = 0; }
    if ((++this.n & 3) === 0) this.port.postMessage(this.queued);
    return true;
  }
}
registerProcessor('n3d-out', N3DOut);`;
    const url = URL.createObjectURL(new Blob([src], { type: 'application/javascript' }));
    ctx.audioWorklet.addModule(url).then(() => {
        const node = new AudioWorkletNode(ctx, 'n3d-out', { numberOfInputs: 0, outputChannelCount: [2] });
        node.port.onmessage = (e) => { st.queued = e.data; st.at = performance.now(); st.sent = 0; };
        node.connect(ctx.destination);
        st.at = performance.now();
        st.node = node;
    }).catch((e) => console.warn('audio worklet failed', e));
    /* autoplay policy: (re)start on any input */
    const resume = () => { if (ctx.state !== 'running') ctx.resume(); };
    for (const ev of ['pointerdown', 'keydown', 'touchend']) window.addEventListener(ev, resume, true);
    resume();
});

/* frames to render now to keep the lead (0 while the worklet loads) */
EM_JS(int, web_audio_need, (void), {
    const st = Module.n3dAudio;
    if (!st || !st.node || st.ctx.state !== 'running') return 0;
    const played = (performance.now() - st.at) * st.rate / 1000;
    const est = Math.max(0, st.queued + st.sent - played); /* frames still queued */
    return Math.max(0, Math.floor(st.lead - est));
});

EM_JS(void, web_audio_push, (const int16_t *buf, int frames), {
    const st = Module.n3dAudio;
    const n = frames * 2, f = new Float32Array(n), h = HEAP16.subarray(buf >> 1, (buf >> 1) + n);
    for (let i = 0; i < n; i++) f[i] = h[i] / 32768;
    st.node.port.postMessage(f, [f.buffer]);
    st.sent += frames;
});

static int web_audio_on;

static void web_audio_pump(void) {
    static int busy;
    static double last;
    if (!web_audio_on || busy) return;
    double now = emscripten_get_now();
    if (now - last < 3.0) return;
    last = now;
    busy = 1;
    static int16_t buf[WEB_AUDIO_CHUNK * 2];
    for (int need = web_audio_need(); need > 0; need -= WEB_AUDIO_CHUNK) {
        int n = need < WEB_AUDIO_CHUNK ? need : WEB_AUDIO_CHUNK;
        audio_callback(NULL, (Uint8 *)buf, n * 4);
        web_audio_push(buf, n);
    }
    busy = 0;
}

void host_audio_poll(void) { web_audio_pump(); }

static void web_audio_start(void) {
    web_audio_init(MIX_RATE, MIX_RATE * WEB_AUDIO_LEAD_MS / 1000);
    web_audio_on = 1;
    music_decode_yield = web_audio_pump; /* tunes decode in one go when they start */
}
#endif

void host_wave_play(const uint8_t *data, uint32_t len, uint32_t rate, int bits, int channels) {
    if (getenv("N3D_DEBUG")) fprintf(stderr, "wave: %u bytes %u Hz %d-bit %dch\n", len, rate, bits, channels);
    uint32_t bytes_per = (uint32_t)(bits / 8) * (uint32_t)channels;
    if (!bytes_per || !rate) return;
    uint32_t in_frames = len / bytes_per;
    uint32_t out_frames = (uint32_t)((uint64_t)in_frames * MIX_RATE / rate);
    int16_t *buf = malloc(sizeof(int16_t) * (out_frames + 1));
    for (uint32_t i = 0; i < out_frames; i++) {
        uint32_t src = (uint32_t)((uint64_t)i * rate / MIX_RATE);
        int s;
        if (bits == 8) s = ((int)data[src * bytes_per] - 128) << 8;
        else s = (int16_t)(data[src * bytes_per] | (data[src * bytes_per + 1] << 8));
        buf[i] = (int16_t)s;
    }
    SDL_LockAudioDevice(audio_dev);
    free(wave_buf);
    wave_buf = buf;
    wave_len = out_frames;
    wave_pos = 0;
    SDL_UnlockAudioDevice(audio_dev);
}

void host_wave_stop(void) {
    SDL_LockAudioDevice(audio_dev);
    wave_pos = wave_len;
    SDL_UnlockAudioDevice(audio_dev);
}

int host_wave_playing(void) {
    SDL_LockAudioDevice(audio_dev);
    int p = wave_pos < wave_len;
    SDL_UnlockAudioDevice(audio_dev);
    return p;
}

void host_wave_volume(uint32_t vol) {
    wave_vol_l = (int)(vol & 0xffff);
    wave_vol_r = (int)(vol >> 16);
}

void host_music_play_file(const char *dosname) {
    if (getenv("N3D_DEBUG")) fprintf(stderr, "music: %s\n", dosname);
    FILE *f = dos_fopen(dosname, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) n = 0;
    fclose(f);
    music_prefetch(buf, (size_t)n); /* decode a pre-rendered tune outside the audio lock */
    SDL_LockAudioDevice(audio_dev);
    music_load_midi(buf, (size_t)n);
    SDL_UnlockAudioDevice(audio_dev);
    free(buf);
}

void host_music_stop(void) {
    SDL_LockAudioDevice(audio_dev);
    music_stop();
    SDL_UnlockAudioDevice(audio_dev);
}

void host_music_volume(uint32_t vol) { music_set_volume((int)(vol & 0xffff)); }

/* ------------------------------------------------------------------ game driver */
static void key_event(int vk, int scancode, int down, int repeat) {
    uint16_t flags = (uint16_t)(scancode & 0xff);
    if (!down) flags |= 0xc000;
    else if (repeat) flags |= 0x4000;
    uint16_t args[2] = {(uint16_t)vk, flags};
    rt_callf(G_KEY_EVENT, 2, args);
}

/* Debug aid: N3D_KEYS="ms:VK[:hold],..." injects key presses (VK in hex) at times
 * relative to start, e.g. N3D_KEYS="3000:0d,4000:0d,6000:26:1500". */
typedef struct { uint32_t t; int vk; uint32_t hold; int state; } script_key_t;
static script_key_t script_keys[128];
static int script_n;

static void script_parse(void) {
    const char *s = getenv("N3D_KEYS");
    while (s && *s && script_n < 128) {
        unsigned t, vk, hold = 80;
        int used = 0;
        if (sscanf(s, "%u:%x:%u%n", &t, &vk, &hold, &used) == 3 || (hold = 80, sscanf(s, "%u:%x%n", &t, &vk, &used) == 2)) {
            script_keys[script_n++] = (script_key_t){t, (int)vk, hold, 0};
        }
        s = strchr(s, ',');
        if (s) s++;
    }
}

static void key_event(int vk, int scancode, int down, int repeat);

static void script_tick(uint32_t now) {
    for (int i = 0; i < script_n; i++) {
        script_key_t *k = &script_keys[i];
        if (k->state == 0 && now >= k->t) {
            k->state = 1;
            vk_down[k->vk] = 1;
            key_event(k->vk, 0, 1, 0);
        } else if (k->state == 1 && now >= k->t + k->hold) {
            k->state = 2;
            vk_down[k->vk] = 0;
            key_event(k->vk, 0, 0, 0);
        }
    }
}

/* Ctrl (fire) and Space (use) can also come from the mouse buttons. */
static void sync_buttons(void) {
    int f = kb_fire || mouse_fire || pad_fire, u = kb_use || mouse_use || pad_use;
    if (f != fire_sent) { fire_sent = f; key_event(0x11, 0x1d, f, 0); }
    if (u != use_sent) { use_sent = u; key_event(0x20, 0x39, u, 0); }
}

static void update_mouse_mode(void) {
    int focused = (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
    /* captured from the start whenever the window has focus */
    int want = mouselook && window_active && focused && !getenv("N3D_HEADLESS");
    static int was_looking = -1;
    int look = want && playing_now();
    if (look != was_looking) { /* entering/leaving play: drop stale turn */
        was_looking = look;
        mouse_accum = 0;
        hd_set_turn_offset(0);
        if (!look && (mouse_fire || mouse_use)) { mouse_fire = mouse_use = 0; sync_buttons(); }
    }
    /* also re-check SDL's actual state: capture can fail while unfocused */
    if (want == rel_mode && (!want || SDL_GetRelativeMouseMode())) return;
    rel_mode = want;
    /* captured while playing: hidden, confined to the window, raw relative
     * motion (no dependence on where the OS cursor is); released in menus,
     * on focus loss (Alt+Tab / Cmd+Tab) and when the game stops ticking */
    SDL_SetWindowGrab(window, want ? SDL_TRUE : SDL_FALSE);
    SDL_SetRelativeMouseMode(want ? SDL_TRUE : SDL_FALSE);
    if (want) {
        int w, h;
        SDL_GetWindowSize(window, &w, &h);
        vcur_x = w / 2;
        vcur_y = h / 2;
    } else {
        int w, h;
        SDL_GetWindowSize(window, &w, &h);
        SDL_WarpMouseInWindow(window, w / 2, h / 2);
    }
    mouse_accum = 0;
    hd_set_turn_offset(0);
    SDL_FlushEvent(SDL_MOUSEMOTION);
    if (!want && (mouse_fire || mouse_use)) { mouse_fire = mouse_use = 0; sync_buttons(); }
}

#ifdef __EMSCRIPTEN__
/* The page (web/n3d-web.js) calls this when the browser released the pointer
 * lock because Esc was pressed and swallowed that key press: during play,
 * hand the game its Esc (menu) like the desktop version gets it. */
EMSCRIPTEN_KEEPALIVE void n3d_web_esc_swallowed(void) {
    if (!looking()) return;
    key_event(0x1b, 0x01, 1, 0);
    key_event(0x1b, 0x01, 0, 0);
}
#endif

/* Debug aid: N3D_MOUSE="ms:dx,ms:L:hold,ms:R:hold" injects mouse motion / buttons. */
typedef struct { uint32_t t; int dx; char button; uint32_t hold; int state; } script_mouse_t;
static script_mouse_t script_mouse[64];
static int script_mouse_n;

static void script_mouse_parse(void) {
    const char *s = getenv("N3D_MOUSE");
    while (s && *s && script_mouse_n < 64) {
        unsigned t, hold = 100;
        int dx;
        char b;
        if (sscanf(s, "%u:%c:%u", &t, &b, &hold) >= 2 && (b == 'L' || b == 'R'))
            script_mouse[script_mouse_n++] = (script_mouse_t){t, 0, b, hold, 0};
        else if (sscanf(s, "%u:%d", &t, &dx) == 2)
            script_mouse[script_mouse_n++] = (script_mouse_t){t, dx, 0, 0, 0};
        s = strchr(s, ',');
        if (s) s++;
    }
}

static void script_mouse_tick(uint32_t now) {
    for (int i = 0; i < script_mouse_n; i++) {
        script_mouse_t *m = &script_mouse[i];
        if (m->state == 0 && now >= m->t) {
            m->state = 1;
            if (!m->button) { mouse_accum += m->dx * mouse_sens; hd_set_turn_offset(mouse_accum); m->state = 2; }
            else { if (m->button == 'L') mouse_fire = 1; else mouse_use = 1; sync_buttons(); }
        } else if (m->state == 1 && now >= m->t + m->hold) {
            m->state = 2;
            if (m->button == 'L') mouse_fire = 0; else mouse_use = 0;
            sync_buttons();
        }
    }
}

/* ---- gamepad (SDL game controller: Xbox / PlayStation / Switch layouts, and
 * the browser's standard gamepad mapping) ----
 * Play: left stick moves and strafes, right stick turns (smoothly, like the
 * mouse), RT fires, LT runs, A/X use, LB/RB previous/next weapon, Y automap.
 * Menus: d-pad / left stick navigate, A selects, B goes back.
 * Start = Esc (menu), Back = Tab (status), d-pad = arrow keys everywhere. */
static SDL_GameController *pad;
static double pad_turn_speed = 200.0; /* --pad-turn: degrees per second at full deflection */
static int pad_weapon = 1;            /* last weapon chosen (1-4), for LB/RB cycling */
static int pad_held_vk[SDL_CONTROLLER_BUTTON_MAX]; /* key each held button sent (0: none, -1: use) */
static int pad_run, pad_menu_dir;
static uint32_t pad_menu_next;
static uint64_t pad_last_us;

static int pad_scancode(int vk) {
    switch (vk) {
    case 0x0d: return 0x1c; case 0x1b: return 0x01; case 0x09: return 0x0f; case 0x78: return 0x43;
    case 0x26: return 0x48; case 0x28: return 0x50; case 0x25: return 0x4b; case 0x27: return 0x4d;
    default: return vk >= '1' && vk <= '9' ? vk - '1' + 2 : 0;
    }
}

static void pad_key(int vk, int down) {
    vk_down[vk] = (uint8_t)down;
    key_event(vk, pad_scancode(vk), down, 0);
}
static void pad_tap(int vk) { pad_key(vk, 1); pad_key(vk, 0); }

static void pad_release_all(void) {
    for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++)
        if (pad_held_vk[b] > 0) pad_key(pad_held_vk[b], 0);
    memset(pad_held_vk, 0, sizeof pad_held_vk);
    if (pad_run) { pad_run = 0; vk_down[0x10] = 0; key_event(0x10, 0x36, 0, 0); }
    pad_forward = pad_strafe = pad_menu_dir = 0;
    if (pad_fire || pad_use) { pad_fire = pad_use = 0; sync_buttons(); }
}

static void pad_button(int b, int down) {
    if (!down) {
        int vk = pad_held_vk[b];
        pad_held_vk[b] = 0;
        if (vk > 0) pad_key(vk, 0);
        else if (vk < 0) { pad_use = 0; sync_buttons(); }
        return;
    }
    int play = playing_now(), vk = 0;
    switch (b) {
    case SDL_CONTROLLER_BUTTON_A: if (play) vk = -1; else vk = 0x0d; break;
    case SDL_CONTROLLER_BUTTON_X: if (play) vk = -1; break;
    case SDL_CONTROLLER_BUTTON_B: if (!play) vk = 0x1b; break;
    case SDL_CONTROLLER_BUTTON_Y: if (play) vk = 0x78; break; /* F9 automap */
    case SDL_CONTROLLER_BUTTON_START: vk = 0x1b; break;
    case SDL_CONTROLLER_BUTTON_BACK: vk = 0x09; break;
    case SDL_CONTROLLER_BUTTON_DPAD_UP: vk = 0x26; break;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: vk = 0x28; break;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: vk = 0x25; break;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: vk = 0x27; break;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
        if (play) { /* weapons the player doesn't have are ignored by the game; the next press moves on */
            pad_weapon += b == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER ? 1 : -1;
            if (pad_weapon > 4) pad_weapon = 1;
            if (pad_weapon < 1) pad_weapon = 4;
            pad_tap('0' + pad_weapon);
        }
        return;
    default: return;
    }
    pad_held_vk[b] = vk;
    if (vk > 0) pad_key(vk, 1);
    else if (vk < 0) { pad_use = 1; sync_buttons(); }
}

static double pad_axis(SDL_GameControllerAxis a) {
    double v = SDL_GameControllerGetAxis(pad, a) / 32767.0;
    return v < -1 ? -1 : v > 1 ? 1 : v;
}

/* sticks and triggers, once per main loop iteration */
static void pad_poll(void) {
    uint64_t now_us = host_time_us();
    double dt = pad_last_us ? (now_us - pad_last_us) / 1e6 : 0;
    pad_last_us = now_us;
    if (dt > 0.1) dt = 0.1;
    if (!pad || !window_active) return;
    int play = playing_now();
    double lx = pad_axis(SDL_CONTROLLER_AXIS_LEFTX), ly = pad_axis(SDL_CONTROLLER_AXIS_LEFTY);
    double rx = pad_axis(SDL_CONTROLLER_AXIS_RIGHTX);
    int fire = pad_axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 0.35;
    int run = play && pad_axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 0.35;
    /* movement is digital in the game (one step per tick): a dead zone is enough */
    pad_forward = play ? (ly < -0.4 ? 1 : ly > 0.4 ? -1 : 0) : 0;
    pad_strafe = play ? (lx > 0.4 ? 1 : lx < -0.4 ? -1 : 0) : 0;
    if (play && fire != pad_fire) {
        pad_fire = fire;
        sync_buttons();
        if (fire) SDL_GameControllerRumble(pad, 0x3000, 0x6000, 60);
    } else if (!play && pad_fire) { pad_fire = 0; sync_buttons(); }
    if (run != pad_run) { pad_run = run; vk_down[0x10] = (uint8_t)run; key_event(0x10, 0x36, run, 0); }
    if (play) {
        /* turning: dead zone, then a squared response for fine aim near the centre */
        double a = fabs(rx);
        if (a > 0.12) {
            double t = (a - 0.12) / 0.88;
            mouse_accum += (rx < 0 ? -1 : 1) * t * t * pad_turn_speed * dt;
            hd_set_turn_offset(mouse_accum);
        }
        pad_menu_dir = 0;
    } else {
        /* left stick in menus: arrow key presses with auto-repeat */
        int dir = ly < -0.6 ? -1 : ly > 0.6 ? 1 : 0;
        uint32_t now = SDL_GetTicks();
        if (dir != pad_menu_dir) { pad_menu_dir = dir; if (dir) { pad_tap(dir < 0 ? 0x26 : 0x28); pad_menu_next = now + 400; } }
        else if (dir && now >= pad_menu_next) { pad_tap(dir < 0 ? 0x26 : 0x28); pad_menu_next = now + 120; }
    }
}

static void pad_device_event(const SDL_Event *e) {
    if (e->type == SDL_CONTROLLERDEVICEADDED) {
        if (pad) return;
        pad = SDL_GameControllerOpen(e->cdevice.which);
        if (pad) fprintf(stderr, "gamepad: %s\n", SDL_GameControllerName(pad));
    } else if (pad && e->cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad))) {
        pad_release_all();
        SDL_GameControllerClose(pad);
        pad = NULL;
        for (int i = 0; i < SDL_NumJoysticks(); i++) /* fall back to another connected pad */
            if (SDL_IsGameController(i) && (pad = SDL_GameControllerOpen(i))) break;
    }
}

static void ds_w16(uint16_t off, uint16_t v) { MEMW16(SEGLIN(RT_SEL_DS, off), v); }

/* First *.sf2 file in dir (sorted by name), or 0. */
static int find_sf2(const char *dir, char *out, size_t n) {
    char best[512] = "";
#ifdef _WIN32
    char pat[1024];
    struct _finddata_t fd;
    snprintf(pat, sizeof pat, "%s\\*.sf2", dir);
    intptr_t h = _findfirst(pat, &fd);
    if (h == -1) return 0;
    do {
        if (!best[0] || strcmp(fd.name, best) < 0) snprintf(best, sizeof best, "%s", fd.name);
    } while (_findnext(h, &fd) == 0);
    _findclose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l > 4 && !strcasecmp(e->d_name + l - 4, ".sf2") && (!best[0] || strcmp(e->d_name, best) < 0))
            snprintf(best, sizeof best, "%s", e->d_name);
    }
    closedir(d);
#endif
    if (!best[0]) return 0;
    snprintf(out, n, "%s/%s", dir, best);
    return 1;
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [--data DIR] [--user DIR] [--soundfont FILE.sf2] [--fm] [--scale N] [--fullscreen] [-- game args]\n"
            "  --data DIR   folder with MAP.1, IMG.1, SND.DAT, UIF.DAT, ... (default: ./data)\n"
            "  --user DIR   folder for saves/config (default: per-user app data)\n"
            "  --soundfont  General MIDI SoundFont for music (default: first *.sf2 in the data or user folder)\n"
            "  --fm         always use the built-in OPL2 FM synth for music\n"
            "  --music DIR  pre-rendered tuneNN.ogg/.wav (default: <data>/music or ./music; 'none' = synth only)\n"
            "  --sb MODEL   Sound Blaster FM chip + output filter for the FM synth: sb1 sb2 sbpro1 sbpro2 sb16 (default)\n"
            "  --fm-driver FILE  FM synth plays like Windows 3.1's Ad Lib driver, patches from your MSADLIB.DRV\n"
            "  --res WxH    render resolution, e.g. 1920x1080, 3440x1440 (default: match window)\n"
            "  --res window | desktop   follow the window size / use the desktop resolution\n"
            "  --classic    original 320x200 frame, scaled (same as --res 320x200)\n"
            "  --scale N    initial window size: 320x240 * N\n"
            "  --fullscreen start in fullscreen (Alt+Enter toggles)\n"
            "  --hidpi / --no-hidpi  render at device pixels on HiDPI screens (default: on; off in the browser)\n"
            "  --sensitivity F  mouse look speed in degrees per mouse count (default 0.12)\n"
            "  --no-mouselook   don't capture the mouse for turning during play\n"
            "  --pad-turn F     gamepad turn speed, degrees per second at full right-stick deflection (default 200)\n"
            "  --fps N      hi-res redraw rate during play (default 60; 0 = every display refresh)\n"
            "  --filter nearest|bilinear|trilinear|aniso   texture filtering (default aniso)\n"
            "  --aniso N    anisotropic taps, 1..16 (default 16)\n"
            "  --upscale none|scale2x|scale3x|scale4x|scale6x   pixel-art upscaling of textures (default scale4x)\n"
            "  --texture-quality off|high   nearest + no upscaling / aniso 16 + scale4x\n"
            "  --hd-pack DIR  replacement art pack (re/upscale/pack.py) for the hi-res renderer\n"
            "               (default: $N3D_HDPACK; N3D_HDPACK_LOG=file lists used images still missing)\n",
            argv0);
}

static int fullscreen;
static char *pref;
static uint32_t start_ticks;

static void host_shutdown(void) {
    static int done;
    if (done) return;
    done = 1;
    rt_callf(G_SHUTDOWN, 0, NULL);
    if (audio_dev) SDL_CloseAudioDevice(audio_dev);
    if (wav_out) { wav_header(wav_out, wav_bytes); fclose(wav_out); }
    SDL_Quit();
    SDL_free(pref);
#ifdef __EMSCRIPTEN__
    emscripten_cancel_main_loop();
    EM_ASM({ if (Module.onGameQuit) Module.onGameQuit(); });
#endif
}

/* One pass of what Windows' message loop did: deliver input, then OnIdle. */
static void main_iteration(void) {
#ifdef __EMSCRIPTEN__
    web_audio_pump();
#endif
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT: quit_requested = 1; break;
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT)) {
                fullscreen = !fullscreen;
                SDL_SetWindowFullscreen(window, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                break;
            }
            int vk = vk_from_sdl(e.key.keysym.sym);
            if (!vk) break;
            vk_down[vk] = e.type == SDL_KEYDOWN;
            if (e.type == SDL_KEYDOWN && vk >= '1' && vk <= '4') pad_weapon = vk - '0';
            if (vk == 0x11 && !e.key.repeat) { kb_fire = vk_down[vk]; sync_buttons(); break; }
            if (vk == 0x20 && !e.key.repeat) { kb_use = vk_down[vk]; sync_buttons(); break; }
            key_event(vk, pc_scancode(e.key.keysym.scancode), e.type == SDL_KEYDOWN, e.key.repeat);
            break;
        }
        case SDL_CONTROLLERDEVICEADDED:
        case SDL_CONTROLLERDEVICEREMOVED:
            pad_device_event(&e);
            break;
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP:
            if (pad && e.cbutton.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad)) &&
                (window_active || e.type == SDL_CONTROLLERBUTTONUP))
                pad_button(e.cbutton.button, e.type == SDL_CONTROLLERBUTTONDOWN);
            break;
        case SDL_MOUSEMOTION:
            if (looking()) { mouse_accum += e.motion.xrel * mouse_sens; hd_set_turn_offset(mouse_accum); }
            else if (rel_mode) {
                int w, h;
                SDL_GetWindowSize(window, &w, &h);
                vcur_x += e.motion.xrel;
                vcur_y += e.motion.yrel;
                if (vcur_x < 0) vcur_x = 0;
                if (vcur_x > w - 1) vcur_x = w - 1;
                if (vcur_y < 0) vcur_y = 0;
                if (vcur_y > h - 1) vcur_y = h - 1;
            }
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            if (!looking()) break; /* menus poll the button state themselves */
            if (e.button.button == SDL_BUTTON_LEFT) mouse_fire = e.type == SDL_MOUSEBUTTONDOWN;
            else if (e.button.button == SDL_BUTTON_RIGHT) mouse_use = e.type == SDL_MOUSEBUTTONDOWN;
            sync_buttons();
            break;
        case SDL_WINDOWEVENT:
            if (!getenv("N3D_HEADLESS") && (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED || e.window.event == SDL_WINDOWEVENT_FOCUS_LOST)) {
                int active = e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED;
                if (active != window_active) {
                    window_active = active;
                    if (!active) {
                        pad_release_all();
                        memset(vk_down, 0, sizeof vk_down);
                        kb_fire = kb_use = mouse_fire = mouse_use = 0;
                        sync_buttons();
                    }
                    uint16_t a[1] = {(uint16_t)active};
                    rt_callf(G_ON_ACTIVATE, 1, a);
                }
            }
            if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) follow_window_size();
            if (e.window.event == SDL_WINDOWEVENT_EXPOSED || e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) redraw();
            break;
        }
    }
    if (quit_requested) { host_shutdown(); return; }
    uint32_t t0 = SDL_GetTicks();
    if (getenv("N3D_DUMPDS") && t0 - start_ticks > (uint32_t)atoi(getenv("N3D_DUMPDS"))) {
        FILE *f = fopen("ds_dump.bin", "wb");
        if (f) { fwrite(rt_seg[RT_SEL_DS], 1, 0x10000, f); fclose(f); }
        quit_requested = 1;
        host_shutdown();
        return;
    }
    if (script_n) script_tick(t0 - start_ticks);
    if (script_mouse_n) script_mouse_tick(t0 - start_ticks);
    update_mouse_mode();
    pad_poll();
    presented = 0;
    rt_callf(G_ON_IDLE, 0, NULL);
    if (hd_view_live()) {
        /* The game draws 25 frames a second; the hi-res view is shown at a
         * steady --fps (0: every display refresh), each frame drawn with
         * everything moved along smoothly (src/hd.c). */
        uint64_t now = host_time_us(), period = target_fps > 0 ? 1000000 / (uint64_t)target_fps : 0;
        if (now >= next_due) {
            next_due = next_due + period > now ? next_due + period : now + period;
            if (hd_rerender()) { redraw(); debug_shot(); }
        }
    }
#ifndef __EMSCRIPTEN__
    if (hd_view_live()) {
        /* paced frames: don't oversleep past the next one */
        uint64_t now = host_time_us();
        if (next_due > now + 2500) SDL_Delay(1);
    } else if (SDL_GetTicks() == t0) SDL_Delay(1);
#endif
}

int main(int argc, char **argv) {
    const char *data_dir = "data";
    const char *user_dir = NULL;
    int scale = 3, force_fm = 0;
#ifdef __EMSCRIPTEN__
    int hidpi = 0; /* full device-pixel resolution is opt-in: the renderer is single-threaded here */
#else
    int hidpi = 1;
#endif
    const char *soundfont = getenv("N3D_SOUNDFONT");
    const char *music_dir = getenv("N3D_MUSIC");
    const char *hd_pack = getenv("N3D_HDPACK");
    char game_args[256] = "";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--data") && i + 1 < argc) data_dir = argv[++i];
        else if (!strcmp(argv[i], "--user") && i + 1 < argc) user_dir = argv[++i];
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fullscreen")) fullscreen = 1;
        else if (!strcmp(argv[i], "--hidpi")) hidpi = 1;
        else if (!strcmp(argv[i], "--no-hidpi")) hidpi = 0;
        else if (!strcmp(argv[i], "--soundfont") && i + 1 < argc) soundfont = argv[++i];
        else if (!strcmp(argv[i], "--music") && i + 1 < argc) music_dir = argv[++i];
        else if (!strcmp(argv[i], "--sb") && i + 1 < argc) { if (!music_set_sb_model(argv[++i])) { usage(argv[0]); return 1; } }
        else if (!strcmp(argv[i], "--fm-driver") && i + 1 < argc) music_set_fm_driver(argv[++i]);
        else if (!strcmp(argv[i], "--fm")) force_fm = 1;
        else if (!strcmp(argv[i], "--sensitivity") && i + 1 < argc) mouse_sens = atof(argv[++i]);
        else if (!strcmp(argv[i], "--no-mouselook")) mouselook = 0;
        else if (!strcmp(argv[i], "--pad-turn") && i + 1 < argc) pad_turn_speed = atof(argv[++i]);
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc) target_fps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--filter") && i + 1 < argc) { if (!hq_parse_filter(argv[++i])) { usage(argv[0]); return 1; } }
        else if (!strcmp(argv[i], "--aniso") && i + 1 < argc) {
            hq.aniso = atoi(argv[++i]);
            if (hq.aniso < 1) hq.aniso = 1;
            if (hq.aniso > 16) hq.aniso = 16;
            hq.filter = HQ_ANISO;
        }
        else if (!strcmp(argv[i], "--upscale") && i + 1 < argc) { if (!hq_parse_upscale(argv[++i])) { usage(argv[0]); return 1; } }
        else if (!strcmp(argv[i], "--texture-quality") && i + 1 < argc) {
            const char *q = argv[++i]; /* presets */
            if (!strcmp(q, "off")) { hq.filter = HQ_NEAREST; hq.upscale = 1; }
            else if (!strcmp(q, "high")) { hq.filter = HQ_ANISO; hq.aniso = 16; hq.upscale = 4; }
            else { usage(argv[0]); return 1; }
        }
        else if (!strcmp(argv[i], "--hd-pack") && i + 1 < argc) hd_pack = argv[++i];
        else if (!strcmp(argv[i], "--classic")) { res_mode = RES_FIXED; render_w = 320; render_h = 200; }
        else if (!strcmp(argv[i], "--res") && i + 1 < argc) {
            const char *r = argv[++i];
            int w, h;
            if (!strcmp(r, "window")) res_mode = RES_WINDOW;
            else if (!strcmp(r, "desktop")) res_mode = RES_DESKTOP;
            else if (sscanf(r, "%dx%d", &w, &h) == 2 && w >= 64 && h >= 48) { res_mode = RES_FIXED; render_w = w; render_h = h; }
            else { usage(argv[0]); return 1; }
        }
        else if (!strcmp(argv[i], "--")) {
            for (i++; i < argc; i++) {
                strncat(game_args, argv[i], sizeof game_args - strlen(game_args) - 2);
                strcat(game_args, " ");
            }
        } else { usage(argv[0]); return 1; }
    }
    if (scale < 1) scale = 1;
    if (hd_pack) hd_pack_load(hd_pack);

#ifdef __EMSCRIPTEN__
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0) {
#else
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0) {
#endif
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    if (!user_dir) {
        pref = SDL_GetPrefPath("Gray Design Associates", "Nitemare3D");
        user_dir = pref ? pref : ".";
    }
    rt_init();
    dos_set_paths(data_dir, user_dir);
    {
        FILE *probe = dos_fopen("map.1", "rb");
        if (!probe) {
            char msg[1200];
            snprintf(msg, sizeof msg, "Game data not found in '%s'.\nCopy MAP.1, IMG.1, SND.DAT, UIF.DAT, NITE3D.BSF from the original game there, or pass --data DIR.", data_dir);
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Nitemare-3D", msg, NULL);
            fprintf(stderr, "%s\n", msg);
            return 1;
        }
        fclose(probe);
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    int win_w = 320 * scale, win_h = 240 * scale;
    {
        SDL_DisplayMode dm;
        if (SDL_GetDesktopDisplayMode(0, &dm) == 0) {
            if (res_mode == RES_DESKTOP) { render_w = dm.w; render_h = dm.h; }
            if (res_mode != RES_WINDOW && !(render_w == 320 && render_h == 200)) {
                /* window with the render aspect, fitting the desktop */
                win_w = render_w; win_h = render_h;
                while (win_w > dm.w * 9 / 10 || win_h > dm.h * 9 / 10) { win_w = win_w * 3 / 4; win_h = win_h * 3 / 4; }
            }
        }
    }
    window = SDL_CreateWindow("Nitemare-3D", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, win_w, win_h,
                              SDL_WINDOW_RESIZABLE | (hidpi ? SDL_WINDOW_ALLOW_HIGHDPI : 0) | (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    SDL_RaiseWindow(window); /* take focus so the mouse is captured from the start */
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) renderer = SDL_CreateRenderer(window, -1, 0);
    {
        int w = render_w, h = render_h;
        render_w = render_h = 0;
        setup_render_target(w, h);
        follow_window_size();
    }

    SDL_AudioSpec want = {0}, have;
    want.freq = MIX_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = audio_callback;
    char sf_path[1024];
    if (!soundfont && (find_sf2(data_dir, sf_path, sizeof sf_path) || find_sf2(user_dir, sf_path, sizeof sf_path)))
        soundfont = sf_path;
    music_init(data_dir, force_fm ? NULL : soundfont);
    if (music_dir) music_set_dir(strcmp(music_dir, "none") ? music_dir : NULL);
    if (getenv("N3D_WAVOUT") && (wav_out = fopen(getenv("N3D_WAVOUT"), "wb"))) wav_header(wav_out, 0);
#ifdef __EMSCRIPTEN__
    (void)want; (void)have;
    web_audio_start(); /* audio_dev stays 0: everything runs on the main thread */
#else
    audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (audio_dev) SDL_PauseAudioDevice(audio_dev, 0);
#endif

    /* --- what MFC did before the game ran: create frame + view, size the view --- */
    ds_w16(DS_HWND_TOOLBAR, 0x0104);
    ds_w16(DS_HWND_STATUS, 0x0106);
    {
        uint16_t a[1] = {0x0102};
        rt_callf(G_VIEW_ONCREATE, 1, a);
        uint16_t s[4] = {0x0102, 0, 320, 200};
        rt_callf(G_VIEW_ONSIZE, 4, s);
    }
    /* CNite3wApp::InitInstance -> game init with m_lpCmdLine */
    {
        uint16_t cmd = rt_local_alloc(0x40, 256);
        rt_strcpy_to_far(SEGLIN(RT_SEL_DS, cmd), game_args, 256);
        uint16_t a[3] = {cmd, RT_SEL_DS, 0x0300};
        rt_callf(G_INIT_INSTANCE, 3, a);
    }
    ds_w16(DS_HWND_MAIN, 0x0100);
    script_parse();
    script_mouse_parse();
    start_ticks = SDL_GetTicks();

#ifdef __EMSCRIPTEN__
    /* the browser drives the loop (one iteration per display refresh) */
    emscripten_set_main_loop(main_iteration, 0, 1);
#else
    while (!quit_requested) main_iteration();
    host_shutdown();
#endif
    return 0;
}
