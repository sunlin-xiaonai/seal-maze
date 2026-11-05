// The levels: a grid of walls ('#') and paths ('.'), where the ball starts, the exit on the grid's edge, the seals
// to collect and the rocks lying at the corridors' edges. Level 1 is traced from the paper maze the design was made
// from (start bottom-left, exit top-left). Plain C: the device draws it, the game rolls on it, the big-screen page
// gets it as JSON, the host tests check it.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAZE_MAX_SEALS 8
#define MAZE_MAX_ROCKS 8
#define MAZE_SEAL_R 0.25f  // a seal is collected when the ball's edge is this close to its centre
#define MAZE_ROCK_R 0.22f

/** A point in cells: x is the column, y the row, a cell's centre is at +0.5. */
typedef struct {
    float x, y;
} maze_pt_t;

typedef struct {
    const char *name_zh, *name_en;
    int w, h;
    const char *const *rows;  // h strings of w characters
    int start_r, start_c;
    int exit_r, exit_c;  // a path cell on the edge; the way out is past that edge
    int n_seals;
    maze_pt_t seals[MAZE_MAX_SEALS];
    int n_rocks;
    maze_pt_t rocks[MAZE_MAX_ROCKS];
} maze_t;

extern const maze_t MAZES[];
extern const int MAZE_COUNT;

/** A wall, or outside the grid. */
bool maze_wall(const maze_t *m, int r, int c);
/** The cell just outside the exit (outside the grid). */
void maze_exit_outside(const maze_t *m, int *r, int *c);
/** The level as the big-screen page reads it: {"t":"level","lang":"zh"|"en","name":..,"w":..,"h":..,"rows":[..],"start":[r,c],
 *  "exit":[r,c],"seals":[[x,y],..],"rocks":[[x,y],..]}. Returns the length written (like snprintf). */
int maze_json(const maze_t *m, bool english, char *out, size_t size);

#ifdef __cplusplus
}
#endif
