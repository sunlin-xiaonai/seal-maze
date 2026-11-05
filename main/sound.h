// The game's sounds, made on the speaker's mixer (yaykit_speak synth voices): no clips to pack. Short and soft, a
// sound for every touch of the world since the device has no motor (round-screen spec, principle 7).
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SND_WALL,    // a knock on a wall
    SND_ROCK,    // a rock: a duller knock
    SND_SEAL,    // a seal collected: two notes up
    SND_LOCKED,  // the exit is still closed
    SND_OPEN,    // the last seal: the exit opens
    SND_WIN,     // home
    SND_TICK,    // the countdown
    SND_GO,
    SND_RESET,   // shaken back to the start
    SND_TAP,     // a button
} snd_t;

void sound_init(void);
/** From any task; dropped when every voice is busy. */
void sound_play(snd_t s);
/** Keep the amplifier on while playing (a sound must not wait 300 ms for it). */
void sound_playing(bool on);

#ifdef __cplusplus
}
#endif
