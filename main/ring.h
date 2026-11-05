// The round maze for playing alone on the round screen: rings of corridors filling the glass, the seals' home in
// the middle. The whole maze is on the screen at once (nothing scrolls); the ball starts on the outer ring, collects
// the five seals and rolls into the home, which opens once they are all in.
//
// Geometry in screen pixels from the centre (x right, y down); angles clockwise from 12 o'clock. Ring i runs from
// radius band[i].r0 to band[i].r1 in band[i].n sectors; a ring has as many sectors as the one inside it or twice as
// many, so every gap is wide enough for the ball. The maze is carved by a seeded depth-first search (the same maze
// every time for a seed). Walls become short straight segments (arcs as chords); the ball collides with them as
// capsules. Events are game.h's GAME_EV_* (no rocks here). Plain C: host tested, drawn by view.c.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "game.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RING_MAX 8
#define RING_SECT_MAX 64
#define RING_SEALS 5
#define RING_SEGS_MAX 1600
#define RING_BIN 32                     // px, the side of a collision bin
#define RING_BINS (512 / RING_BIN)      // bins a side, over -256..256
#define RING_BIN_SEGS 48                // segments a bin holds
#define RING_WALL_R 2.5f                // half a wall's thickness, px
#define RING_BALL_R 8.0f                // px

typedef struct {
    float r0, r1;
    int n;
} ring_band_t;

typedef struct {
    int rings;
    ring_band_t band[RING_MAX];
    float hub_r, outer_r;
    // Walls of each cell: its inner arc (to the ring inside, or the hub for ring 0) and its clockwise radial side.
    uint8_t in_wall[RING_MAX][RING_SECT_MAX], cw_wall[RING_MAX][RING_SECT_MAX];
    int gate;  // the ring-0 sector whose inner arc is the home's door (closed until every seal is in)
    int start_ring, start_sector;
    int seal_ring[RING_SEALS], seal_sector[RING_SEALS];
    float seal_x[RING_SEALS], seal_y[RING_SEALS];
    // Collision geometry: segments (x0, y0, x1, y1), the door's flagged; bins of segment indices.
    int nseg;
    float seg[RING_SEGS_MAX][4];
    uint8_t seg_door[RING_SEGS_MAX];
    uint16_t bin[RING_BINS][RING_BINS][RING_BIN_SEGS];
    uint8_t bin_n[RING_BINS][RING_BINS];
} ring_maze_t;

/** Builds the maze for `seed` (about 60 KB: give it a static or PSRAM home). */
void ring_maze_build(ring_maze_t *m, uint32_t seed);
/** A cell's centre, px. */
void ring_cell_center(const ring_maze_t *m, int ring, int sector, float *x, float *y);
/** The cells open from (ring, sector), into out[k] = {ring, sector}; returns how many. */
int ring_links(const ring_maze_t *m, int ring, int sector, int out[8][2]);
/** How far (px) the point is from the nearest wall's centre line; `door_open`: the door is no wall. */
float ring_wall_dist(const ring_maze_t *m, float x, float y, bool door_open);

typedef struct {
    const ring_maze_t *m;
    float x, y, vx, vy;  // px, px/s
    uint32_t got;
    int last_seal;
    int bumps;  // hard knocks on walls (the rocks' count in the square maze)
    uint32_t ms;
    bool won;
} ring_game_t;

void ring_reset(ring_game_t *g, const ring_maze_t *m);
/** Steps dt seconds with this tilt (g, screen axes); GAME_EV_* (WALL, SEAL, LOCKED, WIN). */
unsigned ring_step(ring_game_t *g, float tilt_x, float tilt_y, float dt);
int ring_got(const ring_game_t *g);
bool ring_open(const ring_game_t *g);

#ifdef __cplusplus
}
#endif
