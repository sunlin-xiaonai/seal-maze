// seal-maze host tests: the levels are sound and the ball plays fair (no device, no ESP-IDF).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../main/game.h"
#include "../main/maze.h"

static int s_fail;
#define CHECK(cond, ...)                                       \
    do {                                                       \
        if (!(cond)) {                                         \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);        \
            printf(__VA_ARGS__);                               \
            printf("\n");                                      \
            s_fail++;                                          \
        }                                                      \
    } while (0)

#define MAXC 64

/** Breadth-first distances over path cells from (r, c); -1 where it cannot go. */
static void bfs(const maze_t *m, int r0, int c0, int dist[MAXC][MAXC]) {
    for (int r = 0; r < MAXC; r++)
        for (int c = 0; c < MAXC; c++) dist[r][c] = -1;
    int qr[MAXC * MAXC], qc[MAXC * MAXC], head = 0, tail = 0;
    dist[r0][c0] = 0;
    qr[tail] = r0, qc[tail++] = c0;
    static const int dr[] = {-1, 1, 0, 0}, dc[] = {0, 0, -1, 1};
    while (head < tail) {
        const int r = qr[head], c = qc[head++];
        for (int k = 0; k < 4; k++) {
            const int nr = r + dr[k], nc = c + dc[k];
            if (maze_wall(m, nr, nc) || dist[nr][nc] >= 0) continue;
            dist[nr][nc] = dist[r][c] + 1;
            qr[tail] = nr, qc[tail++] = nc;
        }
    }
}

static void test_levels(void) {
    for (int i = 0; i < MAZE_COUNT; i++) {
        const maze_t *m = &MAZES[i];
        CHECK(m->w <= MAXC && m->h <= MAXC, "level %d too big", i);
        for (int r = 0; r < m->h; r++) CHECK((int)strlen(m->rows[r]) == m->w, "level %d row %d width", i, r);
        CHECK(!maze_wall(m, m->start_r, m->start_c), "level %d start on a wall", i);
        CHECK(!maze_wall(m, m->exit_r, m->exit_c), "level %d exit on a wall", i);
        CHECK(m->exit_r == 0 || m->exit_c == 0 || m->exit_r == m->h - 1 || m->exit_c == m->w - 1,
              "level %d exit not on the edge", i);
        CHECK(m->n_seals > 0 && m->n_seals <= MAZE_MAX_SEALS, "level %d seals", i);
        int dist[MAXC][MAXC];
        bfs(m, m->start_r, m->start_c, dist);
        CHECK(dist[m->exit_r][m->exit_c] > 0, "level %d exit unreachable", i);
        for (int s = 0; s < m->n_seals; s++) {
            const int r = (int)m->seals[s].y, c = (int)m->seals[s].x;
            CHECK(!maze_wall(m, r, c), "level %d seal %d in a wall", i, s);
            CHECK(dist[r][c] > 0, "level %d seal %d unreachable", i, s);
        }
        char json[2048];
        const int n = maze_json(m, false, json, sizeof(json));
        CHECK(n > 0 && n < (int)sizeof(json), "level %d json length %d", i, n);
        CHECK(strstr(json, m->rows[1]) != NULL, "level %d json rows", i);
        // Truncation is reported like snprintf and stays terminated.
        char small[16];
        CHECK(maze_json(m, false, small, sizeof(small)) == n && strlen(small) == sizeof(small) - 1, "level %d json cut", i);
    }
}

/** How deep the ball is in any wall (0 when it is clear of them). */
static float wall_overlap(const game_t *g) {
    float worst = 0;
    const int r0 = (int)floorf(g->y), c0 = (int)floorf(g->x);
    for (int r = r0 - 1; r <= r0 + 1; r++)
        for (int c = c0 - 1; c <= c0 + 1; c++) {
            if (!maze_wall(g->m, r, c) || r < 0 || c < 0 || r >= g->m->h || c >= g->m->w) continue;
            const float px = fminf(fmaxf(g->x, c), c + 1), py = fminf(fmaxf(g->y, r), r + 1);
            const float d = sqrtf((g->x - px) * (g->x - px) + (g->y - py) * (g->y - py));
            if (GAME_BALL_R - d > worst) worst = GAME_BALL_R - d;
        }
    return worst;
}

static void test_never_in_walls(void) {
    game_t g;
    game_reset(&g, &MAZES[0]);
    srand(7);
    float tx = 0, ty = 0, worst = 0;
    for (int i = 0; i < 10 * 60 * 200 && !g.won; i++) {  // ten minutes of a restless hand, 5 ms steps
        if (i % 40 == 0) tx = (rand() / (float)RAND_MAX - 0.5f) * 1.6f, ty = (rand() / (float)RAND_MAX - 0.5f) * 1.6f;
        game_step(&g, tx, ty, 0.005f);
        const float o = wall_overlap(&g);
        if (o > worst) worst = o;
    }
    CHECK(worst < 0.02f, "ball went %.3f cells into a wall", worst);
    // A long stall is capped: one frame never throws the ball across the maze.
    game_reset(&g, &MAZES[0]);
    game_step(&g, 1, 0, 5.0f);
    CHECK(g.x < MAZES[0].start_c + 0.5f + 1.0f, "a 5 s frame moved the ball to %.2f", g.x);
}

static void test_exit_locked(void) {
    const maze_t *m = &MAZES[0];
    game_t g;
    game_reset(&g, m);
    g.x = m->exit_c + 0.5f, g.y = m->exit_r + 0.5f;
    unsigned all = 0;
    for (int i = 0; i < 600; i++) all |= game_step(&g, -0.5f, 0, 0.005f);
    CHECK(!g.won, "left by a locked exit");
    CHECK(all & GAME_EV_LOCKED, "no locked event at the closed exit");
    g.got = (1u << m->n_seals) - 1;
    all = 0;
    for (int i = 0; i < 600 && !g.won; i++) all |= game_step(&g, -0.5f, 0, 0.005f);
    CHECK(g.won && (all & GAME_EV_WIN), "did not leave by the open exit");
    const uint32_t ms = g.ms;
    game_step(&g, -0.5f, 0, 0.005f);
    CHECK(g.ms == ms, "the clock ran after the win");
}

/** Cells from (r, c) to (tr, tc) along the shortest path, into path[]; returns how many. */
static int route(const maze_t *m, int r, int c, int tr, int tc, int path[][2], int max) {
    int dist[MAXC][MAXC];
    bfs(m, tr, tc, dist);  // from the target: walk downhill from (r, c)
    int n = 0;
    static const int dr[] = {-1, 1, 0, 0}, dc[] = {0, 0, -1, 1};
    while ((r != tr || c != tc) && n < max) {
        for (int k = 0; k < 4; k++) {
            const int nr = r + dr[k], nc = c + dc[k];
            if (!maze_wall(m, nr, nc) && dist[nr][nc] == dist[r][c] - 1) {
                r = nr, c = nc;
                break;
            }
        }
        path[n][0] = r, path[n][1] = c;
        n++;
    }
    return n;
}

/** A player steering along the shortest route through every seal and out: the level can be won by tilting. */
static void test_autopilot_wins(void) {
    const maze_t *m = &MAZES[0];
    static int path[4096][2];
    int n = 0, r = m->start_r, c = m->start_c;
    uint32_t left = (1u << m->n_seals) - 1;
    while (left) {  // nearest seal next
        int dist[MAXC][MAXC], best = -1;
        bfs(m, r, c, dist);
        for (int s = 0; s < m->n_seals; s++)
            if ((left & (1u << s)) &&
                (best < 0 || dist[(int)m->seals[s].y][(int)m->seals[s].x] < dist[(int)m->seals[best].y][(int)m->seals[best].x]))
                best = s;
        n += route(m, r, c, (int)m->seals[best].y, (int)m->seals[best].x, path + n, 4096 - n);
        r = (int)m->seals[best].y, c = (int)m->seals[best].x;
        left &= ~(1u << best);
    }
    n += route(m, r, c, m->exit_r, m->exit_c, path + n, 4096 - n);
    int out_r, out_c;
    maze_exit_outside(m, &out_r, &out_c);
    path[n][0] = out_r, path[n][1] = out_c;
    n++;

    game_t g;
    game_reset(&g, m);
    int at = 0, hits = 0;
    for (int i = 0; i < 600 * 200 && !g.won; i++) {
        const float tx = path[at][1] + 0.5f - g.x, ty = path[at][0] + 0.5f - g.y;
        if (at < n - 1 && tx * tx + ty * ty < 0.35f * 0.35f) at++;
        float ax = 0.8f * tx - 0.35f * g.vx, ay = 0.8f * ty - 0.35f * g.vy;
        ax = fminf(fmaxf(ax, -0.35f), 0.35f), ay = fminf(fmaxf(ay, -0.35f), 0.35f);
        if (game_step(&g, ax, ay, 0.005f) & GAME_EV_WALL) hits++;
    }
    CHECK(g.won, "autopilot did not win (at %d of %d, %d seals)", at, n, game_seals_got(&g));
    CHECK(game_seals_got(&g) == m->n_seals, "won without every seal");
    printf("autopilot: %d cells, won in %.1f s, %d rock bumps, %d wall hits\n", n, g.ms / 1000.0, g.bumps, hits);
}

int ring_tests(void);

int main(void) {
    s_fail += ring_tests();
    test_levels();
    test_never_in_walls();
    test_exit_locked();
    test_autopilot_wins();
    if (s_fail) {
        printf("seal-maze host tests: %d failed\n", s_fail);
        return 1;
    }
    printf("seal-maze host tests passed\n");
    return 0;
}
