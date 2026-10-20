/* Hand-written replacements for recompiled functions listed in overrides.txt.
 *
 * Each override is entered exactly like the generated code: the caller has
 * pushed its arguments and a far return address (CS, IP) on the emulated
 * stack. Being cdecl far functions, overrides leave the arguments alone and
 * only pop the 4-byte return address (the RETF the caller expects). */
#include "rt/rt.h"
#include "gen/rc_funcs.h"
#include "hd.h"
#include "rt/host.h"

static uint16_t ds16(uint16_t off) { return MEMR16(SEGLIN(RT_SEL_DS, off)); }
static void retf(void) { rSP = (uint16_t)(rSP + 4); }

/* 1010:d6de draw_view: build the visible wall list, draw floor/ceiling,
 * walls and sprites into the WinG DIB. The original sequence is kept (it
 * also has game-logic side effects: automap reveal, wall animation, enemy
 * targeting), then the native renderer gets a chance to snapshot it. */
void f_1010_d6de(void) {
    RT_ENTER(0x1010d6deu);
    hd_view_begin();
    MEMW32(SEGLIN(RT_SEL_DS, 0x53cc), MEMR32(SEGLIN(RT_SEL_DS, 0x53cc)) + 1);
    rt_callf(0x1018387eu, 0, NULL); /* collect visible vectors per column */
    rt_callf(0x101061c2u, 0, NULL); /* split into wall segments */
    {
        uint16_t a[4] = {ds16(0x53d4), ds16(0x53d8), ds16(0x53d0), ds16(0x53d2)};
        rt_callf(0x10103d14u, 4, a); /* floor + ceiling */
    }
    rt_callf(0x1010660cu, 0, NULL); /* textured walls */
    rt_callf(0x101062a4u, 0, NULL); /* queue visible objects */
    rt_callf(0x10106870u, 0, NULL); /* draw queued sprites */
    hd_view_drawn();
    retf();
}

/* 1010:b9a6: everything drawn over the 3D view each frame, through
 * FUN_1010_a308 (HUD element painter): 0x12 frame decorations + current
 * message, 2 the weapon, 0x18 the crosshair/indicator. Each is captured as
 * an overlay layer for the hi-res compositor. */
void f_1010_b9a6(void) {
    RT_ENTER(0x1010b9a6u);
    static const uint16_t parts[3] = {0x12, 2, 0x18};
    for (int i = 0; i < 3; i++) {
        hd_layer_begin(HD_ANCHOR_CENTER);
        rt_callf(0x1010a308u, 1, &parts[i]);
    }
    hd_overlays_done();
    retf();
}

/* 1018:035a draw_view_picture(x, y, picture, flags): the gargoyles in the
 * view corners are drawn with it; anchor them to the edges of a widened view. */
void f_1018_035a(void) {
    int prev = hd_layer_current();
    if (prev) {
        int x = (int16_t)MEMR16(SEGLIN(rSS, (uint16_t)(rSP + 4)));
        hd_layer_begin(x < 100 ? HD_ANCHOR_LEFT : x > 220 ? HD_ANCHOR_RIGHT : HD_ANCHOR_CENTER);
    }
    f_1018_035a_orig(); /* pops the return address itself */
    if (prev) hd_layer_begin(prev);
}

/* 1010:9758 player_tick: the original reads the arrow/Ctrl/Space/Alt bits in
 * DS:3752/3753 (Alt+Left/Right strafes) and moves through FUN_1010_8a08
 * (angle, sideways, speed: collision, doors and pickups included). Modern
 * controls are added afterwards through the same calls: W/S forward/back,
 * A/D strafe, mouse X turns via FUN_1010_e40a (set view angle). Skipped while
 * a demo is recorded or played (DS:46a8 1..4), which only stores DS:3752. */
static void move_player(int angle, int sideways, int speed) {
    uint16_t a[3] = {(uint16_t)(((angle % 360) + 360) % 360), (uint16_t)sideways, (uint16_t)speed};
    rt_callf(0x10108a08u, 3, a);
}

void f_1010_9758(void) {
    int playing = MEMR8(SEGLIN(RT_SEL_DS, 0x46a4)) == 1;
    f_1010_9758_orig(); /* pops the return address itself */
    int demo = (int16_t)ds16(0x46a8);
    if (!playing || MEMR8(SEGLIN(RT_SEL_DS, 0x46a4)) != 1 || (demo >= 1 && demo <= 4)) return;
    int fwd = 0, side = 0, turn = 0;
    if (!host_modern_input(&fwd, &side, &turn)) return;
    uint8_t keys = MEMR8(SEGLIN(RT_SEL_DS, 0x3752));
    int speed = (int16_t)ds16(0x53e6);
    if (keys & 0x20) speed *= 2; /* right shift: run */
    if (keys & 0x40) speed = 1;  /* left shift: fine control */
    if (turn) {
        uint16_t a[1] = {(uint16_t)(int16_t)((int16_t)ds16(0x4bda) + turn)};
        rt_callf(0x1010e40au, 1, a);
    }
    int angle = (int16_t)ds16(0x4bda);
    if (fwd > 0 && !(keys & 2)) move_player(angle, 0, speed);
    if (fwd < 0 && !(keys & 4)) move_player(angle + 180, 0, speed);
    if (side < 0) move_player(angle + 270, 1, speed);
    if (side > 0) move_player(angle + 90, 1, speed);
}

/* 1010:cbce queue_object_sprite(object far*): projects one object, queues it
 * for drawing and advances its animation. The hi-res renderer animates the
 * objects that only its wider view shows the same way (hd_animate_object). */
void f_1010_cbce(void) {
    uint32_t obj = MEMR32(SEGLIN(rSS, (uint16_t)(rSP + 4)));
    uint32_t lin = SEGLIN(obj >> 16, obj & 0xffff);
    uint8_t frame = MEMR8(lin + 3); /* the queued slot keeps the frame from before the animation step */
    f_1010_cbce_orig();
    for (int i = 0; i < 100; i++) { /* sprite slots DS:6260, 0x12 bytes: [0]=0 queued, [2] object */
        uint16_t slot = (uint16_t)(0x6260 + i * 0x12);
        if (MEMR8(SEGLIN(RT_SEL_DS, slot)) == 0 && MEMR32(SEGLIN(RT_SEL_DS, (uint16_t)(slot + 2))) == obj) {
            hd_object_queued(lin, frame);
            break;
        }
    }
}

void hd_animate_object(uint32_t obj) {
    uint8_t type = MEMR8(obj + 6), set = MEMR8(obj + 4);
    if (type == 5) return;
    uint16_t a[3] = {(uint16_t)obj, (uint16_t)(obj >> 16), (uint16_t)(0x4734 + set * 8)};
    if (type == 0x2c || type == 0x2d) rt_callf(0x1010ca78u, 3, a); /* 8-direction figure */
    else if (ds16((uint16_t)(0x4736 + set * 8))) rt_callf(0x1010cb26u, 3, a);
}
