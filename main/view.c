#include "view.h"

#include <math.h>
#include <string.h>

#include "yk_round.h"

#define HALF (VIEW_SIZE / 2)
#define RIM_IN 219.0f
#define RIM_OUT 231.0f
#define SEG_GAP_DEG 4.0f
#define TAU 6.28318531f

/** 0xRRGGBB to big-endian RGB565. */
static uint16_t px(uint32_t rgb) {
    const uint16_t c = (uint16_t)(((rgb >> 19) & 0x1F) << 11 | ((rgb >> 10) & 0x3F) << 5 | ((rgb >> 3) & 0x1F));
    return (uint16_t)(c >> 8 | c << 8);
}

static uint32_t mix(uint32_t a, uint32_t b, float k) {
    if (k < 0) k = 0;
    if (k > 1) k = 1;
    uint32_t out = 0;
    for (int s = 0; s < 24; s += 8) {
        const float ca = (a >> s) & 0xFF, cb = (b >> s) & 0xFF;
        out |= (uint32_t)(ca + (cb - ca) * k + 0.5f) << s;
    }
    return out;
}

/** Each row's visible half-width (to the rim's outer edge): drawing stops there, the corners stay black. */
static int16_t s_half[VIEW_SIZE];

static float rnd(view_t *v) {  // 0..1
    v->seed = v->seed * 1664525u + 1013904223u;
    return (v->seed >> 8) / 16777216.0f;
}

int view_rim_size(void) {
    int n = 0;
    for (int y = 0; y < VIEW_SIZE; y++)
        for (int x = 0; x < VIEW_SIZE; x++) {
            const float dx = x + 0.5f - HALF, dy = y + 0.5f - HALF, d = sqrtf(dx * dx + dy * dy);
            if (d >= RIM_IN && d <= RIM_OUT) n++;
        }
    return n;
}

void view_init(view_t *v, uint16_t *fb, uint16_t *bg, uint32_t *rim) {
    memset(v, 0, sizeof(*v));
    v->fb = fb;
    v->bg = bg;
    v->rim = rim;
    v->seed = 12345;
    memset(fb, 0, (size_t)VIEW_SIZE * VIEW_SIZE * 2);
    memset(bg, 0, (size_t)VIEW_SIZE * VIEW_SIZE * 2);
    for (int y = 0; y < VIEW_SIZE; y++) {
        const float dy = y + 0.5f - HALF;
        s_half[y] = (int16_t)(fabsf(dy) < RIM_OUT ? sqrtf(RIM_OUT * RIM_OUT - dy * dy) : 0);
    }
    for (int y = 0; y < VIEW_SIZE; y++)
        for (int x = 0; x < VIEW_SIZE; x++) {
            const float dx = x + 0.5f - HALF, dy = y + 0.5f - HALF, d = sqrtf(dx * dx + dy * dy);
            if (d < RIM_IN || d > RIM_OUT) continue;
            float deg = atan2f(dx, -dy) * 180.0f / (float)M_PI;
            if (deg < 0) deg += 360;
            rim[v->rim_n++] = (uint32_t)(y * VIEW_SIZE + x) << 12 | (uint32_t)(deg * 10);
        }
}

void view_build(view_t *v, const ring_maze_t *m) {
    const uint16_t path = px(YK_PANEL), home = px(YK_PAPER), wall = px(YK_AMBER);
    const uint16_t edge = px(mix(YK_AMBER, YK_PANEL, 0.5f));
    const float outer = m->outer_r + RING_WALL_R + 1;
    for (int y = 0; y < VIEW_SIZE; y++)
        for (int x = 0; x < VIEW_SIZE; x++) {
            const float fx = x + 0.5f - HALF, fy = y + 0.5f - HALF, r = sqrtf(fx * fx + fy * fy);
            uint16_t c = 0;
            if (r < outer) {
                // The door is no wall here: it is drawn on top, closed or open.
                const float d = ring_wall_dist(m, fx, fy, true);
                c = d < RING_WALL_R ? wall : d < RING_WALL_R + 1 ? edge : r < m->hub_r ? home : path;
            }
            v->bg[y * VIEW_SIZE + x] = c;
        }
}

void view_jump(view_t *v, const ring_game_t *g) {
    v->shake = 0;
    v->flash_until = v->open_until = 0;
    memset(v->sparks, 0, sizeof(v->sparks));
    for (int i = 0; i < VIEW_TRAIL; i++) v->trail[i][0] = g->x, v->trail[i][1] = g->y;
}

static void sparks(view_t *v, float x, float y, int n, float speed, float life, uint32_t c1, uint32_t c2) {
    for (int k = 0; k < n; k++) {
        view_spark_t *s = NULL;
        for (int i = 0; i < VIEW_SPARKS; i++)
            if (v->sparks[i].life <= 0) {
                s = &v->sparks[i];
                break;
            }
        if (!s) return;
        const float a = rnd(v) * TAU, sp = speed * (0.4f + 0.6f * rnd(v)), l = life * (0.7f + 0.3f * rnd(v));
        *s = (view_spark_t){x, y, cosf(a) * sp, sinf(a) * sp, l, l, k & 1 ? c2 : c1};
    }
}

void view_event(view_t *v, const ring_game_t *g, unsigned ev) {
    if (ev & GAME_EV_WALL) v->shake = fmaxf(v->shake, 3);
    if ((ev & GAME_EV_SEAL) && g->last_seal >= 0) {
        sparks(v, g->m->seal_x[g->last_seal], g->m->seal_y[g->last_seal], 20, 160, 0.7f, YK_CREAM, YK_SAGE);
        if (ring_open(g)) {
            v->open_until = v->t + 1.6f;
            sparks(v, 0, 0, 24, 140, 1.2f, YK_SAGE, YK_CREAM);
        }
    }
    if (ev & GAME_EV_LOCKED) {
        v->flash_until = v->t + 0.6f;
        v->shake = fmaxf(v->shake, 4);
    }
    if (ev & GAME_EV_WIN) sparks(v, 0, 0, 48, 220, 1.4f, YK_CREAM, YK_SAGE);
}

// ---------------------------------------------------------------- primitives (screen pixels, clipped to the glass)

static inline void fill16(uint16_t *p, int n, uint16_t c) {
    for (int i = 0; i < n; i++) p[i] = c;
}

static void span(view_t *v, int y, int x0, int x1, uint16_t c) {
    if (y < 0 || y >= VIEW_SIZE) return;
    if (x0 < HALF - s_half[y]) x0 = HALF - s_half[y];
    if (x1 > HALF + s_half[y]) x1 = HALF + s_half[y];
    fill16(v->fb + y * VIEW_SIZE + x0, x1 - x0, c);
}

static void disc(view_t *v, float cx, float cy, float r, uint16_t c) {
    for (int y = (int)floorf(cy - r); y <= (int)ceilf(cy + r); y++) {
        const float dy = y + 0.5f - cy;
        if (dy * dy > r * r) continue;
        const float w = sqrtf(r * r - dy * dy);
        span(v, y, (int)lroundf(cx - w), (int)lroundf(cx + w), c);
    }
}

static void ellipse(view_t *v, float cx, float cy, float rx, float ry, uint16_t c) {
    for (int y = (int)floorf(cy - ry); y <= (int)ceilf(cy + ry); y++) {
        const float dy = (y + 0.5f - cy) / ry;
        if (dy * dy > 1) continue;
        const float w = rx * sqrtf(1 - dy * dy);
        span(v, y, (int)lroundf(cx - w), (int)lroundf(cx + w), c);
    }
}

static void rect(view_t *v, int x, int y, int w, int h, uint16_t c) {
    for (int i = 0; i < h; i++) span(v, y + i, x, x + w, c);
}

/** The background into the frame, moved by (sx, sy) for a shake; what it uncovers stays black. */
static void copy_bg(view_t *v, int sx, int sy) {
    for (int y = 0; y < VIEW_SIZE; y++) {
        uint16_t *dst = v->fb + y * VIEW_SIZE;
        const int x0 = HALF - s_half[y], x1 = HALF + s_half[y];
        if (x1 <= x0) continue;
        const int src_y = y - sy;
        if (src_y < 0 || src_y >= VIEW_SIZE) {
            fill16(dst + x0, x1 - x0, 0);
            continue;
        }
        const uint16_t *src = v->bg + src_y * VIEW_SIZE;
        int a = x0, b = x1;
        if (a - sx < 0) fill16(dst + a, sx - a, 0), a = sx;
        if (b - sx > VIEW_SIZE) fill16(dst + VIEW_SIZE + sx, b - (VIEW_SIZE + sx), 0), b = VIEW_SIZE + sx;
        if (b > a) memcpy(dst + a, src + a - sx, (size_t)(b - a) * 2);
    }
}

// ---------------------------------------------------------------- what moves

static void draw_seal(view_t *v, float cx, float cy, float s) {
    ellipse(v, cx + 1.5f, cy + 2, s, s * 0.85f, px(YK_PAPER));
    ellipse(v, cx, cy, s, s * 0.85f, px(YK_CREAM));
    const uint16_t eye = px(YK_PAPER);
    disc(v, cx - s * 0.4f, cy - s * 0.2f, 1.5f, eye);
    disc(v, cx + s * 0.4f, cy - s * 0.2f, 1.5f, eye);
}

static void draw_ball(view_t *v, float cx, float cy, float r) {
    disc(v, cx + 2, cy + 3, r + 2, px(YK_PAPER));
    disc(v, cx, cy, r + 2, px(YK_CORAL));  // a coral edge: the ball is never taken for a seal
    disc(v, cx, cy, r, px(YK_INK));
    const uint16_t eye = px(YK_PAPER);
    disc(v, cx - r * 0.38f, cy - r * 0.2f, 1.6f, eye);
    disc(v, cx + r * 0.38f, cy - r * 0.2f, 1.6f, eye);
    ellipse(v, cx, cy + r * 0.22f, 2.2f, 1.5f, eye);
}

/** The home's door: a coral arc across the gap until it opens (thicker while pushed against). */
static void draw_door(view_t *v, const ring_maze_t *m, int ox, int oy, bool open, bool flash) {
    if (open) return;
    const int n = m->band[0].n;
    const float a0 = TAU * m->gate / n, a1 = TAU * (m->gate + 1) / n, r = flash ? 4.5f : 3.0f;
    for (int k = 0; k <= 16; k++) {
        const float a = a0 + (a1 - a0) * k / 16;
        disc(v, ox + m->hub_r * sinf(a), oy - m->hub_r * cosf(a), r, px(YK_CORAL));
    }
}

static void draw_rim(view_t *v, const ring_game_t *g) {
    const int got = ring_got(g);
    const float seg = 360.0f / RING_SEALS;
    const bool flash = v->flash_until > v->t;
    const float pulse = v->open_until > v->t ? 0.5f + 0.5f * sinf((v->open_until - v->t) * 14) : 0;
    uint16_t colors[RING_SEALS];
    for (int i = 0; i < RING_SEALS; i++)
        colors[i] = i < got ? px(mix(YK_SAGE, YK_CREAM, pulse)) : flash ? px(YK_CORAL) : px(YK_PANEL);
    const uint16_t bg = px(YK_PAPER);
    for (int i = 0; i < v->rim_n; i++) {
        const uint32_t e = v->rim[i];
        const float deg = (e & 0xFFF) / 10.0f;
        const int k = (int)(deg / seg);
        const float in = deg - k * seg;
        v->fb[e >> 12] = in < SEG_GAP_DEG / 2 || in > seg - SEG_GAP_DEG / 2 ? bg : colors[k < RING_SEALS ? k : RING_SEALS - 1];
    }
}

/** Seven-segment digits: a b c d e f g, from the top clockwise, then the middle. */
static const uint8_t SEGS[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};

static void digit(view_t *v, int x, int y, int d, uint16_t c) {
    const int w = 10, h = 18, t = 2, m = h / 2;
    const uint8_t s = SEGS[d];
    if (s & 0x01) rect(v, x + t, y, w - 2 * t, t, c);
    if (s & 0x02) rect(v, x + w - t, y + t, t, m - t, c);
    if (s & 0x04) rect(v, x + w - t, y + m, t, m - t, c);
    if (s & 0x08) rect(v, x + t, y + h - t, w - 2 * t, t, c);
    if (s & 0x10) rect(v, x, y + m, t, m - t, c);
    if (s & 0x20) rect(v, x, y + t, t, m - t, c);
    if (s & 0x40) rect(v, x + t, y + m - t / 2, w - 2 * t, t, c);
}

/** The time, small, in the home. */
static void draw_time(view_t *v, uint32_t ms, int cx, int cy, uint16_t ink) {
    uint32_t s = ms / 1000;
    if (s > 99 * 60 + 59) s = 99 * 60 + 59;
    const int mins = (int)(s / 60), secs = (int)(s % 60), digits = mins >= 10 ? 4 : 3;
    int x = cx - (digits * 13 + 4) / 2;
    const int y = cy - 9;
    if (digits == 4) digit(v, x, y, mins / 10, ink), x += 13;
    digit(v, x, y, mins % 10, ink), x += 13;
    rect(v, x, y + 4, 2, 2, ink), rect(v, x, y + 12, 2, 2, ink), x += 5;
    digit(v, x, y, secs / 10, ink), x += 13;
    digit(v, x, y, secs % 10, ink);
}

void view_render(view_t *v, const ring_game_t *g, float dt) {
    const ring_maze_t *m = g->m;
    v->t += dt;
    int sx = 0, sy = 0;
    if (v->shake > 0.3f) {
        sx = (int)lroundf((rnd(v) - 0.5f) * 2 * v->shake), sy = (int)lroundf((rnd(v) - 0.5f) * 2 * v->shake);
        v->shake *= expf(-dt * 16);
    } else {
        v->shake = 0;
    }
    copy_bg(v, sx, sy);
    const int ox = HALF + sx, oy = HALF + sy;
    const bool open = ring_open(g);

    // The home: glowing once the door is open, the time inside.
    if (open) {
        const float k = 0.35f + 0.15f * sinf(v->t * 4);
        disc(v, ox, oy, m->hub_r - RING_WALL_R - 1, px(mix(YK_PAPER, YK_SAGE, k)));
    }
    draw_time(v, g->ms, ox, oy, px(YK_INK));
    draw_door(v, m, ox, oy, open, v->flash_until > v->t);
    for (int i = 0; i < RING_SEALS; i++) {  // the seals still out there bob gently
        if (g->got & (1u << i)) continue;
        draw_seal(v, ox + m->seal_x[i], oy + m->seal_y[i] + sinf(v->t * 3.2f + i * 1.7f) * 1.5f, 6);
    }
    // A fading tail behind the ball when it is quick.
    v->trail[v->trail_at][0] = g->x, v->trail[v->trail_at][1] = g->y;
    v->trail_at = (v->trail_at + 1) % VIEW_TRAIL;
    if (sqrtf(g->vx * g->vx + g->vy * g->vy) > 90)
        for (int i = 1; i < VIEW_TRAIL; i++) {
            const float *p = v->trail[(v->trail_at + i) % VIEW_TRAIL], f = (float)i / VIEW_TRAIL;
            disc(v, ox + p[0], oy + p[1], RING_BALL_R * (0.35f + 0.5f * f), px(mix(YK_PANEL, YK_INK3, f)));
        }
    draw_ball(v, ox + g->x, oy + g->y, RING_BALL_R);
    for (int i = 0; i < VIEW_SPARKS; i++) {
        view_spark_t *s = &v->sparks[i];
        if (s->life <= 0) continue;
        s->life -= dt;
        s->x += s->vx * dt, s->y += s->vy * dt;
        s->vx *= expf(-dt * 3), s->vy *= expf(-dt * 3);
        const float f = s->life / s->max;
        if (f > 0) disc(v, ox + s->x, oy + s->y, 1.5f + 3.0f * f, px(s->color));
    }
    draw_rim(v, g);
}
