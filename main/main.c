// Seal Home (海豹回家): a tilt maze. Tilt the round screen to roll a little seal through the maze, collect its five
// friends, and leave by the exit. Two ways to play, one game (game.c):
//   alone       the round screen shows a window onto the maze that follows the ball (view.c, drawn straight to
//               the panel)
//   big screen  the round screen is the controller (a level bubble, the time, the seals on the rim) and a browser
//               on the same network shows the whole maze (web.c: http://maze.local, served by the board itself)
// Shake to start over, turn it face down to pause. Startup order as the template: NVS (never erased) -> yaykit_app
// -> board -> speaker -> side key -> Yaykit Link -> screen -> console -> yk_app_ready() -> Wi-Fi.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "board_direct.h"
#include "cjk_font.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "game.h"
#include "maze.h"
#include "ring.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sound.h"
#include "view.h"
#include "web.h"
#include "yaykit_app.h"
#include "yaykit_account.h"
#include "yaykit_key.h"
#include "yaykit_link.h"
#include "yaykit_motion.h"
#include "yaykit_speak.h"
#include "yaykit_ui.h"
#include "yaykit_wifi.h"
#include "yk_hint.h"

#define APP_SLUG "seal-maze"
static const char *TAG = "seal-maze";

#define NVS_NS "yk_sealmaze"
#define COUNT_S 3
#define STEP_S 0.005f        // physics step
#define WEB_PERIOD_US 25000  // states to the big screen, 40 a second

typedef enum { ST_HOME, ST_WAIT, ST_COUNT, ST_PLAY, ST_PAUSE, ST_WON } st_t;

// ---------------------------------------------------------------- shared state (s_mu)

static SemaphoreHandle_t s_mu;
static game_t s_game;              // the big screen's square maze
static ring_maze_t *s_ring_maze;   // the round maze for playing alone (PSRAM)
static ring_game_t s_ring;
#define RING_SEED 7
static volatile st_t s_st = ST_HOME;
static volatile bool s_big;        // playing on the big screen (the round one is the controller)
static float s_g0x, s_g0y;         // gravity in the screen's plane where the player held it at the start
static volatile float s_tx, s_ty;  // the tilt now (g)
static volatile bool s_won_flag;   // the game task saw the win
static volatile int s_fps, s_render_us, s_blit_us;  // the drawn view's speed (`sealmaze.status`)
static volatile int64_t s_override_until_us;  // `sealmaze.tilt`: a tilt from the console instead of the sensor
static volatile float s_ox, s_oy;

// From other tasks (side key, motion), acted on in the LVGL timer.
static volatile bool s_key_tap, s_face_down, s_face_up, s_shake;
static volatile unsigned s_web_ev;    // game events since the last state sent to the big screen
static volatile unsigned s_ui_ev;     // ... and since the controller screen last looked

// The LVGL side.
static bool s_english;
static const maze_t *s_maze = &MAZES[0];
static uint32_t s_best_ms, s_best_ring;  // the big screen's square maze; the round one alone
static int s_games, s_wins, s_last_bumps;
static uint32_t s_last_ms, s_best_this_visit;
static bool s_new_best, s_resume;
static int64_t s_count_start_us;
static float s_sum_x, s_sum_y;
static int s_sum_n;
static int s_shown = -1;
static lv_obj_t *s_screen;
static yk_rim_t *s_rim;
static lv_obj_t *s_count_label, *s_time_label, *s_mini_ball, *s_wait_label, *s_url_label;
static lv_obj_t *s_mini_seals[MAZE_MAX_SEALS], *s_mini_gate;
static uint32_t s_shown_mask;
#define MINI_CELL 16  // the controller's map: px a cell (the maze fills the glass)
static int s_shown_secs = -1, s_shown_got = -1, s_shown_count = -1;

static const char *T(const char *zh, const char *en) { return s_english ? en : zh; }

static st_t state(void) { return s_st; }

static void set_state(st_t st) {
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_st = st;
    xSemaphoreGive(s_mu);
}

static void fmt_time(uint32_t ms, char *out, size_t size) {
    const uint32_t s = ms / 1000;
    snprintf(out, size, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
}

// ---------------------------------------------------------------- input from other tasks

static bool on_key(const yk_key_event_t *ev, void *ctx) {
    (void)ctx;
    if (ev->type == YK_KEY_TAP) {
        s_key_tap = true;
        return true;
    }
    return false;  // a hold: the system asks 「回到 Yay 说？」; the double click opens the app ring
}

static void on_motion(const yk_motion_event_t *ev, void *ctx) {
    (void)ctx;
    switch (ev->kind) {
    case YK_MOTION_FACE_DOWN: s_face_down = true; break;
    case YK_MOTION_FACE_UP: s_face_up = true; break;
    case YK_MOTION_SHAKE_START: s_shake = true; break;
    default: break;
    }
}

static void gravity(float *x, float *y) {
    yk_motion_state_t ms;
    yk_motion_state(&ms);
    *x = ms.now.gravity[0];
    *y = ms.now.gravity[1];
}

// ---------------------------------------------------------------- the game task: physics, the drawn view, the big screen

static void publish(const game_t *g, st_t st) {
    static const char *names[] = {"wait", "wait", "count", "play", "pause", "won"};
    char json[360];
    const int64_t now = esp_timer_get_time();
    const int left = st == ST_COUNT ? COUNT_S - (int)((now - s_count_start_us) / 1000000) : 0;
    const unsigned ev = s_web_ev;
    s_web_ev = 0;
    // "at": the board's clock (ms) when this was true, so the page can play the ball back smoothly whatever the
    // Wi-Fi does to the timing; "vx", "vy" to carry on for a moment when a message is late; "ev": what happened.
    snprintf(json, sizeof(json),
             "{\"t\":\"s\",\"st\":\"%s\",\"solo\":%s,\"at\":%u,\"x\":%.3f,\"y\":%.3f,\"vx\":%.2f,\"vy\":%.2f,"
             "\"got\":%u,\"ms\":%u,\"b\":%d,\"tx\":%.3f,\"ty\":%.3f,\"n\":%d,\"best\":%u,\"ev\":%u,\"ls\":%d,"
             "\"lr\":%d}",
             names[st], !s_big && st != ST_HOME && st != ST_WAIT ? "true" : "false", (unsigned)(now / 1000), g->x, g->y,
             g->vx, g->vy, (unsigned)g->got, (unsigned)g->ms, g->bumps, s_tx, s_ty, left > 0 ? left : 1,
             (unsigned)s_best_ms, ev, g->last_seal, g->last_rock);
    web_publish(json);
}

// Two frames: one is drawn (game task, core 1) while the other is sent to the panel (blit task, core 0).
static QueueHandle_t s_blit_q;
static SemaphoreHandle_t s_blit_idle;

static void blit_task(void *arg) {
    (void)arg;
    for (;;) {
        uint16_t *fb;
        xQueueReceive(s_blit_q, &fb, portMAX_DELAY);
        const int64_t t0 = esp_timer_get_time();
        board_lock(-1);
        board_direct_blit(fb, 0, 0, 0, VIEW_SIZE, VIEW_SIZE);
        board_unlock();
        s_blit_us = (int)(esp_timer_get_time() - t0);
        xSemaphoreGive(s_blit_idle);
    }
}

/** Waits until no frame is being sent. */
static void blit_wait(void) {
    xSemaphoreTake(s_blit_idle, portMAX_DELAY);
    xSemaphoreGive(s_blit_idle);
}

static void game_task(void *arg) {
    (void)arg;
    static view_t view;
    uint16_t *fbs[2] = {heap_caps_malloc((size_t)VIEW_SIZE * VIEW_SIZE * 2, MALLOC_CAP_SPIRAM),
                        heap_caps_malloc((size_t)VIEW_SIZE * VIEW_SIZE * 2, MALLOC_CAP_SPIRAM)};
    uint16_t *bg = heap_caps_malloc((size_t)VIEW_SIZE * VIEW_SIZE * 2, MALLOC_CAP_SPIRAM);
    uint32_t *rim = heap_caps_malloc(sizeof(uint32_t) * view_rim_size(), MALLOC_CAP_SPIRAM);
    s_blit_q = xQueueCreate(1, sizeof(uint16_t *));
    s_blit_idle = xSemaphoreCreateBinary();
    if (!fbs[0] || !fbs[1] || !bg || !rim || !s_blit_q || !s_blit_idle) {
        ESP_LOGE(TAG, "no memory for the view");
        vTaskDelete(NULL);
        return;
    }
    xSemaphoreGive(s_blit_idle);
    xTaskCreatePinnedToCoreWithCaps(blit_task, "blit", 3072, NULL, 5, NULL, 0, MALLOC_CAP_SPIRAM);
    memset(fbs[1], 0, (size_t)VIEW_SIZE * VIEW_SIZE * 2);
    view_init(&view, fbs[0], bg, rim);
    const int64_t t_build = esp_timer_get_time();
    view_build(&view, s_ring_maze);
    ESP_LOGI(TAG, "round maze drawn in %d ms", (int)((esp_timer_get_time() - t_build) / 1000));
    int cur = 0;
    bool direct = false;
    int64_t last = esp_timer_get_time(), last_web = 0, fps_from = last;
    int frames = 0;
    uint32_t last_ms = 0;
    game_t g;
    ring_game_t r;
    for (;;) {
        const int64_t now = esp_timer_get_time();
        float dt = (now - last) / 1e6f;
        last = now;
        if (dt > 0.1f) dt = 0.1f;
        const st_t st = state();

        float tx, ty;
        if (now < s_override_until_us) {
            tx = s_ox, ty = s_oy;
        } else {
            float gx, gy;
            gravity(&gx, &gy);
            tx = gx - s_g0x, ty = gy - s_g0y;
        }
        s_tx = tx, s_ty = ty;

        unsigned ev = 0;
        xSemaphoreTake(s_mu, portMAX_DELAY);
        if (s_st == ST_PLAY) {
            for (float left = dt; left > 1e-6f; left -= STEP_S) {
                const float h = left < STEP_S ? left : STEP_S;
                ev |= s_big ? game_step(&s_game, tx, ty, h) : ring_step(&s_ring, tx, ty, h);
            }
        }
        g = s_game;
        r = s_ring;
        xSemaphoreGive(s_mu);

        if (ev & GAME_EV_SEAL) sound_play((s_big ? game_exit_open(&g) : ring_open(&r)) ? SND_OPEN : SND_SEAL);
        if (ev & GAME_EV_ROCK) sound_play(SND_ROCK);
        else if (ev & GAME_EV_WALL) sound_play(SND_WALL);
        if (ev & GAME_EV_LOCKED) sound_play(SND_LOCKED);
        if (ev) {
            s_web_ev |= ev;
            s_ui_ev |= ev;
        }
        if (ev & GAME_EV_WIN) {
            sound_play(SND_WIN);
            s_won_flag = true;
        }

        const bool want_direct = st == ST_PLAY && !s_big;
        if (want_direct) {
            if (!direct) {
                board_lock(-1);
                direct = board_direct_begin() == ESP_OK;
                board_unlock();
                view_jump(&view, &r);
                frames = 0, fps_from = now;
            }
            if (r.ms < last_ms) view_jump(&view, &r);  // shaken back to the start
            view_event(&view, &r, ev);
            const int64_t t0 = esp_timer_get_time();
            view.fb = fbs[cur];
            view_render(&view, &r, dt);
            s_render_us = (int)(esp_timer_get_time() - t0);
            xSemaphoreTake(s_blit_idle, portMAX_DELAY);
            xQueueSend(s_blit_q, &fbs[cur], portMAX_DELAY);
            cur ^= 1;
            if (++frames >= 30) {
                s_fps = (int)(frames * 1000000LL / (esp_timer_get_time() - fps_from));
                frames = 0, fps_from = esp_timer_get_time();
            }
        } else if (direct) {
            blit_wait();
            board_lock(-1);
            board_direct_end();
            board_unlock();
            direct = false;
        }
        last_ms = r.ms;

        // Alone, a watching page only needs "playing on the round screen": a few states a second leave the radio
        // and PSRAM to the drawn view.
        if (web_running() && now - last_web >= (s_big ? WEB_PERIOD_US : 250000)) {
            last_web = now;
            publish(&g, st);
        }
        vTaskDelay(want_direct ? 1 : pdMS_TO_TICKS(10));
    }
}

// ---------------------------------------------------------------- settings

static void load_best(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    nvs_get_u32(h, "best1", &s_best_ms);
    nvs_get_u32(h, "bestr", &s_best_ring);
    nvs_close(h);
}

/** In a short task of its own with an internal stack: flash is not written from a task whose stack may be in PSRAM. */
static void save_best_task(void *arg) {
    (void)arg;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, "best1", s_best_ms);
        nvs_set_u32(h, "bestr", s_best_ring);
        nvs_commit(h);
        nvs_close(h);
    }
    vTaskDelete(NULL);
}

static void save_best(void) {
    xTaskCreate(save_best_task, "save", 3072, NULL, 2, NULL);
}

// ---------------------------------------------------------------- screens (LVGL task)

static void start_count(bool resume) {
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (!resume) {
        game_reset(&s_game, s_maze);
        ring_reset(&s_ring, s_ring_maze);
    }
    s_resume = resume;
    s_count_start_us = esp_timer_get_time();
    s_sum_x = s_sum_y = 0;
    s_sum_n = 0;
    s_won_flag = false;
    s_st = ST_COUNT;
    xSemaphoreGive(s_mu);
}

static void on_solo(lv_event_t *e) {
    (void)e;
    sound_play(SND_TAP);
    s_big = false;
    start_count(false);
}

static void on_big(lv_event_t *e) {
    (void)e;
    sound_play(SND_TAP);
    s_big = true;
    set_state(ST_WAIT);
}

static void on_start(lv_event_t *e) {
    (void)e;
    sound_play(SND_TAP);
    start_count(false);
}

static void on_resume(lv_event_t *e) {
    (void)e;
    sound_play(SND_TAP);
    start_count(true);
}

static void on_menu(lv_event_t *e) {
    (void)e;
    sound_play(SND_TAP);
    set_state(ST_HOME);
}

static lv_obj_t *dot(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_clickable(o, false);
    lv_obj_set_scrollable(o, false);
    return o;
}

/** The seal's face: the hero of the menu, and the controller's bubble. */
static lv_obj_t *seal_face(lv_obj_t *parent, int cx, int cy, int r) {
    lv_obj_t *o = dot(parent, cx - r, cy - r, 2 * r, 2 * r, YK_INK);
    const int e = r / 7 > 3 ? r / 7 : 3;
    dot(o, r - r * 38 / 100 - e, r - r * 18 / 100 - e, 2 * e, 2 * e, YK_PAPER);
    dot(o, r + r * 38 / 100 - e, r - r * 18 / 100 - e, 2 * e, 2 * e, YK_PAPER);
    dot(o, r - e * 3 / 2, r + r * 20 / 100 - e, 3 * e, 2 * e, YK_PAPER);
    return o;
}

/** The whole maze, small (the controller's map): runs of wall or path as rectangles, the seals, the ball. */
static void minimap(lv_obj_t *parent, int cx, int cy) {
    const maze_t *m = s_maze;
    const int w = m->w * MINI_CELL, h = m->h * MINI_CELL;
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, w, h);
    lv_obj_set_pos(box, cx - w / 2, cy - h / 2);
    lv_obj_set_clickable(box, false);
    lv_obj_set_scrollable(box, false);
    for (int r = 0; r < m->h; r++)
        for (int c = 0; c < m->w;) {
            const bool is_wall = maze_wall(m, r, c);
            int e = c + 1;
            while (e < m->w && maze_wall(m, r, e) == is_wall) e++;
            lv_obj_t *o = dot(box, c * MINI_CELL, r * MINI_CELL, (e - c) * MINI_CELL, MINI_CELL, is_wall ? YK_AMBER : YK_PANEL);
            lv_obj_set_style_radius(o, 0, 0);
            c = e;
        }
    for (int i = 0; i < m->n_seals; i++)
        s_mini_seals[i] = dot(box, (int)(m->seals[i].x * MINI_CELL) - 6, (int)(m->seals[i].y * MINI_CELL) - 5, 12, 10, YK_CREAM);
    s_mini_ball = dot(box, 0, 0, 18, 18, YK_INK);
    s_mini_gate = dot(box, m->exit_c * MINI_CELL, m->exit_r * MINI_CELL, 4, MINI_CELL, YK_CORAL);  // gone once open
    lv_obj_set_style_radius(s_mini_gate, 0, 0);
}

static bool wifi_address(char *out, size_t size) {
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (!netif || esp_netif_get_ip_info(netif, &ip) != ESP_OK || ip.ip.addr == 0) return false;
    snprintf(out, size, IPSTR, IP2STR(&ip.ip));
    return true;
}

static void build(st_t st) {
    if (s_rim) {
        yk_rim_delete(s_rim);
        s_rim = NULL;
    }
    yk_hint_hide();
    lv_obj_clean(s_screen);
    s_count_label = s_time_label = s_mini_ball = s_wait_label = s_url_label = NULL;
    memset(s_mini_seals, 0, sizeof(s_mini_seals));
    s_shown_secs = s_shown_got = s_shown_count = -1;
    s_shown_mask = UINT32_MAX;
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(YK_PAPER), 0);
    s_rim = yk_rim_create(s_screen);
    char text[64], best[16];

    switch (st) {
    case ST_HOME:
        seal_face(s_screen, YK_CX, 118, 52);
        yk_ui_label(s_screen, T("海豹回家", "Seal Home"), 0, 186, 466, YK_SIZE_HEADING, YK_INK);
        yk_ui_button(s_screen, T("单机玩", "Play here"), 103, 236, 260, 64, on_solo, NULL, YK_CORAL);
        yk_ui_button(s_screen, T("连大屏", "Big screen"), 103, 314, 260, 64, on_big, NULL, YK_PANEL);
        if (s_best_ring) {
            fmt_time(s_best_ring, best, sizeof(best));
            snprintf(text, sizeof(text), T("最好 %s", "Best %s"), best);
            yk_ui_label(s_screen, text, 0, 396, 466, YK_SIZE_MIN, YK_INK2);
        }
        if (yk_hint_should(YK_HINT_APPS)) yk_hint_show(s_screen, YK_HINT_APPS, NULL);
        break;
    case ST_WAIT:
        yk_ui_label(s_screen, T("电脑浏览器打开", "In a browser, open"), 0, 84, 466, YK_SIZE_STATUS, YK_INK2);
        yk_ui_label(s_screen, "maze.local", 0, 118, 466, YK_SIZE_HEADING + 8, YK_INK);
        s_url_label = yk_ui_label(s_screen, "", 0, 164, 466, YK_SIZE_MIN, YK_INK2);
        s_wait_label = yk_ui_label(s_screen, "", 0, 204, 466, YK_SIZE_STATUS, YK_SAGE_TEXT);
        yk_ui_button(s_screen, T("开始", "Start"), 133, 248, 200, 64, on_start, NULL, YK_CORAL);
        yk_ui_label(s_screen, T("先拿好板子再点", "Hold it, then tap"), 0, 322, 466, YK_SIZE_MIN, YK_INK2);
        yk_ui_button(s_screen, T("返回", "Back"), 163, 356, 140, 52, on_menu, NULL, YK_PANEL);
        break;
    case ST_COUNT:
        yk_ui_label(s_screen, s_resume ? T("拿好，接着玩", "Hold it, carry on") : T("拿好，这个姿势就是平", "Hold it: this is level"),
                    0, 100, 466, YK_SIZE_STATUS, YK_INK2);
        s_count_label = yk_ui_label(s_screen, "3", 0, 160, 466, 120, YK_CORAL);
        yk_ui_label(s_screen, s_big ? T("摇一摇回起点", "Shake to start over") : T("收齐海豹，回到中间的家", "Find the seals, then go home"),
                    0, 330, 466, YK_SIZE_MIN, YK_INK2);
        yk_rim_set(s_rim, YK_RIM_COUNTDOWN, NULL);
        yk_rim_value(s_rim, 1000);
        break;
    case ST_PLAY:
        if (!s_big) {  // drawn by the game task straight to the panel; LVGL keeps a black screen behind it
            lv_obj_set_style_bg_color(s_screen, lv_color_hex(YK_NIGHT), 0);
            break;
        }
        s_time_label = yk_ui_label(s_screen, "0:00", 0, 40, 466, YK_SIZE_HEADING, YK_INK);
        minimap(s_screen, YK_CX, 244);
        yk_rim_set(s_rim, YK_RIM_PROGRESS, NULL);
        yk_rim_value(s_rim, 0);
        break;
    case ST_PAUSE:
        seal_face(s_screen, YK_CX, 130, 40);
        yk_ui_label(s_screen, T("暂停了", "Paused"), 0, 186, 466, YK_SIZE_HEADING, YK_INK);
        yk_ui_button(s_screen, T("继续", "Resume"), 103, 240, 260, 64, on_resume, NULL, YK_CORAL);
        yk_ui_button(s_screen, T("回菜单", "Menu"), 103, 318, 260, 64, on_menu, NULL, YK_PANEL);
        break;
    case ST_WON:
        yk_ui_label(s_screen, T("到家啦！", "Home!"), 0, 80, 466, YK_SIZE_HEADING, YK_SAGE_TEXT);
        fmt_time(s_last_ms, best, sizeof(best));
        yk_ui_label(s_screen, best, 0, 118, 466, YK_SIZE_HERO_MIN, YK_INK);
        if (s_big) snprintf(text, sizeof(text), T("%s撞石头 %d 次", "%s%d rock bumps"), s_new_best ? T("新纪录 · ", "New best · ") : "", s_last_bumps);
        else snprintf(text, sizeof(text), T("%s猛撞 %d 次", "%s%d hard knocks"), s_new_best ? T("新纪录 · ", "New best · ") : "", s_last_bumps);
        yk_ui_label(s_screen, text, 0, 236, 466, YK_SIZE_STATUS, YK_INK2);
        yk_ui_button(s_screen, T("再来一次", "Again"), 103, 284, 260, 64, on_start, NULL, YK_CORAL);
        yk_ui_button(s_screen, T("回菜单", "Menu"), 133, 360, 200, 52, on_menu, NULL, YK_PANEL);
        yk_rim_set(s_rim, YK_RIM_DONE, NULL);
        break;
    }
}

static void won(void) {
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_last_ms = s_big ? s_game.ms : s_ring.ms;
    s_last_bumps = s_big ? s_game.bumps : s_ring.bumps;
    s_st = ST_WON;
    xSemaphoreGive(s_mu);
    s_wins++;
    uint32_t *best = s_big ? &s_best_ms : &s_best_ring;
    s_new_best = !*best || s_last_ms < *best;
    if (s_new_best) {
        *best = s_last_ms;
        save_best();
    }
    if (!s_best_this_visit || s_last_ms < s_best_this_visit) s_best_this_visit = s_last_ms;
    printf("SEALMAZE_WON ms=%u bumps=%d best=%u big=%d\n", (unsigned)s_last_ms, s_last_bumps, (unsigned)*best, s_big);
}

/** Every touch, where it landed and on what (development builds): `kit logs` shows why a tap did nothing. */
static void log_touch(void) {
    static bool was;
    lv_indev_t *in = board_touch_indev();
    if (!in) return;
    const bool now = lv_indev_get_state(in) == LV_INDEV_STATE_PRESSED;
    if (now != was) {
        lv_point_t p;
        lv_indev_get_point(in, &p);
        lv_obj_t *hit = lv_indev_search_obj(lv_screen_active(), &p);
        printf("SEALMAZE_TOUCH %s x=%d y=%d obj=%s\n", now ? "down" : "up", (int)p.x, (int)p.y,
               !hit ? "none" : hit == s_screen ? "screen" : lv_obj_check_type(hit, &lv_button_class) ? "button" : "other");
        was = now;
    }
}

/** LVGL timer (board lock held): the flow between screens and the live parts of each. */
static void tick(lv_timer_t *timer) {
    (void)timer;
    log_touch();
    st_t st = state();

    // Wi-Fi up: the big screen's page is served from now on.
    if (!web_running() && yk_wifi_state() == YK_WIFI_CONNECTED && web_start() == ESP_OK) {
        char *level = malloc(2048);
        if (level) {
            maze_json(s_maze, s_english, level, 2048);
            web_set_level(level);
            free(level);
        }
    }

    // The radio sleeps between beacons to save power, which holds the big screen's messages back and lets them
    // out in bursts (measured: half of them within 5 ms of another, gaps up to 1.4 s): awake on the big screen.
    static int s_awake = -1;
    const int awake = s_big && st != ST_HOME;
    if (awake != s_awake && yk_wifi_state() == YK_WIFI_CONNECTED) {
        s_awake = awake;
        esp_wifi_set_ps(awake ? WIFI_PS_NONE : WIFI_PS_MIN_MODEM);
    }

    if (s_key_tap) {
        s_key_tap = false;
        if (st == ST_PLAY || st == ST_COUNT) set_state(ST_PAUSE);
        else if (st == ST_WON || st == ST_WAIT || st == ST_PAUSE) set_state(ST_HOME);
    }
    if (s_face_down) {
        s_face_down = false;
        if (st == ST_PLAY || st == ST_COUNT) set_state(ST_PAUSE);
    }
    if (s_face_up) {
        s_face_up = false;
        if (st == ST_PAUSE) start_count(true);
    }
    if (s_shake) {
        s_shake = false;
        if (st == ST_PLAY) {
            xSemaphoreTake(s_mu, portMAX_DELAY);
            game_reset(&s_game, s_maze);
            ring_reset(&s_ring, s_ring_maze);
            xSemaphoreGive(s_mu);
            sound_play(SND_RESET);
            printf("SEALMAZE_SHAKE\n");
        }
    }
    if (s_won_flag && state() == ST_PLAY) {
        s_won_flag = false;
        won();
    }

    st = state();
    if ((int)st != s_shown) {
        if (st == ST_PLAY && !s_resume) s_games++;
        sound_playing(st == ST_COUNT || st == ST_PLAY);
        s_shown = st;
        build(st);
        printf("SEALMAZE_STATE %d big=%d\n", st, s_big);
    }

    if (st == ST_COUNT) {
        const int64_t el = esp_timer_get_time() - s_count_start_us;
        const int left = COUNT_S - (int)(el / 1000000);
        if (left != s_shown_count) {
            s_shown_count = left;
            if (left > 0) {
                char n[12];
                snprintf(n, sizeof(n), "%d", left);
                lv_label_set_text(s_count_label, n);
                sound_play(SND_TICK);
            }
        }
        yk_rim_value(s_rim, (int)(1000 - el / (COUNT_S * 1000)));
        if (el > (COUNT_S - 1) * 1000000LL) {  // the last second: where they hold it is "level"
            float gx, gy;
            gravity(&gx, &gy);
            s_sum_x += gx, s_sum_y += gy, s_sum_n++;
        }
        if (left <= 0) {
            xSemaphoreTake(s_mu, portMAX_DELAY);
            if (s_sum_n) s_g0x = s_sum_x / s_sum_n, s_g0y = s_sum_y / s_sum_n;
            s_st = ST_PLAY;
            xSemaphoreGive(s_mu);
            sound_play(SND_GO);
        }
    } else if (st == ST_PLAY && s_big && s_time_label) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
        const uint32_t ms = s_game.ms;
        const int got = game_seals_got(&s_game);
        xSemaphoreGive(s_mu);
        if ((int)(ms / 1000) != s_shown_secs) {
            s_shown_secs = (int)(ms / 1000);
            char t[16];
            fmt_time(ms, t, sizeof(t));
            lv_label_set_text(s_time_label, t);
        }
        if (got != s_shown_got) {
            s_shown_got = got;
            yk_rim_value(s_rim, got * 1000 / s_maze->n_seals);
        }
        const unsigned ev = s_ui_ev;
        s_ui_ev = 0;
        if (ev & GAME_EV_LOCKED) yk_rim_set(s_rim, YK_RIM_ALERT, NULL);  // one amber flash, then the progress again
        xSemaphoreTake(s_mu, portMAX_DELAY);
        const float bx = s_game.x, by = s_game.y;
        const uint32_t mask = s_game.got;
        xSemaphoreGive(s_mu);
        lv_obj_set_pos(s_mini_ball, (int)(bx * MINI_CELL) - 9, (int)(by * MINI_CELL) - 9);
        if (mask != s_shown_mask) {
            s_shown_mask = mask;
            for (int i = 0; i < s_maze->n_seals; i++) lv_obj_set_hidden(s_mini_seals[i], (mask >> i) & 1);
            lv_obj_set_hidden(s_mini_gate, got == s_maze->n_seals);
        }
    } else if (st == ST_WAIT && s_wait_label) {
        char ip[20], text[48];
        const yk_wifi_state_t w = yk_wifi_state();
        if (w == YK_WIFI_UNCONFIGURED) lv_label_set_text(s_wait_label, T("没有网络，回到 Yay 设置", "No network. Set it up in Yay"));
        else if (w != YK_WIFI_CONNECTED) lv_label_set_text(s_wait_label, T("正在连网…", "Connecting…"));
        else lv_label_set_text(s_wait_label, web_clients() ? T("电脑已连上", "Browser connected") : T("等电脑打开…", "Waiting for the browser…"));
        snprintf(text, sizeof(text), T("或 %s", "or %s"), wifi_address(ip, sizeof(ip)) ? ip : "…");
        lv_label_set_text(s_url_label, text);
    }
}

// ---------------------------------------------------------------- leaving, commands, console

static void on_leave(void *ctx) {
    (void)ctx;
    if (!s_games) return;
    char data[96];
    snprintf(data, sizeof(data), "{\"games\":%d,\"wins\":%d,\"best_ms\":%u}", s_games, s_wins, (unsigned)s_best_this_visit);
    const bool recorded = yk_app_record("sealmaze.play", data) == ESP_OK;
    yk_summary_t summary;
    yk_summary_init(&summary);
    char value[16], sentence[64];
    if (s_wins) {
        fmt_time(s_best_this_visit, value, sizeof(value));
        yk_summary_stat(&summary, value, T("最快", "best"));
        snprintf(sentence, sizeof(sentence), T("海豹回家走通了，最快 %s", "Got the seal home, best %s"), value);
    } else {
        snprintf(sentence, sizeof(sentence), "%s", T("玩了海豹回家，还没走出迷宫", "Played Seal Home, not out yet"));
    }
    snprintf(value, sizeof(value), "%d", s_games);
    yk_summary_stat(&summary, value, T("局", "games"));
    summary.recorded = recorded;
    cJSON *doc = cJSON_Parse(data);
    yk_app_set_result_summary(sentence, &summary, doc);
    cJSON_Delete(doc);
}

static esp_err_t on_command(const char *name, const cJSON *args, cJSON *reply, const char **code, void *ctx) {
    (void)ctx;
    yk_app_input();
    esp_err_t err = yk_app_command(name, args, reply, code);  // app.exit, app.recovery, app.status
    if (err != ESP_ERR_NOT_SUPPORTED) return err;
    err = yk_wifi_command(name, args, reply, code);  // wifi.scan, wifi.join
    if (err != ESP_ERR_NOT_FOUND) return err;
    if (!strcmp(name, "sealmaze.status")) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
        game_t g = s_game;
        const ring_game_t r = s_ring;
        xSemaphoreGive(s_mu);
        cJSON_AddNumberToObject(reply, "state", state());
        cJSON_AddBoolToObject(reply, "big", s_big);
        if (!s_big) g.x = r.x, g.y = r.y, g.ms = r.ms, g.got = r.got;  // alone: the round maze, px from the centre
        cJSON_AddNumberToObject(reply, "x", g.x);
        cJSON_AddNumberToObject(reply, "y", g.y);
        cJSON_AddNumberToObject(reply, "got", game_seals_got(&g));
        cJSON_AddNumberToObject(reply, "ms", g.ms);
        cJSON_AddNumberToObject(reply, "tx", s_tx);
        cJSON_AddNumberToObject(reply, "ty", s_ty);
        cJSON_AddNumberToObject(reply, "fps", s_fps);
        cJSON_AddNumberToObject(reply, "render_us", s_render_us);
        cJSON_AddNumberToObject(reply, "blit_us", s_blit_us);
        cJSON_AddNumberToObject(reply, "pages", web_clients());
        uint32_t sent, dropped, slowest;
        web_stats(&sent, &dropped, &slowest);
        cJSON_AddNumberToObject(reply, "web_sent", sent);
        cJSON_AddNumberToObject(reply, "web_dropped", dropped);
        cJSON_AddNumberToObject(reply, "web_send_us_max", slowest);
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) cJSON_AddNumberToObject(reply, "rssi", ap.rssi);
        wifi_ps_type_t ps;
        if (esp_wifi_get_ps(&ps) == ESP_OK) cJSON_AddNumberToObject(reply, "ps", ps);
        cJSON_AddStringToObject(reply, "wifi", yk_wifi_state_name());
        return ESP_OK;
    }
    if (!strcmp(name, "sealmaze.start")) {  // {"mode":"solo"|"big"}: straight into the countdown
        const cJSON *mode = cJSON_GetObjectItem(args, "mode");
        s_big = cJSON_IsString(mode) && !strcmp(mode->valuestring, "big");
        start_count(false);
        return ESP_OK;
    }
    if (!strcmp(name, "sealmaze.tilt")) {  // {"x":0.2,"y":0,"ms":1500}: a tilt instead of the sensor's, for a while
        const cJSON *x = cJSON_GetObjectItem(args, "x"), *y = cJSON_GetObjectItem(args, "y"), *ms = cJSON_GetObjectItem(args, "ms");
        s_ox = cJSON_IsNumber(x) ? (float)x->valuedouble : 0;
        s_oy = cJSON_IsNumber(y) ? (float)y->valuedouble : 0;
        s_override_until_us = esp_timer_get_time() + (cJSON_IsNumber(ms) ? ms->valueint : 1000) * 1000LL;
        return ESP_OK;
    }
    if (!strcmp(name, "sealmaze.click")) {  // {"x":233,"y":346}: what a tap there would hit, clicked
        const cJSON *x = cJSON_GetObjectItem(args, "x"), *y = cJSON_GetObjectItem(args, "y");
        lv_point_t p = {cJSON_IsNumber(x) ? x->valueint : YK_CX, cJSON_IsNumber(y) ? y->valueint : YK_CY};
        board_lock(-1);
        lv_obj_t *hit = lv_indev_search_obj(lv_screen_active(), &p);
        while (hit && !lv_obj_check_type(hit, &lv_button_class) && lv_obj_get_parent(hit)) hit = lv_obj_get_parent(hit);
        const bool button = hit && lv_obj_check_type(hit, &lv_button_class);
        if (button) lv_obj_send_event(hit, LV_EVENT_CLICKED, NULL);
        board_unlock();
        cJSON_AddBoolToObject(reply, "button", button);
        return ESP_OK;
    }
    if (!strcmp(name, "sealmaze.home")) {
        set_state(ST_HOME);
        return ESP_OK;
    }
    *code = "UNKNOWN_COMMAND";
    return ESP_ERR_NOT_SUPPORTED;
}

static int heap_cmd(int argc, char **argv) {
    (void)argc, (void)argv;
    printf("HEAP internal=%u internal_min=%u psram=%u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return 0;
}

static void start_console(void) {
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    cfg.prompt = APP_SLUG ">";
    cfg.task_stack_size = 8192;
    esp_console_dev_usb_serial_jtag_config_t usb = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&usb, &cfg, &repl));
    esp_console_register_help_command();
    ESP_ERROR_CHECK(yk_link_register_console());
    ESP_ERROR_CHECK(yk_app_register_console());
    ESP_ERROR_CHECK(yk_speak_register_console());
    ESP_ERROR_CHECK(yk_wifi_register_console());
    ESP_ERROR_CHECK(yk_motion_register_console());
    yk_ui_register_snapshot();
    const esp_console_cmd_t heap = {.command = "heap", .help = "available memory", .func = heap_cmd};
    ESP_ERROR_CHECK(esp_console_cmd_register(&heap));
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}

void app_main(void) {
    // 1. NVS: never erased (shared with every app, holds the Wi-Fi credentials).
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) ESP_LOGE(TAG, "NVS unavailable (%s); running without saved settings", esp_err_to_name(err));
    // 2. yaykit_app first: a crash anywhere below counts towards the crash loop guard.
    ESP_ERROR_CHECK(yk_app_start(&(yk_app_config_t){.slug = APP_SLUG, .version = APP_VERSION}));
    yk_app_on_leave(on_leave, NULL);
    s_mu = xSemaphoreCreateMutex();
    game_reset(&s_game, s_maze);
    s_ring_maze = heap_caps_malloc(sizeof(ring_maze_t), MALLOC_CAP_SPIRAM);
    if (!s_ring_maze) {
        ESP_LOGE(TAG, "no memory for the round maze");
        return;
    }
    ring_maze_build(s_ring_maze, RING_SEED);
    ring_reset(&s_ring, s_ring_maze);
    load_best();
    // 3. Board and screen (dark until the first frame), the speaker (low latency: a knock must follow the wall),
    //    the side key, the motion sensor.
    if (!board_display_start_opts(&(board_display_opts_t){.start_dark = true})) {
        ESP_LOGE(TAG, "display failed");
        return;
    }
    board_rtc_init();
    if (yk_speak_init_cfg(&(yk_speak_config_t){.rate = 16000, .block_ms = 5}) == ESP_OK) sound_init();
    ESP_ERROR_CHECK(yk_key_init(&(yk_key_config_t){.delay_tap = true}));
    ESP_ERROR_CHECK(yk_app_on_key(on_key, NULL));
    if (yk_motion_start(&(yk_motion_config_t){.on_event = on_motion}) != ESP_OK) ESP_LOGE(TAG, "no motion sensor");
    // 4. Yaykit Link: `yk info` and `yk cmd` over USB.
    const yk_link_config_t link = {.board = "tikpal-r1", .app_slug = APP_SLUG, .app_version = APP_VERSION,
                                   .layout = "yaykit-v1", .idle_seconds = 90, .on_command = on_command};
    ESP_ERROR_CHECK(yk_link_init(&link));
    // The owner's language, as kept on the device (NVS only: no cloud calls).
    if (yk_account_init(&(yk_account_config_t){.app_slug = APP_SLUG, .app_version = APP_VERSION, .board = "tikpal-r1"}) == ESP_OK)
        s_english = yk_account_english();
    // 5. Launch parameters: none taken in this version.
    yk_launch_t launch;
    (void)yk_app_launch(&launch);

    // 6. The screen; then confirm the image.
    board_lock(-1);
    ESP_ERROR_CHECK(cjk_font_init());
    s_screen = lv_obj_create(NULL);
    lv_obj_set_scrollable(s_screen, false);
    tick(NULL);
    lv_screen_load(s_screen);
    lv_refr_now(NULL);
    board_brightness_set(board_pref_brightness());
    lv_timer_create(tick, 30, NULL);
    board_unlock();
    xTaskCreatePinnedToCoreWithCaps(game_task, "game", 6144, NULL, 4, NULL, 1, MALLOC_CAP_SPIRAM);
    // 7. The console, then yk_app_ready(); then the network for the big screen.
    start_console();
    yk_app_ready();
    err = yk_wifi_start();
    if (err != ESP_OK) ESP_LOGW(TAG, "wifi: %s", esp_err_to_name(err));
}
