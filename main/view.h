// The round screen while playing alone: the whole round maze (ring.h) fills the glass and holds still; only the
// ball, the seals and the effects move. Drawn by hand into a 466 x 466 frame (big-endian RGB565, the panel's order)
// and sent straight to the panel (board_direct.h): LVGL repaints a whole screen in about 70 ms, this is about 20.
// The walls are drawn once into a background; a frame is that background plus what moves.
//
// The home in the middle shows the time; its door is a coral arc until every seal is in, then the home glows.
// The rim is five segments, one per seal, filling up as they are collected. Every touch of the world shows: the
// maze shakes at a hard knock, a collected seal bursts into sparks, the closed door throbs and the missing
// segments flash, the last seal makes the rim pulse, the win throws sparks all round.
// Plain C: the host tests draw frames to look at.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ring.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VIEW_SIZE 466
#define VIEW_SPARKS 64
#define VIEW_TRAIL 6

typedef struct {
    float x, y, vx, vy;  // px from the centre, px/s
    float life, max;     // s
    uint32_t color;      // 0xRRGGBB
} view_spark_t;

typedef struct {
    uint16_t *fb;  // the frame being drawn; the caller may swap it between frames (all start black)
    uint16_t *bg;  // the walls, drawn once (view_build)
    uint32_t *rim;  // the rim band's pixels: index << 12 | angle in tenths of a degree (clockwise from the top)
    int rim_n;
    float t;      // s, the view's own clock (effects)
    float shake;  // px, dying away
    uint32_t seed;
    float flash_until, open_until;  // the door throbs and the missing segments flash; the rim pulses (open)
    view_spark_t sparks[VIEW_SPARKS];
    float trail[VIEW_TRAIL][2];
    int trail_at;
} view_t;

/** `rim` must hold view_rim_size() entries; fb and bg VIEW_SIZE² pixels each. */
int view_rim_size(void);
void view_init(view_t *v, uint16_t *fb, uint16_t *bg, uint32_t *rim);
/** Draws the maze's walls into the background (a few hundred ms on the device: once per maze). */
void view_build(view_t *v, const ring_maze_t *m);
/** Forgets the effects (a new game). */
void view_jump(view_t *v, const ring_game_t *g);
/** What just happened (GAME_EV_* bits from ring_step): the effects for it. */
void view_event(view_t *v, const ring_game_t *g, unsigned ev);
/** Moves the effects on by dt seconds and draws the frame. */
void view_render(view_t *v, const ring_game_t *g, float dt);

#ifdef __cplusplus
}
#endif
