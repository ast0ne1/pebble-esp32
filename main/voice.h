#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* PTT: hold records. Release → loopback if unpaired, or StonePi ask if paired. */

void voice_ptt_down(void);
void voice_ptt_up(void);
bool voice_is_busy(void); /* recording, play, sfx, or ask in flight */

/* After paired PTT release: true if an ask was started (shell should wait). */
bool voice_ask_in_flight(void);
/* Ask finished without playback (error / empty) — clear busy + resume wake. */
void voice_ask_clear(void);

/* Play 22050 Hz mono int16 (takes ownership of pcm via free). */
bool voice_play_pcm22050(int16_t *pcm, size_t nsamp);

/* Short character chirp (nudge). No-op if PTT/loopback busy. variant cycles 0..n-1. */
void voice_play_nudge(unsigned variant);
