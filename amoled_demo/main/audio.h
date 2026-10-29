#pragma once
// The square board's speaker and microphones, for talking with Claude (chat.c). 16 kHz, 16-bit mono both ways.
// What the microphones hear has the speaker's own sound taken out of it (the codec feeds what it plays back in on a
// spare channel, and esp-sr's echo canceller subtracts it), so you can talk over a reply. The Square simulator has the
// page but no sound (a chat only ever uses the board's own speaker and microphones); the round board has neither.
#include <stdbool.h>
#include <stdint.h>

#define AUDIO_RATE 16000

typedef void (*audio_mic_cb)(const int16_t *pcm, int n);   // called from the audio task, a few times a second

bool audio_available(void);              // this board can talk (the square board, and its simulator)
bool audio_start(audio_mic_cb cb);       // microphones and speaker on; false if the hardware didn't come up
void audio_stop(void);                   // both off (the amplifier too)
void audio_play(const int16_t *pcm, int n);   // queue speech for the speaker
void audio_flush(void);                  // drop whatever's still queued (you talked over it)
int audio_queued_ms(void);               // how much is still to play
void audio_set_speaker_mute(bool mute);  // keeps playing (silently), so replies stay in step
float audio_mic_level(void);             // 0..1: how loud the microphone is now (after echo cancelling)
float audio_out_level(void);             // 0..1: how loud what's playing is now
