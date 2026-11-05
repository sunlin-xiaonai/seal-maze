#include "sound.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "yaykit_speak.h"

static const char *TAG = "sound";

#define VOICES 12

/** One note: a sine gliding from f0 to f1, some noise in it, a quick attack and a falling tail. */
typedef struct {
    float f0, f1, dur, vol, noise;
} note_t;

typedef struct {
    volatile bool used;
    note_t n;
    float t, phase;
    uint32_t seed;
} voice_t;

static voice_t s_voices[VOICES];
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_ready;

static bool render(float *out, size_t n, const yk_sfx_block_t *blk, void *ctx) {
    voice_t *v = ctx;
    const float dt = 1.0f / blk->rate;
    for (size_t i = 0; i < n; i++) {
        const float u = v->t / v->n.dur;
        if (u >= 1) {
            out[i] = 0;
            continue;
        }
        const float f = v->n.f0 + (v->n.f1 - v->n.f0) * u;
        v->phase += 2 * (float)M_PI * f * dt;
        if (v->phase > 2 * (float)M_PI) v->phase -= 2 * (float)M_PI;
        v->seed = v->seed * 1664525u + 1013904223u;
        const float noise = (float)(v->seed >> 9) / (float)(1u << 23) * 2 - 1;
        const float attack = v->t < 0.004f ? v->t / 0.004f : 1;
        const float env = attack * (1 - u) * (1 - u);
        out[i] = ((1 - v->n.noise) * sinf(v->phase) + v->n.noise * noise) * env * v->n.vol;
        v->t += dt;
    }
    if (v->t >= v->n.dur) {
        v->used = false;
        return false;
    }
    return true;
}

static void note(note_t n, int delay_ms, uint8_t group, uint16_t gap_ms) {
    if (!s_ready) return;
    voice_t *v = NULL;
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < VOICES; i++)
        if (!s_voices[i].used) {
            v = &s_voices[i];
            v->used = true;
            break;
        }
    taskEXIT_CRITICAL(&s_mux);
    if (!v) return;
    v->n = n;
    v->t = 0;
    v->phase = 0;
    v->seed = (uint32_t)esp_timer_get_time();
    const yk_sfx_opts_t opts = {.at_us = delay_ms ? esp_timer_get_time() + delay_ms * 1000LL : 0,
                                .group = group,
                                .min_gap_ms = gap_ms,
                                .no_duck = true};
    if (yk_sfx_play_synth(render, v, &opts) <= 0) v->used = false;
}

void sound_init(void) {
    s_ready = true;
    ESP_LOGI(TAG, "%d voices", VOICES);
}

void sound_playing(bool on) {
    if (s_ready) yk_speak_hold_amp(on);
}

void sound_play(snd_t s) {
    switch (s) {
    case SND_WALL: note((note_t){170, 80, 0.09f, 0.55f, 0.3f}, 0, 1, 90); break;
    case SND_ROCK:
        note((note_t){420, 220, 0.12f, 0.7f, 0.35f}, 0, 2, 140);
        note((note_t){880, 600, 0.06f, 0.35f, 0.1f}, 0, 0, 0);
        break;
    case SND_SEAL:  // a little rising sparkle
        note((note_t){988, 988, 0.12f, 0.55f, 0}, 0, 0, 0);
        note((note_t){1319, 1319, 0.12f, 0.55f, 0}, 70, 0, 0);
        note((note_t){1976, 1976, 0.25f, 0.45f, 0}, 140, 0, 0);
        break;
    case SND_LOCKED:
        note((note_t){260, 200, 0.16f, 0.6f, 0.1f}, 0, 3, 600);
        note((note_t){220, 170, 0.2f, 0.6f, 0.1f}, 170, 0, 0);
        break;
    case SND_OPEN:
        for (int i = 0; i < 4; i++) {
            const float f = 784.0f * powf(1.26f, (float)i);
            note((note_t){f, f, i == 3 ? 0.4f : 0.16f, 0.55f, 0}, i * 90, 0, 0);
        }
        break;
    case SND_WIN: {
        static const float f[] = {523, 659, 784, 1047, 1319, 1568};
        for (int i = 0; i < 6; i++) note((note_t){f[i], f[i], i == 5 ? 0.7f : 0.18f, 0.6f, 0}, i * 110, 0, 0);
        break;
    }
    case SND_TICK: note((note_t){660, 660, 0.08f, 0.3f, 0}, 0, 0, 0); break;
    case SND_GO: note((note_t){990, 990, 0.18f, 0.35f, 0}, 0, 0, 0); break;
    case SND_RESET: note((note_t){620, 260, 0.25f, 0.3f, 0.05f}, 0, 0, 0); break;
    case SND_TAP: note((note_t){1200, 1000, 0.04f, 0.2f, 0}, 0, 4, 60); break;
    }
}
