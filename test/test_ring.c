// The round maze (ring.c): every cell reachable, the seals placed, the ball never in a wall, and a player steering
// through every seal into the home wins.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../main/ring.h"

int ring_tests(void);

static int s_fail;
#define CHECK(cond, ...)                                \
    do {                                                \
        if (!(cond)) {                                  \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
            s_fail++;                                   \
        }                                               \
    } while (0)

static ring_maze_t s_m;

static void dist_from(const ring_maze_t *m, int r0, int s0, int dist[RING_MAX][RING_SECT_MAX], int prev[RING_MAX][RING_SECT_MAX][2]) {
    for (int i = 0; i < RING_MAX; i++)
        for (int s = 0; s < RING_SECT_MAX; s++) dist[i][s] = -1;
    static int q[RING_MAX * RING_SECT_MAX][2];
    int head = 0, tail = 0;
    dist[r0][s0] = 0;
    q[tail][0] = r0, q[tail++][1] = s0;
    while (head < tail) {
        const int i = q[head][0], s = q[head++][1];
        int nb[8][2];
        const int n = ring_links(m, i, s, nb);
        for (int k = 0; k < n; k++) {
            if (dist[nb[k][0]][nb[k][1]] >= 0) continue;
            dist[nb[k][0]][nb[k][1]] = dist[i][s] + 1;
            prev[nb[k][0]][nb[k][1]][0] = i, prev[nb[k][0]][nb[k][1]][1] = s;
            q[tail][0] = nb[k][0], q[tail++][1] = nb[k][1];
        }
    }
}

static void test_build(void) {
    const ring_maze_t *m = &s_m;
    int dist[RING_MAX][RING_SECT_MAX], prev[RING_MAX][RING_SECT_MAX][2];
    dist_from(m, m->start_ring, m->start_sector, dist, prev);
    int cells = 0;
    for (int i = 0; i < m->rings; i++)
        for (int s = 0; s < m->band[i].n; s++) {
            CHECK(dist[i][s] >= 0, "cell %d/%d unreachable", i, s);
            cells++;
        }
    for (int k = 0; k < RING_SEALS; k++) CHECK(m->seal_ring[k] > 0 && dist[m->seal_ring[k]][m->seal_sector[k]] > 0, "seal %d", k);
    int worst = 0;
    for (int y = 0; y < RING_BINS; y++)
        for (int x = 0; x < RING_BINS; x++) worst = m->bin_n[y][x] > worst ? m->bin_n[y][x] : worst;
    CHECK(worst < RING_BIN_SEGS, "a collision bin is full (%d): walls would be missed", worst);
    CHECK(m->nseg < RING_SEGS_MAX, "segments full");
    printf("ring maze: %d rings, %d cells, %d segments, fullest bin %d, home door at sector %d\n", m->rings, cells, m->nseg, worst, m->gate);
}

static void test_never_in_walls(void) {
    ring_game_t g;
    ring_reset(&g, &s_m);
    srand(3);
    float tx = 0, ty = 0, worst = 0;
    for (int i = 0; i < 10 * 60 * 200 && !g.won; i++) {
        if (i % 40 == 0) tx = (rand() / (float)RAND_MAX - 0.5f) * 1.6f, ty = (rand() / (float)RAND_MAX - 0.5f) * 1.6f;
        ring_step(&g, tx, ty, 0.005f);
        const float d = ring_wall_dist(&s_m, g.x, g.y, ring_open(&g));
        const float in = RING_BALL_R + RING_WALL_R - d;
        if (in > worst) worst = in;
        CHECK(sqrtf(g.x * g.x + g.y * g.y) < s_m.outer_r, "ball left the maze at %.1f %.1f", g.x, g.y);
        if (s_fail) return;
    }
    CHECK(worst < 0.5f, "ball went %.2f px into a wall", worst);
}

static void test_door(void) {
    ring_game_t g;
    ring_reset(&g, &s_m);
    ring_cell_center(&s_m, 0, s_m.gate, &g.x, &g.y);
    unsigned all = 0;
    const float l = sqrtf(g.x * g.x + g.y * g.y);
    for (int i = 0; i < 600; i++) all |= ring_step(&g, -g.x / l * 0.5f, -g.y / l * 0.5f, 0.005f);
    CHECK(!g.won && (all & GAME_EV_LOCKED), "the closed door: won=%d locked=%d", g.won, !!(all & GAME_EV_LOCKED));
    g.got = (1u << RING_SEALS) - 1;
    for (int i = 0; i < 600 && !g.won; i++) ring_step(&g, -g.x / l * 0.5f, -g.y / l * 0.5f, 0.005f);
    CHECK(g.won, "did not get into the open home");
}

/** Waypoints from one cell to the next: along the ring to the next cell's angle, then across. */
static int hop(const ring_maze_t *m, int i, int s, int j, int t, float out[3][2]) {
    float x, y;
    if (i == j) {
        ring_cell_center(m, j, t, &out[0][0], &out[0][1]);
        return 1;
    }
    const float a = 6.28318531f * (t + 0.5f) / m->band[j].n;  // outward: the child's angle, inside the parent
    const float b = 6.28318531f * (s + 0.5f) / m->band[i].n;  // inward: our angle, inside the parent
    const float ang = j > i ? a : b, ri = (m->band[i].r0 + m->band[i].r1) / 2, rj = (m->band[j].r0 + m->band[j].r1) / 2;
    x = ri * sinf(ang), y = -ri * cosf(ang);
    out[0][0] = x, out[0][1] = y;
    out[1][0] = rj * sinf(ang), out[1][1] = -rj * cosf(ang);
    ring_cell_center(m, j, t, &out[2][0], &out[2][1]);
    return 3;
}

static void test_autopilot(void) {
    const ring_maze_t *m = &s_m;
    static float way[4096][2];
    int n = 0, ci = m->start_ring, cs = m->start_sector;
    uint32_t left = (1u << RING_SEALS) - 1;
    int dist[RING_MAX][RING_SECT_MAX], prev[RING_MAX][RING_SECT_MAX][2];
    for (int leg = 0; leg <= RING_SEALS; leg++) {
        int ti, ts;
        dist_from(m, ci, cs, dist, prev);
        if (left) {
            int best = -1;
            for (int k = 0; k < RING_SEALS; k++)
                if ((left & (1u << k)) && (best < 0 || dist[m->seal_ring[k]][m->seal_sector[k]] < dist[m->seal_ring[best]][m->seal_sector[best]])) best = k;
            ti = m->seal_ring[best], ts = m->seal_sector[best];
            left &= ~(1u << best);
        } else {
            ti = 0, ts = m->gate;
        }
        int path[512][2], len = 0;
        for (int i = ti, s = ts; i != ci || s != cs;) {
            path[len][0] = i, path[len++][1] = s;
            const int pi = prev[i][s][0], ps = prev[i][s][1];
            i = pi, s = ps;
        }
        for (int k = len - 1; k >= 0; k--) {
            float w[3][2];
            const int h = hop(m, ci, cs, path[k][0], path[k][1], w);
            for (int q = 0; q < h; q++) way[n][0] = w[q][0], way[n++][1] = w[q][1];
            ci = path[k][0], cs = path[k][1];
        }
    }
    way[n][0] = 0, way[n++][1] = 0;  // into the home
    ring_game_t g;
    ring_reset(&g, m);
    int at = 0;
    for (int i = 0; i < 600 * 200 && !g.won; i++) {
        const float dx = way[at][0] - g.x, dy = way[at][1] - g.y;
        if (at < n - 1 && dx * dx + dy * dy < 6 * 6) at++;
        float ax = 0.02f * dx - 0.012f * g.vx, ay = 0.02f * dy - 0.012f * g.vy;
        ax = fminf(fmaxf(ax, -0.35f), 0.35f), ay = fminf(fmaxf(ay, -0.35f), 0.35f);
        ring_step(&g, ax, ay, 0.005f);
    }
    CHECK(g.won, "autopilot stuck at waypoint %d of %d (%.0f, %.0f), %d seals", at, n, g.x, g.y, ring_got(&g));
    printf("ring autopilot: %d waypoints, home in %.1f s, %d knocks\n", n, g.ms / 1000.0, g.bumps);
}

int ring_tests(void) {
    ring_maze_build(&s_m, 7);
    test_build();
    test_never_in_walls();
    test_door();
    test_autopilot();
    return s_fail;
}
