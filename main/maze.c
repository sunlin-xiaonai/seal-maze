#include "maze.h"

#include <stdio.h>

static const char *const STONE_PATH[] = {
    "###############",
    "..............#",
    "#.###.#####.#.#",
    "#.#...#...#.#.#",
    "#.#.###.#.#.#.#",
    "#...#...#...#.#",
    "###.#.#######.#",
    "#...#.....#...#",
    "#########.#.###",
    "#.......#.#...#",
    "#######.#.###.#",
    "#.....#.#.#...#",
    "#.###.#.#.#.###",
    "#.#...#...#...#",
    "#.#.###.###.#.#",
    "#.#.....#...#.#",
    "#.#####.#.###.#",
    "........#.....#",
    "###############",
};

const maze_t MAZES[] = {
    {
        .name_zh = "石头路",
        .name_en = "Stone Path",
        .w = 15,
        .h = 19,
        .rows = STONE_PATH,
        .start_r = 17,
        .start_c = 0,
        .exit_r = 1,
        .exit_c = 0,
        .n_seals = 5,
        .seals = {{4.5f, 13.5f}, {1.5f, 9.5f}, {4.5f, 3.5f}, {13.5f, 11.5f}, {13.5f, 17.5f}},
        // At the corridors' edges, like the stones on the paper: the ball still fits past them.
        .n_rocks = 3,
        .rocks = {{9.5f, 1.12f}, {1.12f, 11.5f}, {12.5f, 17.88f}},
    },
};
const int MAZE_COUNT = sizeof(MAZES) / sizeof(MAZES[0]);

bool maze_wall(const maze_t *m, int r, int c) {
    if (r < 0 || c < 0 || r >= m->h || c >= m->w) return true;
    return m->rows[r][c] == '#';
}

void maze_exit_outside(const maze_t *m, int *r, int *c) {
    *r = m->exit_r;
    *c = m->exit_c;
    if (m->exit_c == 0) *c = -1;
    else if (m->exit_c == m->w - 1) *c = m->w;
    else if (m->exit_r == 0) *r = -1;
    else *r = m->h;
}

int maze_json(const maze_t *m, bool english, char *out, size_t size) {
    size_t n = 0;
#define PUT(...)                                                                    \
    do {                                                                            \
        int k = snprintf(out + (n < size ? n : size), n < size ? size - n : 0, __VA_ARGS__); \
        if (k > 0) n += (size_t)k;                                                  \
    } while (0)
    PUT("{\"t\":\"level\",\"lang\":\"%s\",\"name\":\"%s\",\"w\":%d,\"h\":%d,\"rows\":[", english ? "en" : "zh",
        english ? m->name_en : m->name_zh, m->w, m->h);
    for (int r = 0; r < m->h; r++) PUT("%s\"%s\"", r ? "," : "", m->rows[r]);
    PUT("],\"start\":[%d,%d],\"exit\":[%d,%d],\"seals\":[", m->start_r, m->start_c, m->exit_r, m->exit_c);
    for (int i = 0; i < m->n_seals; i++) PUT("%s[%.2f,%.2f]", i ? "," : "", m->seals[i].x, m->seals[i].y);
    PUT("],\"rocks\":[");
    for (int i = 0; i < m->n_rocks; i++) PUT("%s[%.2f,%.2f]", i ? "," : "", m->rocks[i].x, m->rocks[i].y);
    PUT("]}");
#undef PUT
    return (int)n;
}
