/* Platform services the Win16 layer needs from the host (implemented with SDL2). */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Video */
void host_set_palette_entry(int i, uint8_t r, uint8_t g, uint8_t b);
void host_get_palette_entry(int i, uint8_t *r, uint8_t *g, uint8_t *b);
void host_present(const uint8_t *pixels, int w, int h);
void host_request_window_size(int w, int h);
void host_set_title(const char *title);
void host_show_cursor(int show);

/* Input (Windows virtual-key codes) */
int host_key_state(int vk);
int host_vk_to_ascii(int vk);
void host_mouse_pos(int *x, int *y);
int host_is_active(void);
/* Modern controls (W/A/S/D + mouse look), polled once per player tick. */
int host_modern_input(int *forward, int *strafe, int *turn_degrees);

/* System */
uint32_t host_ticks_ms(void);
uint64_t host_time_us(void);
int host_message_box(const char *caption, const char *text, unsigned type);
void host_request_quit(void);
/* Called on every Win16 API call / software interrupt: the browser host tops
 * up its audio queue there while game code keeps the main thread busy. */
#ifdef __EMSCRIPTEN__
void host_audio_poll(void);
#else
static inline void host_audio_poll(void) {}
#endif

/* Audio */
void host_wave_play(const uint8_t *data, uint32_t len, uint32_t rate, int bits, int channels);
void host_wave_stop(void);
int host_wave_playing(void);
void host_wave_volume(uint32_t vol);
void host_music_play_file(const char *dosname);
void host_music_stop(void);
void host_music_volume(uint32_t vol);

/* Win16 layer callbacks */
void win16_set_client_size(int w, int h);

#ifdef __cplusplus
}
#endif
