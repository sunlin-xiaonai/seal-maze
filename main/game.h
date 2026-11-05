// The game: a ball rolling on a level as the board tilts. Tilt is gravity in the screen's plane (g, screen
// coordinates: x right, y down) less where the player held it at the start. The ball speeds up downhill, slows on
// its own, bounces off walls and rocks, collects the seals it touches and leaves by the exit once it has all of
// them. Plain C, no time of its own: the caller steps it. One copy of the rules for both ways of playing (the round
// screen draws it, the big screen is sent where the ball is).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "maze.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GAME_BALL_R 0.30f    // cells
#define GAME_ACCEL 26.0f     // cells/s² for 1 g of tilt
#define GAME_DEADZONE 0.02f  // g: a hand never holds perfectly still
#define GAME_DRAG 1.2f       // 1/s: the ball slows by itself
#define GAME_MAX_SPEED 9.0f  // cells/s
#define GAME_WALL_BOUNCE 0.35f
#define GAME_ROCK_BOUNCE 0.6f

/** What happened in a step (a bit set). */
enum {
    GAME_EV_WALL = 1 << 0,    // hit a wall hard enough to hear
    GAME_EV_ROCK = 1 << 1,    // hit a rock (counted in `bumps`; `last_rock`)
    GAME_EV_SEAL = 1 << 2,    // collected a seal (`last_seal`)
    GAME_EV_LOCKED = 1 << 3,  // pressed against the exit while seals are missing
    GAME_EV_WIN = 1 << 4,     // out of the exit
};

typedef struct {
    const maze_t *m;
    float x, y;    // the ball's centre, cells
    float vx, vy;  // cells/s
    uint32_t got;  // a bit per seal collected
    int last_seal;
    int last_rock;  // the rock of the latest GAME_EV_ROCK
    int bumps;
    uint32_t ms;  // time played
    bool won;
    float rock_cool[MAZE_MAX_ROCKS];  // s: a rock counts once per hit, not once per step while touching
} game_t;

void game_reset(game_t *g, const maze_t *m);
/** Steps `dt` seconds with this tilt; returns what happened (GAME_EV_*). Nothing moves once won. */
unsigned game_step(game_t *g, float tilt_x, float tilt_y, float dt);
int game_seals_got(const game_t *g);
bool game_exit_open(const game_t *g);

#ifdef __cplusplus
}
#endif
