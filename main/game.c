#include "game.h"

#include <math.h>
#include <string.h>

#define WALL_HEARD 1.5f  // cells/s into a wall: slower than this is a roll along it, not a hit
#define ROCK_HEARD 0.8f
#define ROCK_COOL_S 0.4f
#define MAX_MOVE 0.1f  // cells per sub-step: the ball never jumps past a wall's corner

void game_reset(game_t *g, const maze_t *m) {
    memset(g, 0, sizeof(*g));
    g->m = m;
    g->x = m->start_c + 0.5f;
    g->y = m->start_r + 0.5f;
    g->last_seal = -1;
    g->last_rock = -1;
}

int game_seals_got(const game_t *g) {
    int n = 0;
    for (uint32_t b = g->got; b; b &= b - 1) n++;
    return n;
}

bool game_exit_open(const game_t *g) { return game_seals_got(g) == g->m->n_seals; }

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/** Pushes the ball out of a disc or box it overlaps and bounces it off; returns the speed it came in at (0: none). */
static float push_out(game_t *g, float px, float py, float reach, float bounce) {
    float dx = g->x - px, dy = g->y - py;
    float d = sqrtf(dx * dx + dy * dy);
    if (d >= reach) return 0;
    if (d < 1e-5f) dx = 0, dy = -1, d = 1;  // centre exactly on it: out upwards
    const float nx = dx / d, ny = dy / d;
    g->x = px + nx * reach;
    g->y = py + ny * reach;
    const float vn = g->vx * nx + g->vy * ny;
    if (vn >= 0) return 0;
    g->vx -= (1 + bounce) * vn * nx;
    g->vy -= (1 + bounce) * vn * ny;
    return -vn;
}

/** Beyond the exit, in line with it and outside the grid: open once every seal is collected. */
static bool past_exit(const maze_t *m, int r, int c, int out_r, int out_c) {
    if (out_c < 0) return c < 0 && r == out_r;
    if (out_c >= m->w) return c >= m->w && r == out_r;
    if (out_r < 0) return r < 0 && c == out_c;
    return r >= m->h && c == out_c;
}

static unsigned collide_walls(game_t *g) {
    unsigned ev = 0;
    int out_r, out_c;
    maze_exit_outside(g->m, &out_r, &out_c);
    const bool open = game_exit_open(g);
    const int r0 = (int)floorf(g->y), c0 = (int)floorf(g->x);
    for (int r = r0 - 1; r <= r0 + 1; r++) {
        for (int c = c0 - 1; c <= c0 + 1; c++) {
            const bool exit_cell = r == out_r && c == out_c;
            if (!maze_wall(g->m, r, c)) continue;
            if (open && past_exit(g->m, r, c, out_r, out_c)) continue;
            const float px = clampf(g->x, (float)c, (float)c + 1), py = clampf(g->y, (float)r, (float)r + 1);
            const float hit = push_out(g, px, py, GAME_BALL_R, GAME_WALL_BOUNCE);
            if (hit > 0 && exit_cell) ev |= GAME_EV_LOCKED;
            else if (hit > WALL_HEARD) ev |= GAME_EV_WALL;
        }
    }
    return ev;
}

static unsigned collide_things(game_t *g) {
    unsigned ev = 0;
    for (int i = 0; i < g->m->n_rocks; i++) {
        const float hit = push_out(g, g->m->rocks[i].x, g->m->rocks[i].y, GAME_BALL_R + MAZE_ROCK_R, GAME_ROCK_BOUNCE);
        if (hit > ROCK_HEARD && g->rock_cool[i] <= 0) {
            g->rock_cool[i] = ROCK_COOL_S;
            g->bumps++;
            g->last_rock = i;
            ev |= GAME_EV_ROCK;
        }
    }
    for (int i = 0; i < g->m->n_seals; i++) {
        if (g->got & (1u << i)) continue;
        const float dx = g->x - g->m->seals[i].x, dy = g->y - g->m->seals[i].y;
        const float reach = GAME_BALL_R + MAZE_SEAL_R;
        if (dx * dx + dy * dy < reach * reach) {
            g->got |= 1u << i;
            g->last_seal = i;
            ev |= GAME_EV_SEAL;
        }
    }
    return ev;
}

static bool outside(const game_t *g) {
    const maze_t *m = g->m;
    return g->x < -GAME_BALL_R || g->y < -GAME_BALL_R || g->x > m->w + GAME_BALL_R || g->y > m->h + GAME_BALL_R;
}

unsigned game_step(game_t *g, float tilt_x, float tilt_y, float dt) {
    if (g->won || dt <= 0) return 0;
    if (dt > 0.1f) dt = 0.1f;  // a stall (a busy frame) is not a launch
    g->ms += (uint32_t)(dt * 1000 + 0.5f);
    for (int i = 0; i < g->m->n_rocks; i++) g->rock_cool[i] -= dt;

    const float t = sqrtf(tilt_x * tilt_x + tilt_y * tilt_y);
    if (t > GAME_DEADZONE) {
        const float k = (t - GAME_DEADZONE) / t * GAME_ACCEL;
        g->vx += tilt_x * k * dt;
        g->vy += tilt_y * k * dt;
    }
    const float keep = expf(-GAME_DRAG * dt);
    g->vx *= keep;
    g->vy *= keep;
    const float v = sqrtf(g->vx * g->vx + g->vy * g->vy);
    if (v > GAME_MAX_SPEED) g->vx *= GAME_MAX_SPEED / v, g->vy *= GAME_MAX_SPEED / v;

    unsigned ev = 0;
    const float move = sqrtf(g->vx * g->vx + g->vy * g->vy) * dt;
    int steps = (int)ceilf(move / MAX_MOVE);
    if (steps < 1) steps = 1;
    const float h = dt / steps;
    for (int s = 0; s < steps; s++) {
        g->x += g->vx * h;
        g->y += g->vy * h;
        ev |= collide_walls(g);
        ev |= collide_things(g);
        if (outside(g)) {
            g->won = true;
            return ev | GAME_EV_WIN;
        }
    }
    return ev;
}
