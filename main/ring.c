#include "ring.h"

#include <math.h>
#include <string.h>

#define TAU 6.28318531f
#define HUB_R 42.0f
#define BAND_W 28.0f
#define RINGS 6
#define MIN_ARC 30.0f    // px: the narrowest sector at a ring's inner edge (the ball must get through its gap)
#define CHORD 6.0f       // px: arcs become chords this long
#define ACCEL 650.0f     // px/s² for 1 g of tilt
#define DRAG 1.2f        // 1/s
#define MAX_SPEED 320.0f
#define MAX_MOVE 2.5f    // px a sub-step
#define BOUNCE 0.35f
#define HEARD 45.0f      // px/s into a wall to hear it
#define BUMP 150.0f      // px/s: a knock worth counting
#define SEAL_REACH 7.0f  // px past the ball's edge

static uint32_t s_rng;
static uint32_t rnd(void) {
    s_rng = s_rng * 1664525u + 1013904223u;
    return s_rng >> 8;
}

static float sect_angle(const ring_maze_t *m, int i, int s) { return TAU * s / m->band[i].n; }

static void polar(float r, float a, float *x, float *y) {
    *x = r * sinf(a);
    *y = -r * cosf(a);
}

void ring_cell_center(const ring_maze_t *m, int i, int s, float *x, float *y) {
    polar((m->band[i].r0 + m->band[i].r1) / 2, TAU * (s + 0.5f) / m->band[i].n, x, y);
}

// ---------------------------------------------------------------- carving

typedef struct {
    int ring, sector;
    uint8_t *wall;  // the wall between this cell and the one asked about
} nb_t;

/** The neighbours of (i, s) and the wall to each; returns how many. */
static int neighbours(ring_maze_t *m, int i, int s, nb_t out[8]) {
    int n = 0;
    const int ns = m->band[i].n;
    out[n++] = (nb_t){i, (s + 1) % ns, &m->cw_wall[i][s]};
    out[n++] = (nb_t){i, (s + ns - 1) % ns, &m->cw_wall[i][(s + ns - 1) % ns]};
    if (i > 0) out[n++] = (nb_t){i - 1, s / (ns / m->band[i - 1].n), &m->in_wall[i][s]};
    if (i < m->rings - 1) {
        const int k = m->band[i + 1].n / ns;
        for (int j = 0; j < k; j++) out[n++] = (nb_t){i + 1, s * k + j, &m->in_wall[i + 1][s * k + j]};
    }
    return n;
}

static void bfs(ring_maze_t *m, int dist[RING_MAX][RING_SECT_MAX]) {
    for (int i = 0; i < RING_MAX; i++)
        for (int s = 0; s < RING_SECT_MAX; s++) dist[i][s] = -1;
    static int q[RING_MAX * RING_SECT_MAX][2];
    int head = 0, tail = 0;
    dist[m->start_ring][m->start_sector] = 0;
    q[tail][0] = m->start_ring, q[tail++][1] = m->start_sector;
    while (head < tail) {
        const int i = q[head][0], s = q[head++][1];
        nb_t nb[8];
        const int n = neighbours(m, i, s, nb);
        for (int k = 0; k < n; k++) {
            if (*nb[k].wall || dist[nb[k].ring][nb[k].sector] >= 0) continue;
            dist[nb[k].ring][nb[k].sector] = dist[i][s] + 1;
            q[tail][0] = nb[k].ring, q[tail++][1] = nb[k].sector;
        }
    }
}

static int openings(ring_maze_t *m, int i, int s) {
    nb_t nb[8];
    const int n = neighbours(m, i, s, nb);
    int open = 0;
    for (int k = 0; k < n; k++) open += !*nb[k].wall;
    return open;
}

int ring_links(const ring_maze_t *m, int i, int s, int out[8][2]) {
    nb_t nb[8];
    const int n = neighbours((ring_maze_t *)m, i, s, nb);
    int k = 0;
    for (int j = 0; j < n; j++)
        if (!*nb[j].wall) out[k][0] = nb[j].ring, out[k][1] = nb[j].sector, k++;
    return k;
}

static float angle_diff(float a, float b) {
    float d = fabsf(a - b);
    while (d > TAU) d -= TAU;
    return d > TAU / 2 ? TAU - d : d;
}

// ---------------------------------------------------------------- geometry

static void add_seg(ring_maze_t *m, float x0, float y0, float x1, float y1, bool door) {
    if (m->nseg >= RING_SEGS_MAX) return;
    float *g = m->seg[m->nseg];
    g[0] = x0, g[1] = y0, g[2] = x1, g[3] = y1;
    m->seg_door[m->nseg] = door;
    const float pad = RING_WALL_R + RING_BALL_R + 2;
    const int bx0 = (int)floorf((fminf(x0, x1) - pad + 256) / RING_BIN), bx1 = (int)floorf((fmaxf(x0, x1) + pad + 256) / RING_BIN);
    const int by0 = (int)floorf((fminf(y0, y1) - pad + 256) / RING_BIN), by1 = (int)floorf((fmaxf(y0, y1) + pad + 256) / RING_BIN);
    for (int by = by0; by <= by1; by++)
        for (int bx = bx0; bx <= bx1; bx++) {
            if (bx < 0 || by < 0 || bx >= RING_BINS || by >= RING_BINS || m->bin_n[by][bx] >= RING_BIN_SEGS) continue;
            m->bin[by][bx][m->bin_n[by][bx]++] = (uint16_t)m->nseg;
        }
    m->nseg++;
}

static void add_arc(ring_maze_t *m, float r, float a0, float a1, bool door) {
    const int n = (int)ceilf(r * (a1 - a0) / CHORD);
    float px, py;
    polar(r, a0, &px, &py);
    for (int k = 1; k <= n; k++) {
        float x, y;
        polar(r, a0 + (a1 - a0) * k / n, &x, &y);
        add_seg(m, px, py, x, y, door);
        px = x, py = y;
    }
}

void ring_maze_build(ring_maze_t *m, uint32_t seed) {
    memset(m, 0, sizeof(*m));
    m->rings = RINGS;
    m->hub_r = HUB_R;
    int n = 0;
    for (int i = 0; i < RINGS; i++) {
        const float r0 = HUB_R + i * BAND_W;
        if (i == 0) {
            n = (int)(TAU * r0 / MIN_ARC);
        } else if (TAU * r0 / (2 * n) >= MIN_ARC && 2 * n <= RING_SECT_MAX) {
            n *= 2;
        }
        m->band[i] = (ring_band_t){r0, r0 + BAND_W, n};
    }
    m->outer_r = m->band[RINGS - 1].r1;
    memset(m->in_wall, 1, sizeof(m->in_wall));
    memset(m->cw_wall, 1, sizeof(m->cw_wall));
    m->start_ring = RINGS - 1;
    m->start_sector = m->band[RINGS - 1].n / 2;  // the bottom

    // Depth-first carving from the start.
    s_rng = seed ? seed : 1;
    static uint8_t seen[RING_MAX][RING_SECT_MAX];
    static int stack[RING_MAX * RING_SECT_MAX][2];
    memset(seen, 0, sizeof(seen));
    int top = 0;
    stack[top][0] = m->start_ring, stack[top++][1] = m->start_sector;
    seen[m->start_ring][m->start_sector] = 1;
    while (top) {
        const int i = stack[top - 1][0], s = stack[top - 1][1];
        nb_t nb[8], fresh[8];
        const int cnt = neighbours(m, i, s, nb);
        int f = 0;
        for (int k = 0; k < cnt; k++)
            if (!seen[nb[k].ring][nb[k].sector]) fresh[f++] = nb[k];
        if (!f) {
            top--;
            continue;
        }
        const nb_t pick = fresh[rnd() % f];
        *pick.wall = 0;
        seen[pick.ring][pick.sector] = 1;
        stack[top][0] = pick.ring, stack[top++][1] = pick.sector;
    }

    // The home's door: the ring-0 sector farthest from the start.
    static int dist[RING_MAX][RING_SECT_MAX];
    bfs(m, dist);
    m->gate = 0;
    for (int s = 0; s < m->band[0].n; s++)
        if (dist[0][s] > dist[0][m->gate]) m->gate = s;

    // Seals: dead ends far from the start, spread round the circle.
    int got = 0;
    for (float sep = 1.2f; got < RING_SEALS && sep > 0; sep -= 0.3f) {
        for (int pass = 0; pass < 400 && got < RING_SEALS; pass++) {
            int bi = -1, bs = -1;
            for (int i = 1; i < RINGS; i++)
                for (int s = 0; s < m->band[i].n; s++) {
                    if ((i == m->start_ring && s == m->start_sector) || openings(m, i, s) != 1) continue;
                    bool taken = false, close = false;
                    const float a = TAU * (s + 0.5f) / m->band[i].n;
                    for (int k = 0; k < got; k++) {
                        taken |= m->seal_ring[k] == i && m->seal_sector[k] == s;
                        close |= angle_diff(a, TAU * (m->seal_sector[k] + 0.5f) / m->band[m->seal_ring[k]].n) < sep;
                    }
                    if (taken || close) continue;
                    if (bi < 0 || dist[i][s] > dist[bi][bs]) bi = i, bs = s;
                }
            if (bi < 0) break;
            m->seal_ring[got] = bi, m->seal_sector[got] = bs;
            ring_cell_center(m, bi, bs, &m->seal_x[got], &m->seal_y[got]);
            got++;
        }
    }

    // Walls into segments.
    for (int i = 0; i < RINGS; i++) {
        const ring_band_t *b = &m->band[i];
        for (int s = 0; s < b->n; s++) {
            const float a0 = sect_angle(m, i, s), a1 = sect_angle(m, i, s + 1);
            const bool door = i == 0 && s == m->gate;
            if (m->in_wall[i][s] || door) add_arc(m, b->r0, a0, a1, door);
            if (m->cw_wall[i][s]) {
                float x0, y0, x1, y1;
                polar(b->r0, a1, &x0, &y0);
                polar(b->r1, a1, &x1, &y1);
                add_seg(m, x0, y0, x1, y1, false);
            }
        }
    }
    m->in_wall[0][m->gate] = 0;  // the door is drawn and collided as the door, not as a wall
    add_arc(m, m->outer_r, 0, TAU, false);
}

static float seg_dist(const float *g, float x, float y, float *cx, float *cy) {
    const float dx = g[2] - g[0], dy = g[3] - g[1];
    const float l2 = dx * dx + dy * dy;
    float t = l2 > 0 ? ((x - g[0]) * dx + (y - g[1]) * dy) / l2 : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    *cx = g[0] + t * dx, *cy = g[1] + t * dy;
    return sqrtf((x - *cx) * (x - *cx) + (y - *cy) * (y - *cy));
}

float ring_wall_dist(const ring_maze_t *m, float x, float y, bool door_open) {
    const int bx = (int)floorf((x + 256) / RING_BIN), by = (int)floorf((y + 256) / RING_BIN);
    if (bx < 0 || by < 0 || bx >= RING_BINS || by >= RING_BINS) return 1e9f;
    float best = 1e9f, cx, cy;
    for (int k = 0; k < m->bin_n[by][bx]; k++) {
        const int i = m->bin[by][bx][k];
        if (door_open && m->seg_door[i]) continue;
        const float d = seg_dist(m->seg[i], x, y, &cx, &cy);
        if (d < best) best = d;
    }
    return best;
}

// ---------------------------------------------------------------- the ball

void ring_reset(ring_game_t *g, const ring_maze_t *m) {
    memset(g, 0, sizeof(*g));
    g->m = m;
    g->last_seal = -1;
    ring_cell_center(m, m->start_ring, m->start_sector, &g->x, &g->y);
}

int ring_got(const ring_game_t *g) {
    int n = 0;
    for (uint32_t b = g->got; b; b &= b - 1) n++;
    return n;
}

bool ring_open(const ring_game_t *g) { return ring_got(g) == RING_SEALS; }

static unsigned collide(ring_game_t *g) {
    const ring_maze_t *m = g->m;
    const bool open = ring_open(g);
    const int bx = (int)floorf((g->x + 256) / RING_BIN), by = (int)floorf((g->y + 256) / RING_BIN);
    if (bx < 0 || by < 0 || bx >= RING_BINS || by >= RING_BINS) return 0;
    unsigned ev = 0;
    const float reach = RING_BALL_R + RING_WALL_R;
    for (int k = 0; k < m->bin_n[by][bx]; k++) {
        const int i = m->bin[by][bx][k];
        if (open && m->seg_door[i]) continue;
        float cx, cy;
        const float d = seg_dist(m->seg[i], g->x, g->y, &cx, &cy);
        if (d >= reach) continue;
        float nx = g->x - cx, ny = g->y - cy;
        if (d < 1e-4f) nx = -g->x, ny = -g->y;  // on the line: towards the centre
        const float l = sqrtf(nx * nx + ny * ny);
        nx /= l, ny /= l;
        g->x = cx + nx * reach, g->y = cy + ny * reach;
        const float vn = g->vx * nx + g->vy * ny;
        if (vn >= 0) continue;
        g->vx -= (1 + BOUNCE) * vn * nx, g->vy -= (1 + BOUNCE) * vn * ny;
        if (m->seg_door[i]) ev |= GAME_EV_LOCKED;
        else if (-vn > HEARD) ev |= GAME_EV_WALL;
        if (-vn > BUMP) g->bumps++;
    }
    return ev;
}

unsigned ring_step(ring_game_t *g, float tilt_x, float tilt_y, float dt) {
    if (g->won || dt <= 0) return 0;
    if (dt > 0.1f) dt = 0.1f;
    g->ms += (uint32_t)(dt * 1000 + 0.5f);
    const float t = sqrtf(tilt_x * tilt_x + tilt_y * tilt_y);
    if (t > GAME_DEADZONE) {
        const float k = (t - GAME_DEADZONE) / t * ACCEL;
        g->vx += tilt_x * k * dt, g->vy += tilt_y * k * dt;
    }
    const float keep = expf(-DRAG * dt);
    g->vx *= keep, g->vy *= keep;
    const float v = sqrtf(g->vx * g->vx + g->vy * g->vy);
    if (v > MAX_SPEED) g->vx *= MAX_SPEED / v, g->vy *= MAX_SPEED / v;

    unsigned ev = 0;
    int steps = (int)ceilf(sqrtf(g->vx * g->vx + g->vy * g->vy) * dt / MAX_MOVE);
    if (steps < 1) steps = 1;
    const float h = dt / steps;
    const ring_maze_t *m = g->m;
    for (int s = 0; s < steps; s++) {
        g->x += g->vx * h, g->y += g->vy * h;
        ev |= collide(g);
        for (int i = 0; i < RING_SEALS; i++) {
            if (g->got & (1u << i)) continue;
            const float dx = g->x - m->seal_x[i], dy = g->y - m->seal_y[i], r = RING_BALL_R + SEAL_REACH;
            if (dx * dx + dy * dy < r * r) {
                g->got |= 1u << i;
                g->last_seal = i;
                ev |= GAME_EV_SEAL;
            }
        }
        if (ring_open(g) && sqrtf(g->x * g->x + g->y * g->y) < m->hub_r - RING_BALL_R - 2) {
            g->won = true;
            return ev | GAME_EV_WIN;
        }
    }
    return ev;
}
