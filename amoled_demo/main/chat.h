#pragma once
// Talking with Claude from the board (the square one: it has a speaker and microphones). The Mac app does the
// listening and thinking: it keeps a connection open to the board (TCP, CHAT_PORT, its paired key first), the board
// sends what the microphones hear, and the Mac sends back Claude's spoken reply, what it heard you say, and what it's
// doing. The page (chat_ui.c) shows it and has the buttons.
//
// Each message on the connection: a type byte, a 3-byte big-endian length, then that many bytes.
//   'A': 16 kHz 16-bit little-endian mono audio (board -> Mac: the microphones; Mac -> board: speech to play)
//   'J': a JSON object with "t" saying what it is:
//     Mac -> board: hello {key} (first, or the board hangs up), state {s: idle|listening|thinking|speaking},
//       heard {text} (what you said, as far as it's got), say {text} (what Claude is saying now), status {text}
//       (what it's doing, or what's wrong: "" clears it), models {names: [...], current}, flush (drop the queued
//       speech: you talked over it), end (the chat's over: Claude said goodbye), ask {id, q: agenda|music, a}
//       (something only the board knows or can do; answered with reply {id, text})
//     board -> Mac: ready {active, muted, speaker}, start (the mic was tapped: a chat begins), mute {on},
//       speaker {on}, model {i}, new (start a new conversation), stop (the chat was closed here), played (the
//       queued speech has all played), reply {id, text}
#include <stdbool.h>
#include <stdint.h>

#ifdef BOBBLE_SIM
#define CHAT_PORT (sim_http_port() + 100)   // (each simulator's own: they run side by side)
#else
#define CHAT_PORT 8470
#endif

enum { CS_IDLE, CS_LISTENING, CS_THINKING, CS_SPEAKING };
#define CHAT_MODELS 6

typedef struct {
    bool mac;                  // the Mac app is connected (nothing works without it)
    bool active;               // a chat is on
    bool muted;                // the microphones are muted (the chat goes on)
    bool speaker_muted;
    int state;                 // CS_*: what Claude is doing, as the Mac says
    char heard[600];           // what you said last (all of it: a long sentence lost its end at 200)
    char said[400];            // what Claude is saying (or said last)
    char status[96];           // what it's doing, or what's wrong
    int n_models, model;       // the models to pick from, and the one in use
    char models[CHAT_MODELS][24];
    uint32_t version;          // goes up whenever any of the above changes
} chat_status_t;

void chat_start(void);         // listens for the Mac (call once, with Wi-Fi up or coming)
bool chat_available(void);     // this board can talk
int chat_port(void);           // for /info, so the Mac knows where to connect (0: can't talk)
void chat_get(chat_status_t *out);
bool chat_active(void);        // a voice chat is on (PSRAM's busy then: the echo canceller)
float chat_level(void);        // 0..1: how loud it is right now (you, or Claude while it speaks): for the button's glow

void chat_tap_mic(void);       // not in a chat: start one; in one: mute / unmute the microphones
void chat_toggle_speaker(void);
void chat_pick_model(int i);
void chat_new(void);           // forget the conversation and start a fresh one
void chat_close(void);         // end the chat (the X, leaving the page, or nobody talking for a while)
