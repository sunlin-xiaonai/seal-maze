// Draws one frame of the round screen as it plays (view.c) into a BMP, to look at: test/run.sh --frames.
#include <stdio.h>
#include <stdlib.h>

#include "../main/view.h"

static void save_bmp(const char *path, const uint16_t *fb) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    const int w = VIEW_SIZE, h = VIEW_SIZE, row = (w * 3 + 3) & ~3, size = 54 + row * h;
    unsigned char hd[54] = {'B', 'M'};
    hd[2] = size & 0xFF, hd[3] = (size >> 8) & 0xFF, hd[4] = (size >> 16) & 0xFF, hd[10] = 54, hd[14] = 40;
    hd[18] = w & 0xFF, hd[19] = w >> 8, hd[22] = h & 0xFF, hd[23] = h >> 8, hd[26] = 1, hd[28] = 24;
    fwrite(hd, 1, 54, f);
    unsigned char *line = calloc(1, row);
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            uint16_t c = fb[y * w + x];
            c = (uint16_t)(c >> 8 | c << 8);
            line[x * 3 + 2] = (c >> 11) << 3, line[x * 3 + 1] = ((c >> 5) & 0x3F) << 2, line[x * 3] = (c & 0x1F) << 3;
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    static uint16_t fb[VIEW_SIZE * VIEW_SIZE], bg[VIEW_SIZE * VIEW_SIZE];
    static ring_maze_t m;
    ring_maze_build(&m, 7);
    uint32_t *rim = malloc(sizeof(uint32_t) * view_rim_size());
    view_t v;
    view_init(&v, fb, bg, rim);
    view_build(&v, &m);
    ring_game_t g;
    char path[512];
    // The start.
    ring_reset(&g, &m);
    view_jump(&v, &g);
    view_render(&v, &g, 0.016f);
    snprintf(path, sizeof(path), "%s/ring_start.bmp", dir);
    save_bmp(path, fb);
    // Two seals in, the third just collected: sparks.
    g.got = 0x7, g.last_seal = 2, g.ms = 84000;
    g.x = m.seal_x[2], g.y = m.seal_y[2];
    view_event(&v, &g, GAME_EV_SEAL);
    for (int i = 0; i < 8; i++) view_render(&v, &g, 0.016f);
    snprintf(path, sizeof(path), "%s/ring_seal.bmp", dir);
    save_bmp(path, fb);
    // All in: the home glows and the door is gone.
    g.got = 0x1F, g.last_seal = 4;
    view_event(&v, &g, GAME_EV_SEAL);
    for (int i = 0; i < 10; i++) view_render(&v, &g, 0.016f);
    snprintf(path, sizeof(path), "%s/ring_open.bmp", dir);
    save_bmp(path, fb);
    free(rim);
    return 0;
}
