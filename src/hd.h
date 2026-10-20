/*
 * Native high-resolution 3D view renderer and frame compositor (src/hd.c).
 *
 * The original renderer keeps running into the 320x200 WinG DIB (its walk
 * over the visible walls and objects also drives game logic: automap reveal,
 * wall animation, enemy targeting). Right after it has drawn the 3D view, the
 * native renderer draws the same world state again at the output resolution,
 * with the original projection scaled up and the horizontal field of view
 * widened for wide aspect ratios (Hor+). The HUD, menus and everything the
 * game draws over the view (weapon, frame decorations, messages) come from
 * the 320x200 frame, scaled with the original 4:3 pixel aspect.
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Output (render) resolution; 0x0 or 320x200 disables the native renderer. */
void hd_set_output(int w, int h);
int hd_enabled(void);

/* Hooks called from the overrides in src/rt/overrides.c. */
void hd_view_begin(void);         /* before the original draws the 3D view */
void hd_view_drawn(void);         /* after the original drew the 3D view */
/* Mouse look: extra view rotation in degrees not yet applied to the game's
 * integer angle; hd_rerender redraws the view with it between game frames. */
void hd_set_turn_offset(double degrees);
int hd_rerender(void);
/* Smooth motion: whole degrees of mouse turn the game angle just took (shown
 * immediately, not interpolated); whether the game is drawing the 3D view (then
 * the host shows hi-res frames on its own schedule, see --fps). */
void hd_note_mouse_turn(int degrees);
int hd_view_live(void);
enum { HD_ANCHOR_CENTER = 1, HD_ANCHOR_LEFT = 2, HD_ANCHOR_RIGHT = 3 };
void hd_layer_begin(int anchor);  /* following draws into the view are an overlay layer */
void hd_layer_end(void);
int hd_layer_current(void);      /* anchor of the open layer, 0 if none */
void hd_overlays_done(void);      /* all regular overlays of this frame are drawn */
void hd_object_queued(uint32_t obj, uint8_t frame); /* the original queued this object with this frame (then animated it) */
void hd_animate_object(uint32_t obj); /* overrides.c: animate like FUN_1010_cbce does */

/* Compose the final frame: frame is the presented 320x200 DIB, out is w*h
 * XRGB8888 pixels (pitch in pixels) of the size given to hd_set_output. */
void hd_compose(const uint8_t *frame, const uint8_t pal[256][3], uint32_t *out, int pitch);

/* Where a 320x200 coordinate ends up on the output (for mouse mapping). */
void hd_output_to_game(int ox, int oy, int *gx, int *gy);

#ifdef __cplusplus
}
#endif
